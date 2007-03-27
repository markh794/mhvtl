/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_SCSI_ADAPTERS_VX_TAPEVAR_H
#define	_SYS_SCSI_ADAPTERS_VX_TAPEVAR_H

#pragma ident	"@(#)vx_tapevar.h	1.1	05/06/27 SMI"

#include <sys/avl.h>
#include <sys/note.h>
#include "vx_tape.h"

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Convenient short hand defines
 */
#define	TRUE			 1
#define	FALSE			 0
#define	UNDEFINED		-1

#define	CNUM(vx_tape)		(ddi_get_instance(vx_tape->vx_tape_tran.tran_dev))

#define	VX_TAPE_RETRY_DELAY		5
#define	VX_TAPE_RETRIES		0	/* retry of selections */
#define	VX_TAPE_INITIAL_SOFT_SPACE	5 /* Used for the softstate_init func */

#define	MSW(x)			(int16_t)(((int32_t)x >> 16) & 0xFFFF)
#define	LSW(x)			(int16_t)((int32_t)x & 0xFFFF)

#define	TGT(sp)			(CMD2PKT(sp)->pkt_address.a_target)
#define	LUN(sp)			(CMD2PKT(sp)->pkt_address.a_lun)

#define	HW_REV(val)		(((val) >>8) & 0xff)
#define	FW_REV(val)		((val) & 0xff)

/*
 * max number of LUNs per target
 */
#define	VX_TAPE_NLUNS_PER_TARGET	32

/*
 * Default vx_tape scsi-options
 */
#define	VX_TAPE_DEFAULT_SCSI_OPTIONS					\
					SCSI_OPTIONS_PARITY	|	\
					SCSI_OPTIONS_DR		|	\
					SCSI_OPTIONS_SYNC	|	\
					SCSI_OPTIONS_TAG	|	\
					SCSI_OPTIONS_FAST	|	\
					SCSI_OPTIONS_WIDE

/*
 *	Tag reject
 */
#define	TAG_REJECT	28
/*
 * Interrupt actions returned by vx_tape_i_flag_event()
 */
#define	ACTION_CONTINUE		0	/* Continue */
#define	ACTION_RETURN		1	/* Exit */
#define	ACTION_IGNORE		2	/* Ignore */

/*
 * Reset actions for vx_tape_i_reset_interface()
 */
#define	VX_TAPE_RESET_BUS_IF_BUSY	0x01 /* reset scsi bus if it is busy */
#define	VX_TAPE_FORCE_RESET_BUS	0x02	/* reset scsi bus on error reco */


/*
 * extracting period and offset from vx_tape_synch
 */
#define	PERIOD_MASK(val)	((val) & 0xff)
#define	OFFSET_MASK(val)	(((val) >>8) & 0xff)

/*
 * timeout values
 */
#define	VX_TAPE_GRACE		10	/* Timeout margin (sec.) */
#define	VX_TAPE_TIMEOUT_DELAY(secs, delay)	(secs * (1000000 / delay))

/*
 * delay time for polling loops
 */
#define	VX_TAPE_NOINTR_POLL_DELAY_TIME		1000	/* usecs */

/*
 * busy wait delay time after chip reset
 */
#define	VX_TAPE_CHIP_RESET_BUSY_WAIT_TIME		100	/* usecs */

/*
 * timeout for VX_TAPE coming out of reset
 */
#define	VX_TAPE_RESET_WAIT				1000	/* ms */
#define	VX_TAPE_SOFT_RESET_TIME			1	/* second */

/*
 * vx_tape_softstate flags for introducing hot plug
 */
#define	VX_TAPE_SS_OPEN		0x01
#define	VX_TAPE_SS_DRAINING		0x02
#define	VX_TAPE_SS_QUIESCED		0x04
#define	VX_TAPE_SS_DRAIN_ERROR	0x08

/*
 * ioctl command definitions
 */
#ifndef	VX_TAPE_RESET_TARGET
#define	VX_TAPE_RESET_TARGET		(('i' << 8) | 0x03)
#endif


/*
 * Debugging macros
 */
#define	VX_TAPE_DEBUG	if (vx_tapedebug) vx_tape_i_log
#define	VX_TAPE_DEBUG2	if (vx_tapedebug > 1) vx_tape_i_log


#define	REQ_TGT_LUN(tgt, lun)			(((tgt) << 8) | (lun))


#define	RESP_CQ_FLAGS(resp)	((resp->resp_header.cq_flags_seq) & 0xff)


#define	VX_TAPE_NDATASEGS		4


/*
 * translate scsi_pkt flags into VX_TAPE request packet flags
 * It would be illegal if two flags are set; the driver does not
 * check for this. Setting NODISCON and a tag flag is harmless.
 */
#define	VX_TAPE_SET_PKT_FLAGS(scsa_flags, vx_tape_flags) {		\
	vx_tape_flags = (scsa_flags >> 11) & 0xe; /* tags */		\
	vx_tape_flags |= (scsa_flags >> 1) & 0x1; /* no disconnect */	\
}

/*
 * throttle values for VX_TAPE request queue
 */
#define	SHUTDOWN_THROTTLE	-1	/* do not submit any requests */
#define	CLEAR_THROTTLE		(VX_TAPE_MAX_REQUESTS -1)


#define	VX_TAPE_GET_PKT_STATE(state)	((uint32_t)(state >> 8))
#define	VX_TAPE_GET_PKT_STATS(stats)	((uint32_t)(stats))

#define	VX_TAPE_STAT_NEGOTIATE	0x0080

#define	VX_TAPE_SET_REASON(sp, reason) { \
	if ((sp) && CMD2PKT(sp)->pkt_reason == CMD_CMPLT) \
		CMD2PKT(sp)->pkt_reason = (reason); \
}

/*
 * mutex short hands
 */
#define	VX_TAPE_REQ_MUTEX(vx_tape)	(&vx_tape->vx_tape_request_mutex)
#define	VX_TAPE_RESP_MUTEX(vx_tape)	(&vx_tape->vx_tape_response_mutex)
#define	VX_TAPE_HOTPLUG_MUTEX(vx_tape)	(&vx_tape->vx_tape_hotplug_mutex)


#define	VX_TAPE_MUTEX_ENTER(vx_tape) mutex_enter(VX_TAPE_RESP_MUTEX(vx_tape)), \
				mutex_enter(VX_TAPE_REQ_MUTEX(vx_tape))
#define	VX_TAPE_MUTEX_EXIT(vx_tape)	mutex_exit(VX_TAPE_RESP_MUTEX(vx_tape)), \
				mutex_exit(VX_TAPE_REQ_MUTEX(vx_tape))

#define	VX_TAPE_CV(vx_tape)			(&(vx_tape)->vx_tape_cv)

/*
 * HBA interface macros
 */
#define	SDEV2TRAN(sd)		((sd)->sd_address.a_hba_tran)
#define	SDEV2ADDR(sd)		(&((sd)->sd_address))
#define	PKT2TRAN(pkt)		((pkt)->pkt_address.a_hba_tran)
#define	ADDR2TRAN(ap)		((ap)->a_hba_tran)

#define	TRAN2VX_TAPE(tran)	((struct vx_tape *)(tran)->tran_hba_private)
#define	SDEV2VX_TAPE(sd)		(TRAN2VX_TAPE(SDEV2TRAN(sd)))
#define	PKT2VX_TAPE(pkt)		(TRAN2VX_TAPE(PKT2TRAN(pkt)))
#define	ADDR2VX_TAPE(ap)		(TRAN2VX_TAPE(ADDR2TRAN(ap)))

#define	CMD2ADDR(cmd)		(&CMD2PKT(cmd)->pkt_address)
#define	CMD2TRAN(cmd)		(CMD2PKT(cmd)->pkt_address.a_hba_tran)
#define	CMD2VX_TAPE(cmd)		(TRAN2VX_TAPE(CMD2TRAN(cmd)))

/*
 * Results of checking for range overlap.
 */
typedef enum vx_tape_rng_overlap {
	O_NONE,			/* No overlap */
	O_SAME,			/* Ranges are identical */
	O_SUBSET,		/* Blocks are contained in range */
	O_OVERLAP		/* Ranges overlap. */
} vx_tape_rng_overlap_t;

/*
 * Rather than keep the entire image of the disk, we only keep
 * the blocks which have been written with non-zeros.  As the
 * purpose of this driver is to exercise format and perhaps other
 * large-disk management tools, only recording the label for
 * i/o is sufficient
 */
typedef struct blklist {
	diskaddr_t	bl_blkno;	/* Disk address of the data */
	uchar_t		*bl_data;	/* Pointer to the data */
	avl_node_t	bl_node;	/* Our linkage in AVL tree */
} blklist_t;

/*
 * Structure to track a range of blocks where writes are to be ignored.
 */
typedef struct vx_tape_nowrite {
	struct vx_tape_nowrite	*vx_tape_nwnext;	/* next item in list */
	vx_tape_range_t		vx_tape_blocked;	/* range to ignore writes */
} vx_tape_nowrite_t;

typedef struct vx_tape_tgt {
	struct scsi_address	vx_tape_tgt_saddr;
	struct vx_tape_tgt	*vx_tape_tgt_next;	/* Next tgt on ctlr */
	vx_tape_nowrite_t	*vx_tape_tgt_nowrite;	/* List of regions to */
							/* skip writes */
	diskaddr_t		vx_tape_tgt_sectors;	/* # sectors in dev */
	char 			vx_tape_tgt_inq[8+16];
	uint_t			vx_tape_tgt_dtype;
	uint_t			vx_tape_tgt_ncyls;	/* # cylinders in dev */
	uint_t			vx_tape_tgt_nheads;	/* # disk heads */
	uint_t			vx_tape_tgt_nsect;	/* # sectors */
	uint64_t		vx_tape_list_length;	/* # data blks */
	avl_tree_t		vx_tape_tgt_data;	/* Tree of data blks */
	kmutex_t		vx_tape_tgt_blk_lock;	/* Protect data blks */
	krwlock_t		vx_tape_tgt_nw_lock;	/* Guard tgt_nowrite */
} vx_tape_tgt_t;

/*
 * vx_tape softstate structure
 */

/*
 * deadline slot structure for timeout handling
 */
struct vx_tape_slot {
	struct vx_tape_cmd	*slot_cmd;
	clock_t		slot_deadline;
};


/*
 * Record the reset notification requests from target drivers.
 */
struct vx_tape_reset_notify_entry {
	struct scsi_address		*ap;
	void				(*callback)(caddr_t);
	caddr_t				arg;
	struct vx_tape_reset_notify_entry	*next;
};


struct vx_tape {

	/*
	 * Transport structure for this instance of the hba
	 */
	scsi_hba_tran_t		*vx_tape_tran;

	/*
	 * dev_info_t reference can be found in the transport structure
	 */
	dev_info_t		*vx_tape_dip;

	/*
	 * Interrupt block cookie
	 */
	ddi_iblock_cookie_t	vx_tape_iblock;

	/*
	 * Firmware revision number
	 */
	uint16_t		vx_tape_major_rev;
	uint16_t		vx_tape_minor_rev;

	/*
	 * timeout id
	 */
	timeout_id_t		vx_tape_timeout_id;

	/*
	 * scsi options, scsi_tag_age_limit  per vx_tape
	 */
	int			vx_tape_scsi_options;
	int			vx_tape_target_scsi_options[NTARGETS_WIDE];
	int			vx_tape_scsi_tag_age_limit;

	/*
	 * scsi_reset_delay per vx_tape
	 */
	clock_t			vx_tape_scsi_reset_delay;

	/*
	 * current host ID
	 */
	uint8_t			vx_tape_initiator_id;

	/*
	 * suspended flag for power management
	 */
	uint8_t			vx_tape_suspended;

	/*
	 * Host adapter capabilities and offset/period values per target
	 */
	uint16_t		vx_tape_cap[NTARGETS_WIDE];
	int16_t			vx_tape_synch[NTARGETS_WIDE];

	/*
	 * VX_TAPE Hardware register pointer.
	 */
	struct vx_taperegs		*vx_tape_reg;


	kmutex_t		vx_tape_request_mutex;
	kmutex_t		vx_tape_response_mutex;

	/*
	 * for keeping track of the max LUNs per target on this bus
	 */
	uchar_t			vx_tape_max_lun[NTARGETS_WIDE];

	/*
	 * for keeping track of each target/lun
	 */
	int	nt_total_sectors[NTARGETS_WIDE][VX_TAPE_NLUNS_PER_TARGET];

	struct vx_tape_reset_notify_entry	*vx_tape_reset_notify_listf;

	ushort_t		vx_tape_backoff;
	uint_t			vx_tape_softstate; /* flags for hotplug */
	int			vx_tape_hotplug_waiting;
	kcondvar_t		vx_tape_cv; /* cv for bus quiesce/unquiesce */
	kmutex_t		vx_tape_hotplug_mutex; /* Mutex for hotplug */
	taskq_t			*vx_tape_taskq;
	vx_tape_tgt_t		*vx_tape_tgt;
};

_NOTE(MUTEX_PROTECTS_DATA(vx_tape::vx_tape_request_mutex,
				vx_tape::vx_tape_queue_space))
_NOTE(MUTEX_PROTECTS_DATA(vx_tape::vx_tape_request_mutex,
				vx_tape::vx_tape_request_in))
_NOTE(MUTEX_PROTECTS_DATA(vx_tape::vx_tape_request_mutex,
				vx_tape::vx_tape_request_out))
_NOTE(MUTEX_PROTECTS_DATA(vx_tape::vx_tape_request_mutex,
				vx_tape::vx_tape_request_ptr))
_NOTE(MUTEX_PROTECTS_DATA(vx_tape::vx_tape_request_mutex,
				vx_tape::vx_tape_mbox))
_NOTE(MUTEX_PROTECTS_DATA(vx_tape::vx_tape_request_mutex,
				vx_tape::vx_tape_slots))

_NOTE(MUTEX_PROTECTS_DATA(vx_tape::vx_tape_response_mutex,
				vx_tape::vx_tape_response_in))
_NOTE(MUTEX_PROTECTS_DATA(vx_tape::vx_tape_response_mutex,
				vx_tape::vx_tape_response_out))
_NOTE(MUTEX_PROTECTS_DATA(vx_tape::vx_tape_response_mutex,
				vx_tape::vx_tape_response_ptr))

extern void vx_tape_bsd_init();
extern void vx_tape_bsd_fini();
extern void vx_tape_bsd_get_props(dev_info_t *);

extern vx_tape_rng_overlap_t vx_tape_overlap(vx_tape_range_t *,
						diskaddr_t, size_t);
extern int vx_tape_bsd_blkcompare(const void *, const void *);
extern int vx_tapedebug;
extern long vx_tape_nowrite_count;
extern kmutex_t vx_tape_stats_mutex;
extern int vx_tape_collect_stats;
extern uint64_t vx_tape_taskq_max;
extern int vx_tape_max_task;
extern int vx_tape_task_nthreads;

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_SCSI_ADAPTERS_VX_TAPEVAR_H */
