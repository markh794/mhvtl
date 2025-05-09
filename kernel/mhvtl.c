/*
 *  linux/kernel/vtl.c
 * vvvvvvvvvvvvvvvvvvvvvvv Original vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
 *  Copyright (C) 1992  Eric Youngdale
 *  Simulate a host adapter with 2 disks attached.  Do a lot of checking
 *  to make sure that we are not getting blocks mixed up, and PANIC if
 *  anything out of the ordinary is seen.
 * ^^^^^^^^^^^^^^^^^^^^^^^ Original ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 *  For documentation see http://sg.danny.cz/sg/sdebug26.html
 *
 *   D. Gilbert (dpg) work for Magneto-Optical device test [20010421]
 *   dpg: work for devfs large number of disks [20010809]
 *        forked for lk 2.5 series [20011216, 20020101]
 *        use vmalloc() more inquiry+mode_sense [20020302]
 *   Patrick Mansfield <patmans@us.ibm.com> max_luns+scsi_level [20021031]
 *   Mike Anderson <andmike@us.ibm.com> sysfs work [20021118]
 *   dpg: change style of boot options to "vtl.num_tgts=2" and
 *        module options to "modprobe vtl num_tgts=2" [20021221]
 *
 *	Mark Harvey 2005 - 2025
 *
 *	markh794@gmail.com
 *
 *	Pinched wholesale from scsi_debug.[ch]
 *
 *	Hacked to represent SCSI tape drives & Library.
 *
 *	Registered char driver to handle data to user space daemon.
 *	Idea is for user space daemons (vtltape & vtllibrary) to emulate
 *	and process the SCSI SSC/SMC device command set.
 *
 *	I've used it for testing NetBackup - but there is no reason any
 *	other backup utility could not use it as well.
 *
 * Modification History:
 *    2010-04-18 hstadler - some source code revision in mhvtl_init,
 *			    mhvtl_exit, some return code checking
 *
 *
 */

#define pr_fmt(fmt) "%s: %s(): " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#include <linux/blkdev.h>
#include <linux/cdev.h>

#include <scsi/scsi_host.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsicam.h>

#include <linux/stat.h>

#ifndef LINUX_VERSION_CODE
#include <linux/version.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39)
#include <linux/smp_lock.h>
#endif

#ifndef _SCSI_H
#define _SCSI_H

#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi.h>

struct Scsi_Host;
struct scsi_cmnd;
struct scsi_device;
struct scsi_target;
struct scatterlist;

#endif /* _SCSI_H */

#include "vtl_common.h"
#include "backport.h"

#if defined(HAVE_GENHD)
#include <linux/genhd.h>
#endif

#include <scsi/scsi_driver.h>
#include <scsi/scsi_ioctl.h>

/* version of scsi_debug I started from
 #define VTL_VERSION "1.75"
*/
#ifndef MHVTL_VERSION
#define MHVTL_VERSION "0.18.34"
#endif
static const char *mhvtl_version_date = "20250212-0";
static const char mhvtl_driver_name[] = "mhvtl";

/* Additional Sense Code (ASC) used */
#define INVALID_FIELD_IN_CDB 0x24

#define VTL_TAGGED_QUEUING 0 /* 0 | MSG_SIMPLE_TAG | MSG_ORDERED_TAG */

#ifndef SCSI_MAX_SG_CHAIN_SEGMENTS
	#define SCSI_MAX_SG_CHAIN_SEGMENTS SG_ALL
#endif

#define	TIMEOUT_FOR_USER_DAEMON	50000

/* Default values for driver parameters */
#define DEF_NUM_HOST	1
#define DEF_NUM_TGTS	0
#define DEF_MAX_LUNS	32
#define DEF_OPTS	1		/* Default to verbose logging */

/* bit mask values for mhvtl_opts */
#define VTL_OPT_NOISE	3

#ifndef MHVTL_DEBUG

#define MHVTL_DBG_PRT_CDB(lvl, s...)

#else

#define MHVTL_DBG_PRT_CDB(lvl, sn, s, len)							\
	{											\
		if ((mhvtl_opts & VTL_OPT_NOISE) >= (lvl)) {					\
			pr_info("(%llu) %d bytes", (long long unsigned)sn, len);		\
			switch (len) {								\
			case 6:									\
				pr_cont(" %02x %02x %02x %02x %02x %02x",			\
							s[0], s[1], s[2], s[3],			\
							s[4], s[5]);				\
				break;								\
			case 8:									\
				pr_cont(" %02x %02x %02x %02x %02x %02x %02x %02x",		\
							s[0], s[1], s[2], s[3],			\
							s[4], s[5], s[6], s[7]);		\
				break;								\
			case 10:								\
				pr_cont(" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",	\
							s[0], s[1], s[2], s[3],			\
							s[4], s[5], s[6], s[7],			\
							s[8], s[9]);				\
				break;								\
			case 12:								\
				pr_cont(" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",		\
							s[0], s[1], s[2], s[3],			\
							s[4], s[5], s[6], s[7],			\
							s[8], s[9], s[10], s[11]);		\
				break;								\
			case 16:								\
			default:								\
				pr_cont(" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",	\
							s[0], s[1], s[2], s[3],			\
							s[4], s[5], s[6], s[7],			\
							s[8], s[9], s[10], s[11],		\
							s[12], s[13], s[14], s[15]);		\
				break;								\
			}									\
		}										\
	}

#endif	/* MHVTL_DEBUG */

/* If REPORT LUNS has luns >= 256 it can choose "flat space" (value 1)
 * or "peripheral device" addressing (value 0) */
#define SAM2_LUN_ADDRESS_METHOD 0

/* Major number assigned to vtl driver => 0 means to ask for one */
static int mhvtl_major = 0;

#define DEF_MAX_MINOR_NO 1024	/* Max number of minor nos. this driver will handle */

#define VTL_CANQUEUE	1	/* needs to be >= 1 */
#define VTL_MAX_CMD_LEN 16

static struct kmem_cache *dsp;
static struct kmem_cache *sgp;

static int mhvtl_add_host = DEF_NUM_HOST;
static int mhvtl_max_luns = DEF_MAX_LUNS;
static int mhvtl_num_tgts = DEF_NUM_TGTS; /* targets per host */
static int mhvtl_opts = DEF_OPTS;

static int mhvtl_cmnd_count = 0;

static unsigned long long serial_number;

struct mhvtl_lu_info {
	struct list_head lu_sibling;
	unsigned char sense_buff[SENSE_BUF_SIZE];	/* weak nexus */
	unsigned int channel;
	unsigned int target;
	unsigned int lun;
	unsigned int minor;
	struct mhvtl_hba_info *mhvtl_hba;
	struct scsi_device *sdev;

	char reset;

	struct list_head cmd_list; /* list of outstanding cmds for this lu */
	spinlock_t cmd_list_lock;
};

static struct mhvtl_lu_info *devp[DEF_MAX_MINOR_NO];

struct mhvtl_hba_info {
	struct list_head hba_sibling; /* List of adapters */
	struct list_head lu_list; /* List of lu */
	struct Scsi_Host *shost;
	struct device dev;
};

#define to_mhvtl_hba(d) \
	container_of(d, struct mhvtl_hba_info, dev)

static LIST_HEAD(mhvtl_hba_list);	/* dll of adapters */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 39)
static spinlock_t mhvtl_hba_list_lock = __SPIN_LOCK_UNLOCKED(mhvtl_hba_list_lock);
#else
static spinlock_t mhvtl_hba_list_lock = SPIN_LOCK_UNLOCKED;
#endif

typedef void (*done_funct_t) (struct scsi_cmnd *);

/* mhvtl_queued_cmd-> state */
enum cmd_state {
	CMD_STATE_FREE = 0,
	CMD_STATE_QUEUED,
	CMD_STATE_IN_USE,
};

struct mhvtl_queued_cmd {
	int state;
	struct timer_list cmnd_timer;
	done_funct_t done_funct;
	struct scsi_cmnd *a_cmnd;
	int scsi_result;
	struct mhvtl_header op_header;

	struct list_head queued_sibling;
	unsigned long long serial_number;
};

static int num_aborts = 0;
static int num_dev_resets = 0;
static int num_bus_resets = 0;
static int num_host_resets = 0;

static int mhvtl_driver_probe(struct device *);
static int mhvtl_driver_remove(struct device *);
static struct bus_type mhvtl_pseudo_lld_bus;

static struct device_driver mhvtl_driverfs_driver = {
	.name		= mhvtl_driver_name,
	.bus		= &mhvtl_pseudo_lld_bus,
	.probe		= mhvtl_driver_probe,
	.remove		= mhvtl_driver_remove,
};

/* function declarations */
static int mhvtl_resp_report_luns(struct scsi_cmnd *SCpnt, struct mhvtl_lu_info *lu);
static int mhvtl_fill_from_user_buffer(struct scsi_cmnd *scp, char __user *arr,
				int arr_len);
static int mhvtl_fill_from_dev_buffer(struct scsi_cmnd *scp, unsigned char *arr,
				int arr_len);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
static void mhvtl_timer_intr_handler(struct timer_list *indx);
#else
static void mhvtl_timer_intr_handler(unsigned long indx);
#endif
static struct mhvtl_lu_info *devInfoReg(struct scsi_device *sdp);
static void mk_sense_buffer(struct mhvtl_lu_info *lu, int key, int asc, int asq);
static void mhvtl_stop_all_queued(void);
static int do_create_driverfs_files(void);
static void do_remove_driverfs_files(void);

static int mhvtl_add_adapter(void);
static void mhvtl_remove_adapter(void);

static int mhvtl_sdev_alloc(struct scsi_device *);
#ifdef DEFINE_QUEUE_LIMITS_SCSI_DEV_CONFIGURE
static int mhvtl_sdev_configure(struct scsi_device *, struct queue_limits *lim);
#else
static int mhvtl_sdev_configure(struct scsi_device *);
#endif
static void mhvtl_sdev_destroy(struct scsi_device *);
#if LINUX_VERSION_CODE != KERNEL_VERSION(2, 6, 9)
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 19, 0) || LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33))
static int mhvtl_change_queue_depth(struct scsi_device *sdev, int qdepth);
#else
static int mhvtl_change_queue_depth(struct scsi_device *sdev, int qdepth,
					int reason);
#endif
#endif
#ifdef QUEUECOMMAND_LCK_ONE_ARG
static int mhvtl_queuecommand_lck(struct scsi_cmnd *);
#else
static int mhvtl_queuecommand_lck(struct scsi_cmnd *, done_funct_t done);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
static int mhvtl_b_ioctl(struct scsi_device *, unsigned int, void __user *);
#else
static int mhvtl_b_ioctl(struct scsi_device *, int, void __user *);
#endif
static long mhvtl_c_ioctl(struct file *, unsigned int, unsigned long);
static int mhvtl_c_ioctl_bkl(struct inode *, struct file *, unsigned int, unsigned long);
static int mhvtl_abort(struct scsi_cmnd *);
static int mhvtl_bus_reset(struct scsi_cmnd *);
static int mhvtl_device_reset(struct scsi_cmnd *);
static int mhvtl_host_reset(struct scsi_cmnd *);
static const char * mhvtl_info(struct Scsi_Host *);
static int mhvtl_open(struct inode *, struct file *);
static int mhvtl_release(struct inode *, struct file *);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 37)
static DEF_SCSI_QCMD(mhvtl_queuecommand)
#endif

static struct device mhvtl_pseudo_primary;

#ifdef DEFINE_CONST_STRUCT_SCSI_HOST_TEMPLATE
static const struct scsi_host_template mhvtl_driver_template = {
#else
static struct scsi_host_template mhvtl_driver_template = {
#endif
	.name =			"VTL",
	.info =			mhvtl_info,
#ifdef DEFINE_QUEUE_LIMITS_SCSI_DEV_CONFIGURE
	.sdev_init =            mhvtl_sdev_alloc,
        .sdev_configure =       mhvtl_sdev_configure,
	.sdev_destroy =         mhvtl_sdev_destroy,
#else
	.slave_alloc =		mhvtl_sdev_alloc,
	.slave_configure =	mhvtl_sdev_configure,
	.slave_destroy =	mhvtl_sdev_destroy,
#endif
	.ioctl =		mhvtl_b_ioctl,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 37)
	.queuecommand =		mhvtl_queuecommand,
#else
	.queuecommand =		mhvtl_queuecommand_lck,
#endif
#if LINUX_VERSION_CODE != KERNEL_VERSION(2, 6, 9)
	.change_queue_depth =	mhvtl_change_queue_depth,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0)
	.ordered_tag =          1,
#endif
#endif
	.eh_abort_handler =	mhvtl_abort,
	.eh_bus_reset_handler = mhvtl_bus_reset,
	.eh_device_reset_handler = mhvtl_device_reset,
	.eh_host_reset_handler = mhvtl_host_reset,
	.can_queue =		VTL_CANQUEUE,
	.this_id =		-1,
	.proc_name =		mhvtl_driver_name,
	.sg_tablesize =		SCSI_MAX_SG_CHAIN_SEGMENTS,
	.cmd_per_lun =		1,
	.max_sectors =		4096,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
	.use_clustering =	ENABLE_CLUSTERING,
#else
	.dma_boundary =		PAGE_SIZE - 1,
#endif
	.module =		THIS_MODULE,
};

static const struct file_operations mhvtl_fops = {
	.owner		= THIS_MODULE,
#if defined(HAVE_UNLOCKED_IOCTL)
	.unlocked_ioctl	= mhvtl_c_ioctl,
#else
	.ioctl		= mhvtl_c_ioctl_bkl,
#endif
	.open		= mhvtl_open,
	.release	= mhvtl_release,
};


#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 17, 0)
 #include "fetch50.c"
#elif LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 26)
 #include "fetch27.c"
#elif LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 26)
 #include "fetch26.c"
#elif LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 23)
 #include "fetch24.c"
#else
 #include "fetch.c"
#endif

/**********************************************************************
 *                misc functions to handle queuing SCSI commands
 **********************************************************************/

/*
 * mhvtl_schedule_resp() - handle SCSI commands that are processed from the
 *                   queuecommand() interface. i.e. No callback to done()
 *                   outside the queuecommand() function.
 *
 *                   Any SCSI command handled directly by the kernel driver
 *                   will use this.
 */
static int mhvtl_schedule_resp(struct scsi_cmnd *SCpnt,
			struct mhvtl_lu_info *lu,
			done_funct_t done, int scsi_result)
{
	if ((VTL_OPT_NOISE & mhvtl_opts) && SCpnt) {
		if (scsi_result) {
			struct scsi_device *sdp = SCpnt->device;

			pr_info(" <%u %u %u %llu> non-zero result=0x%x\n",
				sdp->host->host_no,
				sdp->channel, sdp->id,
				(unsigned long long)sdp->lun, scsi_result);
		}
	}
	if (SCpnt && lu) {
		/* simulate autosense by this driver */
		if (SAM_STAT_CHECK_CONDITION == (scsi_result & 0xff))
			memcpy(SCpnt->sense_buffer, lu->sense_buff,
				(SCSI_SENSE_BUFFERSIZE > SENSE_BUF_SIZE) ?
				SENSE_BUF_SIZE : SCSI_SENSE_BUFFERSIZE);
	}
	if (SCpnt)
		SCpnt->result = scsi_result;
	if (done)
		done(SCpnt);
	return 0;
}

/**********************************************************************
 *                SCSI data handling routines
 **********************************************************************/
static int mhvtl_resp_write_to_user(struct scsi_cmnd *SCpnt,
				void __user *up, int count)
{
	int fetched;

	fetched = mhvtl_fetch_to_dev_buffer(SCpnt, up, count);

	if (fetched < count) {
		pr_err(" cdb indicated=%d, IO sent=%d bytes\n",
				count, fetched);
		return -EIO;
	}

	return 0;
}

static void mhvtl_debug_queued_list(struct mhvtl_lu_info *lu)
{
	unsigned long iflags = 0;
	struct mhvtl_queued_cmd *sqcp, *n;
	int k = 0;

	spin_lock_irqsave(&lu->cmd_list_lock, iflags);
	list_for_each_entry_safe(sqcp, n, &lu->cmd_list, queued_sibling) {
		if (sqcp->state) {
			if (sqcp->a_cmnd) {
				pr_info("%d entry in use "
					"SCpnt: %p, SCSI result: %d, done: %p, "
					"Serial No: %lld\n",
					k, sqcp->a_cmnd, sqcp->scsi_result,
					sqcp->done_funct,
					sqcp->serial_number);
			} else {
				pr_info("%d entry in use "
					"SCpnt: %p, SCSI result: %d, done: %p\n",
					k, sqcp->a_cmnd, sqcp->scsi_result,
					sqcp->done_funct);
			}
		} else
			pr_info("entry free %d\n", k);
		k++;
	}
	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
	pr_info("found %d entr%s\n", k, (k == 1) ? "y" : "ies");
}

static struct mhvtl_hba_info *mhvtl_get_hba_entry(void)
{
	struct mhvtl_hba_info *mhvtl_hba;

	spin_lock(&mhvtl_hba_list_lock);
	if (list_empty(&mhvtl_hba_list))
		mhvtl_hba = NULL;
	else
		mhvtl_hba = list_entry(mhvtl_hba_list.prev,
					struct mhvtl_hba_info, hba_sibling);
	spin_unlock(&mhvtl_hba_list_lock);
	return mhvtl_hba;
}

static void mhvtl_dump_queued_list(void)
{
	struct mhvtl_lu_info *lu, *__lu;

	struct mhvtl_hba_info *mhvtl_hba;

	mhvtl_hba = mhvtl_get_hba_entry();
	if (!mhvtl_hba)
		return;

	/* Now that the work list is split per lu, we have to check each
	 * lu to see if we can find the serial number in question
	 */
	list_for_each_entry_safe(lu, __lu, &mhvtl_hba->lu_list, lu_sibling) {
		pr_debug("Channel %d, ID %d, LUN %d\n", lu->channel, lu->target, lu->lun);
		mhvtl_debug_queued_list(lu);
	}
}

/*********************************************************
 * Generic interface to queue SCSI cmd to userspace daemon
 *********************************************************/
/*
 * mhvtl_q_cmd returns success if we successfully added the SCSI
 * cmd to the queued_list
 *
 * - Set state to indicate that the SCSI cmnd is ready for processing.
 */
static int mhvtl_q_cmd(struct scsi_cmnd *scp,
				done_funct_t done,
				struct mhvtl_lu_info *lu)
{
	unsigned long iflags = 0;
	struct mhvtl_header *vheadp;
	struct mhvtl_queued_cmd *sqcp;

	sqcp = kmalloc(sizeof(*sqcp), GFP_ATOMIC);
	if (!sqcp) {
		pr_err("kmalloc failed %ld bytes\n", sizeof(*sqcp));
		return 1;
	}

	#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
	timer_setup(&sqcp->cmnd_timer, mhvtl_timer_intr_handler, 0);
	#else
	init_timer(&sqcp->cmnd_timer);
	sqcp->cmnd_timer.function = mhvtl_timer_intr_handler;
	#endif
	sqcp->a_cmnd = scp;
	sqcp->scsi_result = 0;
	sqcp->done_funct = done;
	sqcp->cmnd_timer.expires = jiffies + TIMEOUT_FOR_USER_DAEMON;
	add_timer(&sqcp->cmnd_timer);

	vheadp = &sqcp->op_header;
	vheadp->serialNo = serial_number;
	sqcp->serial_number = serial_number;
	/* Make sure serial_number can't wrap to '0' */
	if (unlikely(serial_number < 2))
		serial_number = 2;
	serial_number++;
	memcpy(vheadp->cdb, scp->cmnd, scp->cmd_len);

	/* Set flag.
	 * Next ioctl() poll by user-daemon will check this state.
	 */
	sqcp->state = CMD_STATE_QUEUED;

	spin_lock_irqsave(&lu->cmd_list_lock, iflags);
	list_add_tail(&sqcp->queued_sibling, &lu->cmd_list);
	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);

	if ((mhvtl_opts & VTL_OPT_NOISE) >= 2)
		mhvtl_dump_queued_list();

	return 0;
}

/**********************************************************************
 *                Main interface from SCSI mid level
 **********************************************************************/
static int _mhvtl_queuecommand_lck(struct scsi_cmnd *SCpnt, done_funct_t done)
{
	unsigned char *cmd = (unsigned char *) SCpnt->cmnd;
	int errsts = 0;
	struct mhvtl_lu_info *lu = NULL;

	if (done == NULL)
		return 0;	/* assume mid level reprocessing command */

	if (cmd)
		MHVTL_DBG_PRT_CDB(1, serial_number, cmd, SCpnt->cmd_len);

	if (SCpnt->device->id == mhvtl_driver_template.this_id) {
		pr_err("initiator's id used as target!\n");
		return mhvtl_schedule_resp(SCpnt, NULL, done, DID_NO_CONNECT << 16);
	}

	if (SCpnt->device->lun >= mhvtl_max_luns) {
		pr_err("Max luns exceeded\n");
		return mhvtl_schedule_resp(SCpnt, NULL, done, DID_NO_CONNECT << 16);
	}

	lu = devInfoReg(SCpnt->device);
	if (NULL == lu) {
		pr_err("Could not find lu\n");
		return mhvtl_schedule_resp(SCpnt, NULL, done, DID_NO_CONNECT << 16);
	}

	switch (*cmd) {
	case REPORT_LUNS:	/* mandatory, ignore unit attention */
		errsts = mhvtl_resp_report_luns(SCpnt, lu);
		break;

	/* All commands down the list are handled by a user-space daemon */
	default:	/* Pass on to user space daemon to process */
		errsts = mhvtl_q_cmd(SCpnt, done, lu);
		if (!errsts)
			return 0;
		break;
	}
	return mhvtl_schedule_resp(SCpnt, lu, done, errsts);
}

#ifdef QUEUECOMMAND_LCK_ONE_ARG
static int mhvtl_queuecommand_lck(struct scsi_cmnd *SCpnt)
{
	void (*done)(struct scsi_cmnd *) = scsi_done;

	return _mhvtl_queuecommand_lck(SCpnt, done);
}
#else
static int mhvtl_queuecommand_lck(struct scsi_cmnd *SCpnt, done_funct_t done)
{
	return _mhvtl_queuecommand_lck(SCpnt, done);
}
#endif


/* FIXME: I don't know what version this inline routine was introduced */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,9)

/* RedHat 4 appears to define 'scsi_get_tag_type' but doesn't understand
 * change_queue_depth
 * Disabling for kernel 2.6.9 (RedHat AS 4)
 */

#define MSG_SIMPLE_TAG	0x20
#define MSG_ORDERED_TAG	0x22

/**
 * scsi_get_tag_type - get the type of tag the device supports
 * @sdev:	the scsi device
 *
 * Notes:
 *	If the drive only supports simple tags, returns MSG_SIMPLE_TAG
 *	if it supports all tag types, returns MSG_ORDERED_TAG.
 */
static inline int scsi_get_tag_type(struct scsi_device *sdev)
{
	if (!sdev->tagged_supported)
		return 0;
	if (sdev->ordered_tags)
		return MSG_ORDERED_TAG;
	if (sdev->simple_tags)
		return MSG_SIMPLE_TAG;
	return 0;
}

#endif

/* RedHat 4 appears to define 'scsi_get_tag_type' but doesn't understand
 * change_queue_depth
 * Disabling for kernel 2.6.9 (RedHat AS 4)
 */
#if LINUX_VERSION_CODE != KERNEL_VERSION(2,6,9)
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3,19,0) || LINUX_VERSION_CODE < KERNEL_VERSION(2,6,33))
static int mhvtl_change_queue_depth(struct scsi_device *sdev, int qdepth)
#else
static int mhvtl_change_queue_depth(struct scsi_device *sdev, int qdepth,
					int reason)
#endif
{
	pr_info("queue depth now %d\n", qdepth);

	if (qdepth < 1)
		qdepth = 1;
	else if (qdepth > sdev->host->cmd_per_lun)
		qdepth = sdev->host->cmd_per_lun;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,19,0)
	scsi_adjust_queue_depth(sdev, scsi_get_tag_type(sdev), qdepth);
#else
	scsi_change_queue_depth(sdev, qdepth);
#endif
	return sdev->queue_depth;
}
#endif

static struct mhvtl_queued_cmd *lookup_sqcp(struct mhvtl_lu_info *lu,
						unsigned long serialNo)
{
	unsigned long iflags;
	struct mhvtl_queued_cmd *sqcp;

	spin_lock_irqsave(&lu->cmd_list_lock, iflags);
	list_for_each_entry(sqcp, &lu->cmd_list, queued_sibling) {
		if (sqcp->state && (sqcp->serial_number == serialNo)) {
			spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
			return sqcp;
		}
	}
	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
	return NULL;
}

/*
 * Block device ioctl
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
static int mhvtl_b_ioctl(struct scsi_device *sdp, unsigned int cmd, void __user *arg)
#else
static int mhvtl_b_ioctl(struct scsi_device *sdp, int cmd, void __user *arg)
#endif
{
	pr_debug("cmd=0x%x\n", cmd);

	return -ENOTTY;
}

#define MHVTL_RLUN_ARR_SZ 128

static int mhvtl_resp_report_luns(struct scsi_cmnd *scp, struct mhvtl_lu_info *lu)
{
	unsigned int alloc_len;
	int lun_cnt, i, upper;
	unsigned char *cmd = (unsigned char *)scp->cmnd;
	int select_report = (int)cmd[2];
	struct scsi_lun *one_lun;
	unsigned char arr[MHVTL_RLUN_ARR_SZ];

	alloc_len = cmd[9] + (cmd[8] << 8) + (cmd[7] << 16) + (cmd[6] << 24);
	if ((alloc_len < 16) || (select_report > 2)) {
		mk_sense_buffer(lu, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB,0);
		return SAM_STAT_CHECK_CONDITION;
	}
	/* can produce response with up to 16k luns (lun 0 to lun 16383) */
	memset(arr, 0, MHVTL_RLUN_ARR_SZ);
	lun_cnt = mhvtl_max_luns;
	arr[2] = ((sizeof(struct scsi_lun) * lun_cnt) >> 8) & 0xff;
	arr[3] = (sizeof(struct scsi_lun) * lun_cnt) & 0xff;
	lun_cnt = min((int)((MHVTL_RLUN_ARR_SZ - 8) /
				sizeof(struct scsi_lun)), lun_cnt);
	one_lun = (struct scsi_lun *) &arr[8];
	for (i = 0; i < lun_cnt; i++) {
		upper = (i >> 8) & 0x3f;
		if (upper)
			one_lun[i].scsi_lun[0] =
				(upper | (SAM2_LUN_ADDRESS_METHOD << 6));
		one_lun[i].scsi_lun[1] = i & 0xff;
	}
	return mhvtl_fill_from_dev_buffer(scp, arr, min((int)alloc_len, MHVTL_RLUN_ARR_SZ));
}

static void __mhvtl_remove_sqcp(struct mhvtl_queued_cmd *sqcp)
{
	list_del(&sqcp->queued_sibling);
	kfree(sqcp);
}


static void mhvtl_remove_sqcp(struct mhvtl_lu_info *lu, struct mhvtl_queued_cmd *sqcp)
{
	unsigned long iflags;
	spin_lock_irqsave(&lu->cmd_list_lock, iflags);
	__mhvtl_remove_sqcp(sqcp);
	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
}

/* When timer goes off this function is called. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
static void mhvtl_timer_intr_handler(struct timer_list *t)
{
	struct mhvtl_queued_cmd *sqcp = from_timer(sqcp, t, cmnd_timer);
	unsigned long long indx = sqcp->serial_number;
#else
static void mhvtl_timer_intr_handler(unsigned long indx)
{
	struct mhvtl_queued_cmd *sqcp = NULL;
#endif
	struct mhvtl_lu_info *lu;

	struct mhvtl_hba_info *mhvtl_hba;

	mhvtl_hba = mhvtl_get_hba_entry();
	if (!mhvtl_hba)
		return;

	/* Now that the work list is split per lu, we have to check each
	 * lu to see if we can find the serial number in question
	 */
	list_for_each_entry(lu, &mhvtl_hba->lu_list, lu_sibling) {
		sqcp = lookup_sqcp(lu, indx);
		if (sqcp)
			break;
	}

	if (!sqcp) {
		pr_err("Unexpected interrupt, indx %ld\n", (unsigned long)indx);
		return;
	}

	sqcp->state = CMD_STATE_FREE;
	if (sqcp->done_funct) {
		sqcp->a_cmnd->result = sqcp->scsi_result;
		sqcp->done_funct(sqcp->a_cmnd); /* callback to mid level */
	}
	sqcp->done_funct = NULL;
	mhvtl_remove_sqcp(lu, sqcp);
}

static int mhvtl_sdev_alloc(struct scsi_device *sdp)
{
	struct mhvtl_hba_info *mhvtl_hba;
	struct mhvtl_lu_info *lu = (struct mhvtl_lu_info *)sdp->hostdata;

	pr_debug("<%u %u %u %llu>\n",
			sdp->host->host_no, sdp->channel, sdp->id,
			(unsigned long long)sdp->lun);

	if (lu)
		return 0;

	mhvtl_hba = *(struct mhvtl_hba_info **) sdp->host->hostdata;
	if (!mhvtl_hba) {
		pr_err("Host info NULL\n");
		return -1;
	}

	list_for_each_entry(lu, &mhvtl_hba->lu_list, lu_sibling) {
		if ((lu->channel == sdp->channel) &&
			(lu->target == sdp->id) &&
			(lu->lun == sdp->lun)) {
				pr_debug("line %d found matching lu\n", __LINE__);
				return 0;
		}
	}
	return -1;
}


#ifdef DEFINE_QUEUE_LIMITS_SCSI_DEV_CONFIGURE
static int mhvtl_sdev_configure(struct scsi_device *sdp, struct queue_limits *lim)
#else
static int mhvtl_sdev_configure(struct scsi_device *sdp)
#endif
{
	struct mhvtl_lu_info *lu;

	pr_debug("<%u %u %u %llu>\n",
			sdp->host->host_no, sdp->channel, sdp->id,
			(unsigned long long)sdp->lun);
	if (sdp->host->max_cmd_len != VTL_MAX_CMD_LEN)
		sdp->host->max_cmd_len = VTL_MAX_CMD_LEN;
	lu = devInfoReg(sdp);
	sdp->hostdata = lu;
	if (sdp->host->cmd_per_lun)
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,19,0)
		scsi_adjust_queue_depth(sdp, VTL_TAGGED_QUEUING,
					sdp->host->cmd_per_lun);
#else
		scsi_change_queue_depth(sdp, sdp->host->cmd_per_lun);
#endif
	return 0;
}

static void mhvtl_sdev_destroy(struct scsi_device *sdp)
{
	struct mhvtl_lu_info *lu = (struct mhvtl_lu_info *)sdp->hostdata;

	pr_notice("<%u %u %u %llu>\n",
			sdp->host->host_no, sdp->channel, sdp->id,
			(unsigned long long)sdp->lun);
	if (lu) {
		pr_debug("Removing lu structure, minor %d\n", lu->minor);
		/* make this slot avaliable for re-use */
		devp[lu->minor] = NULL;
		kfree(sdp->hostdata);
		sdp->hostdata = NULL;
	}
}

static struct mhvtl_lu_info *devInfoReg(struct scsi_device *sdp)
{
	struct mhvtl_hba_info *mhvtl_hba;
	struct mhvtl_lu_info *lu = (struct mhvtl_lu_info *)sdp->hostdata;

	if (lu)
		return lu;

	mhvtl_hba = *(struct mhvtl_hba_info **) sdp->host->hostdata;
	if (!mhvtl_hba) {
		pr_err("Host info NULL\n");
		return NULL;
	}

	list_for_each_entry(lu, &mhvtl_hba->lu_list, lu_sibling) {
		if ((lu->channel == sdp->channel) &&
			(lu->target == sdp->id) &&
			(lu->lun == sdp->lun))
				return lu;
	}

	return NULL;
}

static void mk_sense_buffer(struct mhvtl_lu_info *lu, int key, int asc, int asq)
{
	unsigned char *sbuff;

	sbuff = lu->sense_buff;
	memset(sbuff, 0, SENSE_BUF_SIZE);
	sbuff[0] = 0x70;	/* fixed, current */
	sbuff[2] = key;
	sbuff[7] = 0xa;		/* implies 18 byte sense buffer */
	sbuff[12] = asc;
	sbuff[13] = asq;
	pr_notice(" [key,asc,ascq]: [0x%x,0x%x,0x%x]\n", key, asc, asq);
}

static int mhvtl_device_reset(struct scsi_cmnd *SCpnt)
{
	struct mhvtl_lu_info *lu;

	pr_notice("Device reset called\n");
	++num_dev_resets;
	if (SCpnt) {
		lu = devInfoReg(SCpnt->device);
		if (lu)
			lu->reset = 1;
	}
	return SUCCESS;
}

static int mhvtl_bus_reset(struct scsi_cmnd *SCpnt)
{
	struct mhvtl_hba_info *mhvtl_hba;
	struct mhvtl_lu_info *lu;
	struct scsi_device *sdp;
	struct Scsi_Host *hp;

	pr_notice("Bus reset called\n");
	++num_bus_resets;
	if (SCpnt && ((sdp = SCpnt->device)) && ((hp = sdp->host))) {
		mhvtl_hba = *(struct mhvtl_hba_info **) hp->hostdata;
		if (mhvtl_hba) {
			list_for_each_entry(lu, &mhvtl_hba->lu_list,
						lu_sibling)
			lu->reset = 1;
		}
	}
	return SUCCESS;
}

static int mhvtl_host_reset(struct scsi_cmnd *SCpnt)
{
	struct mhvtl_hba_info *mhvtl_hba;
	struct mhvtl_lu_info *lu;

	pr_notice("Host reset called\n");
	++num_host_resets;
	spin_lock(&mhvtl_hba_list_lock);
	list_for_each_entry(mhvtl_hba, &mhvtl_hba_list, hba_sibling) {
		list_for_each_entry(lu, &mhvtl_hba->lu_list, lu_sibling)
		lu->reset = 1;
	}
	spin_unlock(&mhvtl_hba_list_lock);
	mhvtl_stop_all_queued();
	return SUCCESS;
}

/* Returns 1 if found 'cmnd' and deleted its timer. else returns 0 */
static int mhvtl_stop_queued_cmnd(struct scsi_cmnd *SCpnt)
{
	int found = 0;
	unsigned long iflags;
	struct mhvtl_queued_cmd *sqcp, *n;
	struct mhvtl_lu_info *lu;

	lu = devInfoReg(SCpnt->device);

	spin_lock_irqsave(&lu->cmd_list_lock, iflags);
	list_for_each_entry_safe(sqcp, n, &lu->cmd_list, queued_sibling) {
		if (sqcp->state && (SCpnt == sqcp->a_cmnd)) {
			del_timer_sync(&sqcp->cmnd_timer);
			sqcp->state = CMD_STATE_FREE;
			sqcp->a_cmnd = NULL;
			found = 1;
			__mhvtl_remove_sqcp(sqcp);
			break;
		}
	}
	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
	return found;
}

/* Deletes (stops) timers of all queued commands */
static void mhvtl_stop_all_queued(void)
{
	unsigned long iflags;
	struct mhvtl_queued_cmd *sqcp, *n;
	struct mhvtl_hba_info *mhvtl_hba;
	struct mhvtl_lu_info *lu;

	mhvtl_hba = mhvtl_get_hba_entry();
	if (!mhvtl_hba)
		return;

	list_for_each_entry(lu, &mhvtl_hba->lu_list, lu_sibling) {
		spin_lock_irqsave(&lu->cmd_list_lock, iflags);
		list_for_each_entry_safe(sqcp, n, &lu->cmd_list,
			queued_sibling) {
			if (sqcp->state && sqcp->a_cmnd) {
				del_timer_sync(&sqcp->cmnd_timer);
				sqcp->state = CMD_STATE_FREE;
				sqcp->a_cmnd = NULL;
				__mhvtl_remove_sqcp(sqcp);
			}
		}
		spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
	}
}

static int mhvtl_abort(struct scsi_cmnd *SCpnt)
{
	pr_notice("Abort called\n");
	++num_aborts;
	mhvtl_stop_queued_cmnd(SCpnt);
	return SUCCESS;
}

/* SLES 9 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,6)
struct scsi_device *__scsi_add_device(struct Scsi_Host *hpnt, uint channel, uint id, uint lun, char *p )
{
	return scsi_add_device(hpnt, channel, id, lun);
}
#endif

/*
 * According to scsi_mid_low_api.txt
 *
 * A call from LLD scsi_add_device() will result in SCSI mid layer
 *   -> sdev_alloc()
 *   -> sdev_configure()
 */
static int mhvtl_add_device(unsigned int minor, struct mhvtl_ctl *ctl)
{
	struct Scsi_Host *hpnt;
	struct mhvtl_hba_info *mhvtl_hba;
	struct mhvtl_lu_info *lu;
	int error = 0;

	if (devp[minor]) {
		pr_notice("device struct already in place\n");
		return error;
	}

	mhvtl_hba = mhvtl_get_hba_entry();
	if (!mhvtl_hba) {
		pr_err("mhvtl_ost info struct is NULL\n");
		return -ENOTTY;
	}
	pr_debug("mhvtl_hba_info struct is %p\n", mhvtl_hba);

	hpnt = mhvtl_hba->shost;
	if (!hpnt) {
		pr_notice("scsi host structure is NULL\n");
		return -ENOTTY;
	}
	pr_debug("scsi_host struct is %p\n", hpnt);

	lu = kmalloc(sizeof(*lu), GFP_KERNEL);
	if (!lu) {
		pr_err("line %d - out of memory attempting to kmalloc %ld bytes\n", __LINE__, sizeof(*lu));
		return -ENOMEM;
	}
	memset(lu, 0, sizeof(*lu));
	list_add_tail(&lu->lu_sibling, &mhvtl_hba->lu_list);

	lu->minor = minor;
	lu->channel = ctl->channel;
	lu->target = ctl->id;
	lu->lun = ctl->lun;
	lu->mhvtl_hba = mhvtl_hba;
	lu->reset = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39)
	spin_lock_init(&lu->cmd_list_lock);
#else
	lu->cmd_list_lock = SPIN_LOCK_UNLOCKED;
#endif

	/* List of queued SCSI op codes associated with this device */
	INIT_LIST_HEAD(&lu->cmd_list);

	lu->sense_buff[0] = 0x70;
	lu->sense_buff[7] = 0xa;
	devp[minor] = lu;
	pr_debug("Added lu: %p to devp[%d]\n", lu, minor);

	lu->sdev = __scsi_add_device(hpnt, ctl->channel, ctl->id, ctl->lun, NULL);
	if (IS_ERR(lu->sdev)) {
		lu->sdev = NULL;
		error = -ENODEV;
	}
	return error;
}

/* Set 'perm' (4th argument) to 0 to disable module_param's definition
 * of sysfs parameters (which module_param doesn't yet support).
 * Sysfs parameters defined explicitly below.
 */
module_param_named(opts, mhvtl_opts, int, 0); /* perm=0644 */

MODULE_AUTHOR("Eric Youngdale + Douglas Gilbert + Mark Harvey");
MODULE_DESCRIPTION("SCSI vtl adapter driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(MHVTL_VERSION);

MODULE_PARM_DESC(opts, "1->noise, 2->medium_error, 4->...");


static char mhvtl_parm_info[256];

static const char *mhvtl_info(struct Scsi_Host *shp)
{
	sprintf(mhvtl_parm_info, "%s: version %s [%s], "
		"opts=0x%x", mhvtl_driver_name, MHVTL_VERSION,
		mhvtl_version_date, mhvtl_opts);
	return mhvtl_parm_info;
}

static ssize_t opts_show(struct device_driver *ddp, char *buf)
{
	return sysfs_emit(buf, "0x%x\n", mhvtl_opts);
}

static ssize_t opts_store(struct device_driver *ddp,
				const char *buf, size_t count)
{
	int opts;
	char work[20];

	if (1 == sscanf(buf, "%10s", work)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,0,0)
		if (0 == strncasecmp(work, "0x", 2)) {
#else
		if (0 == strnicmp(work, "0x", 2)) {
#endif
			if (1 == sscanf(&work[2], "%x", &opts))
				goto opts_done;
		} else {
			if (1 == sscanf(work, "%d", &opts))
				goto opts_done;
		}
	}
	return -EINVAL;
opts_done:
	mhvtl_opts = opts;
	mhvtl_cmnd_count = 0;
	return count;
}

static ssize_t major_show(struct device_driver *ddp, char *buf)
{
	return sysfs_emit(buf, "%d\n", mhvtl_major);
}

static ssize_t add_lu_store(struct device_driver *ddp,
			const char *buf, size_t count)
{
	int retval;
	unsigned int minor;
	struct mhvtl_ctl ctl;
	char str[512];

	if (strncmp(buf, "add", 3)) {
		pr_err("Invalid command: %s\n", buf);
		return count;
	}

	retval = sscanf(buf, "%s %u %d %d %d",
			str, &minor, &ctl.channel, &ctl.id, &ctl.lun);

	pr_debug("Calling 'mhvtl_add_device(minor: %u,"
			" Channel: %d, ID: %d, LUN: %d)\n",
			minor, ctl.channel, ctl.id, ctl.lun);

	retval = mhvtl_add_device(minor, &ctl);

	return count;
}

#ifdef DRIVER_ATTR
static DRIVER_ATTR(opts, S_IRUGO|S_IWUSR, opts_show, opts_store);
static DRIVER_ATTR(major, S_IRUGO, major_show, NULL);
static DRIVER_ATTR(add_lu, S_IWUSR|S_IWGRP, NULL, add_lu_store);
#else
static DRIVER_ATTR_RW(opts);
static DRIVER_ATTR_RO(major);
static DRIVER_ATTR_WO(add_lu);
#endif

static int do_create_driverfs_files(void)
{
	int	ret;
	ret = driver_create_file(&mhvtl_driverfs_driver, &driver_attr_add_lu);
	ret |= driver_create_file(&mhvtl_driverfs_driver, &driver_attr_opts);
	ret |= driver_create_file(&mhvtl_driverfs_driver, &driver_attr_major);
	return ret;
}

static void do_remove_driverfs_files(void)
{
	driver_remove_file(&mhvtl_driverfs_driver, &driver_attr_major);
	driver_remove_file(&mhvtl_driverfs_driver, &driver_attr_opts);
	driver_remove_file(&mhvtl_driverfs_driver, &driver_attr_add_lu);
}

static int __init mhvtl_init(void)
{
	int ret;

	memset(&devp, 0, sizeof(devp));

	serial_number = 2;	/* Start at something other than 0 */

	mhvtl_major = register_chrdev(mhvtl_major, "mhvtl", &mhvtl_fops);
	if (mhvtl_major < 0) {
		pr_crit("Can't get major number\n");
		goto register_chrdev_error;
	}

	ret = device_register(&mhvtl_pseudo_primary);
	if (ret < 0) {
		pr_crit("Device_register error: %d\n", ret);
		goto device_register_error;
	}

	ret = bus_register(&mhvtl_pseudo_lld_bus);
	if (ret < 0) {
		pr_crit("Bus_register error: %d\n", ret);
		goto bus_register_error;
	}

	ret = driver_register(&mhvtl_driverfs_driver);
	if (ret < 0) {
		pr_crit("Driver_register error: %d\n", ret);
		goto driver_register_error;
	}

	ret = do_create_driverfs_files();
	if (ret < 0) {
		pr_crit("Driver_create_file error: %d\n", ret);
		goto do_create_driverfs_error;
	}

	mhvtl_add_host = 0;

	if (mhvtl_add_adapter()) {
		pr_crit("mhvtl_add_adapter failed\n");
		goto mhvtl_add_adapter_error;
	}

	pr_debug("Built %d host%s\n",
			mhvtl_add_host, (mhvtl_add_host == 1) ? "" : "s");

	dsp = (struct kmem_cache *)kmem_cache_create_usercopy("mhvtl_ds_cache",
				sizeof(struct mhvtl_ds), 0, SLAB_HWCACHE_ALIGN,
				0, sizeof(struct mhvtl_ds), NULL);
	if (!dsp) {
		pr_err("Unable to create ds cache");
		goto mhvtl_kmem_cache_error;
	}

	sgp = (struct kmem_cache *)kmem_cache_create_usercopy("mhvtl_sg_cache",
				SG_SEGMENT_SZ, 0, SLAB_HWCACHE_ALIGN,
				0, SG_SEGMENT_SZ, NULL);
	if (!sgp) {
		pr_err("Unable to create sg cache (size %d)", (int)SG_SEGMENT_SZ);
		goto mhvtl_kmem_cache_error;
	} else {
		pr_info("kmem_cache_user_copy: page size: %d", (int)SG_SEGMENT_SZ);
	}
	pr_debug("Starting serial_number: %lld", serial_number);

	return 0;

mhvtl_kmem_cache_error:
	mhvtl_remove_adapter();

mhvtl_add_adapter_error:
	do_remove_driverfs_files();

do_create_driverfs_error:
	driver_unregister(&mhvtl_driverfs_driver);

driver_register_error:
	bus_unregister(&mhvtl_pseudo_lld_bus);

bus_register_error:
	device_unregister(&mhvtl_pseudo_primary);

device_register_error:
	unregister_chrdev(mhvtl_major, "mhvtl");

register_chrdev_error:

	return -EFAULT;
}

static void __exit mhvtl_exit(void)
{
	int k;

	mhvtl_stop_all_queued();

	for (k = mhvtl_add_host; k; k--)
		mhvtl_remove_adapter();

	if (mhvtl_add_host != 0)
		pr_err("mhvtl_remove_adapter error at line %d\n", __LINE__);

	do_remove_driverfs_files();
	driver_unregister(&mhvtl_driverfs_driver);
	bus_unregister(&mhvtl_pseudo_lld_bus);
	device_unregister(&mhvtl_pseudo_primary);
	unregister_chrdev(mhvtl_major, "mhvtl");
	kmem_cache_destroy(dsp);
	kmem_cache_destroy(sgp);
}

device_initcall(mhvtl_init);
module_exit(mhvtl_exit);

static void mhvtl_pseudo_release(struct device *dev)
{
	pr_notice("Called\n");
}

static struct device mhvtl_pseudo_primary = {
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,30)
	.init_name	= "mhvtl_pseudo",
#else
	.bus_id		= "mhvtl_pseudo",
#endif
	.release	= mhvtl_pseudo_release,
};

static int mhvtl_lld_bus_match(struct device *dev,
#ifdef DEFINE_CONST_STRUCT_DEVICE_DRIVER
			       const
#endif
			       struct device_driver *dev_driver)
{
	return 1;
}

static struct bus_type mhvtl_pseudo_lld_bus = {
	.name = "mhvtl",
	.match = mhvtl_lld_bus_match,
};

static void mhvtl_release_adapter(struct device *dev)
{
	struct mhvtl_hba_info *mhvtl_hba;

	mhvtl_hba = to_mhvtl_hba(dev);
	kfree(mhvtl_hba);
}

/* Simplified from original.
 *
 * Changed so it only adds one hba instance and no logical units
 */
static int mhvtl_add_adapter(void)
{
	int error = 0;
	struct mhvtl_hba_info *mhvtl_hba;

	mhvtl_hba = kmalloc(sizeof(*mhvtl_hba), GFP_KERNEL);

	if (!mhvtl_hba) {
		pr_err("Unable to kmalloc %ld bytes of memory at line %d\n", sizeof(*mhvtl_hba), __LINE__);
		return -ENOMEM;
	}

	memset(mhvtl_hba, 0, sizeof(*mhvtl_hba));
	INIT_LIST_HEAD(&mhvtl_hba->lu_list);

	spin_lock(&mhvtl_hba_list_lock);
	list_add_tail(&mhvtl_hba->hba_sibling, &mhvtl_hba_list);
	spin_unlock(&mhvtl_hba_list_lock);

	mhvtl_hba->dev.bus = &mhvtl_pseudo_lld_bus;
	mhvtl_hba->dev.parent = &mhvtl_pseudo_primary;
	mhvtl_hba->dev.release = &mhvtl_release_adapter;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,30)
	dev_set_name(&mhvtl_hba->dev, "adapter%d", mhvtl_add_host);
#else
	sprintf(mhvtl_hba->dev.bus_id, "adapter%d", mhvtl_add_host);
#endif

	error = device_register(&mhvtl_hba->dev);
	if (error) {
		kfree(mhvtl_hba);
		return error;
	}

	mhvtl_add_host++;

	return error;
}

static void mhvtl_remove_adapter(void)
{
	struct mhvtl_hba_info *mhvtl_hba = NULL;

	spin_lock(&mhvtl_hba_list_lock);
	if (!list_empty(&mhvtl_hba_list)) {
		mhvtl_hba = list_entry(mhvtl_hba_list.prev,
					struct mhvtl_hba_info, hba_sibling);
		list_del(&mhvtl_hba->hba_sibling);
	}
	spin_unlock(&mhvtl_hba_list_lock);

	if (!mhvtl_hba)
		return;

	device_unregister(&mhvtl_hba->dev);
	--mhvtl_add_host;
}

static int mhvtl_driver_probe(struct device *dev)
{
	int error = 0;
	struct mhvtl_hba_info *mhvtl_hba;
	struct Scsi_Host *hpnt;

	mhvtl_hba = to_mhvtl_hba(dev);

	hpnt = scsi_host_alloc(&mhvtl_driver_template, sizeof(*mhvtl_hba));
	if (NULL == hpnt) {
		pr_err("scsi_register failed\n");
		error = -ENODEV;
		return error;
	}

	mhvtl_hba->shost = hpnt;
	*((struct mhvtl_hba_info **)hpnt->hostdata) = mhvtl_hba;
	if ((hpnt->this_id >= 0) && (mhvtl_num_tgts > hpnt->this_id))
		hpnt->max_id = mhvtl_num_tgts + 1;
	else
		hpnt->max_id = mhvtl_num_tgts;
	hpnt->max_lun = mhvtl_max_luns;

	error = scsi_add_host(hpnt, &mhvtl_hba->dev);
	if (error) {
		pr_err("scsi_add_host failed\n");
		error = -ENODEV;
		scsi_host_put(hpnt);
	} else
		scsi_scan_host(hpnt);

	return error;
}

static int mhvtl_driver_remove(struct device *dev)
{
	struct list_head *lh, *lh_sf;
	struct mhvtl_hba_info *mhvtl_hba;
	struct mhvtl_lu_info *lu;

	mhvtl_hba = to_mhvtl_hba(dev);

	if (!mhvtl_hba) {
		pr_err("Unable to locate host info\n");
		return -ENODEV;
	}

	scsi_remove_host(mhvtl_hba->shost);

	list_for_each_safe(lh, lh_sf, &mhvtl_hba->lu_list) {
		lu = list_entry(lh, struct mhvtl_lu_info,
					lu_sibling);
		list_del(&lu->lu_sibling);
		kfree(lu);
	}

	scsi_host_put(mhvtl_hba->shost);
	mhvtl_hba->shost = NULL;
	return 0;
}

/*
 *******************************************************************
 * Char device driver routines
 *******************************************************************
 */
static int mhvtl_get_user_data(unsigned int minor, char __user *arg)
{
	struct mhvtl_queued_cmd *sqcp = NULL;
	struct mhvtl_ds *ds;
	int ret = 0;
	unsigned char __user *up;
	size_t sz;

	ds = kmem_cache_alloc(dsp, 0);
	if (!ds)
		return -EFAULT;

	if (copy_from_user((u8 *)ds, (u8 *)arg, sizeof(struct mhvtl_ds))) {
		ret = -EFAULT;
		goto ret_err;
	}

	pr_debug(" data Cmd S/No  : %lld\n", (unsigned long long)ds->serialNo);
	pr_debug(" data pointer   : %p\n", ds->data);
	pr_debug(" data sz        : %d\n", ds->sz);
	pr_debug(" SAM status     : %d (0x%02x)\n",
					ds->sam_stat, ds->sam_stat);
	up = ds->data;
	sz = ds->sz;
	sqcp = lookup_sqcp(devp[minor], ds->serialNo);
	if (!sqcp) {
		ret = -ENOTTY;
		goto ret_err;
	}

	ret = mhvtl_resp_write_to_user(sqcp->a_cmnd, up, sz);

ret_err:
	kmem_cache_free(dsp, ds);
	return ret;
}

static int mhvtl_put_user_data(unsigned int minor, char __user *arg)
{
	struct mhvtl_queued_cmd *sqcp = NULL;
	struct mhvtl_ds *ds;
	int ret = 0;
	uint8_t *s;

	ds = kmem_cache_alloc(dsp, 0);
	if (!ds) {
		pr_err("Failed to allocate kmem_cache\n");
		ret = -EFAULT;
		goto give_up;
	}

	if (copy_from_user((u8 *)ds, (u8 *)arg, sizeof(struct mhvtl_ds))) {
		pr_err("Failed to copy from user %ld bytes", (unsigned long)sizeof(struct mhvtl_ds));
		ret = -EFAULT;
		goto give_up;
	}
	pr_debug(" data Cmd S/No  : %lld\n", (unsigned long long)ds->serialNo);
	pr_debug(" data pointer   : %p\n", ds->data);
	pr_debug(" data sz        : %d\n", ds->sz);
	pr_debug(" SAM status     : %d (0x%02x)\n",
						ds->sam_stat, ds->sam_stat);
	sqcp = lookup_sqcp(devp[minor], ds->serialNo);
	if (!sqcp) {
		pr_err("Callback function not found for SCSI cmd s/no. %lld, minor: %d\n",
				(unsigned long long)ds->serialNo,
				minor);
		ret = 1;	/* report busy to mid level */
		goto give_up;
	}
	ret = mhvtl_fill_from_user_buffer(sqcp->a_cmnd, ds->data, ds->sz);
	if (ds->sam_stat) { /* Auto-sense */
		sqcp->a_cmnd->result = ds->sam_stat;
		if (copy_from_user(sqcp->a_cmnd->sense_buffer,
						ds->sense_buf, SENSE_BUF_SIZE))
			pr_err("Failed to retrieve autosense data\n");
		sqcp->a_cmnd->sense_buffer[0] |= 0x70; /* force valid sense */
		s = sqcp->a_cmnd->sense_buffer;
		pr_debug("Auto-Sense returned [key/ASC/ASCQ] "
				"[%02x %02x %02x]\n",
				s[2],
				s[12],
				s[13]);
	} else
		sqcp->a_cmnd->result = DID_OK << 16;
	del_timer_sync(&sqcp->cmnd_timer);
	if (sqcp->done_funct)
		sqcp->done_funct(sqcp->a_cmnd);
	else
		pr_err("FATAL, line %d: SCSI done_funct callback => NULL\n", __LINE__);
	mhvtl_remove_sqcp(devp[minor], sqcp);

	ret = 0;

give_up:
	kmem_cache_free(dsp, ds);
	return ret;
}

static int send_mhvtl_header(unsigned int minor, char __user *arg)
{
	struct mhvtl_header *vheadp;
	struct mhvtl_queued_cmd *sqcp, *n;
	int ret = 0;

	list_for_each_entry_safe(sqcp, n, &devp[minor]->cmd_list, queued_sibling) {
		if (sqcp->state == CMD_STATE_QUEUED) {
			vheadp = &sqcp->op_header;
			if (copy_to_user((u8 *)arg, (u8 *)vheadp,
						sizeof(struct mhvtl_header))) {
				ret = -EFAULT;
				goto give_up;
			}
			/* Found an outstanding cmd to send */
			sqcp->state = CMD_STATE_IN_USE;
			ret = VTL_QUEUE_CMD;
			/* Can only send one header at a time */
			goto give_up;
		}
	}

give_up:
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,37)
#if defined(DEFINE_SEMAPHORE_HAS_NUMERIC_ARG)
static DEFINE_SEMAPHORE(tmp_mutex, 1);
#else
static DEFINE_SEMAPHORE(tmp_mutex);
#endif /* DEFINE_SEMAPHORE_HAS_NUMERIC_ARG */
#else
static DECLARE_MUTEX(tmp_mutex);
#endif

static int mhvtl_remove_lu(unsigned int minor, char __user *arg)
{
	struct mhvtl_ctl ctl;
	struct mhvtl_hba_info *mhvtl_hba;
	struct mhvtl_lu_info *lu, *n;
	struct scsi_device *baksdev;
	int ret = -ENODEV;

	down(&tmp_mutex);

	if (copy_from_user((u8 *)&ctl, (u8 *)arg, sizeof(ctl))) {
		ret = -EFAULT;
		goto give_up;
	}

	mhvtl_hba = mhvtl_get_hba_entry();
	if (!mhvtl_hba) {
		ret = 0;
		goto give_up;
	}

	pr_debug("ioctl to remove device <c t l> <%02d %02d %02d>, hba: %p\n",
			ctl.channel, ctl.id, ctl.lun, mhvtl_hba);

	list_for_each_entry_safe(lu, n, &mhvtl_hba->lu_list, lu_sibling) {
		if ((lu->channel == ctl.channel) && (lu->target == ctl.id) &&
						(lu->lun == ctl.lun)) {
			pr_debug("line %d found matching lu\n", __LINE__);
			list_del(&lu->lu_sibling);
			devp[minor] = NULL;
			baksdev = lu->sdev;
			scsi_remove_device(lu->sdev);
			scsi_device_put(baksdev);
		}
	}

	ret = 0;

give_up:
	up(&tmp_mutex);
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39)
static DEFINE_MUTEX(ioctl_mutex);
#endif

static long mhvtl_c_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret;

	struct inode *inode = file_inode(file);
	if (!inode) {
		pr_err("Unable to obtain inode - inode is null\n");
		return -ENODEV;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39)
	mutex_lock(&ioctl_mutex);
#else
	lock_kernel();
#endif
	ret = mhvtl_c_ioctl_bkl(inode, file, cmd, arg);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39)
	mutex_unlock(&ioctl_mutex);
#else
	unlock_kernel();
#endif

	return ret;
}

/*
 * char device ioctl entry point
 */
static int mhvtl_c_ioctl_bkl(struct inode *inode, struct file *file,
					unsigned int cmd, unsigned long arg)
{
	unsigned int minor = iminor(inode);
	int ret;

	if (minor >= DEF_MAX_MINOR_NO) {	/* Check limit minor no. */
		return -ENODEV;
	}

	ret = 0;

	switch (cmd) {

	case VTL_POLL_AND_GET_HEADER:
		if (!devp[minor]) {
			put_user(0, (unsigned int *)arg);
			ret = 0;
			break;
		}
		ret = send_mhvtl_header(minor, (char __user *)arg);
		break;

	case VTL_GET_DATA:
		pr_debug("ioctl(VTL_GET_DATA)\n");
		ret = mhvtl_get_user_data(minor, (char __user *)arg);
		break;

	case VTL_PUT_DATA:
		pr_debug("ioctl(VTL_PUT_DATA)\n");
		ret = mhvtl_put_user_data(minor, (char __user *)arg);
		break;

	case VTL_REMOVE_LU:
		pr_debug("ioctl(VTL_REMOVE_LU)\n");
		ret = mhvtl_remove_lu(minor, (char __user *)arg);
		break;

	default:
		ret = -ENOTTY;
		break;
	}
	return ret;
}

static int mhvtl_release(struct inode *inode, struct file *filp)
{
	unsigned int minor = iminor(inode);

	pr_debug("lu for minor %u Release\n", minor);
	return 0;
}

static int mhvtl_open(struct inode *inode, struct file *filp)
{
	unsigned int minor = iminor(inode);

	pr_debug("mhvtl%u: opened\n", minor);
	return 0;
}
