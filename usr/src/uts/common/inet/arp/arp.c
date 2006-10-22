/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/* Copyright (c) 1990 Mentat Inc. */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/* AR - Address Resolution Protocol */

#include <sys/types.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/errno.h>
#include <sys/strlog.h>
#include <sys/dlpi.h>
#include <sys/sockio.h>
#define	_SUN_TPI_VERSION	2
#include <sys/tihdr.h>
#include <sys/socket.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/sdt.h>
#include <sys/vtrace.h>
#include <sys/strsun.h>
#include <sys/policy.h>
#include <sys/ethernet.h>
#include <sys/zone.h>
#include <sys/random.h>
#include <sys/sdt.h>
#include <sys/hook_event.h>

#include <inet/common.h>
#include <inet/optcom.h>
#include <inet/mi.h>
#include <inet/nd.h>
#include <inet/snmpcom.h>
#include <net/if.h>
#include <inet/arp.h>
#include <netinet/ip6.h>
#include <netinet/arp.h>
#include <inet/ip.h>
#include <inet/ip_ire.h>
#include <inet/ip_ndp.h>
#include <inet/mib2.h>
#include <inet/arp_impl.h>

/*
 * ARP entry life time and design notes
 * ------------------------------------
 *
 * ARP entries (ACEs) must last at least as long as IP knows about a given
 * MAC-IP translation (i.e., as long as the IRE cache entry exists).  It's ok
 * if the ARP entry lasts longer, but not ok if it is removed before the IP
 * entry.  The reason for this is that if ARP doesn't have an entry, we will be
 * unable to detect the difference between an ARP broadcast that represents no
 * change (same, known address of sender) and one that represents a change (new
 * address for existing entry).  In the former case, we must not notify IP, or
 * we can suffer hurricane attack.  In the latter case, we must notify IP, or
 * IP will drift out of sync with the network.
 *
 * Note that IP controls the lifetime of entries, not ARP.
 *
 * We don't attempt to reconfirm aging entries.  If the system is no longer
 * talking to a given peer, then it doesn't matter if we have the right mapping
 * for that peer.  It would be possible to send queries on aging entries that
 * are active, but this isn't done.
 */

/*
 * This is used when scanning for "old" (least recently broadcast) ACEs.  We
 * don't want to have to walk the list for every single one, so we gather up
 * batches at a time.
 */
#define	ACE_RESCHED_LIST_LEN	8

typedef struct {
	arl_t	*art_arl;
	uint_t	art_naces;
	ace_t	*art_aces[ACE_RESCHED_LIST_LEN];
} ace_resched_t;

#define	ACE_RESOLVED(ace)	((ace)->ace_flags & ACE_F_RESOLVED)
#define	ACE_NONPERM(ace)	\
	(((ace)->ace_flags & (ACE_F_RESOLVED | ACE_F_PERMANENT)) == \
	ACE_F_RESOLVED)

#define	AR_DEF_XMIT_INTERVAL	500	/* time in milliseconds */
#define	AR_LL_HDR_SLACK	32	/* Leave the lower layer some room */

#define	AR_SNMP_MSG		T_OPTMGMT_ACK
#define	AR_DRAINING		(void *)0x11

/*
 * The IPv4 Link Local address space is special; we do extra duplicate checking
 * there, as the entire assignment mechanism rests on random numbers.
 */
#define	IS_IPV4_LL_SPACE(ptr)	(((uchar_t *)ptr)[0] == 169 && \
				((uchar_t *)ptr)[1] == 254)

/*
 * Check if the command needs to be enqueued by seeing if there are other
 * commands ahead of us or if some DLPI response is being awaited. Usually
 * there would be an enqueued command in the latter case, however if the
 * stream that originated the command has closed, the close would have
 * cleaned up the enqueued command. AR_DRAINING signifies that the command
 * at the head of the arl_queue has been internally dequeued on completion
 * of the previous command and is being called from ar_dlpi_done
 */
#define	CMD_NEEDS_QUEUEING(mp, arl)					\
	(mp->b_prev != AR_DRAINING && (arl->arl_queue != NULL ||	\
	    arl->arl_dlpi_pending != DL_PRIM_INVAL))

#define	ACE_EXTERNAL_FLAGS_MASK \
	(ACE_F_PERMANENT | ACE_F_PUBLISH | ACE_F_MAPPING | ACE_F_MYADDR | \
	ACE_F_AUTHORITY)

#define	ARH_FIXED_LEN	8

/*
 * MAC-specific intelligence.  Shouldn't be needed, but the DL_INFO_ACK
 * doesn't quite do it for us.
 */
typedef struct ar_m_s {
	t_uscalar_t	ar_mac_type;
	uint32_t	ar_mac_arp_hw_type;
	t_scalar_t	ar_mac_sap_length;
	uint32_t	ar_mac_hw_addr_length;
} ar_m_t;

/* Named Dispatch Parameter Management Structure */
typedef struct arpparam_s {
	uint32_t	arp_param_min;
	uint32_t	arp_param_max;
	uint32_t	arp_param_value;
	char		*arp_param_name;
} arpparam_t;

typedef struct ar_snmp_hashb {
	struct	ar_snmp_hashb		*ar_next_entry;
	mib2_ipNetToMediaEntry_t	*ar_snmp_entry;
} ar_snmp_hashb_t;

static int ar_snmp_hash_size = 64;

typedef struct msg2_args {
	ar_snmp_hashb_t *m2a_hashb;
	mblk_t	*m2a_mpdata;
	mblk_t	*m2a_mptail;
} msg2_args_t;

extern ire_stats_t ire_stats_v4;

static mblk_t	*ar_alloc(uint32_t cmd, int);
static int	ar_ce_create(arl_t *arl, uint32_t proto, uchar_t *hw_addr,
    uint32_t hw_addr_len, uchar_t *proto_addr,
    uint32_t proto_addr_len, uchar_t *proto_mask,
    uchar_t *proto_extract_mask, uint32_t hw_extract_start,
    uint32_t flags);
static void	ar_ce_delete(ace_t *ace);
static void	ar_ce_delete_per_arl(ace_t *ace, void *arg);
static ace_t	**ar_ce_hash(uint32_t proto, const uchar_t *proto_addr,
    uint32_t proto_addr_length);
static ace_t	*ar_ce_lookup(arl_t *arl, uint32_t proto,
    uchar_t *proto_addr, uint32_t proto_addr_length);
static ace_t	*ar_ce_lookup_entry(arl_t *arl, uint32_t proto,
    uchar_t *proto_addr, uint32_t proto_addr_length);
static ace_t	*ar_ce_lookup_from_area(mblk_t *mp, ace_t *matchfn());
static ace_t	*ar_ce_lookup_mapping(arl_t *arl, uint32_t proto,
    uchar_t *proto_addr, uint32_t proto_addr_length);
static boolean_t ar_ce_resolve(ace_t *ace, const uchar_t *hw_addr,
    uint32_t hw_addr_length);
static void	ar_ce_walk(void (*pfi)(ace_t *, void *), void *arg1);

static void	ar_cleanup(void);
static void	ar_client_notify(const arl_t *arl, mblk_t *mp, int code);
static int	ar_close(queue_t *q);
static int	ar_cmd_dispatch(queue_t *q, mblk_t *mp);
static mblk_t	*ar_dlpi_comm(t_uscalar_t prim, size_t size);
static void	ar_dlpi_send(arl_t *, mblk_t *);
static void	ar_dlpi_done(arl_t *, t_uscalar_t);
static void	ar_cmd_done(arl_t *arl);
static int	ar_entry_add(queue_t *q, mblk_t *mp);
static int	ar_entry_delete(queue_t *q, mblk_t *mp);
static int	ar_entry_query(queue_t *q, mblk_t *mp);
static int	ar_entry_squery(queue_t *q, mblk_t *mp);
static int	ar_interface_up(queue_t *q, mblk_t *mp);
static int	ar_interface_down(queue_t *q, mblk_t *mp);
static int	ar_interface_on(queue_t *q, mblk_t *mp);
static int	ar_interface_off(queue_t *q, mblk_t *mp);
static void	ar_ll_cleanup_arl_queue(queue_t *q);
static void	ar_ll_down(arl_t *arl);
static arl_t	*ar_ll_lookup_by_name(const char *name);
static arl_t	*ar_ll_lookup_from_mp(mblk_t *mp);
static void	ar_ll_init(ar_t *, mblk_t *mp);
static void	ar_ll_set_defaults(arl_t *, mblk_t *mp);
static void	ar_ll_clear_defaults(arl_t *);
static int	ar_ll_up(arl_t *arl);
static int	ar_mapping_add(queue_t *q, mblk_t *mp);
static boolean_t	ar_mask_all_ones(uchar_t *mask, uint32_t mask_len);
static ar_m_t	*ar_m_lookup(t_uscalar_t mac_type);
static int	ar_nd_ioctl(queue_t *q, mblk_t *mp);
static int	ar_open(queue_t *q, dev_t *devp, int flag, int sflag,
    cred_t *credp);
static int	ar_param_get(queue_t *q, mblk_t *mp, caddr_t cp, cred_t *cr);
static boolean_t	ar_param_register(arpparam_t *arppa, int cnt);
static int	ar_param_set(queue_t *q, mblk_t *mp, char *value,
    caddr_t cp, cred_t *cr);
static void	ar_query_delete(ace_t *ace, void *ar);
static void	ar_query_reply(ace_t *ace, int ret_val,
    uchar_t *proto_addr, uint32_t proto_addr_len);
static clock_t	ar_query_xmit(ace_t *ace, ace_t *src_ace);
static void	ar_rput(queue_t *q, mblk_t *mp_orig);
static void	ar_rput_dlpi(queue_t *q, mblk_t *mp);
static void	ar_set_address(ace_t *ace, uchar_t *addrpos,
    uchar_t *proto_addr, uint32_t proto_addr_len);
static int	ar_slifname(queue_t *q, mblk_t *mp);
static int	ar_set_ppa(queue_t *q, mblk_t *mp);
static int	ar_snmp_msg(queue_t *q, mblk_t *mp_orig);
static void	ar_snmp_msg2(ace_t *, void *);
static void	ar_wput(queue_t *q, mblk_t *mp);
static void	ar_wsrv(queue_t *q);
static void	ar_xmit(arl_t *arl, uint32_t operation, uint32_t proto,
    uint32_t plen, const uchar_t *haddr1, const uchar_t *paddr1,
    const uchar_t *haddr2, const uchar_t *paddr2, const uchar_t *dstaddr);
static uchar_t  *ar_snmp_msg_element(mblk_t **, uchar_t *, size_t);
static void	ar_cmd_enqueue(arl_t *arl, mblk_t *mp, queue_t *q,
    ushort_t cmd, boolean_t);
static mblk_t	*ar_cmd_dequeue(arl_t *arl);

/*
 * All of these are alterable, within the min/max values given,
 * at run time. arp_publish_interval and arp_publish_count are
 * set by default to 2 seconds and 5 respectively. This is
 * useful during FAILOVER/FAILBACK to make sure that the ARP
 * packets are not lost. Assumed that it does not affect the
 * normal operations.
 */
static arpparam_t	arp_param_arr[] = {
	/* min		max		value	name */
	{ 30000,	3600000,	300000,	"arp_cleanup_interval"},
	{ 1000,		20000,		2000,	"arp_publish_interval"},
	{ 1,		20,		5,	"arp_publish_count"},
	{ 0,		20000,		1000,	"arp_probe_delay"},
	{ 10,		20000,		1500,	"arp_probe_interval"},
	{ 0,		20,		3,	"arp_probe_count"},
	{ 0,		20000,		100,	"arp_fastprobe_delay"},
	{ 10,		20000,		150,	"arp_fastprobe_interval"},
	{ 0,		20,		3,	"arp_fastprobe_count"},
	{ 0,		3600000,	300000,	"arp_defend_interval"},
	{ 0,		20000,		100,	"arp_defend_rate"},
	{ 0,		3600000,	15000,	"arp_broadcast_interval"},
	{ 5,		86400,		3600,	"arp_defend_period"}
};

#define	arp_cleanup_interval	arp_param_arr[0].arp_param_value
#define	arp_publish_interval	arp_param_arr[1].arp_param_value
#define	arp_publish_count	arp_param_arr[2].arp_param_value
#define	arp_probe_delay		arp_param_arr[3].arp_param_value
#define	arp_probe_interval	arp_param_arr[4].arp_param_value
#define	arp_probe_count		arp_param_arr[5].arp_param_value
#define	arp_fastprobe_delay	arp_param_arr[6].arp_param_value
#define	arp_fastprobe_interval	arp_param_arr[7].arp_param_value
#define	arp_fastprobe_count	arp_param_arr[8].arp_param_value
#define	arp_defend_interval	arp_param_arr[9].arp_param_value
#define	arp_defend_rate		arp_param_arr[10].arp_param_value
#define	arp_broadcast_interval	arp_param_arr[11].arp_param_value
#define	arp_defend_period	arp_param_arr[12].arp_param_value

static struct module_info info = {
	0, "arp", 0, INFPSZ, 512, 128
};

static struct qinit rinit = {
	(pfi_t)ar_rput, NULL, ar_open, ar_close, NULL, &info
};

static struct qinit winit = {
	(pfi_t)ar_wput, (pfi_t)ar_wsrv, ar_open, ar_close, NULL, &info
};

struct streamtab arpinfo = {
	&rinit, &winit
};

static void	*ar_g_head;	/* AR Instance Data List Head */
static caddr_t	ar_g_nd;	/* AR Named Dispatch Head */

/*
 * With the introduction of netinfo (neti kernel module), it is now possible
 * to access data structures in the ARP module without the code being
 * executed in the context of the IP module, thus there is no locking being
 * enforced through the use of STREAMS.
 *
 *
 */
krwlock_t	arl_g_lock;
arl_t		*arl_g_head;	/* ARL List Head */

/*
 * TODO: we need a better mechanism to set the ARP hardware type since
 * the DLPI mac type does not include enough predefined values.
 */
static ar_m_t	ar_m_tbl[] = {
	{ DL_CSMACD,	ARPHRD_ETHER,	-2,	6},	/* 802.3 */
	{ DL_TPB,	ARPHRD_IEEE802,	-2,	6},	/* 802.4 */
	{ DL_TPR,	ARPHRD_IEEE802,	-2,	6},	/* 802.5 */
	{ DL_METRO,	ARPHRD_IEEE802,	-2,	6},	/* 802.6 */
	{ DL_ETHER,	ARPHRD_ETHER,	-2,	6},	/* Ethernet */
	{ DL_FDDI,	ARPHRD_ETHER,	-2,	6},	/* FDDI */
	{ DL_IB,	ARPHRD_IB,	-2,	20},	/* Infiniband */
	{ DL_OTHER,	ARPHRD_ETHER,	-2,	6},	/* unknown */
};

/* ARP Cache Entry Hash Table */
static ace_t	*ar_ce_hash_tbl[ARP_HASH_SIZE];

static ace_t	*ar_ce_mask_entries;	/* proto_mask not all ones */

static uint32_t	arp_index_counter = 1;
static uint32_t	arp_counter_wrapped = 0;

/*
 * Note that all routines which need to queue the message for later
 * processing have to be ioctl_aware to be able to queue the complete message.
 * Following are command entry flags in arct_flags
 */
#define	ARF_IOCTL_AWARE	0x1	/* Arp command can come down as M_IOCTL */
#define	ARF_ONLY_CMD	0x2	/* Command is exclusive to ARP */

/* ARP Cmd Table entry */
typedef struct arct_s {
	int		(*arct_pfi)(queue_t *, mblk_t *);
	uint32_t	arct_cmd;
	int		arct_min_len;
	uint32_t	arct_flags;
	int		arct_priv_req;	/* Privilege required for this cmd */
	const char	*arct_txt;
} arct_t;

static arct_t	ar_cmd_tbl[] = {
	{ ar_entry_add,		AR_ENTRY_ADD,		sizeof (area_t),
	    ARF_IOCTL_AWARE | ARF_ONLY_CMD, OP_CONFIG, "AR_ENTRY_ADD" },
	{ ar_entry_delete,	AR_ENTRY_DELETE,	sizeof (ared_t),
	    ARF_IOCTL_AWARE | ARF_ONLY_CMD, OP_CONFIG, "AR_ENTRY_DELETE" },
	{ ar_entry_query,	AR_ENTRY_QUERY,		sizeof (areq_t),
	    ARF_IOCTL_AWARE | ARF_ONLY_CMD, OP_NP, "AR_ENTRY_QUERY" },
	{ ar_entry_squery,	AR_ENTRY_SQUERY,	sizeof (area_t),
	    ARF_IOCTL_AWARE | ARF_ONLY_CMD, OP_NP, "AR_ENTRY_SQUERY" },
	{ ar_mapping_add,	AR_MAPPING_ADD,		sizeof (arma_t),
	    ARF_IOCTL_AWARE | ARF_ONLY_CMD, OP_CONFIG, "AR_MAPPING_ADD" },
	{ ar_interface_up,	AR_INTERFACE_UP,	sizeof (arc_t),
	    ARF_ONLY_CMD, OP_CONFIG, "AR_INTERFACE_UP" },
	{ ar_interface_down,	AR_INTERFACE_DOWN,	sizeof (arc_t),
	    ARF_ONLY_CMD, OP_CONFIG, "AR_INTERFACE_DOWN" },
	{ ar_interface_on,	AR_INTERFACE_ON,	sizeof (arc_t),
	    ARF_ONLY_CMD, OP_CONFIG, "AR_INTERFACE_ON" },
	{ ar_interface_off,	AR_INTERFACE_OFF,	sizeof (arc_t),
	    ARF_ONLY_CMD, OP_CONFIG, "AR_INTERFACE_OFF" },
	{ ar_set_ppa,		(uint32_t)IF_UNITSEL,	sizeof (int),
	    ARF_IOCTL_AWARE, OP_CONFIG, "IF_UNITSEL" },
	{ ar_nd_ioctl,		ND_GET,			1,
	    ARF_IOCTL_AWARE, OP_NP, "ND_GET" },
	{ ar_nd_ioctl,		ND_SET,			1,
	    ARF_IOCTL_AWARE, OP_CONFIG, "ND_SET" },
	{ ar_snmp_msg,		AR_SNMP_MSG,	sizeof (struct T_optmgmt_ack),
	    ARF_IOCTL_AWARE | ARF_ONLY_CMD, OP_NP, "AR_SNMP_MSG" },
	{ ar_slifname,		(uint32_t)SIOCSLIFNAME,	sizeof (struct lifreq),
	    ARF_IOCTL_AWARE, OP_CONFIG, "SIOCSLIFNAME" }
};

/*
 * ARP Cache Entry creation routine.
 * Cache entries are allocated within timer messages and inserted into
 * the global hash list based on protocol and protocol address.
 */
static int
ar_ce_create(arl_t *arl, uint_t proto, uchar_t *hw_addr, uint_t hw_addr_len,
    uchar_t *proto_addr, uint_t proto_addr_len, uchar_t *proto_mask,
    uchar_t *proto_extract_mask, uint_t hw_extract_start, uint_t flags)
{
	static ace_t	ace_null;
	ace_t	*ace;
	ace_t	**acep;
	uchar_t	*dst;
	mblk_t	*mp;

	if ((flags & ~ACE_EXTERNAL_FLAGS_MASK) || arl == NULL)
		return (EINVAL);
	if (flags & ACE_F_MYADDR)
		flags |= ACE_F_PUBLISH | ACE_F_AUTHORITY;

	if (!hw_addr && hw_addr_len == 0) {
		if (flags == ACE_F_PERMANENT) {	/* Not publish */
			/* 224.0.0.0 to zero length address */
			flags |= ACE_F_RESOLVED;
		} else {	/* local address and unresolved case */
			if ((hw_addr = arl->arl_hw_addr) != 0)
				hw_addr_len = arl->arl_hw_addr_length;
			if (flags & ACE_F_PUBLISH)
				flags |= ACE_F_RESOLVED;
		}
	} else {
		flags |= ACE_F_RESOLVED;
	}

	if (!proto_addr || proto_addr_len == 0 ||
	    (proto == IP_ARP_PROTO_TYPE && proto_addr_len != IP_ADDR_LEN))
		return (EINVAL);
	/* Handle hw_addr_len == 0 for DL_ENABMULTI_REQ etc. */
	if (hw_addr_len && !hw_addr)
		return (EINVAL);
	if (hw_addr_len < arl->arl_hw_addr_length && hw_addr_len != 0)
		return (EINVAL);
	if (!proto_extract_mask && (flags & ACE_F_MAPPING))
		return (EINVAL);

	/*
	 * If the underlying link doesn't have reliable up/down notification or
	 * if we're working with the IPv4 169.254.0.0/16 Link Local Address
	 * space, then don't use the fast timers.  Otherwise, use them.
	 */
	if (arl->arl_notifies &&
	    !(proto == IP_ARP_PROTO_TYPE && IS_IPV4_LL_SPACE(proto_addr))) {
		flags |= ACE_F_FAST;
	}

	/*
	 * Allocate the timer block to hold the ace.
	 * (ace + proto_addr + proto_addr_mask + proto_extract_mask + hw_addr)
	 */
	mp = mi_timer_alloc(sizeof (ace_t) + proto_addr_len + proto_addr_len +
	    proto_addr_len + hw_addr_len);
	if (!mp)
		return (ENOMEM);
	ace = (ace_t *)mp->b_rptr;
	*ace = ace_null;
	ace->ace_proto = proto;
	ace->ace_mp = mp;
	ace->ace_arl = arl;

	dst = (uchar_t *)&ace[1];

	ace->ace_proto_addr = dst;
	ace->ace_proto_addr_length = proto_addr_len;
	bcopy(proto_addr, dst, proto_addr_len);
	dst += proto_addr_len;
	/*
	 * The proto_mask allows us to add entries which will let us respond
	 * to requests for a group of addresses.  This makes it easy to provide
	 * proxy ARP service for machines that don't understand about the local
	 * subnet structure, if, for example, there are BSD4.2 systems lurking.
	 */
	ace->ace_proto_mask = dst;
	if (proto_mask != NULL) {
		bcopy(proto_mask, dst, proto_addr_len);
		dst += proto_addr_len;
	} else {
		while (proto_addr_len-- > 0)
			*dst++ = (uchar_t)~0;
	}

	if (proto_extract_mask != NULL) {
		ace->ace_proto_extract_mask = dst;
		bcopy(proto_extract_mask, dst, ace->ace_proto_addr_length);
		dst += ace->ace_proto_addr_length;
	} else {
		ace->ace_proto_extract_mask = NULL;
	}
	ace->ace_hw_extract_start = hw_extract_start;
	ace->ace_hw_addr_length = hw_addr_len;
	ace->ace_hw_addr = dst;
	if (hw_addr != NULL) {
		bcopy(hw_addr, dst, hw_addr_len);
		dst += hw_addr_len;
	}

	ace->ace_arl = arl;
	ace->ace_flags = flags;

	if (ar_mask_all_ones(ace->ace_proto_mask,
	    ace->ace_proto_addr_length)) {
		acep = ar_ce_hash(ace->ace_proto, ace->ace_proto_addr,
		    ace->ace_proto_addr_length);
	} else {
		acep = &ar_ce_mask_entries;
	}
	if ((ace->ace_next = *acep) != NULL)
		ace->ace_next->ace_ptpn = &ace->ace_next;
	*acep = ace;
	ace->ace_ptpn = acep;
	return (0);
}

/* Delete a cache entry. */
static void
ar_ce_delete(ace_t *ace)
{
	ace_t	**acep;

	/* Get out of the hash list. */
	acep = ace->ace_ptpn;
	if (ace->ace_next)
		ace->ace_next->ace_ptpn = acep;
	acep[0] = ace->ace_next;
	/* Mark it dying in case we have a timer about to fire. */
	ace->ace_flags |= ACE_F_DYING;
	/* Complete any outstanding queries immediately. */
	ar_query_reply(ace, ENXIO, NULL, (uint32_t)0);
	/* Free the timer, immediately, or when it fires. */
	mi_timer_free(ace->ace_mp);
}

/*
 * ar_ce_walk routine.	Delete the ace if it is associated with the arl
 * that is going away.
 */
static void
ar_ce_delete_per_arl(ace_t *ace, void *arl)
{
	if (ace->ace_arl == arl) {
		ace->ace_flags &= ~ACE_F_PERMANENT;
		ar_ce_delete(ace);
	}
}

/* Cache entry hash routine, based on protocol and protocol address. */
static ace_t **
ar_ce_hash(uint32_t proto, const uchar_t *proto_addr,
    uint32_t proto_addr_length)
{
	const uchar_t *up = proto_addr;
	unsigned int hval = proto;
	int	len = proto_addr_length;

	while (--len >= 0)
		hval ^= *up++;
	return (&ar_ce_hash_tbl[hval % A_CNT(ar_ce_hash_tbl)]);
}

/* Cache entry lookup.	Try to find an ace matching the parameters passed. */
ace_t *
ar_ce_lookup(arl_t *arl, uint32_t proto, uchar_t *proto_addr,
    uint32_t proto_addr_length)
{
	ace_t	*ace;

	ace = ar_ce_lookup_entry(arl, proto, proto_addr, proto_addr_length);
	if (!ace)
		ace = ar_ce_lookup_mapping(arl, proto, proto_addr,
		    proto_addr_length);
	return (ace);
}

/*
 * Cache entry lookup.	Try to find an ace matching the parameters passed.
 * Look only for exact entries (no mappings)
 */
static ace_t *
ar_ce_lookup_entry(arl_t *arl, uint32_t proto, uchar_t *proto_addr,
    uint32_t proto_addr_length)
{
	ace_t	*ace;

	if (!proto_addr)
		return (NULL);
	ace = *ar_ce_hash(proto, proto_addr, proto_addr_length);
	for (; ace; ace = ace->ace_next) {
		if (ace->ace_arl == arl &&
		    ace->ace_proto_addr_length == proto_addr_length &&
		    ace->ace_proto == proto) {
			int	i1 = proto_addr_length;
			uchar_t	*ace_addr = ace->ace_proto_addr;
			uchar_t	*mask = ace->ace_proto_mask;
			/*
			 * Note that the ace_proto_mask is applied to the
			 * proto_addr before comparing to the ace_addr.
			 */
			do {
				if (--i1 < 0)
					return (ace);
			} while ((proto_addr[i1] &  mask[i1]) == ace_addr[i1]);
		}
	}
	return (ace);
}

/*
 * Extract cache entry lookup parameters from an external command message, then
 * call the supplied match function.
 */
static ace_t *
ar_ce_lookup_from_area(mblk_t *mp, ace_t *matchfn())
{
	uchar_t	*proto_addr;
	area_t	*area = (area_t *)mp->b_rptr;

	proto_addr = mi_offset_paramc(mp, area->area_proto_addr_offset,
	    area->area_proto_addr_length);
	if (!proto_addr)
		return (NULL);
	return ((*matchfn)(ar_ll_lookup_from_mp(mp), area->area_proto,
	    proto_addr, area->area_proto_addr_length));
}

/*
 * Cache entry lookup.	Try to find an ace matching the parameters passed.
 * Look only for mappings.
 */
static ace_t *
ar_ce_lookup_mapping(arl_t *arl, uint32_t proto, uchar_t *proto_addr,
    uint32_t proto_addr_length)
{
	ace_t	*ace;

	if (!proto_addr)
		return (NULL);
	ace = ar_ce_mask_entries;
	for (; ace; ace = ace->ace_next) {
		if (ace->ace_arl == arl &&
		    ace->ace_proto_addr_length == proto_addr_length &&
		    ace->ace_proto == proto) {
			int	i1 = proto_addr_length;
			uchar_t	*ace_addr = ace->ace_proto_addr;
			uchar_t	*mask = ace->ace_proto_mask;
			/*
			 * Note that the ace_proto_mask is applied to the
			 * proto_addr before comparing to the ace_addr.
			 */
			do {
				if (--i1 < 0)
					return (ace);
			} while ((proto_addr[i1] &  mask[i1]) == ace_addr[i1]);
		}
	}
	return (ace);
}

/*
 * Look for a permanent entry for proto_addr across all interfaces.
 * This is used for sending ARP requests out. Requests may come from
 * IP on le0 with the source address of le1 and we need to send out
 * the request on le1 so that ARP does not think that somebody else
 * is using its PERMANENT address. If le0 and le1 are sitting on
 * the same wire, the same IP -> ethernet mapping might exist on
 * both the interfaces. But we should look for the permanent
 * mapping to avoid arp interpreting it as a duplicate.
 */
static ace_t *
ar_ce_lookup_permanent(uint32_t proto, uchar_t *proto_addr,
    uint32_t proto_addr_length)
{
	ace_t	*ace;

	ace = *ar_ce_hash(proto, proto_addr, proto_addr_length);
	for (; ace != NULL; ace = ace->ace_next) {
		if (!(ace->ace_flags & ACE_F_PERMANENT))
			continue;
		if (ace->ace_proto_addr_length == proto_addr_length &&
		    ace->ace_proto == proto) {
			int	i1 = proto_addr_length;
			uchar_t	*ace_addr = ace->ace_proto_addr;
			uchar_t	*mask = ace->ace_proto_mask;

			/*
			 * Note that the ace_proto_mask is applied to the
			 * proto_addr before comparing to the ace_addr.
			 */
			do {
				if (--i1 < 0)
					return (ace);
			} while ((proto_addr[i1] &  mask[i1]) == ace_addr[i1]);
		}
	}
	return (ace);
}

/*
 * ar_ce_resolve is called when a response comes in to an outstanding request.
 * Returns 'true' if the address has changed and we need to tell the client.
 * (We don't need to tell the client if there's still an outstanding query.)
 */
static boolean_t
ar_ce_resolve(ace_t *ace, const uchar_t *hw_addr, uint32_t hw_addr_length)
{
	boolean_t hwchanged;

	if (hw_addr_length == ace->ace_hw_addr_length) {
		ASSERT(ace->ace_hw_addr != NULL);
		hwchanged = bcmp(hw_addr, ace->ace_hw_addr,
		    hw_addr_length) != 0;
		if (hwchanged)
			bcopy(hw_addr, ace->ace_hw_addr, hw_addr_length);
		/*
		 * No need to bother with ar_query_reply if no queries are
		 * waiting.
		 */
		ace->ace_flags |= ACE_F_RESOLVED;
		if (ace->ace_query_mp != NULL)
			ar_query_reply(ace, 0, NULL, (uint32_t)0);
		else if (hwchanged)
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * There are 2 functions performed by this function.
 * 1. Resolution of unresolved entries and update of resolved entries.
 * 2. Detection of nodes with our own IP address (duplicates).
 *
 * This is complicated by ill groups.  We don't currently have knowledge of ill
 * groups, so we can't distinguish between a packet that comes in on one of the
 * arls that's part of the group versus one that's on an unrelated arl.  Thus,
 * we take a conservative approach.  If the arls match, then we update resolved
 * and unresolved entries alike.  If they don't match, then we update only
 * unresolved entries.
 *
 * For all entries, we first check to see if this is a duplicate (probable
 * loopback) message.  If so, then just ignore it.
 *
 * Next, check to see if the entry has completed DAD.  If not, then we've
 * failed, because someone is already using the address.  Notify IP of the DAD
 * failure and remove the broken ace.
 *
 * Next, we check if we're the authority for this address.  If so, then it's
 * time to defend it, because the other node is a duplicate.  Report it as a
 * 'bogon' and let IP decide how to defend.
 *
 * Finally, if it's unresolved or if the arls match, we just update the MAC
 * address.  This allows a published 'static' entry to be updated by an ARP
 * request from the node for which we're a proxy ARP server -- e.g., when a
 * mobile node returns home.  If the address has changed, then tell IP.
 *
 * Note that this logic does not update published ARP entries for mismatched
 * arls, as for example when we proxy arp across 2 subnets with differing
 * subnet masks.
 *
 * Return Values below
 */

#define	AR_NOTFOUND	1	/* No matching ace found in cache */
#define	AR_MERGED	2	/* Matching ace updated (RFC 826 Merge_flag) */
#define	AR_LOOPBACK	3	/* Our own arp packet was received */
#define	AR_BOGON	4	/* Another host has our IP addr. */
#define	AR_FAILED	5	/* Duplicate Address Detection has failed */
#define	AR_CHANGED	6	/* Address has changed; tell IP (and merged) */

static int
ar_ce_resolve_all(arl_t *arl, uint32_t proto, const uchar_t *src_haddr,
    uint32_t hlen, const uchar_t *src_paddr, uint32_t plen)
{
	ace_t *ace;
	ace_t *ace_next;
	int i1;
	const uchar_t *paddr;
	uchar_t *ace_addr;
	uchar_t *mask;
	int retv = AR_NOTFOUND;

	ace = *ar_ce_hash(proto, src_paddr, plen);
	for (; ace != NULL; ace = ace_next) {

		/* ar_ce_resolve may delete the ace; fetch next pointer now */
		ace_next = ace->ace_next;

		if (ace->ace_proto_addr_length != plen ||
		    ace->ace_proto != proto) {
			continue;
		}

		/*
		 * Note that the ace_proto_mask is applied to the proto_addr
		 * before comparing to the ace_addr.
		 */
		paddr = src_paddr;
		i1 = plen;
		ace_addr = ace->ace_proto_addr;
		mask = ace->ace_proto_mask;
		while (--i1 >= 0) {
			if ((*paddr++ & *mask++) != *ace_addr++)
				break;
		}
		if (i1 >= 0)
			continue;

		/*
		 * If both IP addr and hardware address match what we already
		 * have, then this is a broadcast packet emitted by one of our
		 * interfaces, reflected by the switch and received on another
		 * interface.  We return AR_LOOPBACK.
		 */
		if ((ace->ace_flags & ACE_F_MYADDR) &&
		    hlen == ace->ace_hw_addr_length &&
		    bcmp(ace->ace_hw_addr, src_haddr,
		    ace->ace_hw_addr_length) == 0) {
			return (AR_LOOPBACK);
		}

		/*
		 * If the entry is unverified, then we've just verified that
		 * someone else already owns this address, because this is a
		 * message with the same protocol address but different
		 * hardware address.
		 */
		if (ace->ace_flags & ACE_F_UNVERIFIED) {
			ar_ce_delete(ace);
			return (AR_FAILED);
		}

		/*
		 * If the IP address matches ours and we're authoritative for
		 * this entry, then some other node is using our IP addr, so
		 * return AR_BOGON.  Also reset the transmit count to zero so
		 * that, if we're currently in initial announcement mode, we
		 * switch back to the lazier defense mode.  Knowing that
		 * there's at least one duplicate out there, we ought not
		 * blindly announce.
		 */
		if (ace->ace_flags & ACE_F_AUTHORITY) {
			ace->ace_xmit_count = 0;
			return (AR_BOGON);
		}

		/*
		 * Limit updating across other ills to unresolved
		 * entries only.  We don't want to inadvertently update
		 * published entries.
		 */
		if (ace->ace_arl == arl || !ACE_RESOLVED(ace)) {
			if (ar_ce_resolve(ace, src_haddr, hlen))
				retv = AR_CHANGED;
			else if (retv == AR_NOTFOUND)
				retv = AR_MERGED;
		}
	}
	return (retv);
}

/* Pass arg1 to the pfi supplied, along with each ace in existence. */
static void
ar_ce_walk(void (*pfi)(ace_t *, void *), void *arg1)
{
	ace_t	*ace;
	ace_t	*ace1;
	ace_t	**acep;

	for (acep = ar_ce_hash_tbl; acep < A_END(ar_ce_hash_tbl); acep++) {
		/*
		 * We walk the hash chain in a way that allows the current
		 * ace to get blown off by the called routine.
		 */
		for (ace = *acep; ace; ace = ace1) {
			ace1 = ace->ace_next;
			(*pfi)(ace, arg1);
		}
	}
	for (ace = ar_ce_mask_entries; ace; ace = ace1) {
		ace1 = ace->ace_next;
		(*pfi)(ace, arg1);
	}
}

/* Free the ND tables if the last ar has gone away. */
static void
ar_cleanup(void)
{
	if (!ar_g_head)
		nd_free(&ar_g_nd);
}

/*
 * Send a copy of interesting packets to the corresponding IP instance.
 * The corresponding IP instance is the ARP-IP-DEV instance for this
 * DEV (i.e. ARL).
 */
static void
ar_client_notify(const arl_t *arl, mblk_t *mp, int code)
{
	ar_t	*ar = ((ar_t *)arl->arl_rq->q_ptr)->ar_arl_ip_assoc;
	arcn_t	*arcn;
	mblk_t	*mp1;
	int	arl_namelen = strlen(arl->arl_name) + 1;

	/* Looks like the association disappeared */
	if (ar == NULL) {
		freemsg(mp);
		return;
	}

	/* ar is the corresponding ARP-IP instance for this ARL */
	ASSERT(ar->ar_arl == NULL && ar->ar_wq->q_next != NULL);

	mp1 = allocb(sizeof (arcn_t) + arl_namelen, BPRI_MED);
	if (mp1 == NULL) {
		freemsg(mp);
		return;
	}
	DB_TYPE(mp1) = M_CTL;
	mp1->b_cont = mp;
	arcn = (arcn_t *)mp1->b_rptr;
	mp1->b_wptr = (uchar_t *)&arcn[1] + arl_namelen;
	arcn->arcn_cmd = AR_CLIENT_NOTIFY;
	arcn->arcn_name_offset = sizeof (arcn_t);
	arcn->arcn_name_length = arl_namelen;
	arcn->arcn_code = code;
	bcopy(arl->arl_name, &arcn[1], arl_namelen);

	putnext(ar->ar_wq, mp1);
}

/*
 * Send a delete-notify message down to IP.  We've determined that IP doesn't
 * have a cache entry for the IP address itself, but it may have other cache
 * entries with the same hardware address, and we don't want to see those grow
 * stale.  (The alternative is sending down updates for every ARP message we
 * get that doesn't match an existing ace.  That's much more expensive than an
 * occasional delete and reload.)
 */
static void
ar_delete_notify(const ace_t *ace)
{
	const arl_t *arl = ace->ace_arl;
	mblk_t	*mp;
	size_t	len;
	arh_t	*arh;

	len = sizeof (*arh) + 2 * ace->ace_proto_addr_length;
	mp = allocb(len, BPRI_MED);
	if (mp == NULL)
		return;
	arh = (arh_t *)mp->b_rptr;
	mp->b_wptr = (uchar_t *)arh + len;
	U16_TO_BE16(arl->arl_arp_hw_type, arh->arh_hardware);
	U16_TO_BE16(ace->ace_proto, arh->arh_proto);
	arh->arh_hlen = 0;
	arh->arh_plen = ace->ace_proto_addr_length;
	U16_TO_BE16(ARP_RESPONSE, arh->arh_operation);
	bcopy(ace->ace_proto_addr, arh + 1, ace->ace_proto_addr_length);
	bcopy(ace->ace_proto_addr, (uchar_t *)(arh + 1) +
	    ace->ace_proto_addr_length, ace->ace_proto_addr_length);
	ar_client_notify(arl, mp, AR_CN_ANNOUNCE);
}

/* ARP module close routine. */
static int
ar_close(queue_t *q)
{
	ar_t	*ar = (ar_t *)q->q_ptr;
	char	name[LIFNAMSIZ];
	arl_t	*arl;
	arl_t	**arlp;
	cred_t	*cr;
	arc_t	*arc;
	mblk_t	*mp1;
	int	index;

	TRACE_1(TR_FAC_ARP, TR_ARP_CLOSE,
	    "arp_close: q %p", q);

	arl = ar->ar_arl;
	if (arl == NULL) {
		index = 0;
		/*
		 * If this is the <ARP-IP-Driver> stream send down
		 * a closing message to IP and wait for IP to send
		 * an ack. This helps to make sure that messages
		 * that are currently being sent up by IP are not lost.
		 */
		if (ar->ar_on_ill_stream) {
			mp1 = allocb(sizeof (arc_t), BPRI_MED);
			if (mp1 != NULL) {
				DB_TYPE(mp1) = M_CTL;
				arc = (arc_t *)mp1->b_rptr;
				mp1->b_wptr = mp1->b_rptr + sizeof (arc_t);
				arc->arc_cmd = AR_ARP_CLOSING;
				putnext(WR(q), mp1);
				while (!ar->ar_ip_acked_close)
					/* If we are interrupted break out */
					if (qwait_sig(q) == 0)
						break;
			}
		}
		/* Delete all our pending queries, 'arl' is not dereferenced */
		ar_ce_walk(ar_query_delete, ar);
		/*
		 * The request could be pending on some arl_queue also. This
		 * happens if the arl is not yet bound, and bind is pending.
		 */
		ar_ll_cleanup_arl_queue(q);
	} else {
		index = arl->arl_index;
		(void) strcpy(name, arl->arl_name);
		arl->arl_closing = 1;
		while (arl->arl_queue != NULL)
			qwait(arl->arl_rq);

		if (arl->arl_state == ARL_S_UP)
			ar_ll_down(arl);

		while (arl->arl_state != ARL_S_DOWN)
			qwait(arl->arl_rq);

		ar_ll_clear_defaults(arl);
		/*
		 * If this is the control stream for an arl, delete anything
		 * hanging off our arl.
		 */
		ar_ce_walk(ar_ce_delete_per_arl, arl);
		/* Free any messages waiting for a bind_ack */
		/* Get the arl out of the chain. */
		rw_enter(&arl_g_lock, RW_WRITER);
		for (arlp = &arl_g_head; *arlp; arlp = &(*arlp)->arl_next) {
			if (*arlp == arl) {
				*arlp = arl->arl_next;
				break;
			}
		}

		ASSERT(arl->arl_dlpi_deferred == NULL);
		ar->ar_arl = NULL;
		rw_exit(&arl_g_lock);

		mi_free((char *)arl);
	}
	/* Let's break the association between an ARL and IP instance */
	if (ar->ar_arl_ip_assoc != NULL) {
		ASSERT(ar->ar_arl_ip_assoc->ar_arl_ip_assoc != NULL &&
		    ar->ar_arl_ip_assoc->ar_arl_ip_assoc == ar);
		ar->ar_arl_ip_assoc->ar_arl_ip_assoc = NULL;
		ar->ar_arl_ip_assoc = NULL;
	}
	cr = ar->ar_credp;
	/* mi_close_comm frees the instance data. */
	(void) mi_close_comm(&ar_g_head, q);
	ar_cleanup();
	qprocsoff(q);
	crfree(cr);

	if (index != 0) {
		hook_nic_event_t info;

		info.hne_nic = index;
		info.hne_lif = 0;
		info.hne_event = NE_UNPLUMB;
		info.hne_data = name;
		info.hne_datalen = strlen(name);
		(void) hook_run(arpnicevents, (hook_data_t)&info);
	}
	return (0);
}

/*
 * Dispatch routine for ARP commands.  This routine can be called out of
 * either ar_wput or ar_rput, in response to IOCTLs or M_PROTO messages.
 */
/* TODO: error reporting for M_PROTO case */
static int
ar_cmd_dispatch(queue_t *q, mblk_t *mp_orig)
{
	arct_t	*arct;
	uint32_t	cmd;
	ssize_t	len;
	mblk_t	*mp = mp_orig;
	cred_t *cr = NULL;

	if (!mp)
		return (ENOENT);

	/* We get both M_PROTO and M_IOCTL messages, so watch out! */
	if (DB_TYPE(mp) == M_IOCTL) {
		struct iocblk *ioc;
		ioc = (struct iocblk *)mp->b_rptr;
		cmd = ioc->ioc_cmd;
		cr = ioc->ioc_cr;
		mp = mp->b_cont;
		if (!mp)
			return (ENOENT);
	}
	len = mp->b_wptr - mp->b_rptr;
	if (len < sizeof (uint32_t) || !OK_32PTR(mp->b_rptr))
		return (ENOENT);
	if (mp_orig == mp)
		cmd = *(uint32_t *)mp->b_rptr;
	for (arct = ar_cmd_tbl; ; arct++) {
		if (arct >= A_END(ar_cmd_tbl))
			return (ENOENT);
		if (arct->arct_cmd == cmd)
			break;
	}
	if (len < arct->arct_min_len) {
		/*
		 * If the command is exclusive to ARP, we return EINVAL,
		 * else we need to pass the command downstream, so return
		 * ENOENT
		 */
		return ((arct->arct_flags & ARF_ONLY_CMD) ? EINVAL : ENOENT);
	}
	if (arct->arct_priv_req != OP_NP) {
		int error;

		if (cr == NULL)
			cr = DB_CREDDEF(mp_orig, ((ar_t *)q->q_ptr)->ar_credp);

		if ((error = secpolicy_net(cr, arct->arct_priv_req,
		    B_FALSE)) != 0)
			return (error);
	}
	if (arct->arct_flags & ARF_IOCTL_AWARE)
		mp = mp_orig;

	DTRACE_PROBE3(cmd_dispatch, queue_t *, q, mblk_t *, mp,
	    arct_t *, arct);
	return (*arct->arct_pfi)(q, mp);
}

/* Allocate and do common initializations for DLPI messages. */
static mblk_t *
ar_dlpi_comm(t_uscalar_t prim, size_t size)
{
	mblk_t	*mp;

	if ((mp = allocb(size, BPRI_HI)) == NULL)
		return (NULL);

	/*
	 * DLPIv2 says that DL_INFO_REQ and DL_TOKEN_REQ (the latter
	 * of which we don't seem to use) are sent with M_PCPROTO, and
	 * that other DLPI are M_PROTO.
	 */
	DB_TYPE(mp) = (prim == DL_INFO_REQ) ? M_PCPROTO : M_PROTO;

	mp->b_wptr = mp->b_rptr + size;
	bzero(mp->b_rptr, size);
	((union DL_primitives *)mp->b_rptr)->dl_primitive = prim;

	return (mp);
}

/*
 * The following two functions serialize DLPI messages to the driver, much
 * along the lines of ill_dlpi_send and ill_dlpi_done in IP. Basically,
 * we wait for a DLPI message, sent downstream, to be acked before sending
 * the next. If there are DLPI messages that have not yet been sent, queue
 * this message (mp), else send it downstream.
 */
static void
ar_dlpi_send(arl_t *arl, mblk_t *mp)
{
	ASSERT(arl != NULL);
	ASSERT(DB_TYPE(mp) == M_PROTO || DB_TYPE(mp) == M_PCPROTO);

	if (arl->arl_dlpi_pending != DL_PRIM_INVAL) {
		mblk_t **mpp;

		/* Must queue message. Tail insertion */
		mpp = &arl->arl_dlpi_deferred;
		while (*mpp != NULL)
			mpp = &((*mpp)->b_next);
		*mpp = mp;

		DTRACE_PROBE2(dlpi_defer, arl_t *, arl, mblk_t *, mp);
		return;
	}

	arl->arl_dlpi_pending =
	    ((union DL_primitives *)mp->b_rptr)->dl_primitive;
	DTRACE_PROBE2(dlpi_send, arl_t *, arl, mblk_t *, mp);
	putnext(arl->arl_wq, mp);
}

/*
 * Called when an DLPI control message has been acked; send down the next
 * queued message (if any).
 * The DLPI messages of interest being bind, attach, unbind and detach since
 * these are the only ones sent by ARP via ar_dlpi_send.
 */
static void
ar_dlpi_done(arl_t *arl, t_uscalar_t prim)
{
	mblk_t *mp;

	if (arl->arl_dlpi_pending != prim) {
		DTRACE_PROBE2(dlpi_done_unexpected, arl_t *, arl,
		    t_uscalar_t, prim);
		return;
	}

	if ((mp = arl->arl_dlpi_deferred) == NULL) {
		DTRACE_PROBE2(dlpi_done_idle, arl_t *, arl, t_uscalar_t, prim);
		arl->arl_dlpi_pending = DL_PRIM_INVAL;
		ar_cmd_done(arl);
		return;
	}

	arl->arl_dlpi_deferred = mp->b_next;
	mp->b_next = NULL;

	ASSERT(DB_TYPE(mp) == M_PROTO || DB_TYPE(mp) == M_PCPROTO);

	arl->arl_dlpi_pending =
	    ((union DL_primitives *)mp->b_rptr)->dl_primitive;
	DTRACE_PROBE2(dlpi_done_next, arl_t *, arl, mblk_t *, mp);
	putnext(arl->arl_wq, mp);
}

static void
ar_cmd_done(arl_t *arl)
{
	mblk_t			*mp;
	int			cmd;
	int			err;
	mblk_t			*mp1;
	mblk_t			*dlpi_op_done_mp = NULL;
	queue_t			*dlpi_op_done_q;
	ar_t			*ar_arl;
	ar_t			*ar_ip;
	queue_t			*q;

	ASSERT(arl->arl_state == ARL_S_UP || arl->arl_state == ARL_S_DOWN);

	/*
	 * If the current operation was initiated by IP there must be
	 * an op enqueued in arl_queue. But if ar_close has sent down
	 * a detach/unbind, there is no command enqueued. Also if the IP-ARP
	 * stream has closed the cleanup would be done and there won't be any mp
	 */
	if ((mp = arl->arl_queue) == NULL)
		return;

	if ((cmd = (uintptr_t)mp->b_prev) & CMD_IN_PROGRESS) {
		mp1 = ar_cmd_dequeue(arl);
		ASSERT(mp == mp1);

		cmd &= ~CMD_IN_PROGRESS;
		if (cmd == AR_INTERFACE_UP) {
			/*
			 * There is an ioctl waiting for us...
			 */
			if (arl->arl_state == ARL_S_UP)
				err = 0;
			else
				err = EINVAL;

			dlpi_op_done_mp = ar_alloc(AR_DLPIOP_DONE, err);
			if (dlpi_op_done_mp != NULL) {
				/*
				 * Better performance if we send the response
				 * after the potential MAPPING_ADDs command
				 * that are likely to follow. (Do it below the
				 * while loop, instead of putnext right now)
				 */
				dlpi_op_done_q = WR(mp->b_queue);
			}

			if (err == 0) {
				/*
				 * Now that we have the ARL instance
				 * corresponding to the IP instance let's make
				 * the association here.
				 */
				ar_ip = (ar_t *)mp->b_queue->q_ptr;
				ar_arl = (ar_t *)arl->arl_rq->q_ptr;
				ar_arl->ar_arl_ip_assoc = ar_ip;
				ar_ip->ar_arl_ip_assoc = ar_arl;
			}
		}
		inet_freemsg(mp);
	}

	/*
	 * Run the commands that have been enqueued while we were waiting
	 * for the last command (AR_INTERFACE_UP or AR_INTERFACE_DOWN)
	 * to complete.
	 */
	while ((mp = ar_cmd_dequeue(arl)) != NULL) {
		mp->b_prev = AR_DRAINING;
		q = mp->b_queue;
		mp->b_queue = NULL;

		/*
		 * Don't call put(q, mp) since it can lead to reorder of
		 * messages by sending the current messages to the end of
		 * arp's syncq
		 */
		if (q->q_flag & QREADR)
			ar_rput(q, mp);
		else
			ar_wput(q, mp);

		if ((mp = arl->arl_queue) == NULL)
			goto done;	/* no work to do */

		if ((cmd = (uintptr_t)mp->b_prev) & CMD_IN_PROGRESS) {
			/*
			 * The current command is an AR_INTERFACE_UP or
			 * AR_INTERFACE_DOWN and is waiting for a DLPI ack
			 * from the driver. Return. We can't make progress now.
			 */
			goto done;
		}
	}

done:
	if (dlpi_op_done_mp != NULL) {
		DTRACE_PROBE3(cmd_done_next, arl_t *, arl,
		    queue_t *, dlpi_op_done_q, mblk_t *, dlpi_op_done_mp);
		putnext(dlpi_op_done_q, dlpi_op_done_mp);
	}
}

/*
 * Queue all arp commands coming from clients. Typically these commands
 * come from IP, but could also come from other clients. The commands
 * are serviced in FIFO order. Some commands need to wait and restart
 * after the DLPI response from the driver is received. Typically
 * AR_INTERFACE_UP and AR_INTERFACE_DOWN. ar_dlpi_done restarts
 * the command and then dequeues the queue at arl_queue and calls ar_rput
 * or ar_wput for each enqueued command. AR_DRAINING is used to signify
 * that the command is being executed thru a drain from ar_dlpi_done.
 * Functions handling the individual commands such as ar_entry_add
 * check for this flag in b_prev to determine whether the command has
 * to be enqueued for later processing or must be processed now.
 *
 * b_next used to thread the enqueued command mblks
 * b_queue used to identify the queue of the originating request(client)
 * b_prev used to store the command itself for easy parsing.
 */
static void
ar_cmd_enqueue(arl_t *arl, mblk_t *mp, queue_t *q, ushort_t cmd,
    boolean_t tail_insert)
{
	mp->b_queue = q;
	if (arl->arl_queue == NULL) {
		ASSERT(arl->arl_queue_tail == NULL);
		mp->b_prev = (void *)((uintptr_t)(cmd | CMD_IN_PROGRESS));
		mp->b_next = NULL;
		arl->arl_queue = mp;
		arl->arl_queue_tail = mp;
	} else if (tail_insert) {
		mp->b_prev = (void *)((uintptr_t)cmd);
		mp->b_next = NULL;
		arl->arl_queue_tail->b_next = mp;
		arl->arl_queue_tail = mp;
	} else {
		/* head insert */
		mp->b_prev = (void *)((uintptr_t)cmd | CMD_IN_PROGRESS);
		mp->b_next = arl->arl_queue;
		arl->arl_queue = mp;
	}
}

static mblk_t *
ar_cmd_dequeue(arl_t *arl)
{
	mblk_t	*mp;

	if (arl->arl_queue == NULL) {
		ASSERT(arl->arl_queue_tail == NULL);
		return (NULL);
	}
	mp = arl->arl_queue;
	arl->arl_queue = mp->b_next;
	if (arl->arl_queue == NULL)
		arl->arl_queue_tail = NULL;
	mp->b_next = NULL;
	return (mp);
}

/*
 * Standard ACE timer handling: compute 'fuzz' around a central value or from 0
 * up to a value, and then set the timer.  The randomization is necessary to
 * prevent groups of systems from falling into synchronization on the network
 * and producing ARP packet storms.
 */
static void
ace_set_timer(ace_t *ace, boolean_t initial_time)
{
	clock_t intv, rnd, frac;

	(void) random_get_pseudo_bytes((uint8_t *)&rnd, sizeof (rnd));
	/* Note that clock_t is signed; must chop off bits */
	rnd &= (1ul << (NBBY * sizeof (rnd) - 1)) - 1;
	intv = ace->ace_xmit_interval;
	if (initial_time) {
		/* Set intv to be anywhere in the [1 .. intv] range */
		if (intv <= 0)
			intv = 1;
		else
			intv = (rnd % intv) + 1;
	} else {
		/* Compute 'frac' as 20% of the configured interval */
		if ((frac = intv / 5) <= 1)
			frac = 2;
		/* Set intv randomly in the range [intv-frac .. intv+frac] */
		if ((intv = intv - frac + rnd % (2 * frac + 1)) <= 0)
			intv = 1;
	}
	mi_timer(ace->ace_arl->arl_wq, ace->ace_mp, intv);
}

/*
 * Process entry add requests from external messages.
 * It is also called by ip_rput_dlpi_writer() through
 * ipif_resolver_up() to change hardware address when
 * an asynchronous hardware address change notification
 * arrives from the driver.
 */
static int
ar_entry_add(queue_t *q, mblk_t *mp_orig)
{
	area_t	*area;
	ace_t	*ace;
	uchar_t	*hw_addr;
	uint32_t	hw_addr_len;
	uchar_t	*proto_addr;
	uint32_t	proto_addr_len;
	uchar_t	*proto_mask;
	arl_t	*arl;
	mblk_t	*mp = mp_orig;
	int	err;
	uint_t	aflags;
	boolean_t unverified;

	/* We handle both M_IOCTL and M_PROTO messages. */
	if (DB_TYPE(mp) == M_IOCTL)
		mp = mp->b_cont;
	arl = ar_ll_lookup_from_mp(mp);
	if (arl == NULL)
		return (EINVAL);
	/*
	 * Newly received commands from clients go to the tail of the queue.
	 */
	if (CMD_NEEDS_QUEUEING(mp_orig, arl)) {
		DTRACE_PROBE3(eadd_enqueued, queue_t *, q, mblk_t *, mp_orig,
		    arl_t *, arl);
		ar_cmd_enqueue(arl, mp_orig, q, AR_ENTRY_ADD, B_TRUE);
		return (EINPROGRESS);
	}
	mp_orig->b_prev = NULL;

	area = (area_t *)mp->b_rptr;
	aflags = area->area_flags;

	/*
	 * If this is a replacement, ditch the original, but remember the
	 * duplicate address detection state.  If it's a new entry, then we're
	 * obligated to do duplicate address detection now.
	 */
	if ((ace = ar_ce_lookup_from_area(mp, ar_ce_lookup_entry)) != NULL) {
		unverified = (ace->ace_flags & ACE_F_UNVERIFIED) != 0;
		ar_ce_delete(ace);
	} else {
		unverified = (aflags & ACE_F_PUBLISH) != 0;
	}

	/* Allow client to request DAD restart */
	if (aflags & ACE_F_UNVERIFIED)
		unverified = B_TRUE;

	/* Extract parameters from the message. */
	hw_addr_len = area->area_hw_addr_length;
	hw_addr = mi_offset_paramc(mp, area->area_hw_addr_offset, hw_addr_len);
	proto_addr_len = area->area_proto_addr_length;
	proto_addr = mi_offset_paramc(mp, area->area_proto_addr_offset,
	    proto_addr_len);
	proto_mask = mi_offset_paramc(mp, area->area_proto_mask_offset,
	    proto_addr_len);
	if (proto_mask == NULL) {
		DTRACE_PROBE2(eadd_bad_mask, arl_t *, arl, area_t *, area);
		return (EINVAL);
	}
	err = ar_ce_create(
	    arl,
	    area->area_proto,
	    hw_addr,
	    hw_addr_len,
	    proto_addr,
	    proto_addr_len,
	    proto_mask,
	    NULL,
	    (uint32_t)0,
	    aflags & ~ACE_F_MAPPING & ~ACE_F_UNVERIFIED & ~ACE_F_DEFEND);
	if (err != 0) {
		DTRACE_PROBE3(eadd_create_failed, arl_t *, arl, area_t *, area,
		    int, err);
		return (err);
	}

	if (aflags & ACE_F_PUBLISH) {
		if (hw_addr == NULL || hw_addr_len == 0) {
			hw_addr = arl->arl_hw_addr;
		} else if (aflags & ACE_F_MYADDR) {
			/*
			 * If hardware address changes, then make sure
			 * that the hardware address and hardware
			 * address length fields in arl_t get updated
			 * too. Otherwise, they will continue carrying
			 * the old hardware address information.
			 */
			ASSERT((hw_addr != NULL) && (hw_addr_len != 0));
			bcopy(hw_addr, arl->arl_hw_addr, hw_addr_len);
			arl->arl_hw_addr_length = hw_addr_len;
		}

		ace = ar_ce_lookup(arl, area->area_proto, proto_addr,
		    proto_addr_len);
		ASSERT(ace != NULL);

		if (ace->ace_flags & ACE_F_FAST) {
			ace->ace_xmit_count = arp_fastprobe_count;
			ace->ace_xmit_interval = arp_fastprobe_delay;
		} else {
			ace->ace_xmit_count = arp_probe_count;
			ace->ace_xmit_interval = arp_probe_delay;
		}

		/*
		 * If the user has disabled duplicate address detection for
		 * this kind of interface (fast or slow) by setting the probe
		 * count to zero, then pretend as if we've verified the
		 * address, and go right to address defense mode.
		 */
		if (ace->ace_xmit_count == 0)
			unverified = B_FALSE;

		/*
		 * If we need to do duplicate address detection, then kick that
		 * off.  Otherwise, send out a gratuitous ARP message in order
		 * to update everyone's caches with the new hardware address.
		 */
		if (unverified) {
			ace->ace_flags |= ACE_F_UNVERIFIED;
			if (ace->ace_xmit_interval == 0) {
				/*
				 * User has configured us to send the first
				 * probe right away.  Do so, and set up for
				 * the subsequent probes.
				 */
				DTRACE_PROBE2(eadd_probe, ace_t *, ace,
				    area_t *, area);
				ar_xmit(arl, ARP_REQUEST, area->area_proto,
				    proto_addr_len, hw_addr, NULL, NULL,
				    proto_addr, NULL);
				ace->ace_xmit_count--;
				ace->ace_xmit_interval =
				    (ace->ace_flags & ACE_F_FAST) ?
				    arp_fastprobe_interval :
				    arp_probe_interval;
				ace_set_timer(ace, B_FALSE);
			} else {
				DTRACE_PROBE2(eadd_delay, ace_t *, ace,
				    area_t *, area);
				/* Regular delay before initial probe */
				ace_set_timer(ace, B_TRUE);
			}
		} else {
			DTRACE_PROBE2(eadd_announce, ace_t *, ace,
			    area_t *, area);
			ar_xmit(arl, ARP_REQUEST, area->area_proto,
			    proto_addr_len, hw_addr, proto_addr,
			    arl->arl_arp_addr, proto_addr, NULL);
			ace->ace_last_bcast = ddi_get_lbolt();

			/*
			 * If AUTHORITY is set, it is not just a proxy arp
			 * entry; we believe we're the authority for this
			 * entry.  In that case, and if we're not just doing
			 * one-off defense of the address, we send more than
			 * one copy, so that if this is an IPMP failover, we'll
			 * still have a good chance of updating everyone even
			 * when there's a packet loss or two.
			 */
			if ((aflags & ACE_F_AUTHORITY) &&
			    !(aflags & ACE_F_DEFEND) &&
			    arp_publish_count > 0) {
				/* Account for the xmit we just did */
				ace->ace_xmit_count = arp_publish_count - 1;
				ace->ace_xmit_interval = arp_publish_interval;
				if (ace->ace_xmit_count > 0)
					ace_set_timer(ace, B_FALSE);
			}
		}
	}
	return (0);
}

/* Process entry delete requests from external messages. */
static int
ar_entry_delete(queue_t *q, mblk_t *mp_orig)
{
	ace_t	*ace;
	arl_t	*arl;
	mblk_t	*mp = mp_orig;

	/* We handle both M_IOCTL and M_PROTO messages. */
	if (DB_TYPE(mp) == M_IOCTL)
		mp = mp->b_cont;
	arl = ar_ll_lookup_from_mp(mp);
	if (arl == NULL)
		return (EINVAL);
	/*
	 * Newly received commands from clients go to the tail of the queue.
	 */
	if (CMD_NEEDS_QUEUEING(mp_orig, arl)) {
		DTRACE_PROBE3(edel_enqueued, queue_t *, q, mblk_t *, mp_orig,
		    arl_t *, arl);
		ar_cmd_enqueue(arl, mp_orig, q, AR_ENTRY_DELETE, B_TRUE);
		return (EINPROGRESS);
	}
	mp_orig->b_prev = NULL;

	/*
	 * Need to know if it is a mapping or an exact match.  Check exact
	 * match first.
	 */
	ace = ar_ce_lookup_from_area(mp, ar_ce_lookup);
	if (ace != NULL) {
		/*
		 * If it's a permanent entry, then the client is the one who
		 * told us to delete it, so there's no reason to notify.
		 */
		if (ACE_NONPERM(ace))
			ar_delete_notify(ace);
		ar_ce_delete(ace);
		return (0);
	}
	return (ENXIO);
}

/*
 * Process entry query requests from external messages.
 * Bump up the ire_stats_freed for all errors except
 * EINPROGRESS - which means the packet has been queued.
 * For all other errors the packet is going to be freed
 * and hence we account for ire being freed if it
 * is a M_PROTO message.
 */
static int
ar_entry_query(queue_t *q, mblk_t *mp_orig)
{
	ace_t	*ace;
	ace_t	*src_ace = NULL;
	areq_t	*areq;
	arl_t	*arl;
	int	err;
	mblk_t	*mp = mp_orig;
	uchar_t	*proto_addr;
	uchar_t	*sender_addr;
	uint32_t proto_addr_len;
	clock_t	ms;
	boolean_t is_mproto = B_TRUE;

	/* We handle both M_IOCTL and M_PROTO messages. */
	if (DB_TYPE(mp) == M_IOCTL) {
		is_mproto = B_FALSE;
		mp = mp->b_cont;
	}
	arl = ar_ll_lookup_from_mp(mp);
	if (arl == NULL) {
		DTRACE_PROBE2(query_no_arl, queue_t *, q, mblk_t *, mp);
		err = EINVAL;
		goto err_ret;
	}
	/*
	 * Newly received commands from clients go to the tail of the queue.
	 */
	if (CMD_NEEDS_QUEUEING(mp_orig, arl)) {
		DTRACE_PROBE3(query_enqueued, queue_t *, q, mblk_t *, mp_orig,
		    arl_t *, arl);
		ar_cmd_enqueue(arl, mp_orig, q, AR_ENTRY_QUERY, B_TRUE);
		return (EINPROGRESS);
	}
	mp_orig->b_prev = NULL;

	areq = (areq_t *)mp->b_rptr;
	proto_addr_len = areq->areq_target_addr_length;
	proto_addr = mi_offset_paramc(mp, areq->areq_target_addr_offset,
	    proto_addr_len);
	if (proto_addr == NULL) {
		DTRACE_PROBE1(query_illegal_address, areq_t *, areq);
		err = EINVAL;
		goto err_ret;
	}
	/* Stash the reply queue pointer for later use. */
	mp->b_prev = (mblk_t *)OTHERQ(q);
	mp->b_next = NULL;
	if (areq->areq_xmit_interval == 0)
		areq->areq_xmit_interval = AR_DEF_XMIT_INTERVAL;
	ace = ar_ce_lookup(arl, areq->areq_proto, proto_addr, proto_addr_len);
	if (ace != NULL && (ace->ace_flags & ACE_F_OLD)) {
		/*
		 * This is a potentially stale entry that IP's asking about.
		 * Since IP is asking, it must not have an answer anymore,
		 * either due to periodic ARP flush or due to SO_DONTROUTE.
		 * Rather than go forward with what we've got, restart
		 * resolution.
		 */
		DTRACE_PROBE2(query_stale_ace, ace_t *, ace, areq_t *, areq);
		ar_ce_delete(ace);
		ace = NULL;
	}
	if (ace != NULL) {
		mblk_t	**mpp;
		uint32_t	count = 0;

		/*
		 * There is already a cache entry.  This means there is either
		 * a permanent entry, or address resolution is in progress.
		 * If the latter, there should be one or more queries queued
		 * up.	We link the current one in at the end, if there aren't
		 * too many outstanding.
		 */
		for (mpp = &ace->ace_query_mp; mpp[0]; mpp = &mpp[0]->b_next) {
			if (++count > areq->areq_max_buffered) {
				DTRACE_PROBE2(query_overflow, ace_t *, ace,
				    areq_t *, areq);
				mp->b_prev = NULL;
				err = EALREADY;
				goto err_ret;
			}
		}
		/* Put us on the list. */
		mpp[0] = mp;
		if (count != 0) {
			/*
			 * If a query was already queued up, then we must not
			 * have an answer yet.
			 */
			DTRACE_PROBE2(query_in_progress, ace_t *, ace,
			    areq_t *, areq);
			return (EINPROGRESS);
		}
		if (ACE_RESOLVED(ace)) {
			/*
			 * We have an answer already.
			 * Keep a dup of mp since proto_addr points to it
			 * and mp has been placed on the ace_query_mp list.
			 */
			mblk_t *mp1;

			DTRACE_PROBE2(query_resolved, ace_t *, ace,
			    areq_t *, areq);
			mp1 = dupmsg(mp);
			ar_query_reply(ace, 0, proto_addr, proto_addr_len);
			freemsg(mp1);
			return (EINPROGRESS);
		}
		if (ace->ace_flags & ACE_F_MAPPING) {
			/* Should never happen */
			DTRACE_PROBE2(query_unresolved_mapping, ace_t *, ace,
			    areq_t *, areq);
			mpp[0] = mp->b_next;
			err = ENXIO;
			goto err_ret;
		}
		if (arl->arl_xmit_template == NULL) {
			/* Can't get help if we don't know how. */
			DTRACE_PROBE2(query_no_template, ace_t *, ace,
			    areq_t *, areq);
			mpp[0] = NULL;
			mp->b_prev = NULL;
			err = ENXIO;
			goto err_ret;
		}
		DTRACE_PROBE2(query_unresolved, ace_t, ace, areq_t *, areq);
	} else {
		/* No ace yet.	Make one now.  (This is the common case.) */
		if (areq->areq_xmit_count == 0 ||
		    arl->arl_xmit_template == NULL) {
			DTRACE_PROBE2(query_template, arl_t *, arl,
			    areq_t *, areq);
			mp->b_prev = NULL;
			err = ENXIO;
			goto err_ret;
		}
		/*
		 * Check for sender addr being NULL or not before
		 * we create the ace. It is easy to cleanup later.
		 */
		sender_addr = mi_offset_paramc(mp,
		    areq->areq_sender_addr_offset,
		    areq->areq_sender_addr_length);
		if (sender_addr == NULL) {
			DTRACE_PROBE2(query_no_sender, arl_t *, arl,
			    areq_t *, areq);
			mp->b_prev = NULL;
			err = EINVAL;
			goto err_ret;
		}
		err = ar_ce_create(arl, areq->areq_proto, NULL, 0,
		    proto_addr, proto_addr_len, NULL,
		    NULL, (uint32_t)0,
		    areq->areq_flags);
		if (err != 0) {
			DTRACE_PROBE3(query_create_failed, arl_t *, arl,
			    areq_t *, areq, int, err);
			mp->b_prev = NULL;
			goto err_ret;
		}
		ace = ar_ce_lookup(arl, areq->areq_proto, proto_addr,
		    proto_addr_len);
		if (ace == NULL || ace->ace_query_mp != NULL) {
			/* Shouldn't happen! */
			DTRACE_PROBE3(query_lookup_failed, arl_t *, arl,
			    areq_t *, areq, ace_t *, ace);
			mp->b_prev = NULL;
			err = ENXIO;
			goto err_ret;
		}
		ace->ace_query_mp = mp;
		/*
		 * We don't have group information here. But if the sender
		 * address belongs to a different arl, we might as well
		 * search the other arl for a resolved ACE. If we find one,
		 * we resolve it rather than sending out a ARP request.
		 */
		src_ace = ar_ce_lookup_permanent(areq->areq_proto, sender_addr,
		    areq->areq_sender_addr_length);
		if (src_ace == NULL) {
			DTRACE_PROBE3(query_source_missing, arl_t *, arl,
			    areq_t *, areq, ace_t *, ace);
			ar_query_reply(ace, ENXIO, NULL, (uint32_t)0);
			/*
			 * ar_query_reply has already freed the mp.
			 * Return EINPROGRESS, so that caller won't attempt
			 * to free the 'mp' again.
			 */
			return (EINPROGRESS);
		}
		if (src_ace->ace_arl != ace->ace_arl) {
			ace_t *dst_ace;

			/*
			 * Check for a resolved entry in the src_ace->ace_arl.
			 */
			dst_ace = ar_ce_lookup_entry(src_ace->ace_arl,
			    areq->areq_proto, proto_addr, proto_addr_len);

			if (dst_ace != NULL && ACE_RESOLVED(dst_ace)) {
				DTRACE_PROBE3(query_other_arl, arl_t *, arl,
				    areq_t *, areq, ace_t *, dst_ace);
				(void) ar_ce_resolve(ace, dst_ace->ace_hw_addr,
				    dst_ace->ace_hw_addr_length);
				return (EINPROGRESS);
			}
		}
	}
	ms = ar_query_xmit(ace, src_ace);
	if (ms == 0) {
		/* Immediate reply requested. */
		ar_query_reply(ace, ENXIO, NULL, (uint32_t)0);
	} else {
		mi_timer(arl->arl_wq, ace->ace_mp, ms);
	}
	return (EINPROGRESS);
err_ret:
	if (is_mproto)
		BUMP_IRE_STATS(ire_stats_v4, ire_stats_freed);
	return (err);
}

/* Handle simple query requests. */
static int
ar_entry_squery(queue_t *q, mblk_t *mp_orig)
{
	ace_t	*ace;
	area_t	*area;
	arl_t	*arl;
	uchar_t	*hw_addr;
	uint32_t	hw_addr_len;
	mblk_t	*mp = mp_orig;
	uchar_t	*proto_addr;
	int	proto_addr_len;

	if (DB_TYPE(mp) == M_IOCTL)
		mp = mp->b_cont;
	arl = ar_ll_lookup_from_mp(mp);
	if (arl == NULL)
		return (EINVAL);
	/*
	 * Newly received commands from clients go to the tail of the queue.
	 */
	if (CMD_NEEDS_QUEUEING(mp_orig, arl)) {
		DTRACE_PROBE3(squery_enqueued, queue_t *, q, mblk_t *, mp_orig,
		    arl_t *, arl);
		ar_cmd_enqueue(arl, mp_orig, q, AR_ENTRY_SQUERY, B_TRUE);
		return (EINPROGRESS);
	}
	mp_orig->b_prev = NULL;

	/* Extract parameters from the request message. */
	area = (area_t *)mp->b_rptr;
	proto_addr_len = area->area_proto_addr_length;
	proto_addr = mi_offset_paramc(mp, area->area_proto_addr_offset,
	    proto_addr_len);
	hw_addr_len = area->area_hw_addr_length;
	hw_addr = mi_offset_paramc(mp, area->area_hw_addr_offset, hw_addr_len);
	if (proto_addr == NULL || hw_addr == NULL) {
		DTRACE_PROBE1(squery_illegal_address, area_t *, area);
		return (EINVAL);
	}
	ace = ar_ce_lookup(arl, area->area_proto, proto_addr, proto_addr_len);
	if (ace == NULL) {
		return (ENXIO);
	}
	if (hw_addr_len < ace->ace_hw_addr_length) {
		return (EINVAL);
	}
	if (ACE_RESOLVED(ace)) {
		/* Got it, prepare the response. */
		ASSERT(area->area_hw_addr_length == ace->ace_hw_addr_length);
		ar_set_address(ace, hw_addr, proto_addr, proto_addr_len);
	} else {
		/*
		 * We have an incomplete entry.	 Set the length to zero and
		 * just return out the flags.
		 */
		area->area_hw_addr_length = 0;
	}
	area->area_flags = ace->ace_flags;
	if (mp == mp_orig) {
		/* Non-ioctl case */
		/* TODO: change message type? */
		DB_TYPE(mp) = M_CTL; /* Caught by ip_wput */
		DTRACE_PROBE3(squery_reply, queue_t *, q, mblk_t *, mp,
		    arl_t *, arl);
		qreply(q, mp);
		return (EINPROGRESS);
	}
	return (0);
}

/* Process an interface down causing us to detach and unbind. */
/* ARGSUSED */
static int
ar_interface_down(queue_t *q, mblk_t *mp)
{
	arl_t	*arl;

	arl = ar_ll_lookup_from_mp(mp);
	if (arl == NULL || arl->arl_closing) {
		DTRACE_PROBE2(down_no_arl, queue_t *, q, mblk_t *, mp);
		return (EINVAL);
	}

	/*
	 * Newly received commands from clients go to the tail of the queue.
	 */
	if (CMD_NEEDS_QUEUEING(mp, arl)) {
		DTRACE_PROBE3(down_enqueued, queue_t *, q, mblk_t *, mp,
		    arl_t *, arl);
		ar_cmd_enqueue(arl, mp, q, AR_INTERFACE_DOWN, B_TRUE);
		return (EINPROGRESS);
	}
	mp->b_prev = NULL;
	/*
	 * The arl is already down, no work to do.
	 */
	if (arl->arl_state == ARL_S_DOWN) {
		/* ar_rput frees the mp */
		return (0);
	}

	/*
	 * This command cannot complete in a single shot now itself.
	 * It has to be restarted after the receipt of the ack from
	 * the driver. So we need to enqueue the command (at the head).
	 */
	ar_cmd_enqueue(arl, mp, q, AR_INTERFACE_DOWN, B_FALSE);

	ASSERT(arl->arl_state == ARL_S_UP);

	/* Free all arp entries for this interface */
	ar_ce_walk(ar_ce_delete_per_arl, arl);

	ar_ll_down(arl);
	/* Return EINPROGRESS so that ar_rput does not free the 'mp' */
	return (EINPROGRESS);
}


/* Process an interface up causing the info req sequence to start. */
/* ARGSUSED */
static int
ar_interface_up(queue_t *q, mblk_t *mp)
{
	arl_t	*arl;
	int	err;
	mblk_t	*mp1;

	arl = ar_ll_lookup_from_mp(mp);
	if (arl == NULL || arl->arl_closing) {
		DTRACE_PROBE2(up_no_arl, queue_t *, q, mblk_t *, mp);
		err = EINVAL;
		goto done;
	}

	/*
	 * Newly received commands from clients go to the tail of the queue.
	 */
	if (CMD_NEEDS_QUEUEING(mp, arl)) {
		DTRACE_PROBE3(up_enqueued, queue_t *, q, mblk_t *, mp,
		    arl_t *, arl);
		ar_cmd_enqueue(arl, mp, q, AR_INTERFACE_UP, B_TRUE);
		return (EINPROGRESS);
	}
	mp->b_prev = NULL;

	/*
	 * The arl is already up. No work to do.
	 */
	if (arl->arl_state == ARL_S_UP) {
		err = 0;
		goto done;
	}

	/*
	 * This command cannot complete in a single shot now itself.
	 * It has to be restarted after the receipt of the ack from
	 * the driver. So we need to enqueue the command (at the head).
	 */
	ar_cmd_enqueue(arl, mp, q, AR_INTERFACE_UP, B_FALSE);

	err = ar_ll_up(arl);

	/* Return EINPROGRESS so that ar_rput does not free the 'mp' */
	return (EINPROGRESS);

done:
	/* caller frees 'mp' */

	mp1 = ar_alloc(AR_DLPIOP_DONE, err);
	if (mp1 != NULL) {
		q = WR(q);
		DTRACE_PROBE3(up_send_err, queue_t *, q, mblk_t *, mp1,
		    int, err);
		putnext(q, mp1);
	}
	return (err);
}

/*
 * Enable an interface to
 * process of ARP_REQUEST and ARP_RESPONSE messages
 */
/* ARGSUSED */
static int
ar_interface_on(queue_t *q, mblk_t *mp)
{
	arl_t	*arl;

	arl = ar_ll_lookup_from_mp(mp);
	if (arl == NULL) {
		DTRACE_PROBE2(on_no_arl, queue_t *, q, mblk_t *, mp);
		return (EINVAL);
	}
	/* Turn off the IFF_NOARP flag  and activate ARP */
	DTRACE_PROBE3(on_intf, queue_t *, q, mblk_t *, mp, arl_t *, arl);
	arl->arl_flags = 0;
	return (0);
}

/*
 * Disable an interface from processing
 * ARP_REQUEST and ARP_RESPONSE messages
 */
/* ARGSUSED */
static int
ar_interface_off(queue_t *q, mblk_t *mp)
{
	arl_t	*arl;

	arl = ar_ll_lookup_from_mp(mp);
	if (arl == NULL) {
		DTRACE_PROBE2(off_no_arl, queue_t *, q, mblk_t *, mp);
		return (EINVAL);
	}
	/* Turn on the IFF_NOARP flag and deactivate ARP */
	DTRACE_PROBE3(off_intf, queue_t *, q, mblk_t *, mp, arl_t *, arl);
	arl->arl_flags = ARL_F_NOARP;
	return (0);
}

/*
 * The queue 'q' is closing. Walk all the arl's and free any message
 * pending in the arl_queue if it originated from the closing q.
 * Also cleanup the ip_pending_queue, if the arp-IP stream is closing.
 */
static void
ar_ll_cleanup_arl_queue(queue_t *q)
{
	arl_t	*arl;
	mblk_t	*mp;
	mblk_t	*mpnext;
	mblk_t	*prev;

	for (arl = arl_g_head; arl != NULL; arl = arl->arl_next) {
		for (prev = NULL, mp = arl->arl_queue; mp != NULL;
		    mp = mpnext) {
			mpnext = mp->b_next;
			if ((void *)mp->b_queue == (void *)q ||
			    (void *)mp->b_queue == (void *)OTHERQ(q)) {
				if (prev == NULL)
					arl->arl_queue = mp->b_next;
				else
					prev->b_next = mp->b_next;
				if (arl->arl_queue_tail == mp)
					arl->arl_queue_tail = prev;
				if (DB_TYPE(mp) == M_PROTO &&
				    *(uint32_t *)mp->b_rptr == AR_ENTRY_QUERY) {
					BUMP_IRE_STATS(ire_stats_v4,
					    ire_stats_freed);
				}
				inet_freemsg(mp);
			} else {
				prev = mp;
			}
		}
	}
}

/*
 * Look up a lower level tap by name.
 */
static arl_t *
ar_ll_lookup_by_name(const char *name)
{
	arl_t	*arl;

	for (arl = arl_g_head; arl; arl = arl->arl_next) {
		if (strcmp(arl->arl_name, name) == 0) {
			return (arl);
		}
	}
	return (NULL);
}

/*
 * Look up a lower level tap using parameters extracted from the common
 * portion of the ARP command.
 */
static arl_t *
ar_ll_lookup_from_mp(mblk_t *mp)
{
	arc_t	*arc = (arc_t *)mp->b_rptr;
	uint8_t	*name;
	size_t	namelen = arc->arc_name_length;

	name = mi_offset_param(mp, arc->arc_name_offset, namelen);
	if (name == NULL || name[namelen - 1] != '\0')
		return (NULL);
	return (ar_ll_lookup_by_name((char *)name));
}

static void
ar_ll_init(ar_t *ar, mblk_t *mp)
{
	arl_t	*arl;
	dl_info_ack_t *dlia = (dl_info_ack_t *)mp->b_rptr;

	ASSERT(ar->ar_arl == NULL);

	if ((arl = (arl_t *)mi_zalloc(sizeof (arl_t))) == NULL)
		return;

	arl->arl_provider_style = dlia->dl_provider_style;
	arl->arl_rq = ar->ar_rq;
	arl->arl_wq = ar->ar_wq;

	arl->arl_dlpi_pending = DL_PRIM_INVAL;
	arl->arl_link_up = B_TRUE;

	ar->ar_arl = arl;

	/*
	 * If/when ARP gets pushed into the IP module then this code to make
	 * a number uniquely identify an ARP instance can be removed and the
	 * ifindex from IP used.  Rather than try and reinvent or copy the
	 * code used by IP for the purpose of allocating an index number
	 * (and trying to keep the number small), just allocate it in an
	 * ever increasing manner.  This index number isn't ever exposed to
	 * users directly, its only use is for providing the pfhooks interface
	 * with a number it can use to uniquely identify an interface in time.
	 *
	 * Using a 32bit counter, over 136 plumbs would need to be done every
	 * second of every day (non-leap year) for it to wrap around and the
	 * for() loop below to kick in as a performance concern.
	 */
	if (arp_counter_wrapped) {
		arl_t *as;

		do {
			for (as = arl_g_head; as != NULL; as = as->arl_next)
				if (as->arl_index == arp_index_counter) {
					arp_index_counter++;
					if (arp_index_counter == 0) {
						arp_counter_wrapped++;
						arp_index_counter = 1;
					}
					break;
			}
		} while (as != NULL);
	} else {
		arl->arl_index = arp_index_counter;
	}
	arp_index_counter++;
	if (arp_index_counter == 0) {
		arp_counter_wrapped++;
		arp_index_counter = 1;
	}
}

/*
 * This routine is called during module initialization when the DL_INFO_ACK
 * comes back from the device.	We set up defaults for all the device dependent
 * doo-dads we are going to need.  This will leave us ready to roll if we are
 * attempting auto-configuration.  Alternatively, these defaults can be
 * overidden by initialization procedures possessing higher intelligence.
 */
static void
ar_ll_set_defaults(arl_t *arl, mblk_t *mp)
{
	ar_m_t		*arm;
	dl_info_ack_t	*dlia = (dl_info_ack_t *)mp->b_rptr;
	dl_unitdata_req_t *dlur;
	int		hw_addr_length;
	int		i1;
	uchar_t		*up;
	t_scalar_t	sap_length;

	/* Sanity check... */
	if (arl == NULL)
		return;
	/*
	 * If we receive multiple DL_INFO_ACkS make sure there are no
	 * leaks by clearing the defaults now
	 */
	if (arl->arl_data != NULL ||arl->arl_xmit_template != NULL)
		ar_ll_clear_defaults(arl);

	if ((arm = ar_m_lookup(dlia->dl_mac_type)) == NULL)
		arm = ar_m_lookup(DL_OTHER);
	ASSERT(arm != NULL);

	/*
	 * We initialize based on parameters in the (currently) not too
	 * exhaustive ar_m_tbl.
	 */
	if (dlia->dl_version == DL_VERSION_2) {
		hw_addr_length = dlia->dl_brdcst_addr_length;
	} else {
		hw_addr_length = arm->ar_mac_hw_addr_length;
	}

	if ((arl->arl_data = mi_zalloc(2 * hw_addr_length)) == NULL)
		goto bad;

	arl->arl_arp_hw_type = arm->ar_mac_arp_hw_type;

	/*
	 * Someday DLPI will provide the multicast address?  Meanwhile we
	 * assume an address of all ones, known to work on some popular
	 * networks.
	 */
	up = (uchar_t *)arl->arl_data;
	arl->arl_arp_addr = up;
	if (dlia->dl_version == DL_VERSION_2) {
		uchar_t *up2;

		up2 = mi_offset_param(mp, dlia->dl_brdcst_addr_offset,
		    hw_addr_length);
		if (up2 == NULL)
			goto bad;

		bcopy(up2, up, hw_addr_length);
		up += hw_addr_length;
		/*
		 * TODO Note that sap_length can be 0 before binding according
		 * to the DLPI spec.
		 */
		sap_length = dlia->dl_sap_length;
	} else {
		for (i1 = 0; i1 < hw_addr_length; i1++)
			*up++ = (char)~0;
		sap_length = arm->ar_mac_sap_length;
	}

	arl->arl_sap_length = sap_length;

	/*
	 * The hardware address will be filled in when we see the DL_BIND_ACK.
	 * We reserve space for it here, and make arl_hw_addr point to it.
	 */
	arl->arl_hw_addr = up;
	arl->arl_hw_addr_length = hw_addr_length;
	up += arl->arl_hw_addr_length;

	/*
	 * Make us a template DL_UNITDATA_REQ message which we will use for
	 * broadcasting resolution requests, and which we will clone to hand
	 * back as responses to the protocols.
	 */
	arl->arl_xmit_template = ar_dlpi_comm(DL_UNITDATA_REQ,
	    sizeof (dl_unitdata_req_t) + arl->arl_hw_addr_length +
	    ABS(arl->arl_sap_length));
	if (arl->arl_xmit_template == NULL)
		goto bad;

	dlur = (dl_unitdata_req_t *)arl->arl_xmit_template->b_rptr;
	dlur->dl_priority.dl_min = 0;
	dlur->dl_priority.dl_max = 0;
	dlur->dl_dest_addr_length = hw_addr_length + ABS(arl->arl_sap_length);
	dlur->dl_dest_addr_offset = sizeof (dl_unitdata_req_t);

	/* Note the destination address offset permanently in the arl. */
	if (arl->arl_sap_length < 0) {
		arl->arl_xmit_template_addr_offset = dlur->dl_dest_addr_offset;
		arl->arl_xmit_template_sap_offset = dlur->dl_dest_addr_offset +
		    dlur->dl_dest_addr_length + arl->arl_sap_length;
	} else {
		/* The sap is first in the address */
		arl->arl_xmit_template_addr_offset = dlur->dl_dest_addr_offset
		    + arl->arl_sap_length;
		arl->arl_xmit_template_sap_offset = dlur->dl_dest_addr_offset;
	}

	*(uint16_t *)(arl->arl_xmit_template->b_rptr +
	    arl->arl_xmit_template_sap_offset) = ETHERTYPE_ARP;

	return;
bad:
	ar_ll_clear_defaults(arl);
}

static void
ar_ll_clear_defaults(arl_t *arl)
{
	if (arl->arl_data) {
		mi_free(arl->arl_data);
		arl->arl_data = NULL;
		arl->arl_arp_addr = NULL;
		arl->arl_sap_length = 0;
		arl->arl_hw_addr = NULL;
		arl->arl_hw_addr_length = NULL;
	}
	if (arl->arl_xmit_template) {
		freemsg(arl->arl_xmit_template);
		arl->arl_xmit_template = NULL;
	}
}

static void
ar_ll_down(arl_t *arl)
{
	mblk_t	*mp;
	ar_t	*ar;

	ASSERT(arl->arl_state == ARL_S_UP);

	/* Let's break the association between an ARL and IP instance */
	ar = (ar_t *)arl->arl_rq->q_ptr;
	if (ar->ar_arl_ip_assoc != NULL) {
		ASSERT(ar->ar_arl_ip_assoc->ar_arl_ip_assoc != NULL &&
		    ar->ar_arl_ip_assoc->ar_arl_ip_assoc == ar);
		ar->ar_arl_ip_assoc->ar_arl_ip_assoc = NULL;
		ar->ar_arl_ip_assoc = NULL;
	}

	arl->arl_state = ARL_S_PENDING;

	mp = arl->arl_unbind_mp;
	ASSERT(mp != NULL);
	ar_dlpi_send(arl, mp);
	arl->arl_unbind_mp = NULL;

	if (arl->arl_provider_style == DL_STYLE2) {
		mp = arl->arl_detach_mp;
		ASSERT(mp != NULL);
		ar_dlpi_send(arl, mp);
		arl->arl_detach_mp = NULL;
	}
}

static int
ar_ll_up(arl_t *arl)
{
	mblk_t	*attach_mp = NULL;
	mblk_t	*bind_mp = NULL;
	mblk_t	*detach_mp = NULL;
	mblk_t	*unbind_mp = NULL;
	mblk_t	*info_mp = NULL;
	mblk_t	*notify_mp = NULL;

	ASSERT(arl->arl_state == ARL_S_DOWN);

	if (arl->arl_provider_style == DL_STYLE2) {
		attach_mp =
		    ar_dlpi_comm(DL_ATTACH_REQ, sizeof (dl_attach_req_t));
		if (attach_mp == NULL)
			goto bad;
		((dl_attach_req_t *)attach_mp->b_rptr)->dl_ppa =
		    arl->arl_ppa;

		detach_mp =
		    ar_dlpi_comm(DL_DETACH_REQ, sizeof (dl_detach_req_t));
		if (detach_mp == NULL)
			goto bad;
	}

	info_mp = ar_dlpi_comm(DL_INFO_REQ, sizeof (dl_info_req_t));
	if (info_mp == NULL)
		goto bad;

	/* Allocate and initialize a bind message. */
	bind_mp = ar_dlpi_comm(DL_BIND_REQ, sizeof (dl_bind_req_t));
	if (bind_mp == NULL)
		goto bad;
	((dl_bind_req_t *)bind_mp->b_rptr)->dl_sap = ETHERTYPE_ARP;
	((dl_bind_req_t *)bind_mp->b_rptr)->dl_service_mode = DL_CLDLS;

	unbind_mp = ar_dlpi_comm(DL_UNBIND_REQ, sizeof (dl_unbind_req_t));
	if (unbind_mp == NULL)
		goto bad;

	notify_mp = ar_dlpi_comm(DL_NOTIFY_REQ, sizeof (dl_notify_req_t));
	if (notify_mp == NULL)
		goto bad;
	((dl_notify_req_t *)notify_mp->b_rptr)->dl_notifications =
	    DL_NOTE_LINK_UP | DL_NOTE_LINK_DOWN;

	arl->arl_state = ARL_S_PENDING;
	if (arl->arl_provider_style == DL_STYLE2) {
		ar_dlpi_send(arl, attach_mp);
		ASSERT(detach_mp != NULL);
		arl->arl_detach_mp = detach_mp;
	}
	ar_dlpi_send(arl, info_mp);
	ar_dlpi_send(arl, bind_mp);
	arl->arl_unbind_mp = unbind_mp;
	ar_dlpi_send(arl, notify_mp);
	return (0);

bad:
	freemsg(attach_mp);
	freemsg(bind_mp);
	freemsg(detach_mp);
	freemsg(unbind_mp);
	freemsg(info_mp);
	freemsg(notify_mp);
	return (ENOMEM);
}

/* Process mapping add requests from external messages. */
static int
ar_mapping_add(queue_t *q, mblk_t *mp_orig)
{
	arma_t	*arma;
	mblk_t	*mp = mp_orig;
	ace_t	*ace;
	uchar_t	*hw_addr;
	uint32_t	hw_addr_len;
	uchar_t	*proto_addr;
	uint32_t	proto_addr_len;
	uchar_t	*proto_mask;
	uchar_t	*proto_extract_mask;
	uint32_t	hw_extract_start;
	arl_t	*arl;

	/* We handle both M_IOCTL and M_PROTO messages. */
	if (DB_TYPE(mp) == M_IOCTL)
		mp = mp->b_cont;
	arl = ar_ll_lookup_from_mp(mp);
	if (arl == NULL)
		return (EINVAL);
	/*
	 * Newly received commands from clients go to the tail of the queue.
	 */
	if (CMD_NEEDS_QUEUEING(mp_orig, arl)) {
		DTRACE_PROBE3(madd_enqueued, queue_t *, q, mblk_t *, mp_orig,
		    arl_t *, arl);
		ar_cmd_enqueue(arl, mp_orig, q, AR_MAPPING_ADD, B_TRUE);
		return (EINPROGRESS);
	}
	mp_orig->b_prev = NULL;

	arma = (arma_t *)mp->b_rptr;
	if ((ace = ar_ce_lookup_from_area(mp, ar_ce_lookup_mapping)) != NULL)
		ar_ce_delete(ace);
	hw_addr_len = arma->arma_hw_addr_length;
	hw_addr = mi_offset_paramc(mp, arma->arma_hw_addr_offset, hw_addr_len);
	proto_addr_len = arma->arma_proto_addr_length;
	proto_addr = mi_offset_paramc(mp, arma->arma_proto_addr_offset,
	    proto_addr_len);
	proto_mask = mi_offset_paramc(mp, arma->arma_proto_mask_offset,
	    proto_addr_len);
	proto_extract_mask = mi_offset_paramc(mp,
	    arma->arma_proto_extract_mask_offset, proto_addr_len);
	hw_extract_start = arma->arma_hw_mapping_start;
	if (proto_mask == NULL || proto_extract_mask == NULL) {
		DTRACE_PROBE2(madd_illegal_mask, arl_t *, arl, arpa_t *, arma);
		return (EINVAL);
	}
	return (ar_ce_create(
	    arl,
		arma->arma_proto,
		hw_addr,
		hw_addr_len,
		proto_addr,
		proto_addr_len,
		proto_mask,
		proto_extract_mask,
		hw_extract_start,
		arma->arma_flags | ACE_F_MAPPING));
}

static boolean_t
ar_mask_all_ones(uchar_t *mask, uint32_t mask_len)
{
	if (mask == NULL)
		return (B_TRUE);

	while (mask_len-- > 0) {
		if (*mask++ != 0xFF) {
			return (B_FALSE);
		}
	}
	return (B_TRUE);
}

/* Find an entry for a particular MAC type in the ar_m_tbl. */
static ar_m_t	*
ar_m_lookup(t_uscalar_t mac_type)
{
	ar_m_t	*arm;

	for (arm = ar_m_tbl; arm < A_END(ar_m_tbl); arm++) {
		if (arm->ar_mac_type == mac_type)
			return (arm);
	}
	return (NULL);
}

/* Respond to Named Dispatch requests. */
static int
ar_nd_ioctl(queue_t *q, mblk_t *mp)
{
	if (DB_TYPE(mp) == M_IOCTL && nd_getset(q, ar_g_nd, mp))
		return (0);
	return (ENOENT);
}

/* ARP module open routine. */
static int
ar_open(queue_t *q, dev_t *devp, int flag, int sflag, cred_t *credp)
{
	ar_t	*ar;
	int	err;
	queue_t *tmp_q;
	mblk_t *mp;

	TRACE_1(TR_FAC_ARP, TR_ARP_OPEN,
	    "arp_open: q %p", q);
	/* Allow a reopen. */
	if (q->q_ptr != NULL) {
		return (0);
	}
	/* Load up the Named Dispatch tables, if not already done. */
	if (ar_g_nd == NULL &&
	    !ar_param_register(arp_param_arr, A_CNT(arp_param_arr))) {
		ar_cleanup();
		return (ENOMEM);
	}
	/* mi_open_comm allocates the instance data structure, etc. */
	err = mi_open_comm(&ar_g_head, sizeof (ar_t), q, devp, flag, sflag,
	    credp);
	if (err) {
		ar_cleanup();
		return (err);
	}

	/*
	 * We are D_MTPERMOD so it is safe to do qprocson before
	 * the instance data has been initialized.
	 */
	qprocson(q);

	ar = (ar_t *)q->q_ptr;
	ar->ar_rq = q;
	q = WR(q);
	ar->ar_wq = q;
	crhold(credp);
	ar->ar_credp = credp;

	/*
	 * Probe for the DLPI info if we are not pushed on IP. Wait for
	 * the reply. In case of error call ar_close() which will take
	 * care of doing everything required to close this instance, such
	 * as freeing the arl, restarting the timer on a different queue etc.
	 */
	if (strcmp(q->q_next->q_qinfo->qi_minfo->mi_idname, "ip") == 0) {
		arc_t *arc;

		/*
		 * We are pushed directly on top of IP. There is no need to
		 * send down a DL_INFO_REQ. Return success. This could
		 * either be an ill stream (i.e. <arp-IP-Driver> stream)
		 * or a stream corresponding to an open of /dev/arp
		 * (i.e. <arp-IP> stream). Note that we don't support
		 * pushing some module in between arp and IP.
		 *
		 * Tell IP, though, that we're an extended implementation, so
		 * it knows to expect a DAD response after bringing an
		 * interface up.  Old ATM drivers won't do this, and IP will
		 * just bring the interface up immediately.
		 */
		ar->ar_on_ill_stream = (q->q_next->q_next != NULL);
		if (!ar->ar_on_ill_stream)
			return (0);
		mp = allocb(sizeof (arc_t), BPRI_MED);
		if (mp == NULL) {
			(void) ar_close(RD(q));
			return (ENOMEM);
		}
		DB_TYPE(mp) = M_CTL;
		arc = (arc_t *)mp->b_rptr;
		mp->b_wptr = mp->b_rptr + sizeof (arc_t);
		arc->arc_cmd = AR_ARP_EXTEND;
		putnext(q, mp);
		return (0);
	}
	tmp_q = q;
	/* Get the driver's queue */
	while (tmp_q->q_next != NULL)
		tmp_q = tmp_q->q_next;

	ASSERT(tmp_q->q_qinfo->qi_minfo != NULL);

	if (strcmp(tmp_q->q_qinfo->qi_minfo->mi_idname, "ip") == 0) {
		/*
		 * We don't support pushing ARP arbitrarily on an IP driver
		 * stream.  ARP has to be pushed directly above IP.
		 */
		(void) ar_close(RD(q));
		return (ENOTSUP);
	} else {
		/*
		 * Send down a DL_INFO_REQ so we can find out what we are
		 * talking to.
		 */
		mp = ar_dlpi_comm(DL_INFO_REQ, sizeof (dl_info_req_t));
		if (mp == NULL) {
			(void) ar_close(RD(q));
			return (ENOMEM);
		}
		putnext(ar->ar_wq, mp);
		while (ar->ar_arl == NULL) {
			if (!qwait_sig(ar->ar_rq)) {
				(void) ar_close(RD(q));
				return (EINTR);
			}
		}
	}
	return (0);
}

/* Get current value of Named Dispatch item. */
/* ARGSUSED */
static int
ar_param_get(queue_t *q, mblk_t *mp, caddr_t cp, cred_t *cr)
{
	arpparam_t	*arppa = (arpparam_t *)cp;

	(void) mi_mpprintf(mp, "%d", arppa->arp_param_value);
	return (0);
}

/*
 * Walk through the param array specified registering each element with the
 * named dispatch handler.
 */
static boolean_t
ar_param_register(arpparam_t *arppa, int cnt)
{
	for (; cnt-- > 0; arppa++) {
		if (arppa->arp_param_name && arppa->arp_param_name[0]) {
			if (!nd_load(&ar_g_nd, arppa->arp_param_name,
			    ar_param_get, ar_param_set,
			    (caddr_t)arppa)) {
				nd_free(&ar_g_nd);
				return (B_FALSE);
			}
		}
	}
	return (B_TRUE);
}

/* Set new value of Named Dispatch item. */
/* ARGSUSED */
static int
ar_param_set(queue_t *q, mblk_t *mp, char *value, caddr_t cp, cred_t *cr)
{
	long		new_value;
	arpparam_t	*arppa = (arpparam_t *)cp;

	if (ddi_strtol(value, NULL, 10, &new_value) != 0 ||
	    new_value < arppa->arp_param_min ||
	    new_value > arppa->arp_param_max) {
		return (EINVAL);
	}
	arppa->arp_param_value = new_value;
	return (0);
}

/*
 * Process an I_PLINK ioctl. If the lower stream is an arp device stream,
 * append another mblk to the chain, that will carry the device name,
 * and the muxid. IP uses this info to lookup the corresponding ill, and
 * set the ill_arp_muxid atomically, as part of the I_PLINK, instead of
 * waiting for the SIOCSLIFMUXID. (which may never happen if ifconfig is
 * killed, and this has the bad effect of not being able to unplumb
 * subsequently)
 */
static int
ar_plink_send(queue_t *q, mblk_t *mp)
{
	char	*name;
	mblk_t 	*muxmp;
	mblk_t 	*mp1;
	ar_t	*ar;
	struct	linkblk *li;
	struct	ipmx_s	*ipmxp;
	queue_t	*arpwq;

	mp1 = mp->b_cont;
	ASSERT((mp1 != NULL) && (mp1->b_cont == NULL));
	li = (struct linkblk *)mp1->b_rptr;
	arpwq = li->l_qbot;

	/*
	 * Allocate a new mblk which will hold an ipmx_s and chain it to
	 * the M_IOCTL chain. The final chain will consist of 3 mblks,
	 * namely the M_IOCTL, followed by the linkblk, followed by the ipmx_s
	 */
	muxmp =  allocb(sizeof (struct ipmx_s), BPRI_MED);
	if (muxmp == NULL)
		return (ENOMEM);
	ipmxp = (struct ipmx_s *)muxmp->b_wptr;
	ipmxp->ipmx_arpdev_stream = 0;
	muxmp->b_wptr += sizeof (struct ipmx_s);
	mp1->b_cont = muxmp;

	/*
	 * The l_qbot represents the uppermost write queue of the
	 * lower stream. Walk down this stream till we hit ARP.
	 * We can safely walk, since STREAMS has made sure the stream
	 * cannot close till the IOCACK goes up, and is not interruptible.
	 */
	while (arpwq != NULL) {
		/*
		 * Beware of broken modules like logsubr.c that
		 * may not have a q_qinfo or qi_minfo.
		 */
		if ((q->q_qinfo != NULL) && (q->q_qinfo->qi_minfo != NULL)) {
			name = arpwq->q_qinfo->qi_minfo->mi_idname;
			if (name != NULL && name[0] != NULL &&
			    (strcmp(name, info.mi_idname) == 0))
				break;
		}
		arpwq = arpwq->q_next;
	}

	/*
	 * Check if arpwq corresponds to an arp device stream, by walking
	 * the mi list. If it does, then add the muxid and device name info
	 * for use by IP. IP will send the M_IOCACK.
	 */
	if (arpwq != NULL) {
		for (ar = (ar_t *)mi_first_ptr(&ar_g_head); ar != NULL;
		    ar = (ar_t *)mi_next_ptr(&ar_g_head, (void *)ar)) {
			if ((ar->ar_wq == arpwq) && (ar->ar_arl != NULL)) {
				ipmxp->ipmx_arpdev_stream = 1;
				(void) strcpy((char *)ipmxp->ipmx_name,
				    ar->ar_arl->arl_name);
				break;
			}
		}
	}

	putnext(q, mp);
	return (0);
}

/*
 * ar_ce_walk routine to delete any outstanding queries for an ar that is
 * going away.
 */
static void
ar_query_delete(ace_t *ace, void *arg)
{
	ar_t	*ar = arg;
	mblk_t	**mpp = &ace->ace_query_mp;
	mblk_t	*mp;

	while ((mp = *mpp) != NULL) {
		/* The response queue was stored in the query b_prev. */
		if ((queue_t *)mp->b_prev == ar->ar_wq ||
		    (queue_t *)mp->b_prev == ar->ar_rq) {
			*mpp = mp->b_next;
			if (DB_TYPE(mp) == M_PROTO &&
			    *(uint32_t *)mp->b_rptr == AR_ENTRY_QUERY) {
				BUMP_IRE_STATS(ire_stats_v4, ire_stats_freed);
			}
			inet_freemsg(mp);
		} else {
			mpp = &mp->b_next;
		}
	}
}

/*
 * This routine is called either when an address resolution has just been
 * found, or when it is time to give, or in some other error situation.
 * If a non-zero ret_val is provided, any outstanding queries for the
 * specified ace will be completed using that error value.  Otherwise,
 * the completion status will depend on whether the address has been
 * resolved.
 */
static void
ar_query_reply(ace_t *ace, int ret_val, uchar_t *proto_addr,
    uint32_t proto_addr_len)
{
	mblk_t	*areq_mp;
	arl_t	*arl = ace->ace_arl;
	mblk_t	*mp;
	mblk_t	*template;

	/* Cancel any outstanding timer. */
	mi_timer(arl->arl_wq, ace->ace_mp, -1L);
	/* Establish the return value appropriate. */
	if (ret_val == 0) {
		if (!ACE_RESOLVED(ace) || arl->arl_xmit_template == NULL)
			ret_val = ENXIO;
	}
	/* Terminate all outstanding queries. */
	while ((mp = ace->ace_query_mp) != 0) {
		/* The response queue was saved in b_prev. */
		queue_t	*q = (queue_t *)mp->b_prev;
		mp->b_prev = NULL;
		ace->ace_query_mp = mp->b_next;
		mp->b_next = NULL;
		/*
		 * If we have the answer, attempt to get a copy of the xmit
		 * template to prepare for the client.
		 */
		if (ret_val == 0 &&
		    !(template = copyb(arl->arl_xmit_template))) {
			/* Too bad, buy more memory. */
			ret_val = ENOMEM;
		}
		/* Complete the response based on how the request arrived. */
		if (DB_TYPE(mp) == M_IOCTL) {
			struct iocblk *ioc = (struct iocblk *)mp->b_rptr;

			ioc->ioc_error = ret_val;
			if (ret_val != 0) {
				DB_TYPE(mp) = M_IOCNAK;
				ioc->ioc_count = 0;
				putnext(q, mp);
				continue;
			}
			/*
			 * Return the xmit template out with the successful
			 * IOCTL.
			 */
			DB_TYPE(mp) = M_IOCACK;
			ioc->ioc_count = template->b_wptr - template->b_rptr;
			/* Remove the areq mblk from the IOCTL. */
			areq_mp = mp->b_cont;
			mp->b_cont = areq_mp->b_cont;
		} else {
			if (ret_val != 0) {
				/* TODO: find some way to let the guy know? */
				inet_freemsg(mp);
				BUMP_IRE_STATS(ire_stats_v4, ire_stats_freed);
				continue;
			}
			/*
			 * In the M_PROTO case, the areq message is followed by
			 * a message chain to be returned to the protocol.  ARP
			 * doesn't know (or care) what is in this chain, but in
			 * the event that the reader is pondering the
			 * relationship between ARP and IP (for example), the
			 * areq is followed by an incipient IRE, and then the
			 * original outbound packet.  Here we detach the areq.
			 */
			areq_mp = mp;
			mp = mp->b_cont;
		}
		if (ABS(arl->arl_sap_length) != 0) {
			/*
			 * Copy the SAP type specified in the request into
			 * the xmit template.
			 */
			areq_t	*areq = (areq_t *)areq_mp->b_rptr;
			bcopy(&areq->areq_sap[0],
			    (char *)template->b_rptr +
			    arl->arl_xmit_template_sap_offset,
			    ABS(arl->arl_sap_length));
		}
		/* Done with the areq message. */
		freeb(areq_mp);
		/*
		 * Copy the resolved hardware address into the xmit template
		 * or perform the mapping operation.
		 */
		ar_set_address(ace, (uchar_t *)template->b_rptr
		    + arl->arl_xmit_template_addr_offset,
		    proto_addr, proto_addr_len);
		/*
		 * Now insert the xmit template after the response message.  In
		 * the M_IOCTL case, it will be the returned data block.  In
		 * the M_PROTO case, (again using IP as an example) it will
		 * appear after the IRE and before the outbound packet.
		 */
		template->b_cont = mp->b_cont;
		mp->b_cont = template;
		putnext(q, mp);
	}

	/*
	 * Unless we are responding from a permanent cache entry, start the
	 * cleanup timer or (on error) delete the entry.
	 */
	if (!(ace->ace_flags & (ACE_F_PERMANENT | ACE_F_DYING))) {
		if (!ACE_RESOLVED(ace) || arl->arl_xmit_template == NULL) {
			/*
			 * No need to notify IP here, because the entry was
			 * never resolved, so IP can't have any cached copies
			 * of the address.
			 */
			ar_ce_delete(ace);
		} else {
			mi_timer(arl->arl_wq, ace->ace_mp,
			    arp_cleanup_interval);
		}
	}
}

/*
 * Returns number of milliseconds after which we should either rexmit or abort.
 * Return of zero means we should abort. src_ace is the ace corresponding
 * to the source address in the areq sent by IP.
 */
static clock_t
ar_query_xmit(ace_t *ace, ace_t *src_ace)
{
	areq_t	*areq;
	mblk_t	*mp;
	uchar_t	*proto_addr;
	uchar_t	*sender_addr;
	arl_t *src_arl;

	mp = ace->ace_query_mp;
	if (!mp)
		return (0);
	if (DB_TYPE(mp) == M_IOCTL)
		mp = mp->b_cont;
	areq = (areq_t *)mp->b_rptr;
	if (areq->areq_xmit_count == 0)
		return (0);
	areq->areq_xmit_count--;
	proto_addr = mi_offset_paramc(mp, areq->areq_target_addr_offset,
	    areq->areq_target_addr_length);
	sender_addr = mi_offset_paramc(mp, areq->areq_sender_addr_offset,
	    areq->areq_sender_addr_length);

	/*
	 * Get the source h/w address for the sender addr. With interface
	 * groups, IP sends us source address belonging to a different
	 * interface.
	 */
	if (src_ace == NULL) {
		src_ace = ar_ce_lookup_permanent(areq->areq_proto, sender_addr,
		    areq->areq_sender_addr_length);
		if (src_ace == NULL) {
			DTRACE_PROBE3(xmit_no_source, ace_t *, ace,
			    areq_t *, areq, uchar_t *, sender_addr);
			return (0);
		}
	}

	/*
	 * If we haven't yet finished duplicate address checking on this source
	 * address, then do *not* use it on the wire.  Doing so will corrupt
	 * the world's caches.  Just allow the timer to restart.  Note that
	 * duplicate address checking will eventually complete one way or the
	 * other, so this cannot go on "forever."
	 */
	if (src_ace->ace_flags & ACE_F_UNVERIFIED) {
		DTRACE_PROBE2(xmit_source_unverified, ace_t *, ace,
		    ace_t *, src_ace);
		areq->areq_xmit_count++;
		return (areq->areq_xmit_interval);
	}

	/*
	 * Transmit on src_arl. We should transmit on src_arl. Otherwise
	 * the switch will send back a copy on other interfaces of the
	 * same group and as we could be using somebody else's source
	 * address + hardware address, ARP will treat this as a bogon.
	 */
	src_arl = src_ace->ace_arl;
	DTRACE_PROBE3(xmit_send, ace_t *, ace, ace_t *, src_ace,
	    areq_t *, areq);
	ar_xmit(src_arl, ARP_REQUEST, areq->areq_proto,
	    areq->areq_sender_addr_length, src_arl->arl_hw_addr, sender_addr,
	    src_arl->arl_arp_addr, proto_addr, NULL);
	src_ace->ace_last_bcast = ddi_get_lbolt();
	return (areq->areq_xmit_interval);
}

/* Our read side put procedure. */
static void
ar_rput(queue_t *q, mblk_t *mp)
{
	arh_t	*arh;
	arl_t	*arl;
	ace_t	*dst_ace;
	uchar_t	*dst_paddr;
	int	err;
	uint32_t	hlen;
	struct iocblk	*ioc;
	mblk_t	*mp1;
	int	op;
	uint32_t	plen;
	uint32_t	proto;
	uchar_t	*src_haddr;
	uchar_t	*src_paddr;
	boolean_t is_probe;
	int i;

	TRACE_1(TR_FAC_ARP, TR_ARP_RPUT_START,
	    "arp_rput_start: q %p", q);

	/*
	 * We handle ARP commands from below both in M_IOCTL and M_PROTO
	 * messages.  Actual ARP requests and responses will show up as
	 * M_PROTO messages containing DL_UNITDATA_IND blocks.
	 */
	switch (DB_TYPE(mp)) {
	case M_IOCTL:
		err = ar_cmd_dispatch(q, mp);
		switch (err) {
		case ENOENT:
			DB_TYPE(mp) = M_IOCNAK;
			if ((mp1 = mp->b_cont) != 0) {
				/*
				 * Collapse the data as a note to the
				 * originator.
				 */
				mp1->b_wptr = mp1->b_rptr;
			}
			break;
		case EINPROGRESS:
			TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
			    "arp_rput_end: q %p (%S)", q, "ioctl/inprogress");
			return;
		default:
			DB_TYPE(mp) = M_IOCACK;
			break;
		}
		ioc = (struct iocblk *)mp->b_rptr;
		ioc->ioc_error = err;
		if ((mp1 = mp->b_cont) != 0)
			ioc->ioc_count = mp1->b_wptr - mp1->b_rptr;
		else
			ioc->ioc_count = 0;
		qreply(q, mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		    "arp_rput_end: q %p (%S)", q, "ioctl");
		return;
	case M_CTL:
		/*
		 * IP is acking the AR_ARP_CLOSING message that we sent
		 * in ar_close.
		 */
		if (MBLKL(mp) == sizeof (arc_t)) {
			if (((arc_t *)mp->b_rptr)->arc_cmd == AR_ARP_CLOSING)
				((ar_t *)q->q_ptr)->ar_ip_acked_close = 1;
		}
		freemsg(mp);
		return;
	case M_PCPROTO:
	case M_PROTO:
		if (MBLKL(mp) >= sizeof (dl_unitdata_ind_t) &&
		    ((dl_unitdata_ind_t *)mp->b_rptr)->dl_primitive ==
		    DL_UNITDATA_IND) {
			arl = ((ar_t *)q->q_ptr)->ar_arl;
			if (arl != NULL) {
				/* Real messages from the wire! */
				break;
			}
			putnext(q, mp);
			TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
			    "arp_rput_end: q %p (%S)", q, "default");
			return;
		}
		err = ar_cmd_dispatch(q, mp);
		switch (err) {
		case ENOENT:
			/* Miscellaneous DLPI messages get shuffled off. */
			ar_rput_dlpi(q, mp);
			TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
			    "arp_rput_end: q %p (%S)", q, "proto/dlpi");
			break;
		case EINPROGRESS:
			TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
			    "arp_rput_end: q %p (%S)", q, "proto");
			break;
		default:
			inet_freemsg(mp);
			break;
		}
		return;
	default:
		putnext(q, mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		    "arp_rput_end: q %p (%S)", q, "default");
		return;
	}
	/*
	 * If the IFF_NOARP flag is on, then do not process any
	 * incoming ARP_REQUEST or incoming ARP_RESPONSE.
	 */
	if (arl->arl_flags & ARL_F_NOARP) {
		freemsg(mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		"arp_rput_end: q %p (%S)", q, "interface has IFF_NOARP set");
		return;
	}

	/*
	 * What we should have at this point is a DL_UNITDATA_IND message
	 * followed by an ARP packet.  We do some initial checks and then
	 * get to work.
	 */
	mp1 = mp->b_cont;
	if (mp1 == NULL) {
		freemsg(mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		    "arp_rput_end: q %p (%S)", q, "baddlpi");
		return;
	}
	if (mp1->b_cont != NULL) {
		/* No fooling around with funny messages. */
		if (!pullupmsg(mp1, -1)) {
			freemsg(mp);
			TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
			    "arp_rput_end: q %p (%S)", q, "pullupmsgfail");
			return;
		}
	}
	arh = (arh_t *)mp1->b_rptr;
	hlen = arh->arh_hlen;
	plen = arh->arh_plen;
	if (MBLKL(mp1) < ARH_FIXED_LEN + 2 * hlen + 2 * plen) {
		freemsg(mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		    "arp_rput_end: q %p (%S)", q, "short");
		return;
	}
	/*
	 * hlen 0 is used for RFC 1868 UnARP.
	 *
	 * Note that the rest of the code checks that hlen is what we expect
	 * for this hardware address type, so might as well discard packets
	 * here that don't match.
	 */
	if ((hlen > 0 && hlen != arl->arl_hw_addr_length) || plen == 0) {
		DTRACE_PROBE2(rput_bogus, arl_t *, arl, mblk_t *, mp1);
		freemsg(mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		    "arp_rput_end: q %p (%S)", q, "hlenzero/plenzero");
		return;
	}
	/*
	 * Historically, Solaris has been lenient about hardware type numbers.
	 * We should check here, but don't.
	 */
	DTRACE_PROBE2(rput_normal, arl_t *, arl, arh_t *, arh);

	DTRACE_PROBE3(arp__physical__in__start,
	    arl_t *, arl, arh_t *, arh, mblk_t *, mp);

	ARP_HOOK_IN(arp_physical_in_event, arp_physical_in,
		    arl->arl_index, arh, mp, mp1);

	DTRACE_PROBE1(arp__physical__in__end, mblk_t *, mp);

	if (mp == NULL)
		return;

	proto = (uint32_t)BE16_TO_U16(arh->arh_proto);
	src_haddr = (uchar_t *)arh;
	src_haddr = &src_haddr[ARH_FIXED_LEN];
	src_paddr = &src_haddr[hlen];
	dst_paddr = &src_haddr[hlen + plen + hlen];
	op = BE16_TO_U16(arh->arh_operation);

	/* Determine if this is just a probe */
	for (i = 0; i < plen; i++)
		if (src_paddr[i] != 0)
			break;
	is_probe = i >= plen;

	/*
	 * RFC 826: first check if the <protocol, sender protocol address> is
	 * in the cache, if there is a sender protocol address.  Note that this
	 * step also handles resolutions based on source.
	 */
	if (is_probe)
		err = AR_NOTFOUND;
	else
		err = ar_ce_resolve_all(arl, proto, src_haddr, hlen, src_paddr,
		    plen);
	switch (err) {
	case AR_BOGON:
		ar_client_notify(arl, mp1, AR_CN_BOGON);
		mp1 = NULL;
		break;
	case AR_FAILED:
		ar_client_notify(arl, mp1, AR_CN_FAILED);
		mp1 = NULL;
		break;
	case AR_LOOPBACK:
		DTRACE_PROBE2(rput_loopback, arl_t *, arl, arh_t *, arh);
		freemsg(mp1);
		mp1 = NULL;
		break;
	}
	if (mp1 == NULL) {
		freeb(mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		    "arp_rput_end: q %p (%S)", q, "unneeded");
		return;
	}

	/*
	 * Now look up the destination address.  By RFC 826, we ignore the
	 * packet at this step if the target isn't one of our addresses.  This
	 * is true even if the target is something we're trying to resolve and
	 * the packet is a response.
	 *
	 * Note that in order to do this correctly, we need to know when to
	 * notify IP of a change implied by the source address of the ARP
	 * message.  That implies that the local ARP table has entries for all
	 * of the resolved entries cached in the client.  This is why we must
	 * notify IP when we delete a resolved entry and we know that IP may
	 * have cached answers.
	 */
	dst_ace = ar_ce_lookup_entry(arl, proto, dst_paddr, plen);
	if (dst_ace == NULL || !ACE_RESOLVED(dst_ace) ||
	    !(dst_ace->ace_flags & ACE_F_PUBLISH)) {
		/*
		 * Let the client know if the source mapping has changed, even
		 * if the destination provides no useful information for the
		 * client.
		 */
		if (err == AR_CHANGED)
			ar_client_notify(arl, mp1, AR_CN_ANNOUNCE);
		else
			freemsg(mp1);
		freeb(mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		    "arp_rput_end: q %p (%S)", q, "nottarget");
		return;
	}

	/*
	 * If the target is unverified by DAD, then one of two things is true:
	 * either it's someone else claiming this address (on a probe or an
	 * announcement) or it's just a regular request.  The former is
	 * failure, but a regular request is not.
	 */
	if (dst_ace->ace_flags & ACE_F_UNVERIFIED) {
		/*
		 * Check for a reflection.  Some misbehaving bridges will
		 * reflect our own transmitted packets back to us.
		 */
		if (hlen == dst_ace->ace_hw_addr_length &&
		    bcmp(src_haddr, dst_ace->ace_hw_addr, hlen) == 0) {
			DTRACE_PROBE3(rput_probe_reflected, arl_t *, arl,
			    arh_t *, arh, ace_t *, dst_ace);
			freeb(mp);
			freemsg(mp1);
			TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
			    "arp_rput_end: q %p (%S)", q, "reflection");
			return;
		}
		if (is_probe || op == ARP_RESPONSE) {
			ar_client_notify(arl, mp1, AR_CN_FAILED);
			ar_ce_delete(dst_ace);
		} else if (err == AR_CHANGED) {
			ar_client_notify(arl, mp1, AR_CN_ANNOUNCE);
		} else {
			DTRACE_PROBE3(rput_request_unverified, arl_t *, arl,
			    arh_t *, arh, ace_t *, dst_ace);
			freemsg(mp1);
		}
		freeb(mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		    "arp_rput_end: q %p (%S)", q, "unverified");
		return;
	}

	/*
	 * If it's a request, then we reply to this, and if we think the
	 * sender's unknown, then we create an entry to avoid unnecessary ARPs.
	 * The design assumption is that someone ARPing us is likely to send us
	 * a packet soon, and that we'll want to reply to it.
	 */
	if (op == ARP_REQUEST) {
		const uchar_t *dstaddr = src_haddr;
		clock_t now;

		/*
		 * This implements periodic address defense based on a modified
		 * version of the RFC 3927 requirements.  Instead of sending a
		 * broadcasted reply every time, as demanded by the RFC, we
		 * send at most one broadcast reply per arp_broadcast_interval.
		 */
		now = ddi_get_lbolt();
		if ((now - dst_ace->ace_last_bcast) >
		    MSEC_TO_TICK(arp_broadcast_interval)) {
			DTRACE_PROBE3(rput_bcast_reply, arl_t *, arl,
			    arh_t *, arh, ace_t *, dst_ace);
			dst_ace->ace_last_bcast = now;
			dstaddr = arl->arl_arp_addr;
			/*
			 * If this is one of the long-suffering entries, then
			 * pull it out now.  It no longer needs separate
			 * defense, because we're doing now that with this
			 * broadcasted reply.
			 */
			dst_ace->ace_flags &= ~ACE_F_DELAYED;
		}

		ar_xmit(arl, ARP_RESPONSE, dst_ace->ace_proto, plen,
		    dst_ace->ace_hw_addr, dst_ace->ace_proto_addr,
		    src_haddr, src_paddr, dstaddr);
		if (!is_probe && err == AR_NOTFOUND &&
		    ar_ce_create(arl, proto, src_haddr, hlen, src_paddr, plen,
		    NULL, NULL, 0, 0) == 0) {
			ace_t *ace;

			ace = ar_ce_lookup(arl, proto, src_paddr, plen);
			ASSERT(ace != NULL);
			mi_timer(arl->arl_wq, ace->ace_mp,
			    arp_cleanup_interval);
		}
	}
	if (err == AR_CHANGED) {
		freeb(mp);
		ar_client_notify(arl, mp1, AR_CN_ANNOUNCE);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		    "arp_rput_end: q %p (%S)", q, "reqchange");
	} else {
		freemsg(mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_RPUT_END,
		    "arp_rput_end: q %p (%S)", q, "end");
	}
}

static void
ar_ce_restart_dad(ace_t *ace, void *arl)
{
	if ((ace->ace_arl == arl) &&
	    (ace->ace_flags & (ACE_F_UNVERIFIED|ACE_F_DAD_ABORTED)) ==
	    (ACE_F_UNVERIFIED|ACE_F_DAD_ABORTED)) {
		/*
		 * Slight cheat here: we don't use the initial probe delay
		 * in this obscure case.
		 */
		if (ace->ace_flags & ACE_F_FAST) {
			ace->ace_xmit_count = arp_fastprobe_count;
			ace->ace_xmit_interval = arp_fastprobe_interval;
		} else {
			ace->ace_xmit_count = arp_probe_count;
			ace->ace_xmit_interval = arp_probe_interval;
		}
		ace->ace_flags &= ~ACE_F_DAD_ABORTED;
		ace_set_timer(ace, B_FALSE);
	}
}

/* DLPI messages, other than DL_UNITDATA_IND are handled here. */
static void
ar_rput_dlpi(queue_t *q, mblk_t *mp)
{
	ar_t		*ar = q->q_ptr;
	arl_t		*arl = ar->ar_arl;
	union DL_primitives *dlp;
	const char	*err_str;

	if (MBLKL(mp) < sizeof (dlp->dl_primitive)) {
		putnext(q, mp);
		return;
	}
	dlp = (union DL_primitives *)mp->b_rptr;
	switch (dlp->dl_primitive) {
	case DL_ERROR_ACK:
		/*
		 * ce is confused about how DLPI works, so we have to interpret
		 * an "error" on DL_NOTIFY_ACK (which we never could have sent)
		 * as really meaning an error on DL_NOTIFY_REQ.
		 *
		 * Note that supporting DL_NOTIFY_REQ is optional, so printing
		 * out an error message on the console isn't warranted except
		 * for debug.
		 */
		if (dlp->error_ack.dl_error_primitive == DL_NOTIFY_ACK ||
		    dlp->error_ack.dl_error_primitive == DL_NOTIFY_REQ) {
			ar_dlpi_done(arl, DL_NOTIFY_REQ);
			freemsg(mp);
			return;
		}
		err_str = dlpi_prim_str(dlp->error_ack.dl_error_primitive);
		DTRACE_PROBE2(rput_dl_error, arl_t *, arl,
		    dl_error_ack_t *, &dlp->error_ack);
		switch (dlp->error_ack.dl_error_primitive) {
		case DL_UNBIND_REQ:
			if (arl->arl_provider_style == DL_STYLE1)
				arl->arl_state = ARL_S_DOWN;
			break;
		case DL_DETACH_REQ:
		case DL_BIND_REQ:
			arl->arl_state = ARL_S_DOWN;
			break;
		case DL_ATTACH_REQ:
			break;
		default:
			/* If it's anything else, we didn't send it. */
			putnext(q, mp);
			return;
		}
		ar_dlpi_done(arl, dlp->error_ack.dl_error_primitive);
		(void) mi_strlog(q, 1, SL_ERROR|SL_TRACE,
		    "ar_rput_dlpi: %s failed, dl_errno %d, dl_unix_errno %d",
		    err_str, dlp->error_ack.dl_errno,
		    dlp->error_ack.dl_unix_errno);
		break;
	case DL_INFO_ACK:
		/*
		 * We have a response back from the driver.  Go set up transmit
		 * defaults.
		 */
		DTRACE_PROBE2(rput_dl_info, arl_t *, arl,
		    dl_info_ack_t *, &dlp->info_ack);
		if (arl != NULL) {
			ar_ll_set_defaults(arl, mp);
			ar_dlpi_done(arl, DL_INFO_REQ);
		} else {
			ar_ll_init(ar, mp);
		}
		/* Kick off any awaiting messages */
		qenable(WR(q));
		break;
	case DL_OK_ACK:
		DTRACE_PROBE2(rput_dl_ok, arl_t *, arl,
		    dl_ok_ack_t *, &dlp->ok_ack);
		switch (dlp->ok_ack.dl_correct_primitive) {
		case DL_UNBIND_REQ:
			if (arl->arl_provider_style == DL_STYLE1)
				arl->arl_state = ARL_S_DOWN;
			break;
		case DL_DETACH_REQ:
			arl->arl_state = ARL_S_DOWN;
			break;
		case DL_ATTACH_REQ:
			break;
		default:
			putnext(q, mp);
			return;
		}
		ar_dlpi_done(arl, dlp->ok_ack.dl_correct_primitive);
		break;
	case DL_NOTIFY_ACK:
		DTRACE_PROBE2(rput_dl_notify, arl_t *, arl,
		    dl_notify_ack_t *, &dlp->notify_ack);
		/*
		 * We mostly care about interface-up transitions, as this is
		 * when we need to redo duplicate address detection.
		 */
		arl->arl_notifies =
		    (dlp->notify_ack.dl_notifications & DL_NOTE_LINK_UP) != 0;
		ar_dlpi_done(arl, DL_NOTIFY_REQ);
		break;
	case DL_BIND_ACK:
		DTRACE_PROBE2(rput_dl_bind, arl_t *, arl,
		    dl_bind_ack_t *, &dlp->bind_ack);
		if (arl->arl_sap_length < 0)
			bcopy((char *)dlp + dlp->bind_ack.dl_addr_offset,
			    arl->arl_hw_addr, arl->arl_hw_addr_length);
		else
			bcopy((char *)dlp + dlp->bind_ack.dl_addr_offset +
			    arl->arl_sap_length, arl->arl_hw_addr,
			    arl->arl_hw_addr_length);

		arl->arl_state = ARL_S_UP;
		ar_dlpi_done(arl, DL_BIND_REQ);
		break;
	case DL_NOTIFY_IND:
		DTRACE_PROBE2(rput_dl_notify_ind, arl_t *, arl,
		    dl_notify_ind_t *, &dlp->notify_ind);
		switch (dlp->notify_ind.dl_notification) {
		case DL_NOTE_LINK_UP:
			arl->arl_link_up = B_TRUE;
			ar_ce_walk(ar_ce_restart_dad, arl);
			break;
		case DL_NOTE_LINK_DOWN:
			arl->arl_link_up = B_FALSE;
			break;
		}
		break;
	case DL_UDERROR_IND:
		DTRACE_PROBE2(rput_dl_uderror, arl_t *, arl,
		    dl_uderror_ind_t *, &dlp->uderror_ind);
		(void) mi_strlog(q, 1, SL_ERROR | SL_TRACE,
		    "ar_rput_dlpi: "
		    "DL_UDERROR_IND, dl_dest_addr_length %d dl_errno %d",
		    dlp->uderror_ind.dl_dest_addr_length,
		    dlp->uderror_ind.dl_errno);
		putnext(q, mp);
		return;
	default:
		DTRACE_PROBE2(rput_dl_badprim, arl_t *, arl,
		    union DL_primitives *, dlp);
		putnext(q, mp);
		return;
	}
	freemsg(mp);
}

static void
ar_set_address(ace_t *ace, uchar_t *addrpos, uchar_t *proto_addr,
    uint32_t proto_addr_len)
{
	uchar_t	*mask, *to;
	int	len;

	ASSERT(ace->ace_hw_addr != NULL);

	bcopy(ace->ace_hw_addr, addrpos, ace->ace_hw_addr_length);
	if (ace->ace_flags & ACE_F_MAPPING &&
	    proto_addr != NULL &&
	    ace->ace_proto_extract_mask) {	/* careful */
		len = MIN((int)ace->ace_hw_addr_length
		    - ace->ace_hw_extract_start,
		    proto_addr_len);
		mask = ace->ace_proto_extract_mask;
		to = addrpos + ace->ace_hw_extract_start;
		while (len-- > 0)
			*to++ |= *mask++ & *proto_addr++;
	}
}

static int
ar_slifname(queue_t *q, mblk_t *mp_orig)
{
	ar_t	*ar = q->q_ptr;
	arl_t	*arl = ar->ar_arl;
	struct lifreq *lifr;
	mblk_t *mp = mp_orig;
	arl_t *old_arl;
	mblk_t *ioccpy;
	struct iocblk *iocp;
	hook_nic_event_t info;

	if (ar->ar_on_ill_stream) {
		/*
		 * This command is for IP, since it is coming down
		 * the <arp-IP-driver> stream. Return ENOENT so that
		 * it will be sent downstream by the caller
		 */
		return (ENOENT);
	}
	/* We handle both M_IOCTL and M_PROTO messages */
	if (DB_TYPE(mp) == M_IOCTL)
		mp = mp->b_cont;
	if (q->q_next == NULL || arl == NULL) {
		/*
		 * If the interface was just opened and
		 * the info ack has not yet come back from the driver
		 */
		DTRACE_PROBE2(slifname_no_arl, queue_t *, q,
		    mblk_t *, mp_orig);
		(void) putq(q, mp_orig);
		return (EINPROGRESS);
	}

	if (MBLKL(mp) < sizeof (struct lifreq)) {
		DTRACE_PROBE2(slifname_malformed, queue_t *, q,
		    mblk_t *, mp);
	}

	if (arl->arl_name[0] != '\0') {
		DTRACE_PROBE1(slifname_already, arl_t *, arl);
		return (EALREADY);
	}

	lifr = (struct lifreq *)mp->b_rptr;

	if (strlen(lifr->lifr_name) >= LIFNAMSIZ) {
		DTRACE_PROBE2(slifname_bad_name, arl_t *, arl,
		    struct lifreq *, lifr);
		return (ENXIO);
	}

	/* Check whether the name is already in use. */

	old_arl = ar_ll_lookup_by_name(lifr->lifr_name);
	if (old_arl != NULL) {
		DTRACE_PROBE2(slifname_exists, arl_t *, arl, arl_t *, old_arl);
		return (EEXIST);
	}

	/* Make a copy of the message so we can send it downstream. */
	if ((ioccpy = allocb(sizeof (struct iocblk), BPRI_MED)) == NULL ||
	    (ioccpy->b_cont = copymsg(mp)) == NULL) {
		if (ioccpy != NULL)
			freeb(ioccpy);
		return (ENOMEM);
	}

	(void) strlcpy(arl->arl_name, lifr->lifr_name, sizeof (arl->arl_name));

	/* The ppa is sent down by ifconfig */
	arl->arl_ppa = lifr->lifr_ppa;

	/*
	 * A network device is not considered to be fully plumb'd until
	 * its name has been set using SIOCSLIFNAME.  Once it has
	 * been set, it cannot be set again (see code above), so there
	 * is currently no danger in this function causing two NE_PLUMB
	 * events without an intervening NE_UNPLUMB.
	 */
	info.hne_nic = arl->arl_index;
	info.hne_lif = 0;
	info.hne_event = NE_PLUMB;
	info.hne_data = arl->arl_name;
	info.hne_datalen = strlen(arl->arl_name);
	(void) hook_run(arpnicevents, (hook_data_t)&info);

	/* Chain in the new arl. */
	rw_enter(&arl_g_lock, RW_WRITER);
	arl->arl_next = arl_g_head;
	arl_g_head = arl;
	DTRACE_PROBE1(slifname_set, arl_t *, arl);

	/*
	 * Send along a copy of the ioctl; this is just for hitbox.  Use
	 * M_CTL to avoid confusing anyone else who might be listening.
	 */
	DB_TYPE(ioccpy) = M_CTL;
	iocp = (struct iocblk *)ioccpy->b_rptr;
	bzero(iocp, sizeof (*iocp));
	iocp->ioc_cmd = SIOCSLIFNAME;
	iocp->ioc_count = msgsize(ioccpy->b_cont);
	ioccpy->b_wptr = (uchar_t *)(iocp + 1);
	putnext(arl->arl_wq, ioccpy);
	rw_exit(&arl_g_lock);

	return (0);
}

static int
ar_set_ppa(queue_t *q, mblk_t *mp_orig)
{
	ar_t	*ar = (ar_t *)q->q_ptr;
	arl_t	*arl = ar->ar_arl;
	int	ppa;
	char	*cp;
	mblk_t	*mp = mp_orig;
	arl_t	*old_arl;

	if (ar->ar_on_ill_stream) {
		/*
		 * This command is for IP, since it is coming down
		 * the <arp-IP-driver> stream. Return ENOENT so that
		 * it will be sent downstream by the caller
		 */
		return (ENOENT);
	}

	/* We handle both M_IOCTL and M_PROTO messages. */
	if (DB_TYPE(mp) == M_IOCTL)
		mp = mp->b_cont;
	if (q->q_next == NULL || arl == NULL) {
		/*
		 * If the interface was just opened and
		 * the info ack has not yet come back from the driver.
		 */
		DTRACE_PROBE2(setppa_no_arl, queue_t *, q,
		    mblk_t *, mp_orig);
		(void) putq(q, mp_orig);
		return (EINPROGRESS);
	}

	if (arl->arl_name[0] != '\0') {
		DTRACE_PROBE1(setppa_already, arl_t *, arl);
		return (EALREADY);
	}

	do {
		q = q->q_next;
	} while (q->q_next != NULL);
	cp = q->q_qinfo->qi_minfo->mi_idname;

	ppa = *(int *)(mp->b_rptr);
	(void) snprintf(arl->arl_name, sizeof (arl->arl_name), "%s%d", cp, ppa);

	old_arl = ar_ll_lookup_by_name(arl->arl_name);
	if (old_arl != NULL) {
		DTRACE_PROBE2(setppa_exists, arl_t *, arl, arl_t *, old_arl);
		/* Make it a null string again */
		arl->arl_name[0] = '\0';
		return (EBUSY);
	}

	arl->arl_ppa = ppa;
	DTRACE_PROBE1(setppa_done, arl_t *, arl);
	/* Chain in the new arl. */
	rw_enter(&arl_g_lock, RW_WRITER);
	arl->arl_next = arl_g_head;
	arl_g_head = arl;
	rw_exit(&arl_g_lock);

	return (0);
}

/*
 * create hash table for comparison.
 * The data recvd from IP is hashed on IP address for fast matching.
 */
static ar_snmp_hashb_t *
ar_create_snmp_hash(mblk_t *mpdata)
{
	int entries;
	mib2_ipNetToMediaEntry_t	*np;
	mblk_t		*mp1;
	ar_snmp_hashb_t  *hashb;
	ar_snmp_hashb_t  *start_entry;
	ar_snmp_hashb_t  *next_entry;
	ar_snmp_hashb_t  *ar_snmp_hash_tbl;

	entries = msgdsize(mpdata) / sizeof (mib2_ipNetToMediaEntry_t);

	ar_snmp_hash_tbl = (ar_snmp_hashb_t *)mi_zalloc(
	    (sizeof (ar_snmp_hashb_t) * (entries + ar_snmp_hash_size)));
	if (ar_snmp_hash_tbl == NULL)
		return (NULL);

	start_entry = ar_snmp_hash_tbl + ar_snmp_hash_size;

	np = NULL;
	mp1 = mpdata;
	next_entry = start_entry;
	while ((np = (mib2_ipNetToMediaEntry_t *)ar_snmp_msg_element(&mp1,
	    (uchar_t *)np, sizeof (mib2_ipNetToMediaEntry_t))) != NULL) {

		hashb = &ar_snmp_hash_tbl[IRE_ADDR_HASH
		    (np->ipNetToMediaNetAddress, ar_snmp_hash_size)];
		ASSERT(next_entry <= start_entry + entries);
		next_entry->ar_snmp_entry = np;
		next_entry->ar_next_entry = hashb->ar_next_entry;
		hashb->ar_next_entry = next_entry;

		next_entry++;
	}
	return (ar_snmp_hash_tbl);
}

static int
ar_snmp_msg(queue_t *q, mblk_t *mp_orig)
{
	mblk_t			*mpdata, *mp = mp_orig;
	struct opthdr		*optp;
	ar_snmp_hashb_t		*ar_snmp_hash_tbl;
	msg2_args_t		args;

	if (!mp)
		return (0);
	/*
	 * ar_cmd_dispatch() already checked for us that "mp->b_cont" is valid
	 * in case of an M_IOCTL message.
	 */
	if (DB_TYPE(mp) == M_IOCTL)
		mp = mp->b_cont;

	optp = (struct opthdr *)(&mp->b_rptr[sizeof (struct T_optmgmt_ack)]);
	if (optp->level != MIB2_IP || optp->name != MIB2_IP_MEDIA) {
		putnext(q, mp_orig);
		return (EINPROGRESS);
	}
	/*
	 * this is an ipNetToMediaTable msg from IP that needs (unique)
	 * arp cache entries appended...
	 */
	if ((mpdata = mp->b_cont) == NULL)
		return (EINVAL);

	ar_snmp_hash_tbl = ar_create_snmp_hash(mpdata);

	if (ar_snmp_hash_tbl != NULL) {
		args.m2a_hashb = ar_snmp_hash_tbl;
		args.m2a_mpdata = NULL;
		args.m2a_mptail = NULL;
		ar_ce_walk(ar_snmp_msg2, &args);

		mi_free(ar_snmp_hash_tbl);
		/*
		 * if a new entry was added link it with the list passed in.
		 */
		if (args.m2a_mpdata != NULL)
			linkb(mpdata, args.m2a_mpdata);
		optp->len = msgdsize(mpdata);
	}

	putnext(q, mp_orig);
	return (EINPROGRESS);	/* so that rput() exits doing nothing... */
}

static uchar_t *
ar_snmp_msg_element(mblk_t **mpp, uchar_t *oldptr, size_t len)
{
	mblk_t	*mp;

	mp = *mpp;
	if (!mp)
		return (NULL);
	if (oldptr)
		oldptr += len;
	else
		oldptr = mp->b_rptr;

	if (oldptr + len > mp->b_wptr) {
		mp = mp->b_cont;
		if (!mp)
			return (NULL);
		oldptr = mp->b_rptr;
		if (oldptr + len > mp->b_wptr)
			return (NULL);
	}
	*mpp = mp;
	return (oldptr);
}

static void
ar_snmp_msg2(ace_t *ace, void *arg)
{
	const char	*name = "unknown";
	mib2_ipNetToMediaEntry_t ntme;
	ar_snmp_hashb_t *hashb;
	mib2_ipNetToMediaEntry_t *np;
	msg2_args_t	*m2ap = arg;
	ar_snmp_hashb_t	*ar_snmp_hash_tbl;

	ASSERT(ace != NULL && ace->ace_arl != NULL);
	if (ace->ace_arl != NULL)
		name = ace->ace_arl->arl_name;
	ntme.ipNetToMediaIfIndex.o_length = MIN(OCTET_LENGTH, strlen(name));
	bcopy(name, ntme.ipNetToMediaIfIndex.o_bytes,
	    ntme.ipNetToMediaIfIndex.o_length);

	bcopy(ace->ace_proto_addr, &ntme.ipNetToMediaNetAddress,
	    MIN(sizeof (uint32_t), ace->ace_proto_addr_length));

	ntme.ipNetToMediaInfo.ntm_mask.o_length =
	    MIN(OCTET_LENGTH, ace->ace_proto_addr_length);
	bcopy(ace->ace_proto_mask, ntme.ipNetToMediaInfo.ntm_mask.o_bytes,
	    ntme.ipNetToMediaInfo.ntm_mask.o_length);

	ar_snmp_hash_tbl = m2ap->m2a_hashb;
	/*
	 * Append this arp entry only if not already there...
	 * if found, verify/modify ipNetToMediaType to agree with arp cache
	 * entry.
	 * entries within arp cache are unique, so match only with entries
	 * passed in.
	 */
	hashb = &ar_snmp_hash_tbl[IRE_ADDR_HASH(ntme.ipNetToMediaNetAddress,
	    ar_snmp_hash_size)];
	/*
	 * get the first entry.
	 */
	hashb = hashb->ar_next_entry;
	while (hashb != NULL) {
		ASSERT(hashb->ar_snmp_entry != NULL);
		np = hashb->ar_snmp_entry;
		if (np->ipNetToMediaNetAddress ==
		    ntme.ipNetToMediaNetAddress &&
		    np->ipNetToMediaInfo.ntm_mask.o_length ==
		    ntme.ipNetToMediaInfo.ntm_mask.o_length &&
		    (bcmp(np->ipNetToMediaInfo.ntm_mask.o_bytes,
		    ntme.ipNetToMediaInfo.ntm_mask.o_bytes,
		    ntme.ipNetToMediaInfo.ntm_mask.o_length) == 0) &&
		    (bcmp(np->ipNetToMediaIfIndex.o_bytes,
		    ntme.ipNetToMediaIfIndex.o_bytes,
		    ntme.ipNetToMediaIfIndex.o_length) == 0)) {
			if (ace->ace_flags & ACE_F_PERMANENT) {
				/* permanent arp entries are "static" */
				np->ipNetToMediaType = 4;
			}
			np->ipNetToMediaInfo.ntm_flags = ace->ace_flags;
			return;
		}
		hashb = hashb->ar_next_entry;
	}

	/*
	 * Allocate the first structure, the rest will be allocated
	 * by snmp_append_data.
	 */
	if (m2ap->m2a_mpdata == NULL) {
		m2ap->m2a_mpdata = allocb(sizeof (mib2_ipNetToMediaEntry_t),
		    BPRI_HI);
		if (m2ap->m2a_mpdata == NULL) {
			DTRACE_PROBE(snmp_allocb_failure);
			return;
		}
	}
	/*
	 * ace-> is a new entry to append
	 */
	ntme.ipNetToMediaPhysAddress.o_length =
	    MIN(OCTET_LENGTH, ace->ace_hw_addr_length);
	if ((ace->ace_flags & ACE_F_RESOLVED) == 0)
	    ntme.ipNetToMediaPhysAddress.o_length = 0;
	bcopy(ace->ace_hw_addr, ntme.ipNetToMediaPhysAddress.o_bytes,
	    ntme.ipNetToMediaPhysAddress.o_length);
	ntme.ipNetToMediaType = (ace->ace_flags & ACE_F_PERMANENT) ? 4 : 3;

	ntme.ipNetToMediaInfo.ntm_flags = ace->ace_flags;
	(void) snmp_append_data2(m2ap->m2a_mpdata, &m2ap->m2a_mptail,
	    (char *)&ntme, sizeof (ntme));
}

/* Write side put procedure. */
static void
ar_wput(queue_t *q, mblk_t *mp)
{
	int	err;
	struct iocblk	*ioc;
	mblk_t	*mp1;

	TRACE_1(TR_FAC_ARP, TR_ARP_WPUT_START,
	    "arp_wput_start: q %p", q);

	/*
	 * Here we handle ARP commands coming from controlling processes
	 * either in the form of M_IOCTL messages, or M_PROTO messages.
	 */
	switch (DB_TYPE(mp)) {
	case M_IOCTL:
		switch (err = ar_cmd_dispatch(q, mp)) {
		case ENOENT:
			/*
			 * If it is an I_PLINK, process it. Otherwise
			 * we don't recognize it, so pass it down.
			 * Since ARP is a module there is always someone
			 * below.
			 */
			ASSERT(q->q_next != NULL);
			ioc = (struct iocblk *)mp->b_rptr;
			if ((ioc->ioc_cmd != I_PLINK) &&
			    (ioc->ioc_cmd != I_PUNLINK)) {
				putnext(q, mp);
				TRACE_2(TR_FAC_ARP, TR_ARP_WPUT_END,
				    "arp_wput_end: q %p (%S)",
				    q, "ioctl/enoent");
				return;
			}
			err = ar_plink_send(q, mp);
			if (err == 0) {
				return;
			}
			if ((mp1 = mp->b_cont) != 0)
				mp1->b_wptr = mp1->b_rptr;
			break;
		case EINPROGRESS:
			/*
			 * If the request resulted in an attempt to resolve
			 * an address, we return out here.  The IOCTL will
			 * be completed in ar_rput if something comes back,
			 * or as a result of the timer expiring.
			 */
			TRACE_2(TR_FAC_ARP, TR_ARP_WPUT_END,
			    "arp_wput_end: q %p (%S)", q, "inprog");
			return;
		default:
			DB_TYPE(mp) = M_IOCACK;
			break;
		}
		ioc = (struct iocblk *)mp->b_rptr;
		if (err != 0)
			ioc->ioc_error = err;
		if (ioc->ioc_error != 0) {
			DB_TYPE(mp) = M_IOCNAK;
			freemsg(mp->b_cont);
			mp->b_cont = NULL;
		}
		ioc->ioc_count = msgdsize(mp->b_cont);
		qreply(q, mp);
		TRACE_2(TR_FAC_ARP, TR_ARP_WPUT_END,
		    "arp_wput_end: q %p (%S)", q, "ioctl");
		return;
	case M_FLUSH:
		if (*mp->b_rptr & FLUSHW)
			flushq(q, FLUSHDATA);
		if (*mp->b_rptr & FLUSHR) {
			flushq(RD(q), FLUSHDATA);
			*mp->b_rptr &= ~FLUSHW;
			qreply(q, mp);
			TRACE_2(TR_FAC_ARP, TR_ARP_WPUT_END,
			    "arp_wput_end: q %p (%S)", q, "flush");
			return;
		}
		/*
		 * The normal behaviour of a STREAMS module should be
		 * to pass down M_FLUSH messages. However there is a
		 * complex sequence of events during plumb/unplumb that
		 * can cause DLPI messages in the driver's queue to be
		 * flushed. So we don't send down M_FLUSH. This has been
		 * reported for some drivers (Eg. le) that send up an M_FLUSH
		 * in response to unbind request which will eventually be
		 * looped back at the mux head and sent down. Since IP
		 * does not queue messages in a module instance queue
		 * of IP, nothing is lost by not sending down the flush.
		 */
		freemsg(mp);
		return;
	case M_PROTO:
	case M_PCPROTO:
		/*
		 * Commands in the form of PROTO messages are handled very
		 * much the same as IOCTLs, but no response is returned.
		 */
		switch (err = ar_cmd_dispatch(q, mp)) {
		case ENOENT:
			if (q->q_next) {
				putnext(q, mp);
				TRACE_2(TR_FAC_ARP, TR_ARP_WPUT_END,
				    "arp_wput_end: q %p (%S)", q,
				    "proto/enoent");
				return;
			}
			break;
		case EINPROGRESS:
			TRACE_2(TR_FAC_ARP, TR_ARP_WPUT_END,
			    "arp_wput_end: q %p (%S)", q, "proto/einprog");
			return;
		default:
			break;
		}
		break;
	case M_IOCDATA:
		/*
		 * We pass M_IOCDATA downstream because it could be as a
		 * result of a previous M_COPYIN/M_COPYOUT message sent
		 * upstream.
		 */
		/* FALLTHRU */
	case M_CTL:
		/*
		 * We also send any M_CTL downstream as it could
		 * contain control information for a module downstream.
		 */
		putnext(q, mp);
		return;
	default:
		break;
	}
	/* Free any message we don't understand */
	freemsg(mp);
	TRACE_2(TR_FAC_ARP, TR_ARP_WPUT_END,
	    "arp_wput_end: q %p (%S)", q, "end");
}

static boolean_t
arp_say_ready(ace_t *ace)
{
	mblk_t *mp;
	arl_t *arl;
	arh_t *arh;
	uchar_t *cp;

	arl = ace->ace_arl;
	mp = allocb(sizeof (*arh) + 2 * (arl->arl_hw_addr_length +
	    ace->ace_proto_addr_length), BPRI_MED);
	if (mp == NULL) {
		/* skip a beat on allocation trouble */
		ace->ace_xmit_count = 1;
		ace_set_timer(ace, B_FALSE);
		return (B_FALSE);
	}
	/* Tell IP address is now usable */
	arh = (arh_t *)mp->b_rptr;
	U16_TO_BE16(arl->arl_arp_hw_type, arh->arh_hardware);
	U16_TO_BE16(ace->ace_proto, arh->arh_proto);
	arh->arh_hlen = arl->arl_hw_addr_length;
	arh->arh_plen = ace->ace_proto_addr_length;
	U16_TO_BE16(ARP_REQUEST, arh->arh_operation);
	cp = (uchar_t *)(arh + 1);
	bcopy(ace->ace_hw_addr, cp, arl->arl_hw_addr_length);
	cp += arl->arl_hw_addr_length;
	bcopy(ace->ace_proto_addr, cp, ace->ace_proto_addr_length);
	cp += ace->ace_proto_addr_length;
	bcopy(ace->ace_hw_addr, cp, arl->arl_hw_addr_length);
	cp += arl->arl_hw_addr_length;
	bcopy(ace->ace_proto_addr, cp, ace->ace_proto_addr_length);
	cp += ace->ace_proto_addr_length;
	mp->b_wptr = cp;
	ar_client_notify(arl, mp, AR_CN_READY);
	DTRACE_PROBE1(ready, ace_t *, ace);
	return (B_TRUE);
}

/*
 * Pick the longest-waiting aces for defense.
 */
static void
ace_reschedule(ace_t *ace, void *arg)
{
	ace_resched_t *art = arg;
	ace_t **aces;
	ace_t **acemax;
	ace_t *atemp;

	if (ace->ace_arl != art->art_arl)
		return;
	/*
	 * Only published entries that are ready for announcement are eligible.
	 */
	if ((ace->ace_flags & (ACE_F_PUBLISH | ACE_F_UNVERIFIED | ACE_F_DYING |
	    ACE_F_DELAYED)) != ACE_F_PUBLISH) {
		return;
	}
	if (art->art_naces < ACE_RESCHED_LIST_LEN) {
		art->art_aces[art->art_naces++] = ace;
	} else {
		aces = art->art_aces;
		acemax = aces + ACE_RESCHED_LIST_LEN;
		for (; aces < acemax; aces++) {
			if ((*aces)->ace_last_bcast > ace->ace_last_bcast) {
				atemp = *aces;
				*aces = ace;
				ace = atemp;
			}
		}
	}
}

/*
 * Reschedule the ARP defense of any long-waiting ACEs.  It's assumed that this
 * doesn't happen very often (if at all), and thus it needn't be highly
 * optimized.  (Note, though, that it's actually O(N) complexity, because the
 * outer loop is bounded by a constant rather than by the length of the list.)
 */
static void
arl_reschedule(arl_t *arl)
{
	ace_resched_t art;
	int i;
	ace_t *ace;

	i = arl->arl_defend_count;
	arl->arl_defend_count = 0;
	/* If none could be sitting around, then don't reschedule */
	if (i < arp_defend_rate) {
		DTRACE_PROBE1(reschedule_none, arl_t *, arl);
		return;
	}
	art.art_arl = arl;
	while (arl->arl_defend_count < arp_defend_rate) {
		art.art_naces = 0;
		ar_ce_walk(ace_reschedule, &art);
		for (i = 0; i < art.art_naces; i++) {
			ace = art.art_aces[i];
			ace->ace_flags |= ACE_F_DELAYED;
			ace_set_timer(ace, B_FALSE);
			if (++arl->arl_defend_count >= arp_defend_rate)
				break;
		}
		if (art.art_naces < ACE_RESCHED_LIST_LEN)
			break;
	}
	DTRACE_PROBE1(reschedule, arl_t *, arl);
}

/*
 * Write side service routine.	The only action here is delivery of transmit
 * timer events and delayed messages while waiting for the info_ack (ar_arl
 * not yet set).
 */
static void
ar_wsrv(queue_t *q)
{
	ace_t *ace;
	arl_t *arl;
	mblk_t *mp;
	clock_t	ms;

	TRACE_1(TR_FAC_ARP, TR_ARP_WSRV_START,
	    "arp_wsrv_start: q %p", q);

	while ((mp = getq(q)) != NULL) {
		switch (DB_TYPE(mp)) {
		case M_PCSIG:
			if (!mi_timer_valid(mp))
				continue;
			ace = (ace_t *)mp->b_rptr;
			if (ace->ace_flags & ACE_F_DYING)
				continue;
			arl = ace->ace_arl;
			if (ace->ace_flags & ACE_F_UNVERIFIED) {
				ASSERT(ace->ace_flags & ACE_F_PUBLISH);
				ASSERT(ace->ace_query_mp == NULL);
				/*
				 * If the link is down, give up for now.  IP
				 * will give us the go-ahead to try again when
				 * the link restarts.
				 */
				if (!arl->arl_link_up) {
					DTRACE_PROBE1(timer_link_down,
					    ace_t *, ace);
					ace->ace_flags |= ACE_F_DAD_ABORTED;
					continue;
				}
				if (ace->ace_xmit_count > 0) {
					DTRACE_PROBE1(timer_probe,
					    ace_t *, ace);
					ace->ace_xmit_count--;
					ar_xmit(arl, ARP_REQUEST,
					    ace->ace_proto,
					    ace->ace_proto_addr_length,
					    ace->ace_hw_addr, NULL, NULL,
					    ace->ace_proto_addr, NULL);
					ace_set_timer(ace, B_FALSE);
					continue;
				}
				if (!arp_say_ready(ace))
					continue;
				DTRACE_PROBE1(timer_ready, ace_t *, ace);
				ace->ace_xmit_interval = arp_publish_interval;
				ace->ace_xmit_count = arp_publish_count;
				if (ace->ace_xmit_count == 0)
					ace->ace_xmit_count++;
				ace->ace_flags &= ~ACE_F_UNVERIFIED;
			}
			if (ace->ace_flags & ACE_F_PUBLISH) {
				clock_t now;

				/*
				 * If an hour has passed, then free up the
				 * entries that need defense by rescheduling
				 * them.
				 */
				now = ddi_get_lbolt();
				if (arp_defend_rate > 0 &&
				    now - arl->arl_defend_start >
				    SEC_TO_TICK(arp_defend_period)) {
					arl->arl_defend_start = now;
					arl_reschedule(arl);
				}
				/*
				 * Finish the job that we started in
				 * ar_entry_add.  When we get to zero
				 * announcement retransmits left, switch to
				 * address defense.
				 */
				ASSERT(ace->ace_query_mp == NULL);
				if (ace->ace_xmit_count > 0) {
					ace->ace_xmit_count--;
					DTRACE_PROBE1(timer_announce,
					    ace_t *, ace);
				} else if (ace->ace_flags & ACE_F_DELAYED) {
					/*
					 * This guy was rescheduled as one of
					 * the really old entries needing
					 * on-going defense.  Let him through
					 * now.
					 */
					DTRACE_PROBE1(timer_send_delayed,
					    ace_t *, ace);
					ace->ace_flags &= ~ACE_F_DELAYED;
				} else if (arp_defend_rate > 0 &&
				    (arl->arl_defend_count >= arp_defend_rate ||
				    ++arl->arl_defend_count >=
				    arp_defend_rate)) {
					/*
					 * If we're no longer allowed to send
					 * unbidden defense messages, then just
					 * wait for rescheduling.
					 */
					DTRACE_PROBE1(timer_excess_defense,
					    ace_t *, ace);
					ace_set_timer(ace, B_FALSE);
					continue;
				} else {
					DTRACE_PROBE1(timer_defend,
					    ace_t *, ace);
				}
				ar_xmit(arl, ARP_REQUEST,
				    ace->ace_proto,
				    ace->ace_proto_addr_length,
				    ace->ace_hw_addr,
				    ace->ace_proto_addr,
				    arl->arl_arp_addr,
				    ace->ace_proto_addr, NULL);
				ace->ace_last_bcast = now;
				if (ace->ace_xmit_count == 0)
					ace->ace_xmit_interval =
					    arp_defend_interval;
				if (ace->ace_xmit_interval != 0)
					ace_set_timer(ace, B_FALSE);
				continue;
			}

			/*
			 * If this is a non-permanent (regular) resolved ARP
			 * entry, then it's now time to check if it can be
			 * retired.  As an optimization, we check with IP
			 * first, and just restart the timer if the address is
			 * still in use.
			 */
			if (ACE_NONPERM(ace)) {
				if (ace->ace_proto == IP_ARP_PROTO_TYPE &&
				    ndp_lookup_ipaddr(*(ipaddr_t *)
				    ace->ace_proto_addr)) {
					ace->ace_flags |= ACE_F_OLD;
					mi_timer(arl->arl_wq, ace->ace_mp,
					    arp_cleanup_interval);
				} else {
					ar_delete_notify(ace);
					ar_ce_delete(ace);
				}
				continue;
			}

			/*
			 * ar_query_xmit returns the number of milliseconds to
			 * wait following this transmit.  If the number of
			 * allowed transmissions has been exhausted, it will
			 * return zero without transmitting.  If that happens
			 * we complete the operation with a failure indication.
			 * Otherwise, we restart the timer.
			 */
			ASSERT(ace->ace_query_mp != NULL);
			ms = ar_query_xmit(ace, NULL);
			if (ms == 0)
				ar_query_reply(ace, ENXIO, NULL, (uint32_t)0);
			else
				mi_timer(q, mp, ms);
			continue;
		default:
			put(q, mp);
			continue;
		}
	}
	TRACE_1(TR_FAC_ARP, TR_ARP_WSRV_END,
	    "arp_wsrv_end: q %p", q);
}

/* ar_xmit is called to transmit an ARP Request or Response. */
static void
ar_xmit(arl_t *arl, uint32_t operation, uint32_t proto, uint32_t plen,
    const uchar_t *haddr1, const uchar_t *paddr1, const uchar_t *haddr2,
    const uchar_t *paddr2, const uchar_t *dstaddr)
{
	arh_t	*arh;
	uint8_t	*cp;
	uint_t	hlen;
	mblk_t	*mp;

	/* IFF_NOARP flag is set or interface down: do not send arp messages */
	if ((arl->arl_flags & ARL_F_NOARP) || !arl->arl_link_up)
		return;

	mp = arl->arl_xmit_template;
	if (mp == NULL || (mp = copyb(mp)) == NULL)
		return;
	hlen = arl->arl_hw_addr_length;
	mp->b_cont = allocb(AR_LL_HDR_SLACK + ARH_FIXED_LEN + (hlen * 4) +
	    plen + plen, BPRI_MED);
	if (mp->b_cont == NULL) {
		freeb(mp);
		return;
	}

	/* Get the L2 destination address for the message */
	if (haddr2 == NULL)
		dstaddr = arl->arl_arp_addr;
	else if (dstaddr == NULL)
		dstaddr = haddr2;

	/*
	 * Figure out where the target hardware address goes in the
	 * DL_UNITDATA_REQ header, and copy it in.
	 */
	cp = mi_offset_param(mp, arl->arl_xmit_template_addr_offset, hlen);
	ASSERT(cp != NULL);
	if (cp == NULL) {
		freemsg(mp);
		return;
	}
	bcopy(dstaddr, cp, hlen);

	/* Fill in the ARP header. */
	cp = mp->b_cont->b_rptr + (AR_LL_HDR_SLACK + hlen + hlen);
	mp->b_cont->b_rptr = cp;
	arh = (arh_t *)cp;
	U16_TO_BE16(arl->arl_arp_hw_type, arh->arh_hardware);
	U16_TO_BE16(proto, arh->arh_proto);
	arh->arh_hlen = (uint8_t)hlen;
	arh->arh_plen = (uint8_t)plen;
	U16_TO_BE16(operation, arh->arh_operation);
	cp += ARH_FIXED_LEN;
	bcopy(haddr1, cp, hlen);
	cp += hlen;
	if (paddr1 == NULL)
		bzero(cp, plen);
	else
		bcopy(paddr1, cp, plen);
	cp += plen;
	if (haddr2 == NULL)
		bzero(cp, hlen);
	else
		bcopy(haddr2, cp, hlen);
	cp += hlen;
	bcopy(paddr2, cp, plen);
	cp += plen;
	mp->b_cont->b_wptr = cp;

	DTRACE_PROBE3(arp__physical__out__start,
	    arl_t *, arl, arh_t *, arh, mblk_t *, mp);

	ARP_HOOK_OUT(arp_physical_out_event, arp_physical_out,
	    arl->arl_index, arh, mp, mp->b_cont);

	DTRACE_PROBE1(arp__physical__out__end, mblk_t *, mp);

	if (mp == NULL)
		return;

	/* Ship it out. */
	if (canputnext(arl->arl_wq))
		putnext(arl->arl_wq, mp);
	else
		freemsg(mp);
}

static mblk_t *
ar_alloc(uint32_t cmd, int err)
{
	uint32_t	len;
	mblk_t		*mp;
	mblk_t		*mp1;
	char		*cp;
	arc_t		*arc;

	/* For now only one type of command is accepted */
	if (cmd != AR_DLPIOP_DONE)
		return (NULL);
	len = sizeof (arc_t);
	mp = allocb(len, BPRI_HI);
	if (!mp)
		return (NULL);

	DB_TYPE(mp) = M_CTL;
	cp = (char *)mp->b_rptr;
	arc = (arc_t *)(mp->b_rptr);
	arc->arc_cmd = cmd;
	mp->b_wptr = (uchar_t *)&cp[len];
	len = sizeof (int);
	mp1 = allocb(len, BPRI_HI);
	if (!mp1) {
		freeb(mp);
		return (NULL);
	}
	cp = (char *)mp->b_rptr;
	/* Initialize the error code */
	*((int *)mp1->b_rptr) = err;
	mp1->b_wptr = (uchar_t *)&cp[len];
	linkb(mp, mp1);
	return (mp);
}
