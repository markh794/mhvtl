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

#pragma ident	"@(#) $Id: vx_tape.c,v 1.5.2.1 2006-08-06 07:59:05 markh Exp $"

/*
 * SCSA HBA nexus driver that emulates an HBA connected to SCSI target
 * devices.
 *
 * Modified/hacked to pass SCSI cmds thru to userspace daemons via a
 * char device 'backend'. The userspace daemons will respond to the
 * SCSI commands as the (currently SSC & SMC) target devices.
 *
 * markh794 at gmail dot com
 * mark_harvey at symantec dot com
 */

#ifdef DEBUG
#define	VX_TAPEDEBUG 1
#endif

#include <sys/scsi/scsi.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/taskq.h>
#include <sys/disp.h>
#include <sys/types.h>
#include <sys/buf.h>
#include <sys/cpuvar.h>
#include <sys/dklabel.h>

#include "vx_tape.h"
#include "vx_tapecmd.h"
#include "vx_tapevar.h"

int vx_tape_usetaskq	= 1;	/* set to zero for debugging */
int vx_tapedebug		= 1;
#ifdef	VX_TAPEDEBUG
static int vx_tape_cdb_debug	= 1;
#include <sys/debug.h>
#endif

/*
 * cb_ops function prototypes
 */
static int vx_tape_ioctl(dev_t, int cmd, intptr_t arg, int mode,
			cred_t *credp, int *rvalp);
static int vx_tape_c_read(dev_t, struct uio *, cred_t *);
static int vx_tape_c_write(dev_t, struct uio *, cred_t *);

/*
 * dev_ops functions prototypes
 */
static int vx_tape_info(dev_info_t *dip, ddi_info_cmd_t infocmd,
    void *arg, void **result);
static int vx_tape_attach(dev_info_t *dip, ddi_attach_cmd_t cmd);
static int vx_tape_detach(dev_info_t *dip, ddi_detach_cmd_t cmd);

/*
 * Function prototypes
 *
 * SCSA functions exported by means of the transport table
 */
static int vx_tape_tran_tgt_init(dev_info_t *hba_dip, dev_info_t *tgt_dip,
	scsi_hba_tran_t *tran, struct scsi_device *sd);
static int vx_tape_scsi_start(struct scsi_address *ap, struct scsi_pkt *pkt);
static void vx_tape_pkt_comp(void *);
static int vx_tape_scsi_abort(struct scsi_address *ap, struct scsi_pkt *pkt);
static int vx_tape_scsi_reset(struct scsi_address *ap, int level);
static int vx_tape_scsi_getcap(struct scsi_address *ap, char *cap, int whom);
static int vx_tape_scsi_setcap(struct scsi_address *ap, char *cap, int value,
    int whom);
static struct scsi_pkt *vx_tape_scsi_init_pkt(struct scsi_address *ap,
    struct scsi_pkt *pkt, struct buf *bp, int cmdlen, int statuslen,
    int tgtlen, int flags, int (*callback)(), caddr_t arg);
static void vx_tape_scsi_destroy_pkt(struct scsi_address *ap,
					struct scsi_pkt *pkt);
static void vx_tape_scsi_dmafree(struct scsi_address *ap, struct scsi_pkt *pkt);
static void vx_tape_scsi_sync_pkt(struct scsi_address *ap, struct scsi_pkt *pkt);
static int vx_tape_scsi_reset_notify(struct scsi_address *ap, int flag,
    void (*callback)(caddr_t), caddr_t arg);

/*
 * internal functions
 */
static void vx_tape_i_initcap(struct vx_tape *vx_tape);

static void vx_tape_i_log(struct vx_tape *vx_tape, int level, char *fmt, ...);
static int vx_tape_get_tgtrange(struct vx_tape *,
				intptr_t,
				vx_tape_tgt_t **,
				vx_tape_tgt_range_t *);
static int vx_tape_write_off(struct vx_tape *,
			    vx_tape_tgt_t *,
			    vx_tape_tgt_range_t *);
static int vx_tape_write_on(struct vx_tape *,
				vx_tape_tgt_t *,
				vx_tape_tgt_range_t *);
static vx_tape_nowrite_t *vx_tape_nowrite_alloc(vx_tape_range_t *);
static void vx_tape_nowrite_free(vx_tape_nowrite_t *);
static vx_tape_nowrite_t *vx_tape_find_nowrite(vx_tape_tgt_t *,
					diskaddr_t start_block,
					size_t blkcnt,
					vx_tape_rng_overlap_t *overlapp,
					vx_tape_nowrite_t ***prevp);

extern vx_tape_tgt_t *find_tgt(struct vx_tape *, ushort_t, ushort_t);

#ifdef VX_TAPEDEBUG
static void vx_tape_debug_dump_cdb(struct scsi_address *ap,
		struct scsi_pkt *pkt);
#endif


#ifdef	_DDICT
static int	ddi_in_panic(void);
static int	ddi_in_panic() { return (0); }
#ifndef	SCSI_CAP_RESET_NOTIFICATION
#define	SCSI_CAP_RESET_NOTIFICATION		14
#endif
#ifndef	SCSI_RESET_NOTIFY
#define	SCSI_RESET_NOTIFY			0x01
#endif
#ifndef	SCSI_RESET_CANCEL
#define	SCSI_RESET_CANCEL			0x02
#endif
#endif

/*
 * Tunables:
 *
 * vx_tape_max_task
 *	The taskq facility is used to queue up SCSI start requests on a per
 *	controller basis.  If the maximum number of queued tasks is hit,
 *	taskq_ent_alloc() delays for a second, which adversely impacts our
 *	performance.  This value establishes the maximum number of task
 *	queue entries when taskq_create is called.
 *
 * vx_tape_task_nthreads
 *	Specifies the number of threads that should be used to process a
 *	controller's task queue.  Our init function sets this to the number
 *	of CPUs on the system, but this can be overridden in vx_tape.conf.
 */
int vx_tape_max_task = 16;
int vx_tape_task_nthreads = 1;

/*
 * Local static data
 */
static void		*vx_tape_state = NULL;

/*
 * Character/block operations.
 */
static struct cb_ops vx_tape_cbops = {
	scsi_hba_open,		/* cb_open */
	scsi_hba_close,		/* cb_close */
	nodev,			/* cb_strategy */
	nodev,			/* cb_print */
	nodev,			/* cb_dump */
	vx_tape_c_read,		/* cb_read */
	vx_tape_c_write,	/* cb_write */
	vx_tape_ioctl,		/* cb_ioctl */
	nodev,			/* cb_devmap */
	nodev,			/* cb_mmap */
	nodev,			/* cb_segmap */
	nochpoll,		/* cb_chpoll */
	ddi_prop_op,		/* cb_prop_op */
	NULL,			/* cb_str */
	D_MP | D_64BIT | D_HOTPLUG, /* cb_flag */
	CB_REV,			/* cb_rev */
	nodev,			/* cb_aread */
	nodev			/* cb_awrite */
};

/*
 * autoconfiguration routines.
 */

static struct dev_ops vx_tape_ops = {
	DEVO_REV,			/* rev, */
	0,				/* refcnt */
	vx_tape_info,			/* getinfo */
	nulldev,			/* identify */
	nulldev,			/* probe */
	vx_tape_attach,			/* attach */
	vx_tape_detach,			/* detach */
	nodev,				/* reset */
	&vx_tape_cbops,			/* char/block ops */
	NULL				/* bus ops */
};

char _depends_on[] = "misc/scsi";

static struct modldrv modldrv = {
	&mod_driverops,			/* module type - driver */
	"vx_tape SCSI Host Bus Adapter",	/* module name */
	&vx_tape_ops,			/* driver ops */
};

static struct modlinkage modlinkage = {
	MODREV_1,			/* ml_rev - must be MODREV_1 */
	&modldrv,			/* ml_linkage */
	NULL				/* end of driver linkage */
};

int
_init(void)
{
	int	ret;

	ret = ddi_soft_state_init(&vx_tape_state, sizeof (struct vx_tape),
	    VX_TAPE_INITIAL_SOFT_SPACE);
	if (ret != 0)
		return (ret);

	if ((ret = scsi_hba_init(&modlinkage)) != 0) {
		ddi_soft_state_fini(&vx_tape_state);
		return (ret);
	}

	cmn_err(CE_NOTE, "completed scsi_hba_init()");

	/* Set the number of task threads to the number of CPUs */
	if (boot_max_ncpus == -1) {
		vx_tape_task_nthreads = max_ncpus;
	} else {
		vx_tape_task_nthreads = boot_max_ncpus;
	}

	vx_tape_bsd_init();
	cmn_err(CE_NOTE, "completed vx_tape_bsd_init()");

	ret = mod_install(&modlinkage);
	if (ret != 0) {
		vx_tape_bsd_fini();
		scsi_hba_fini(&modlinkage);
		ddi_soft_state_fini(&vx_tape_state);
	}

	return (ret);
}

int
_fini(void)
{
	int	ret;

	if ((ret = mod_remove(&modlinkage)) != 0)
		return (ret);

	vx_tape_bsd_fini();

	scsi_hba_fini(&modlinkage);

	ddi_soft_state_fini(&vx_tape_state);

	return (ret);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

/*
 * Given the device number return the devinfo pointer
 * from the scsi_device structure.
 */
/*ARGSUSED*/
static int
vx_tape_info(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
	struct vx_tape	*foo;
	int		instance = getminor((dev_t)arg);

	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		foo = ddi_get_soft_state(vx_tape_state, instance);
		if (foo != NULL)
			*result = (void *)foo->vx_tape_dip;
		else {
			*result = NULL;
			return (DDI_FAILURE);
		}
		break;

	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)(uintptr_t)instance;
		break;

	default:
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

/*
 * Attach an instance of an vx_tape host adapter.  Allocate data structures,
 * initialize the vx_tape and we're on the air.
 */
/*ARGSUSED*/
static int
vx_tape_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	int		mutex_initted = 0;
	struct vx_tape	*vx_tape;
	int		instance;
	scsi_hba_tran_t	*tran = NULL;
	ddi_dma_attr_t	tmp_dma_attr;
	int		node_no;
	char		name[8];

	vx_tape_bsd_get_props(dip);

	bzero((void *) &tmp_dma_attr, sizeof (tmp_dma_attr));
	instance = ddi_get_instance(dip);

	switch (cmd) {
	case DDI_ATTACH:
		for(node_no = 0; node_no < 10; node_no++) {
			(void) sprintf(name, "vx%d", node_no);
			if(ddi_create_minor_node(dip, name, S_IFCHR,
						 node_no + 8, DDI_PSEUDO, 0))
			return(DDI_FAILURE);
		}
		break;

	case DDI_RESUME:
		tran = (scsi_hba_tran_t *)ddi_get_driver_private(dip);
		if (!tran) {
			return (DDI_FAILURE);
		}
		vx_tape = TRAN2VX_TAPE(tran);

		return (DDI_SUCCESS);

	default:
		vx_tape_i_log(NULL, CE_WARN,
		    "vx_tape%d: Cmd != DDI_ATTACH/DDI_RESUME", instance);
		return (DDI_FAILURE);
	}

	/*
	 * Allocate vx_tape data structure.
	 */
	if (ddi_soft_state_zalloc(vx_tape_state, instance) != DDI_SUCCESS) {
		vx_tape_i_log(NULL, CE_WARN,
			"vx_tape%d: Failed to alloc soft state",
			instance);
		return (DDI_FAILURE);
	}

	vx_tape = (struct vx_tape *)ddi_get_soft_state(vx_tape_state, instance);
	if (vx_tape == (struct vx_tape *)NULL) {
		vx_tape_i_log(NULL, CE_WARN, "vx_tape%d: Bad soft state",
			instance);
		ddi_soft_state_free(vx_tape_state, instance);
		return (DDI_FAILURE);
	}


	/*
	 * Allocate a transport structure
	 */
	tran = scsi_hba_tran_alloc(dip, SCSI_HBA_CANSLEEP);
	if (tran == NULL) {
		cmn_err(CE_WARN, "vx_tape: scsi_hba_tran_alloc failed\n");
		goto fail;
	}

	vx_tape->vx_tape_tran			= tran;
	vx_tape->vx_tape_dip			= dip;

	tran->tran_hba_private		= vx_tape;
	tran->tran_tgt_private		= NULL;
	tran->tran_tgt_init		= vx_tape_tran_tgt_init;
	tran->tran_tgt_probe		= scsi_hba_probe;
	tran->tran_tgt_free		= NULL;

	tran->tran_start		= vx_tape_scsi_start;
	tran->tran_abort		= vx_tape_scsi_abort;
	tran->tran_reset		= vx_tape_scsi_reset;
	tran->tran_getcap		= vx_tape_scsi_getcap;
	tran->tran_setcap		= vx_tape_scsi_setcap;
	tran->tran_init_pkt		= vx_tape_scsi_init_pkt;
	tran->tran_destroy_pkt		= vx_tape_scsi_destroy_pkt;
	tran->tran_dmafree		= vx_tape_scsi_dmafree;
	tran->tran_sync_pkt		= vx_tape_scsi_sync_pkt;
	tran->tran_reset_notify 	= vx_tape_scsi_reset_notify;

	tmp_dma_attr.dma_attr_minxfer = 0x1;
	tmp_dma_attr.dma_attr_burstsizes = 0x7f;

	/*
	 * Attach this instance of the hba
	 */
	if (scsi_hba_attach_setup(dip, &tmp_dma_attr, tran,
	    0) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "vx_tape: scsi_hba_attach failed\n");
		goto fail;
	}

	vx_tape->vx_tape_initiator_id = 2;

	/*
	 * Look up the scsi-options property
	 */
	vx_tape->vx_tape_scsi_options =
		ddi_prop_get_int(DDI_DEV_T_ANY, dip, 0, "scsi-options",
		    VX_TAPE_DEFAULT_SCSI_OPTIONS);
	VX_TAPE_DEBUG(vx_tape, SCSI_DEBUG, "vx_tape scsi-options=%x",
	    vx_tape->vx_tape_scsi_options);


	/* mutexes to protect the vx_tape request and response queue */
	mutex_init(VX_TAPE_REQ_MUTEX(vx_tape), NULL, MUTEX_DRIVER,
	    vx_tape->vx_tape_iblock);
	mutex_init(VX_TAPE_RESP_MUTEX(vx_tape), NULL, MUTEX_DRIVER,
	    vx_tape->vx_tape_iblock);

	mutex_initted = 1;

	VX_TAPE_MUTEX_ENTER(vx_tape);

	/*
	 * Initialize the default Target Capabilities and Sync Rates
	 */
	vx_tape_i_initcap(vx_tape);

	VX_TAPE_MUTEX_EXIT(vx_tape);


	ddi_report_dev(dip);
	vx_tape->vx_tape_taskq = taskq_create("vx_tape_comp",
		vx_tape_task_nthreads, MINCLSYSPRI, 1, vx_tape_max_task, 0);

	cmn_err(CE_NOTE, "vx_tape exiting vx_tape_attach()");

	return (DDI_SUCCESS);

fail:
	vx_tape_i_log(NULL, CE_WARN, "vx_tape%d: Unable to attach", instance);

	if (mutex_initted) {
		mutex_destroy(VX_TAPE_REQ_MUTEX(vx_tape));
		mutex_destroy(VX_TAPE_RESP_MUTEX(vx_tape));
	}
	if (tran) {
		scsi_hba_tran_free(tran);
	}
	ddi_soft_state_free(vx_tape_state, instance);
	return (DDI_FAILURE);
}

/*ARGSUSED*/
static int
vx_tape_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	struct vx_tape	*vx_tape;
	scsi_hba_tran_t	*tran;
	int		instance = ddi_get_instance(dip);


	/* get transport structure pointer from the dip */
	if (!(tran = (scsi_hba_tran_t *)ddi_get_driver_private(dip))) {
		return (DDI_FAILURE);
	}

	/* get soft state from transport structure */
	vx_tape = TRAN2VX_TAPE(tran);

	if (!vx_tape) {
		return (DDI_FAILURE);
	}

	VX_TAPE_DEBUG(vx_tape, SCSI_DEBUG, "vx_tape_detach: cmd = %d", cmd);

	switch (cmd) {
	case DDI_DETACH:
		VX_TAPE_MUTEX_ENTER(vx_tape);

		taskq_destroy(vx_tape->vx_tape_taskq);
		(void) scsi_hba_detach(dip);

		scsi_hba_tran_free(vx_tape->vx_tape_tran);


		VX_TAPE_MUTEX_EXIT(vx_tape);

		mutex_destroy(VX_TAPE_REQ_MUTEX(vx_tape));
		mutex_destroy(VX_TAPE_RESP_MUTEX(vx_tape));


		VX_TAPE_DEBUG(vx_tape, SCSI_DEBUG, "vx_tape_detach: done");
		ddi_soft_state_free(vx_tape_state, instance);

		ddi_remove_minor_node(dip, NULL);

		return (DDI_SUCCESS);

	case DDI_SUSPEND:
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}
}

/*
 * Function name : vx_tape_tran_tgt_init
 *
 * Return Values : DDI_SUCCESS if target supported, DDI_FAILURE otherwise
 *
 */
/*ARGSUSED*/
static int
vx_tape_tran_tgt_init(dev_info_t *hba_dip, dev_info_t *tgt_dip,
	scsi_hba_tran_t *tran, struct scsi_device *sd)
{
	struct vx_tape	*vx_tape;
	vx_tape_tgt_t	*tgt;
	char		**geo_vidpid = NULL;
	char		*geo, *vidpid;
	uint32_t	*geoip = NULL;
	uint_t		length;
	uint_t		length2;
	lldaddr_t	sector_count;
	char		prop_name[15];
	int		ret = DDI_FAILURE;

	vx_tape = TRAN2VX_TAPE(tran);
	VX_TAPE_MUTEX_ENTER(vx_tape);

	/*
	 * We get called for each target driver.conf node, multiple
	 * nodes may map to the same tgt,lun (sd.conf, st.conf, etc).
	 * Check to see if transport to tgt,lun already established.
	 */
	tgt = find_tgt(vx_tape, sd->sd_address.a_target, sd->sd_address.a_lun);
	if (tgt) {
		ret = DDI_SUCCESS;
		goto out;
	}

	/* see if we have driver.conf specified device for this target,lun */
	(void) snprintf(prop_name, sizeof (prop_name), "targ_%d_%d",
	    sd->sd_address.a_target, sd->sd_address.a_lun);
	if (ddi_prop_lookup_string_array(DDI_DEV_T_ANY, hba_dip,
	    DDI_PROP_DONTPASS, prop_name,
	    &geo_vidpid, &length) != DDI_PROP_SUCCESS)
		goto out;
	if (length < 2) {
		cmn_err(CE_WARN, "vx_tape: %s property does not have 2 "
			"elements", prop_name);
		goto out;
	}

	/* pick geometry name and vidpid string from string array */
	geo = *geo_vidpid;
	vidpid = *(geo_vidpid + 1);

	/* lookup geometry property integer array */
	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, hba_dip, DDI_PROP_DONTPASS,
	    geo, (int **)&geoip, &length2) != DDI_PROP_SUCCESS) {
		cmn_err(CE_WARN, "vx_tape: didn't get prop '%s'", geo);
		goto out;
	}
	if (length2 < 6) {
		cmn_err(CE_WARN, "vx_tape: property %s does not have 6 "
			"elements", *geo_vidpid);
		goto out;
	}

	/* allocate and initialize tgt structure for tgt,lun */
	tgt = kmem_zalloc(sizeof (vx_tape_tgt_t), KM_SLEEP);
	rw_init(&tgt->vx_tape_tgt_nw_lock, NULL, RW_DRIVER, NULL);
	mutex_init(&tgt->vx_tape_tgt_blk_lock, NULL, MUTEX_DRIVER, NULL);

	/* create avl for data block storage */
	avl_create(&tgt->vx_tape_tgt_data, vx_tape_bsd_blkcompare,
		sizeof (blklist_t), offsetof(blklist_t, bl_node));

	/* save scsi_address and vidpid */
	bcopy(sd, &tgt->vx_tape_tgt_saddr, sizeof (struct scsi_address));
	(void) strncpy(tgt->vx_tape_tgt_inq, vidpid,
		sizeof (vx_tape->vx_tape_tgt->vx_tape_tgt_inq));

	/*
	 * The high order 4 bytes of the sector count always come first in
	 * vx_tape.conf.  They are followed by the low order 4 bytes.  Not
	 * all CPU types want them in this order, but laddr_t takes care of
	 * this for us.  We then pick up geometry (ncyl X nheads X nsect).
	 */
	sector_count._p._u	= *(geoip + 0);
	sector_count._p._l	= *(geoip + 1);
	/*
	 * On 32-bit platforms, fix block size if it's greater than the
	 * allowable maximum.
	 */
#if !defined(_LP64)
	if (sector_count._f > DK_MAX_BLOCKS)
		sector_count._f = DK_MAX_BLOCKS;
#endif
	tgt->vx_tape_tgt_sectors = sector_count._f;
	tgt->vx_tape_tgt_dtype	= *(geoip + 2);
	tgt->vx_tape_tgt_ncyls	= *(geoip + 3);
	tgt->vx_tape_tgt_nheads	= *(geoip + 4);
	tgt->vx_tape_tgt_nsect	= *(geoip + 5);

	VX_TAPE_DEBUG(vx_tape, SCSI_DEBUG,
			"vx_tape_trans_tgt_init(): target type: %d",
				tgt->vx_tape_tgt_dtype);

	/* insert target structure into list */
	tgt->vx_tape_tgt_next = vx_tape->vx_tape_tgt;
	vx_tape->vx_tape_tgt = tgt;
	ret = DDI_SUCCESS;

out:	VX_TAPE_MUTEX_EXIT(vx_tape);
	if (geoip)
		ddi_prop_free(geoip);
	if (geo_vidpid)
		ddi_prop_free(geo_vidpid);
	return (ret);
}

/*
 * Function name : vx_tape_i_initcap
 *
 * Return Values : NONE
 * Description	 : Initializes the default target capabilities and
 *		   Sync Rates.
 *
 * Context	 : Called from the user thread through attach.
 *
 */
static void
vx_tape_i_initcap(struct vx_tape *vx_tape)
{
	uint16_t	cap, synch;
	int		i;

	cap = 0;
	synch = 0;
	for (i = 0; i < NTARGETS_WIDE; i++) {
		vx_tape->vx_tape_cap[i] = cap;
		vx_tape->vx_tape_synch[i] = synch;
	}
	VX_TAPE_DEBUG(vx_tape, SCSI_DEBUG, "default cap = 0x%x", cap);
}

/*
 * Function name : vx_tape_scsi_getcap()
 *
 * Return Values : current value of capability, if defined
 *		   -1 if capability is not defined
 * Description	 : returns current capability value
 *
 * Context	 : Can be called from different kernel process threads.
 *		   Can be called by interrupt thread.
 */
static int
vx_tape_scsi_getcap(struct scsi_address *ap, char *cap, int whom)
{
	struct vx_tape	*vx_tape	= ADDR2VX_TAPE(ap);
	int		rval = 0;

	/*
	 * We don't allow inquiring about capabilities for other targets
	 */
	if (cap == NULL || whom == 0) {
		return (-1);
	}

	VX_TAPE_MUTEX_ENTER(vx_tape);

	switch (scsi_hba_lookup_capstr(cap)) {
	case SCSI_CAP_DMA_MAX:
		rval = 1 << 24; /* Limit to 16MB max transfer */
		break;
	case SCSI_CAP_MSG_OUT:
		rval = 1;
		break;
	case SCSI_CAP_DISCONNECT:
		rval = 1;
		break;
	case SCSI_CAP_SYNCHRONOUS:
		rval = 1;
		break;
	case SCSI_CAP_WIDE_XFER:
		rval = 1;
		break;
	case SCSI_CAP_TAGGED_QING:
		rval = 1;
		break;
	case SCSI_CAP_UNTAGGED_QING:
		rval = 1;
		break;
	case SCSI_CAP_PARITY:
		rval = 1;
		break;
	case SCSI_CAP_INITIATOR_ID:
		rval = vx_tape->vx_tape_initiator_id;
		break;
	case SCSI_CAP_ARQ:
		rval = 1;
		break;
	case SCSI_CAP_LINKED_CMDS:
		break;
	case SCSI_CAP_RESET_NOTIFICATION:
		rval = 1;
		break;

	default:
		rval = -1;
		break;
	}

	VX_TAPE_MUTEX_EXIT(vx_tape);

	return (rval);
}

/*
 * Function name : vx_tape_scsi_setcap()
 *
 * Return Values : 1 - capability exists and can be set to new value
 *		   0 - capability could not be set to new value
 *		  -1 - no such capability
 *
 * Description	 : sets a capability for a target
 *
 * Context	 : Can be called from different kernel process threads.
 *		   Can be called by interrupt thread.
 */
static int
vx_tape_scsi_setcap(struct scsi_address *ap, char *cap, int value, int whom)
{
	struct vx_tape	*vx_tape	= ADDR2VX_TAPE(ap);
	int		rval = 0;

	/*
	 * We don't allow setting capabilities for other targets
	 */
	if (cap == NULL || whom == 0) {
		return (-1);
	}

	VX_TAPE_MUTEX_ENTER(vx_tape);

	switch (scsi_hba_lookup_capstr(cap)) {
	case SCSI_CAP_DMA_MAX:
	case SCSI_CAP_MSG_OUT:
	case SCSI_CAP_PARITY:
	case SCSI_CAP_UNTAGGED_QING:
	case SCSI_CAP_LINKED_CMDS:
	case SCSI_CAP_RESET_NOTIFICATION:
		/*
		 * None of these are settable via
		 * the capability interface.
		 */
		break;
	case SCSI_CAP_DISCONNECT:
		rval = 1;
		break;
	case SCSI_CAP_SYNCHRONOUS:
		rval = 1;
		break;
	case SCSI_CAP_TAGGED_QING:
		rval = 1;
		break;
	case SCSI_CAP_WIDE_XFER:
		rval = 1;
		break;
	case SCSI_CAP_INITIATOR_ID:
		rval = -1;
		break;
	case SCSI_CAP_ARQ:
		rval = 1;
		break;
	case SCSI_CAP_TOTAL_SECTORS:
		vx_tape->nt_total_sectors[ap->a_target][ap->a_lun] = value;
		rval = TRUE;
		break;
	case SCSI_CAP_SECTOR_SIZE:
		rval = TRUE;
		break;
	default:
		rval = -1;
		break;
	}


	VX_TAPE_MUTEX_EXIT(vx_tape);

	return (rval);
}

/*
 * Function name : vx_tape_scsi_init_pkt
 *
 * Return Values : pointer to scsi_pkt, or NULL
 * Description	 : Called by kernel on behalf of a target driver
 *		   calling scsi_init_pkt(9F).
 *		   Refer to tran_init_pkt(9E) man page
 *
 * Context	 : Can be called from different kernel process threads.
 *		   Can be called by interrupt thread.
 */
/* ARGSUSED */
static struct scsi_pkt *
vx_tape_scsi_init_pkt(struct scsi_address *ap, struct scsi_pkt *pkt,
	struct buf *bp, int cmdlen, int statuslen, int tgtlen,
	int flags, int (*callback)(), caddr_t arg)
{
	struct vx_tape		*vx_tape	= ADDR2VX_TAPE(ap);
	struct vx_tape_cmd	*sp;

	ASSERT(callback == NULL_FUNC || callback == SLEEP_FUNC);

	/*
	 * First step of vx_tape_scsi_init_pkt:  pkt allocation
	 */
	if (pkt == NULL) {
		pkt = scsi_hba_pkt_alloc(vx_tape->vx_tape_dip, ap, cmdlen,
			statuslen,
			tgtlen, sizeof (struct vx_tape_cmd), callback, arg);
		if (pkt == NULL) {
			cmn_err(CE_WARN, "vx_tape_scsi_init_pkt: "
				"scsi_hba_pkt_alloc failed");
			return (NULL);
		}

		sp = PKT2CMD(pkt);

		/*
		 * Initialize the new pkt - we redundantly initialize
		 * all the fields for illustrative purposes.
		 */
		sp->cmd_pkt		= pkt;
		sp->cmd_flags		= 0;
		sp->cmd_scblen		= statuslen;
		sp->cmd_cdblen		= cmdlen;
		sp->cmd_vx_tape		= vx_tape;
		pkt->pkt_address	= *ap;
		pkt->pkt_comp		= (void (*)())NULL;
		pkt->pkt_flags		= 0;
		pkt->pkt_time		= 0;
		pkt->pkt_resid		= 0;
		pkt->pkt_statistics	= 0;
		pkt->pkt_reason		= 0;

	} else {
		sp = PKT2CMD(pkt);
	}

	/*
	 * Second step of vx_tape_scsi_init_pkt:  dma allocation/move
	 */
	if (bp && bp->b_bcount != 0) {
		if (bp->b_flags & B_READ) {
			sp->cmd_flags &= ~CFLAG_DMASEND;
		} else {
			sp->cmd_flags |= CFLAG_DMASEND;
		}
		bp_mapin(bp);
		sp->cmd_addr = (unsigned char *) bp->b_un.b_addr;
		sp->cmd_count = bp->b_bcount;
		pkt->pkt_resid = 0;
	}

	return (pkt);
}


/*
 * Function name : vx_tape_scsi_destroy_pkt
 *
 * Return Values : none
 * Description	 : Called by kernel on behalf of a target driver
 *		   calling scsi_destroy_pkt(9F).
 *		   Refer to tran_destroy_pkt(9E) man page
 *
 * Context	 : Can be called from different kernel process threads.
 *		   Can be called by interrupt thread.
 */
static void
vx_tape_scsi_destroy_pkt(struct scsi_address *ap, struct scsi_pkt *pkt)
{
	struct vx_tape_cmd	*sp = PKT2CMD(pkt);

	/*
	 * vx_tape_scsi_dmafree inline to make things faster
	 */
	if (sp->cmd_flags & CFLAG_DMAVALID) {
		/*
		 * Free the mapping.
		 */
		sp->cmd_flags &= ~CFLAG_DMAVALID;
	}

	/*
	 * Free the pkt
	 */
	scsi_hba_pkt_free(ap, pkt);
}


/*
 * Function name : vx_tape_scsi_dmafree()
 *
 * Return Values : none
 * Description	 : free dvma resources
 *
 * Context	 : Can be called from different kernel process threads.
 *		   Can be called by interrupt thread.
 */
/*ARGSUSED*/
static void
vx_tape_scsi_dmafree(struct scsi_address *ap, struct scsi_pkt *pkt)
{
}

/*
 * Function name : vx_tape_scsi_sync_pkt()
 *
 * Return Values : none
 * Description	 : sync dma
 *
 * Context	 : Can be called from different kernel process threads.
 *		   Can be called by interrupt thread.
 */
/*ARGSUSED*/
static void
vx_tape_scsi_sync_pkt(struct scsi_address *ap, struct scsi_pkt *pkt)
{
}

/*
 * routine for reset notification setup, to register or cancel.
 */
static int
vx_tape_scsi_reset_notify(struct scsi_address *ap, int flag,
void (*callback)(caddr_t), caddr_t arg)
{
	struct vx_tape				*vx_tape = ADDR2VX_TAPE(ap);
	struct vx_tape_reset_notify_entry	*p, *beforep;
	int					rval = DDI_FAILURE;

	mutex_enter(VX_TAPE_REQ_MUTEX(vx_tape));

	p = vx_tape->vx_tape_reset_notify_listf;
	beforep = NULL;

	while (p) {
		if (p->ap == ap)
			break;	/* An entry exists for this target */
		beforep = p;
		p = p->next;
	}

	if ((flag & SCSI_RESET_CANCEL) && (p != NULL)) {
		if (beforep == NULL) {
			vx_tape->vx_tape_reset_notify_listf = p->next;
		} else {
			beforep->next = p->next;
		}
		kmem_free((caddr_t)p,
			sizeof (struct vx_tape_reset_notify_entry));
		rval = DDI_SUCCESS;

	} else if ((flag & SCSI_RESET_NOTIFY) && (p == NULL)) {
		p = kmem_zalloc(sizeof (struct vx_tape_reset_notify_entry),
			KM_SLEEP);
		p->ap = ap;
		p->callback = callback;
		p->arg = arg;
		p->next = vx_tape->vx_tape_reset_notify_listf;
		vx_tape->vx_tape_reset_notify_listf = p;
		rval = DDI_SUCCESS;
	}

	mutex_exit(VX_TAPE_REQ_MUTEX(vx_tape));

	return (rval);
}

/*
 * Function name : vx_tape_scsi_start()
 *
 * Return Values : TRAN_FATAL_ERROR	- vx_tape has been shutdown
 *		   TRAN_BUSY		- request queue is full
 *		   TRAN_ACCEPT		- pkt has been submitted to vx_tape
 *
 * Description	 : init pkt, start the request
 *
 * Context	 : Can be called from different kernel process threads.
 *		   Can be called by interrupt thread.
 */
static int
vx_tape_scsi_start(struct scsi_address *ap, struct scsi_pkt *pkt)
{
	struct vx_tape_cmd	*sp	= PKT2CMD(pkt);
	int			rval	= TRAN_ACCEPT;
	struct vx_tape		*vx_tape	= ADDR2VX_TAPE(ap);
	clock_t			cur_lbolt;
	taskqid_t		dispatched;

	ASSERT(mutex_owned(VX_TAPE_REQ_MUTEX(vx_tape)) == 0 || ddi_in_panic());
	ASSERT(mutex_owned(VX_TAPE_RESP_MUTEX(vx_tape)) == 0 || ddi_in_panic());

	cmn_err(CE_NOTE, "vx_tape_scsi_start(%x)", sp);

	VX_TAPE_DEBUG2(vx_tape, SCSI_DEBUG, "vx_tape_scsi_start %x", sp);

	pkt->pkt_reason = CMD_CMPLT;

#ifdef	VX_TAPEDEBUG
	if (vx_tape_cdb_debug) {
		vx_tape_debug_dump_cdb(ap, pkt);
	}
#endif	/* VX_TAPEDEBUG */

	/*
	 * calculate deadline from pkt_time
	 * Instead of multiplying by 100 (ie. HZ), we multiply by 128 so
	 * we can shift and at the same time have a 28% grace period
	 * we ignore the rare case of pkt_time == 0 and deal with it
	 * in vx_tape_i_watch()
	 */
	cur_lbolt = ddi_get_lbolt();
	sp->cmd_deadline = cur_lbolt + (pkt->pkt_time * 128);

	if ((vx_tape_usetaskq == 0) || (pkt->pkt_flags & FLAG_NOINTR) != 0) {
		vx_tape_pkt_comp((caddr_t)pkt);
	} else {
		dispatched = NULL;
		if (vx_tape_collect_stats) {
			/*
			 * If we are collecting statistics, call
			 * taskq_dispatch in no sleep mode, so that we can
			 * detect if we are exceeding the queue length that
			 * was established in the call to taskq_create in
			 * vx_tape_attach.  If the no sleep call fails
			 * (returns NULL), the task will be dispatched in
			 * sleep mode below.
			 */
			dispatched = taskq_dispatch(vx_tape->vx_tape_taskq,
						vx_tape_pkt_comp,
						(void *)pkt, TQ_NOSLEEP);
			if (dispatched == NULL) {
				/* Queue was full.  dispatch failed. */
				mutex_enter(&vx_tape_stats_mutex);
				vx_tape_taskq_max++;
				mutex_exit(&vx_tape_stats_mutex);
			}
		}
		if (dispatched == NULL) {
			(void) taskq_dispatch(vx_tape->vx_tape_taskq,
				vx_tape_pkt_comp, (void *)pkt, TQ_SLEEP);
		}
	}

done:
	ASSERT(mutex_owned(VX_TAPE_REQ_MUTEX(vx_tape)) == 0 || ddi_in_panic());
	ASSERT(mutex_owned(VX_TAPE_RESP_MUTEX(vx_tape)) == 0 || ddi_in_panic());

	return (rval);
}

void
vx_tape_check_cond(struct scsi_pkt *pkt, uchar_t key, uchar_t asc, uchar_t ascq)
{
	struct scsi_arq_status *arq =
			(struct scsi_arq_status *)pkt->pkt_scbp;

	/* got check, no data transferred and ARQ done */
	arq->sts_status.sts_chk = 1;
	pkt->pkt_state |= STATE_ARQ_DONE;
	pkt->pkt_state &= ~STATE_XFERRED_DATA;

	/* for ARQ */
	arq->sts_rqpkt_reason = CMD_CMPLT;
	arq->sts_rqpkt_resid = 0;
	arq->sts_rqpkt_state = STATE_GOT_BUS | STATE_GOT_TARGET |
	    STATE_SENT_CMD | STATE_XFERRED_DATA | STATE_GOT_STATUS;
	arq->sts_sensedata.es_valid = 1;
	arq->sts_sensedata.es_class = 0x7;
	arq->sts_sensedata.es_key = key;
	arq->sts_sensedata.es_add_code = asc;
	arq->sts_sensedata.es_qual_code = ascq;
}

int bsd_scsi_start_stop_unit(struct scsi_pkt *);
int bsd_scsi_test_unit_ready(struct scsi_pkt *);
int bsd_scsi_request_sense(struct scsi_pkt *);
int bsd_scsi_inquiry(struct scsi_pkt *);
int bsd_scsi_format(struct scsi_pkt *);
int bsd_scsi_io(struct scsi_pkt *);
int bsd_scsi_log_sense(struct scsi_pkt *);
int bsd_scsi_mode_sense(struct scsi_pkt *);
int bsd_scsi_mode_select(struct scsi_pkt *);
int bsd_scsi_read_capacity(struct scsi_pkt *);
int bsd_scsi_read_capacity_16(struct scsi_pkt *);
int bsd_scsi_reserve(struct scsi_pkt *);
int bsd_scsi_format(struct scsi_pkt *);
int bsd_scsi_release(struct scsi_pkt *);
int bsd_scsi_read_defect_list(struct scsi_pkt *);
int bsd_scsi_reassign_block(struct scsi_pkt *);
int bsd_freeblkrange(vx_tape_tgt_t *, vx_tape_range_t *);

static void
vx_tape_handle_cmd(struct scsi_pkt *pkt)
{
	cmn_err(CE_NOTE, "vx_tape_handle_cmd(cmd: 0x%x)", pkt->pkt_cdbp[0]);
	switch (pkt->pkt_cdbp[0]) {
	case SCMD_START_STOP:
		(void) bsd_scsi_start_stop_unit(pkt);
		break;
	case SCMD_TEST_UNIT_READY:
		(void) bsd_scsi_test_unit_ready(pkt);
		break;
	case SCMD_REQUEST_SENSE:
		(void) bsd_scsi_request_sense(pkt);
		break;
	case SCMD_INQUIRY:
		(void) bsd_scsi_inquiry(pkt);
		break;
	case SCMD_FORMAT:
		(void) bsd_scsi_format(pkt);
		break;
	case SCMD_READ:
	case SCMD_WRITE:
	case SCMD_READ_G1:
	case SCMD_WRITE_G1:
	case SCMD_READ_G4:
	case SCMD_WRITE_G4:
		(void) bsd_scsi_io(pkt);
		break;
	case SCMD_LOG_SENSE_G1:
		(void) bsd_scsi_log_sense(pkt);
		break;
	case SCMD_MODE_SENSE:
	case SCMD_MODE_SENSE_G1:
		(void) bsd_scsi_mode_sense(pkt);
		break;
	case SCMD_MODE_SELECT:
	case SCMD_MODE_SELECT_G1:
		(void) bsd_scsi_mode_select(pkt);
		break;
	case SCMD_READ_CAPACITY:
		(void) bsd_scsi_read_capacity(pkt);
		break;
	case SCMD_SVC_ACTION_IN_G4:
		if (pkt->pkt_cdbp[1] == SSVC_ACTION_READ_CAPACITY_G4) {
			(void) bsd_scsi_read_capacity_16(pkt);
		} else {
			cmn_err(CE_WARN, "vx_tape: unrecognized G4 service "
				"action 0x%x", pkt->pkt_cdbp[1]);
		}
		break;
	case SCMD_RESERVE:
	case SCMD_RESERVE_G1:
		(void) bsd_scsi_reserve(pkt);
		break;
	case SCMD_RELEASE:
	case SCMD_RELEASE_G1:
		(void) bsd_scsi_release(pkt);
		break;
	case SCMD_REASSIGN_BLOCK:
		(void) bsd_scsi_reassign_block(pkt);
		break;
	case SCMD_READ_DEFECT_LIST:
		(void) bsd_scsi_read_defect_list(pkt);
		break;
	case SCMD_PRIN:
	case SCMD_PROUT:
	case SCMD_REPORT_LUNS:
		/* ASC 0x24 INVALID FIELD IN CDB */
		vx_tape_check_cond(pkt, KEY_ILLEGAL_REQUEST, 0x24, 0x0);
		break;
	default:
		cmn_err(CE_WARN, "vx_tape: unrecognized "
		    "SCSI cmd 0x%x", pkt->pkt_cdbp[0]);
		vx_tape_check_cond(pkt, KEY_ILLEGAL_REQUEST, 0x24, 0x0);
		break;
	case SCMD_GET_CONFIGURATION:
	case 0x35:			/* SCMD_SYNCHRONIZE_CACHE */
		/* Don't complain */
		break;
	}
}

static void
vx_tape_pkt_comp(void * arg)
{
	struct scsi_pkt		*pkt = (struct scsi_pkt *)arg;
	struct vx_tape_cmd	*sp = PKT2CMD(pkt);
	vx_tape_tgt_t		*tgt;

	cmn_err(CE_NOTE, "vx_tape_pkt_comp()");

	VX_TAPE_MUTEX_ENTER(sp->cmd_vx_tape);
	tgt = find_tgt(sp->cmd_vx_tape,
		pkt->pkt_address.a_target, pkt->pkt_address.a_lun);
	VX_TAPE_MUTEX_EXIT(sp->cmd_vx_tape);
	if (!tgt) {
		pkt->pkt_reason = CMD_TIMEOUT;
		pkt->pkt_state = STATE_GOT_BUS | STATE_SENT_CMD;
		pkt->pkt_statistics = STAT_TIMEOUT;
	} else {
		pkt->pkt_reason = CMD_CMPLT;
		*pkt->pkt_scbp = STATUS_GOOD;
		pkt->pkt_state = STATE_GOT_BUS | STATE_GOT_TARGET |
		    STATE_SENT_CMD | STATE_XFERRED_DATA | STATE_GOT_STATUS;
		pkt->pkt_statistics = 0;
		vx_tape_handle_cmd(pkt);
	}
	(*pkt->pkt_comp)(pkt);
}

/* ARGSUSED */
static int
vx_tape_scsi_abort(struct scsi_address *ap, struct scsi_pkt *pkt)
{
	return (1);
}

/* ARGSUSED */
static int
vx_tape_scsi_reset(struct scsi_address *ap, int level)
{
	return (1);
}

static int
vx_tape_get_tgtrange(struct vx_tape *vx_tape,
		    intptr_t arg,
		    vx_tape_tgt_t **tgtp,
		    vx_tape_tgt_range_t *tgtr)
{
	if (ddi_copyin((void *)arg, tgtr, sizeof (*tgtr), 0) != 0) {
		cmn_err(CE_WARN, "vx_tape: ioctl - copy in failed\n");
		return (EFAULT);
	}
	VX_TAPE_MUTEX_ENTER(vx_tape);
	*tgtp = find_tgt(vx_tape, tgtr->vx_tape_target, tgtr->vx_tape_lun);
	VX_TAPE_MUTEX_EXIT(vx_tape);
	if (*tgtp == NULL) {
		cmn_err(CE_WARN, "vx_tape: ioctl - no target for %d,%d on %d",
			tgtr->vx_tape_target, tgtr->vx_tape_lun,
			ddi_get_instance(vx_tape->vx_tape_dip));
		return (ENXIO);
	}
	return (0);
}

static int
vx_tape_c_read(dev_t dev, struct uio *uiop, cred_t *credp)
{
	cmn_err(CE_NOTE, "Inside vx_tape char device vx_tape_c_read()");
	return DDI_SUCCESS;
}

static int
vx_tape_c_write(dev_t dev, struct uio *uiop, cred_t *credp)
{
	cmn_err(CE_NOTE, "Inside vx_tape char device vx_tape_c_write()");
	return DDI_SUCCESS;
}

static int
vx_tape_ioctl(dev_t dev,
	int cmd,
	intptr_t arg,
	int mode,
	cred_t *credp,
	int *rvalp)
{
	struct vx_tape		*vx_tape;
	int			instance;
	int			rv = 0;
	vx_tape_tgt_range_t	tgtr;
	vx_tape_tgt_t		*tgt;

	instance = MINOR2INST(getminor(dev));
	vx_tape = (struct vx_tape *)ddi_get_soft_state(vx_tape_state, instance);
	if (vx_tape == NULL) {
		cmn_err(CE_WARN, "vx_tape: ioctl - no softstate for %d\n",
			getminor(dev));
		return (ENXIO);
	}

	switch (cmd) {
	case VX_TAPE_WRITE_OFF:
		rv = vx_tape_get_tgtrange(vx_tape, arg, &tgt, &tgtr);
		if (rv == 0) {
			rv = vx_tape_write_off(vx_tape, tgt, &tgtr);
		}
		break;
	case VX_TAPE_WRITE_ON:
		rv = vx_tape_get_tgtrange(vx_tape, arg, &tgt, &tgtr);
		if (rv == 0) {
			rv = vx_tape_write_on(vx_tape, tgt, &tgtr);
		}
		break;
	case VX_TAPE_ZERO_RANGE:
		rv = vx_tape_get_tgtrange(vx_tape, arg, &tgt, &tgtr);
		if (rv == 0) {
			mutex_enter(&tgt->vx_tape_tgt_blk_lock);
			rv = bsd_freeblkrange(tgt, &tgtr.vx_tape_blkrange);
			mutex_exit(&tgt->vx_tape_tgt_blk_lock);
		}
		break;
	default:
		rv  = scsi_hba_ioctl(dev, cmd, arg, mode, credp, rvalp);
		break;
	}
	return (rv);
}

/* ARGSUSED */
static int
vx_tape_write_off(struct vx_tape *vx_tape,
	vx_tape_tgt_t *tgt,
	vx_tape_tgt_range_t *tgtr)
{
	size_t			blkcnt = tgtr->vx_tape_blkrange.vx_tape_blkcnt;
	vx_tape_nowrite_t	*cur;
	vx_tape_nowrite_t	*nowrite;
	vx_tape_rng_overlap_t	overlap = O_NONE;
	vx_tape_nowrite_t	**prev = NULL;
	diskaddr_t		sb = tgtr->vx_tape_blkrange.vx_tape_sb;

	nowrite = vx_tape_nowrite_alloc(&tgtr->vx_tape_blkrange);

	/* Find spot in list */
	rw_enter(&tgt->vx_tape_tgt_nw_lock, RW_WRITER);
	cur = vx_tape_find_nowrite(tgt, sb, blkcnt, &overlap, &prev);
	if (overlap == O_NONE) {
		/* Insert into list */
		*prev = nowrite;
		nowrite->vx_tape_nwnext = cur;
	}
	rw_exit(&tgt->vx_tape_tgt_nw_lock);
	if (overlap == O_NONE) {
		if (vx_tape_collect_stats) {
			mutex_enter(&vx_tape_stats_mutex);
			vx_tape_nowrite_count++;
			mutex_exit(&vx_tape_stats_mutex);
		}
	} else {
		cmn_err(CE_WARN, "vx_tape: VX_TAPE_WRITE_OFF 0x%llx,0x%"
		    PRIx64 "overlaps 0x%llx,0x%" PRIx64 "\n",
		    nowrite->vx_tape_blocked.vx_tape_sb,
		    nowrite->vx_tape_blocked.vx_tape_blkcnt,
		    cur->vx_tape_blocked.vx_tape_sb,
		    cur->vx_tape_blocked.vx_tape_blkcnt);
		vx_tape_nowrite_free(nowrite);
		return (EINVAL);
	}
	return (0);
}

/* ARGSUSED */
static int
vx_tape_write_on(struct vx_tape *vx_tape,
		vx_tape_tgt_t *tgt,
		vx_tape_tgt_range_t *tgtr)
{
	size_t			blkcnt = tgtr->vx_tape_blkrange.vx_tape_blkcnt;
	vx_tape_nowrite_t	*cur;
	vx_tape_rng_overlap_t	overlap = O_NONE;
	vx_tape_nowrite_t	**prev = NULL;
	int			rv = 0;
	diskaddr_t		sb = tgtr->vx_tape_blkrange.vx_tape_sb;

	/* Find spot in list */
	rw_enter(&tgt->vx_tape_tgt_nw_lock, RW_WRITER);
	cur = vx_tape_find_nowrite(tgt, sb, blkcnt, &overlap, &prev);
	if (overlap == O_SAME) {
		/* Remove from list */
		*prev = cur->vx_tape_nwnext;
	}
	rw_exit(&tgt->vx_tape_tgt_nw_lock);

	switch (overlap) {
	case O_NONE:
		cmn_err(CE_WARN, "vx_tape: VX_TAPE_WRITE_ON 0x%llx,0x%lx "
			"range not found\n", sb, blkcnt);
		rv = ENXIO;
		break;
	case O_SAME:
		if (vx_tape_collect_stats) {
			mutex_enter(&vx_tape_stats_mutex);
			vx_tape_nowrite_count--;
			mutex_exit(&vx_tape_stats_mutex);
		}
		vx_tape_nowrite_free(cur);
		break;
	case O_OVERLAP:
	case O_SUBSET:
		cmn_err(CE_WARN, "vx_tape: VX_TAPE_WRITE_ON 0x%llx,0x%lx "
			"overlaps 0x%llx,0x%" PRIx64 "\n",
			sb, blkcnt, cur->vx_tape_blocked.vx_tape_sb,
			cur->vx_tape_blocked.vx_tape_blkcnt);
		rv = EINVAL;
		break;
	}
	return (rv);
}

static vx_tape_nowrite_t *
vx_tape_find_nowrite(vx_tape_tgt_t *tgt,
		    diskaddr_t sb,
		    size_t blkcnt,
		    vx_tape_rng_overlap_t *overlap,
		    vx_tape_nowrite_t ***prevp)
{
	vx_tape_nowrite_t	*cur;
	vx_tape_nowrite_t	**prev;

	/* Find spot in list */
	*overlap = O_NONE;
	prev = &tgt->vx_tape_tgt_nowrite;
	cur = tgt->vx_tape_tgt_nowrite;
	while (cur != NULL) {
		*overlap = vx_tape_overlap(&cur->vx_tape_blocked, sb, blkcnt);
		if (*overlap != O_NONE)
			break;
		prev = &cur->vx_tape_nwnext;
		cur = cur->vx_tape_nwnext;
	}

	*prevp = prev;
	return (cur);
}

static vx_tape_nowrite_t *
vx_tape_nowrite_alloc(vx_tape_range_t *range)
{
	vx_tape_nowrite_t	*nw;

	nw = kmem_zalloc(sizeof (*nw), KM_SLEEP);
	bcopy((void *) range,
		(void *) &nw->vx_tape_blocked,
		sizeof (nw->vx_tape_blocked));
	return (nw);
}

static void
vx_tape_nowrite_free(vx_tape_nowrite_t *nw)
{
	kmem_free((void *) nw, sizeof (*nw));
}

vx_tape_rng_overlap_t
vx_tape_overlap(vx_tape_range_t *rng, diskaddr_t sb, size_t cnt)
{

	if (rng->vx_tape_sb >= sb + cnt)
		return (O_NONE);
	if (rng->vx_tape_sb + rng->vx_tape_blkcnt <= sb)
		return (O_NONE);
	if ((rng->vx_tape_sb == sb) && (rng->vx_tape_blkcnt == cnt))
		return (O_SAME);
	if ((sb >= rng->vx_tape_sb) &&
	    ((sb + cnt) <= (rng->vx_tape_sb + rng->vx_tape_blkcnt))) {
		return (O_SUBSET);
	}
	return (O_OVERLAP);
}

#include <sys/varargs.h>

/*
 * Error logging, printing, and debug print routines
 */

/*VARARGS3*/
static void
vx_tape_i_log(struct vx_tape *vx_tape, int level, char *fmt, ...)
{
	char	buf[256];
	va_list	ap;

	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);

	scsi_log(vx_tape ? vx_tape->vx_tape_dip : NULL,
	    "vx_tape", level, "%s\n", buf);
}


#ifdef VX_TAPEDEBUG

static void
vx_tape_debug_dump_cdb(struct scsi_address *ap, struct scsi_pkt *pkt)
{
	static char	hex[]	= "0123456789abcdef";
	struct vx_tape	*vx_tape	= ADDR2VX_TAPE(ap);
	struct vx_tape_cmd	*sp	= PKT2CMD(pkt);
	uint8_t		*cdb	= pkt->pkt_cdbp;
	char		buf [256];
	char		*p;
	int		i;

	(void) snprintf(buf, sizeof (buf), "vx_tape%d: <%d,%d> ",
		ddi_get_instance(vx_tape->vx_tape_dip),
		ap->a_target, ap->a_lun);

	p = buf + strlen(buf);

	*p++ = '[';
	for (i = 0; i < sp->cmd_cdblen; i++, cdb++) {
		if (i != 0)
			*p++ = ' ';
		*p++ = hex[(*cdb >> 4) & 0x0f];
		*p++ = hex[*cdb & 0x0f];
	}
	*p++ = ']';
	*p++ = '\n';
	*p = 0;

	cmn_err(CE_CONT, buf);
}
#endif	/* VX_TAPEDEBUG */
