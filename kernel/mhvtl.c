/*
 *  linux/kernel/vtl.c
 * vvvvvvvvvvvvvvvvvvvvvvv Original vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
 *  Copyright (C) 1992  Eric Youngdale
 *  Simulate a host adapter with 2 disks attached.  Do a lot of checking
 *  to make sure that we are not getting blocks mixed up, and PANIC if
 *  anything out of the ordinary is seen.
 * ^^^^^^^^^^^^^^^^^^^^^^^ Original ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 *  For documentation see http://www.torque.net/sg/sdebug25.html
 *
 *   D. Gilbert (dpg) work for Magneto-Optical device test [20010421]
 *   dpg: work for devfs large number of disks [20010809]
 *        forked for lk 2.5 series [20011216, 20020101]
 *        use vmalloc() more inquiry+mode_sense [20020302]
 *        add timers for delayed responses [20020721]
 *   Patrick Mansfield <patmans@us.ibm.com> max_luns+scsi_level [20021031]
 *   Mike Anderson <andmike@us.ibm.com> sysfs work [20021118]
 *   dpg: change style of boot options to "vtl.num_tgts=2" and
 *        module options to "modprobe vtl num_tgts=2" [20021221]
 *
 *	Mark Harvey 2005-6-1
 * 
 *	markh794 at gmail dot com
 *	  or
 *	Current employ address: mark_harvey at symantec dot com
 *
 *	Pinched wholesale from scsi_debug.[ch]
 *
 *	Hacked to represent SCSI tape drives & Library.
 *
 *	Registered char driver to handle data to user space daemon.
 *	Idea is for user space daemons (vxtape & vxlibrary) to emulate
 *	and process the SCSI SSC/SMC device command set.
 *
 *	I've used it for testing NetBackup - but there is no reason any
 *	other backup utility could not use it as well.
 *
 *	Requires Linux kernel 2.6.10 for the 'generic' circular buffer
 *	- My thanks to Stelian Pop
 *
 *	A patch for pre-2.6.10 kernels is available to add the 'kfifo'
 *	circular buffer to this src.
 *
 */

// #include <linux/config.h>
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/genhd.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/smp_lock.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <asm/uaccess.h>

#include <linux/blkdev.h>
#include <linux/cdev.h>

#include <scsi/scsi_host.h>
#include <scsi/scsicam.h>

#include <linux/stat.h>

#ifndef LINUX_VERSION_CODE
#include <linux/version.h>
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

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,9)
#include <linux/kfifo.h>
#else
#include "kfifo.h"
#endif

#include "vtl.h"

#include <scsi/scsi_driver.h>
#include <scsi/scsi_ioctl.h>

/* version of scsi_debug I started from
 #define VTL_VERSION "1.75"
*/
#define VTL_VERSION "0.15.4"
static const char *vtl_version_date = "20080404-0";

/* SCSI command definations not covered in default scsi.h */
#define WRITE_ATTRIBUTE 0x8d

/* Additional Sense Code (ASC) used */
#define NO_ADDED_SENSE 0x0
#define LOGICAL_UNIT_NOT_READY 0x04
#define UNRECOVERED_READ_ERR 0x11
#define UNRECOVERED_WRITE_ERR 0x11
#define INVALID_OPCODE 0x20
#define ADDR_OUT_OF_RANGE 0x21
#define INVALID_FIELD_IN_CDB 0x24
#define POWERON_RESET 0x29
#define SAVING_PARAMS_UNSUP 0x39
#define INTERNAL_TARGET_FAILURE 0x44
#define THRESHHOLD_EXCEEDED 0x5d
#define NOT_SELF_CONFIGURED 0x3e

/* Additional Sense Code Qualfier (ASCQ) used */
#define PROCESS_OF_BECOMMING_READY 0x01

#define VTL_TAGGED_QUEUING 0 /* 0 | MSG_SIMPLE_TAG | MSG_ORDERED_TAG */

/* Default values for driver parameters */
#define DEF_NUM_HOST   1
#define DEF_NUM_TGTS   1
#define DEF_MAX_LUNS   1
#define DEF_DELAY   1
#define DEF_EVERY_NTH   0
#define DEF_NUM_PARTS   0
#define DEF_OPTS   0		/* Default to quiet logging */
#define DEF_SCSI_LEVEL   5	/* INQUIRY, byte2 [5->SPC-3] */
#define DEF_D_SENSE   0
#define DEF_RETRY_REQUEUE 4	/* How many times to re-try a cmd requeue */

#define VTL_FIRMWARE 0x5400

// FIXME: Currently needs to be manually kept in sync with vx.h
#define SENSE_BUF_SIZE	38

#define TAPE_BUFFER_SZ 524288
#define MEDIUM_CHANGER_SZ 524288

/* bit mask values for vtl_opts */
#define VTL_OPT_NOISE   1
#define VTL_OPT_MEDIUM_ERR   2
#define VTL_OPT_TIMEOUT   4
#define VTL_OPT_RECOVERED_ERR   8
/* When "every_nth" > 0 then modulo "every_nth" commands:
 *   - a no response is simulated if VTL_OPT_TIMEOUT is set
 *   - a RECOVERED_ERROR is simulated on successful read and write
 *     commands if VTL_OPT_RECOVERED_ERR is set.
 *
 * When "every_nth" < 0 then after "- every_nth" commands:
 *   - a no response is simulated if VTL_OPT_TIMEOUT is set
 *   - a RECOVERED_ERROR is simulated on successful read and write
 *     commands if VTL_OPT_RECOVERED_ERR is set.
 * This will continue until some other action occurs (e.g. the user
 * writing a new value (other than -1 or 1) to every_nth via sysfs).
 */

/* If REPORT LUNS has luns >= 256 it can choose "flat space" (value 1)
 * or "peripheral device" addressing (value 0) */
#define SAM2_LUN_ADDRESS_METHOD 0

/* Major number assigned to vtl driver => 0 means to ask for one */
static int vtl_Major = 0;

#define DEF_MAX_MINOR_NO 256	/* Max number of minor nos. this driver will handle */

static int vtl_add_host = DEF_NUM_HOST;
static int vtl_set_serial_num = DEF_NUM_HOST;
static int vtl_delay = DEF_DELAY;
static int vtl_every_nth = DEF_EVERY_NTH;
static int vtl_max_luns = DEF_MAX_LUNS;
static int vtl_num_tgts = DEF_NUM_TGTS; /* targets per host */
static int vtl_opts = DEF_OPTS;
static int vtl_scsi_level = DEF_SCSI_LEVEL;
static int vtl_dsense = DEF_D_SENSE;
static int vtl_ssc_buffer_sz = TAPE_BUFFER_SZ;
static char *vtl_serial_prefix = NULL;
static int vtl_set_firmware = VTL_FIRMWARE;
static char *vtl_firmware = NULL;
static char inq_product_rev[6];

static int vtl_cmnd_count = 0;

#define DEV_READONLY(TGT)      (0)
#define DEV_REMOVEABLE(TGT)    (1)

/* default sector size is 512 bytes, 2**9 bytes */
#define POW2_SECT_SIZE 9
#define SECT_SIZE (1 << POW2_SECT_SIZE)
#define SECT_SIZE_PER(TGT) SECT_SIZE

#define SDEBUG_SENSE_LEN 32

struct vtl_header {
	u32 serialNo;
	u8 cdb[16];
	u8 *buf;
};

struct vtl_dev_info {
	struct list_head dev_list;
	unsigned char sense_buff[SDEBUG_SENSE_LEN];	/* weak nexus */
	unsigned int channel;
	unsigned int target;
	unsigned int lun;
	unsigned int minor;
	unsigned int ptype;
	char *serial_no;
	struct vtl_host_info *vtl_host;

	char reset;
	char used;
	char device_offline;
	unsigned int status;
	unsigned int status_argv;

	unsigned char *rw_buf;
	int rw_buf_sz;
	struct kfifo *fifo;

	struct semaphore lock;

	spinlock_t spin_in_progress;

	// vtl_header used to pass SCSI CDB & SCSI Command S/No to user daemon.
	struct vtl_header *vtl_header;

	// If we need to pass any data to user-daemon, store SCSI pointer.
	struct scsi_cmnd *SCpnt;
	int count;
};

static struct vtl_dev_info *devp[DEF_MAX_MINOR_NO];

struct vtl_host_info {
	struct list_head host_list;
	struct Scsi_Host *shost;
	struct device dev;
	struct list_head dev_info_list;
};

#define to_vtl_host(d)	\
	container_of(d, struct vtl_host_info, dev)

static LIST_HEAD(vtl_host_list);
static spinlock_t vtl_host_list_lock = SPIN_LOCK_UNLOCKED;

typedef void (* done_funct_t) (struct scsi_cmnd *);

struct vtl_queued_cmd {
	int in_use;
	struct timer_list cmnd_timer;
	done_funct_t done_funct;
	struct scsi_cmnd *a_cmnd;
	int scsi_result;
};
static struct vtl_queued_cmd queued_arr[VTL_CANQUEUE];

static struct scsi_host_template vtl_driver_template = {
	.proc_info =		vtl_proc_info,
	.name =			"VTL",
	.info =			vtl_info,
	.slave_alloc =		vtl_slave_alloc,
	.slave_configure =	vtl_slave_configure,
	.slave_destroy =	vtl_slave_destroy,
	.ioctl =		vtl_b_ioctl,
	.queuecommand =		vtl_queuecommand,
	.eh_abort_handler =	vtl_abort,
	.eh_bus_reset_handler = vtl_bus_reset,
	.eh_device_reset_handler = vtl_device_reset,
	.eh_host_reset_handler = vtl_host_reset,
	.can_queue =		VTL_CANQUEUE,
	.this_id =		7,
	.sg_tablesize =		64,
	.cmd_per_lun =		1,
	.max_sectors =		4096,
	.unchecked_isa_dma = 	0,
	.use_clustering = 	DISABLE_CLUSTERING,
	.module =		THIS_MODULE,
};

static int num_aborts = 0;
static int num_dev_resets = 0;
static int num_bus_resets = 0;
static int num_host_resets = 0;
static int num_requeue = 0;

static spinlock_t queued_arr_lock = SPIN_LOCK_UNLOCKED;
static rwlock_t atomic_rw = RW_LOCK_UNLOCKED;

static char vtl_driver_name[] = "vtl";

static int vtl_driver_probe(struct device *);
static int vtl_driver_remove(struct device *);
static struct bus_type pseudo_lld_bus;

static struct device_driver vtl_driverfs_driver = {
	.name 		= vtl_driver_name,
	.bus		= &pseudo_lld_bus,
	.probe          = vtl_driver_probe,
	.remove         = vtl_driver_remove,
};

static const int check_condition_result =
		(DRIVER_SENSE << 24) | SAM_STAT_CHECK_CONDITION;

/* function declarations */
static int resp_inquiry(struct scsi_cmnd *SCpnt, int target,
			struct vtl_dev_info *devip);
static int resp_requests(struct scsi_cmnd *SCpnt, struct vtl_dev_info *devip);
static int resp_readblocklimits(struct scsi_cmnd *SCpnt,
			struct vtl_dev_info *devip);
static int resp_report_luns(struct scsi_cmnd *SCpnt,
			    struct vtl_dev_info *devip);
static int fill_from_dev_buffer(struct scsi_cmnd *scp, unsigned char *arr,
				int arr_len, struct kfifo *fifo);
static void timer_intr_handler(unsigned long);
static struct vtl_dev_info *devInfoReg(struct scsi_device *sdev);
static void mk_sense_buffer(struct vtl_dev_info *devip, int key,
			    int asc, int asq);
static int check_reset(struct scsi_cmnd *SCpnt, struct vtl_dev_info *devip);
static void __init init_all_queued(void);
static void stop_all_queued(void);
static int stop_queued_cmnd(struct scsi_cmnd *cmnd);
static int inquiry_evpd_83(unsigned char *arr, int dev_id_num,
				const char *dev_id_str, int dev_id_str_len,
				struct vtl_dev_info *devip);
static int do_create_driverfs_files(void);
static void do_remove_driverfs_files(void);

static int vtl_add_adapter(void);
static void vtl_remove_adapter(void);
static void vtl_max_tgts_luns(void);

static int allocate_minor_no(struct vtl_dev_info *);

static struct device pseudo_primary;
static struct bus_type pseudo_lld_bus;

static struct file_operations vtl_fops = {
	.owner   =  THIS_MODULE,
	.write   =  vtl_write,
	.read    =  vtl_read,
	.ioctl   =  vtl_c_ioctl,
	.open    =  vtl_open,
	.release =  vtl_release,
};

/**********************************************************************
 *                misc functions to handle queuing SCSI commands
 **********************************************************************/

/*
 * schedule_resp() - handle SCSI commands that are processed from the
 *                   queuecommand() interface. i.e. No callback to done()
 *                   outside the queuecommand() function.
 *
 *                   Any SCSI command handled directly by the kernel driver
 *                   will use this.
 */
static int schedule_resp(struct scsi_cmnd *cmnd,
			 struct vtl_dev_info *devip,
			 done_funct_t done, int scsi_result, int delta_jiff)
{
	if ((VTL_OPT_NOISE & vtl_opts) && cmnd) {
		if (scsi_result) {
			struct scsi_device *sdp = cmnd->device;

			printk(KERN_INFO "vtl:    <%u %u %u %u> "
			       "non-zero result=0x%x\n", sdp->host->host_no,
			       sdp->channel, sdp->id, sdp->lun, scsi_result);
		}
	}
	if (cmnd && devip) {
		/* simulate autosense by this driver */
		if (SAM_STAT_CHECK_CONDITION == (scsi_result & 0xff))
			memcpy(cmnd->sense_buffer, devip->sense_buff,
			       (SCSI_SENSE_BUFFERSIZE > SDEBUG_SENSE_LEN) ?
			       SDEBUG_SENSE_LEN : SCSI_SENSE_BUFFERSIZE);
	}
	if (delta_jiff <= 0) {
		if (cmnd)
			cmnd->result = scsi_result;
		if (done)
			done(cmnd);
		return 0;
	} else {
		unsigned long iflags;
		int k;
		struct vtl_queued_cmd *sqcp = NULL;

		spin_lock_irqsave(&queued_arr_lock, iflags);
		for (k = 0; k < VTL_CANQUEUE; ++k) {
			sqcp = &queued_arr[k];
			if (! sqcp->in_use)
				break;
		}
		if (k >= VTL_CANQUEUE) {
			spin_unlock_irqrestore(&queued_arr_lock, iflags);
			printk(KERN_WARNING "vtl: can_queue exceeded\n");
			return 1;	/* report busy to mid level */
		}
		sqcp->in_use = 1;
		sqcp->a_cmnd = cmnd;
		sqcp->scsi_result = scsi_result;
		sqcp->done_funct = done;
		sqcp->cmnd_timer.function = timer_intr_handler;
		sqcp->cmnd_timer.data = k;
		sqcp->cmnd_timer.expires = jiffies + delta_jiff;
		add_timer(&sqcp->cmnd_timer);
		spin_unlock_irqrestore(&queued_arr_lock, iflags);
		if (cmnd)
			cmnd->result = 0;
		return 0;
	}
}

/*
 * The SCSI error code when the user space daemon is not connected.
 */
static int resp_becomming_ready(struct vtl_dev_info *devip)
{
	mk_sense_buffer(devip, NOT_READY, NOT_SELF_CONFIGURED, NO_ADDED_SENSE);
	return check_condition_result;
}

/*
 * Copy data from SCSI command buffer to device buffer
 *  (SCSI command buffer -> kfifo -> user space)
 *
 * Returns number of bytes fetched into 'arr'/FIFO or -1 if error.
 */
static int fetch_to_dev_buffer(struct scsi_cmnd *scp, struct kfifo *fifo,
		       int max_arr_len)
{
	int k, req_len, act_len, len, active;
	void *kaddr;
	void *kaddr_off;
	struct scatterlist *sg;

	if (0 == scp->request_bufflen)
		return 0;
	if (NULL == scp->request_buffer)
		return -1;
	if (NULL == fifo) {
		printk("%s, kfifo is NULL\n", __func__);
		WARN_ON(1);
	}

	if (! ((scp->sc_data_direction == DMA_BIDIRECTIONAL) ||
	      (scp->sc_data_direction == DMA_TO_DEVICE)))
		return -1;
	if (0 == scp->use_sg) {
		req_len = scp->request_bufflen;
		act_len = (req_len < max_arr_len) ? req_len : max_arr_len;
		kfifo_put(fifo, scp->request_buffer, act_len);
		return act_len;
	}
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
	active = 1;
	req_len = act_len = 0;
	scsi_for_each_sg(scp, sg, scp->use_sg, k) {
		if (active) {
			kaddr = (unsigned char *)
				kmap_atomic(sg_page(sg), KM_USER0);
			if (NULL == kaddr)
				return (DID_ERROR << 16);
			kaddr_off = (unsigned char *)kaddr + sg->offset;
			len = sg->length;
			if ((req_len + len) > max_arr_len) {
				active = 0;
				len = max_arr_len - req_len;
			}
			kfifo_put(fifo, kaddr_off, len);
			kunmap_atomic(kaddr, KM_USER0);
			act_len += len;
		}
		req_len += sg->length;
	}
	if (scp->resid)
		scp->resid -= act_len;
	else
		scp->resid = req_len - act_len;
	return 0;
#else
	sg = (struct scatterlist *)scp->request_buffer;
	for (k = 0, req_len = 0, active = 0; k < scp->use_sg; ++k, ++sg) {
		kaddr = (unsigned char *)kmap_atomic(sg->page, KM_USER0);
		if (NULL == kaddr)
			return -1;
		kaddr_off = (unsigned char *)kaddr + sg->offset;
		len = sg->length;
		if ((req_len + len) > max_arr_len) {
			len = max_arr_len - req_len;
			active = 1;
		}
		kfifo_put(fifo, kaddr_off, len);
		kunmap_atomic(kaddr, KM_USER0);
		if (active)
			return req_len + len;
		req_len += sg->length;
	}
	return req_len;
#endif
}

/**********************************************************************
 *                SCSI data handling routines
 **********************************************************************/
static int resp_write_to_user(struct scsi_cmnd *SCpnt,
			  struct vtl_dev_info *devip, int count)
{
	int fetched;
	unsigned long iflags = 0;

	write_lock_irqsave(&atomic_rw, iflags);
	fetched = fetch_to_dev_buffer(SCpnt, devip->fifo, count);
	write_unlock_irqrestore(&atomic_rw, iflags);

	if (-1 == fetched) {
		devip->status_argv = 0;
		devip->status = 0;
		return (DID_ERROR << 16);
	}

	if ((fetched < count) && (VTL_OPT_NOISE & vtl_opts))
		printk(KERN_INFO "vtl: write: cdb indicated=%d, "
		       " IO sent=%d bytes\n", count, fetched);

	return 0;
}

/*********************************************************
 * Generic interface to queue SCSI cmd to userspace daemon
 *********************************************************/
/*
 * Find an unused spot in the queued_arr[] and add
 * this SCSI command to it.
 * Return 1 on success, 0 on failure (array if full)
 */
static int add_q_cmd(struct scsi_cmnd *SCpnt, done_funct_t done)
{
	int k;
	unsigned long iflags;
	struct vtl_queued_cmd *sqcp;

	spin_lock_irqsave(&queued_arr_lock, iflags);
	for (k = 0; k < VTL_CANQUEUE; ++k) {
		sqcp = &queued_arr[k];
		if ( ! sqcp->in_use) {
			sqcp->in_use = 1;
			sqcp->a_cmnd = SCpnt;
			sqcp->done_funct = done;
			sqcp->cmnd_timer.function = timer_intr_handler;
			sqcp->cmnd_timer.data = k;
			sqcp->cmnd_timer.expires = jiffies + 25000;
			add_timer(&sqcp->cmnd_timer);
			break;
		}
	}
	spin_unlock_irqrestore(&queued_arr_lock, iflags);
	return (k < VTL_CANQUEUE) ? 0 : 1;
}

/*
 * q_cmd returns success if we successfully added the SCSI
 * cmd to the queued_cmd[] array.
 *
 * - Set flag that SCSI cmnd is ready for processing.
 * - Copy the SCSI s/no. to user space
 * - Copy the SCSI cdb to user space
 * - The data block for each SCSI cmd is not processed here.
 *   It is processed via a char ioctl..
 */
static int q_cmd(struct scsi_cmnd *scp,
				done_funct_t done,
				struct vtl_dev_info *devip)
{
	unsigned long iflags;
	struct vtl_header *vheadp;

	/* No user space daemon talking to us */
	if (devip->device_offline) {
		printk("Offline: No user-space daemons registered\n");
		return resp_becomming_ready(devip);
	}

	vheadp = devip->vtl_header;

	/*
	 * Check fifo is empty, return error if not
	 */
	if (kfifo_len(devip->fifo)) {
		printk("%s, line %d: kfifo not empty, len %d"
			", reset fifo, S/No %ld"
			", target: %d, lun: %d\n",
				__func__, __LINE__,
				kfifo_len(devip->fifo),
				scp->serial_number,
				devip->target, devip->lun);
		num_requeue++;
		kfifo_reset(devip->fifo);
		if (num_requeue > DEF_RETRY_REQUEUE) {
			printk("%s : Giving up, resetting fifo\n",
							__func__);
			num_requeue = 0;
			return schedule_resp(scp, NULL, done, DID_RESET << 16, 0);
		}
		return schedule_resp(scp, NULL, done, DID_SOFT_ERROR << 16, 0);
	}
	num_requeue = 0;

	/* Return error if we could not add SCSI cmd
	   to the queued_arr[] */
	if (add_q_cmd(scp, done))
		return schedule_resp(scp, NULL, done,
					DID_NO_CONNECT << 16, 0);

	spin_lock_irqsave(&queued_arr_lock, iflags);

	vheadp->serialNo = htonl(scp->serial_number);
	memcpy(vheadp->cdb, scp->cmnd, scp->cmd_len);

	/* Set flag.
	 * Next poll by user-daemon will see there is a SCSI command ready for
	 * processing. This is handled by c_ioctl routines.
	 */
	devip->status = VTL_QUEUE_CMD;
	devip->status_argv = scp->cmd_len;

	spin_unlock_irqrestore(&queued_arr_lock, iflags);

	return 0;
}

/**********************************************************************
 *                Main interface from SCSI mid level
 **********************************************************************/
static int vtl_queuecommand(struct scsi_cmnd *SCpnt, done_funct_t done)
{
	unsigned char *cmd = (unsigned char *) SCpnt->cmnd;
	int num;
	int k;
	int errsts = 0;
	int target = SCpnt->device->id;
	struct vtl_dev_info *devip = NULL;
	int inj_recovered = 0;

	if (done == NULL)
		return 0;	/* assume mid level reprocessing command */

	if ((VTL_OPT_NOISE & vtl_opts) && cmd) {
		if (TEST_UNIT_READY != cmd[0]) {	// Skip TUR *
			printk(KERN_INFO "vtl: cmd ");
			for (k = 0, num = SCpnt->cmd_len; k < num; ++k)
				printk("%02x ", (int)cmd[k]);
			printk("\n");
		}
	}

	if (target == vtl_driver_template.this_id) {
		printk(KERN_INFO "vtl: initiator's id used as "
		       "target!\n");
		return schedule_resp(SCpnt, NULL, done,
				     DID_NO_CONNECT << 16, 0);
	}

	if (SCpnt->device->lun >= vtl_max_luns)
		return schedule_resp(SCpnt, NULL, done,
				     DID_NO_CONNECT << 16, 0);
	devip = devInfoReg(SCpnt->device);
	if (NULL == devip)
		return schedule_resp(SCpnt, NULL, done,
				     DID_NO_CONNECT << 16, 0);

	if ((vtl_every_nth != 0) &&
			(++vtl_cmnd_count >= abs(vtl_every_nth))) {
		vtl_cmnd_count = 0;
		if (vtl_every_nth < -1)
			vtl_every_nth = -1;
		if (VTL_OPT_TIMEOUT & vtl_opts)
			return 0; /* ignore command causing timeout */
		else if (VTL_OPT_RECOVERED_ERR & vtl_opts)
			inj_recovered = 1; /* to reads and writes below */
	}

	switch (*cmd) {
	case INQUIRY:		/* mandatory, ignore unit attention */
//		if (devip->device_offline)
			errsts = resp_inquiry(SCpnt, target, devip);
//		else {	/* Go to user space for info */
//			errsts = q_cmd(SCpnt, done, devip);
//			if (errsts == 0)
//				return 0;
//		}
		break;
	case REQUEST_SENSE:	/* mandatory, ignore unit attention */
		if (devip->device_offline) {
			/* internal REQUEST SENSE routine */
			errsts = resp_requests(SCpnt, devip);
		} else {
			/* User space REQUEST SENSE */
			errsts = q_cmd(SCpnt, done, devip);
			if (errsts == 0)
				return 0;
		}
		break;
	case MODE_SENSE:
	case MODE_SENSE_10:
		/* User space REQUEST SENSE */
		errsts = q_cmd(SCpnt, done, devip);
		if (errsts == 0)
			return 0;
		break;
	case READ_BLOCK_LIMITS:
		errsts = resp_readblocklimits(SCpnt, devip);
		break;
	case REPORT_LUNS:	/* mandatory, ignore unit attention */
		errsts = resp_report_luns(SCpnt, devip);
		break;
	case VERIFY:		/* 10 byte SBC-2 command */
		errsts = check_reset(SCpnt, devip);
		break;
	case SYNCHRONIZE_CACHE:
		errsts = check_reset(SCpnt, devip);
		break;

	/* All commands down the list are handled by a user-space daemon */
	case MODE_SELECT:
		num   = cmd[4];

		errsts = q_cmd(SCpnt, done, devip);
		if (errsts == 0)
			return resp_write_to_user(SCpnt, devip, num);
		break;
	case MODE_SELECT_10:
		num   = (cmd[7] <<  8) +
			 cmd[8];

		errsts = q_cmd(SCpnt, done, devip);
		if (errsts == 0)
			return resp_write_to_user(SCpnt, devip, num);
		break;
	case WRITE_16:
	case WRITE_12:
		printk("Write_12: not supported **");
	case WRITE_10:
		printk("Write_10: not supported **");
	case WRITE_6:
		if ((errsts = check_reset(SCpnt, devip)))
			break;

		if (devip->ptype == TYPE_TAPE) {
			/* Currently only handle 6byte CDB var block writes */
			if (((*cmd) != WRITE_6) || (cmd[1] && 0x1)){
				mk_sense_buffer(devip, ILLEGAL_REQUEST,
						 INVALID_FIELD_IN_CDB,0);
				errsts = check_condition_result;
				break;
			}
			/* Calculate the size of data to 'write' */
			num   = (cmd[2] << 16) +
				(cmd[3] <<  8) +
				 cmd[4];

			errsts = q_cmd(SCpnt, done, devip);
			if (errsts == 0)
				return resp_write_to_user(SCpnt, devip, num);
		} else {	/* Fail any thing other than a WRITE_6 */
			printk("Fixed block Writes: not supported **");
			mk_sense_buffer(devip, ILLEGAL_REQUEST,
					 INVALID_FIELD_IN_CDB, 0);
			errsts = check_condition_result;
		}
		break;
	case WRITE_ATTRIBUTE:
		/* Calculate the size of data to 'write' */
		num   = (cmd[10] << 24) +
			(cmd[11] << 16) +
			(cmd[12] <<  8) +
			 cmd[13];
		// Check for buffer overflow..
		if (num > devip->rw_buf_sz) {
			mk_sense_buffer(devip, ILLEGAL_REQUEST,
					 	INVALID_FIELD_IN_CDB,0);
			errsts = check_condition_result;
		}
		errsts = q_cmd(SCpnt, done, devip);
		if (errsts == 0)
			return resp_write_to_user(SCpnt, devip, num);
		break;

	default:	// Pass on to user space daemon to process
		if ((errsts = check_reset(SCpnt, devip)))
			break;
		errsts = q_cmd(SCpnt, done, devip);
		if (errsts == 0)
			return 0;
		break;
	}
	return schedule_resp(SCpnt, devip, done, errsts, vtl_delay);
}

/*
 * Block device ioctl
 */
static int vtl_b_ioctl(struct scsi_device *dev, int cmd, void __user *arg)
{
	if (VTL_OPT_NOISE & vtl_opts) {
		printk(KERN_INFO "vtl: ioctl: cmd=0x%x\n", cmd);
	}
	return -ENOTTY; // correct return but upsets fdisk */
}

static int check_reset(struct scsi_cmnd *SCpnt, struct vtl_dev_info *devip)
{
	if (devip->reset) {
		if (VTL_OPT_NOISE & vtl_opts)
			printk(KERN_INFO "vtl: Reporting Unit "
			       "attention: power on reset\n");
		devip->reset = 0;
		mk_sense_buffer(devip, UNIT_ATTENTION, POWERON_RESET, 0);
		return check_condition_result;
	}
	return 0;
}

/* Returns 0 if ok else (DID_ERROR << 16). Sets scp->resid . */
static int fill_from_dev_buffer(struct scsi_cmnd *scp, unsigned char *arr,
				int arr_len, struct kfifo *fifo)
{
	int k, req_len, act_len, len, active;
	void *kaddr;
	void *kaddr_off;
	struct scatterlist *sg;

	if (0 == scp->request_bufflen)
		return 0;
	if (NULL == scp->request_buffer)
		return (DID_ERROR << 16);
	if (! ((scp->sc_data_direction == DMA_BIDIRECTIONAL) ||
	      (scp->sc_data_direction == DMA_FROM_DEVICE)))
		return (DID_ERROR << 16);
	if (0 == scp->use_sg) {
		req_len = scp->request_bufflen;
		act_len = (req_len < arr_len) ? req_len : arr_len;
		if (NULL == arr)
			kfifo_get(fifo, scp->request_buffer, act_len);
		else
			memcpy(scp->request_buffer, arr, act_len);
		scp->resid = req_len - act_len;
		return 0;
	}
	active = 1;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
	req_len = act_len = 0;
	scsi_for_each_sg(scp, sg, scp->use_sg, k) {
		if (active) {
			kaddr = (unsigned char *)
				kmap_atomic(sg_page(sg), KM_USER0);
			if (NULL == kaddr)
				return (DID_ERROR << 16);
			kaddr_off = (unsigned char *)kaddr + sg->offset;
			len = sg->length;
			if ((req_len + len) > arr_len) {
				active = 0;
				len = arr_len - req_len;
			}
			if (NULL == arr)
				kfifo_get(fifo, kaddr_off, len);
			else
				memcpy(kaddr_off, arr + req_len, len);
			kunmap_atomic(kaddr, KM_USER0);
			act_len += len;
		}
		req_len += sg->length;
	}
	if (scp->resid)
		scp->resid -= act_len;
	else
		scp->resid = req_len - act_len;

#else
	sg = (struct scatterlist *)scp->request_buffer;
	for (k = 0, req_len = 0, act_len = 0; k < scp->use_sg; ++k, ++sg) {
		if (active) {
			kaddr = (unsigned char *)
				kmap_atomic(sg->page, KM_USER0);
			if (NULL == kaddr)
				return (DID_ERROR << 16);
			kaddr_off = (unsigned char *)kaddr + sg->offset;
			len = sg->length;
			if ((req_len + len) > arr_len) {
				active = 0;
				len = arr_len - req_len;
			}
			if (NULL == arr)
				kfifo_get(fifo, kaddr_off, len);
			else
				memcpy(kaddr_off, arr + req_len, len);
			kunmap_atomic(kaddr, KM_USER0);
			act_len += len;
		}
		req_len += sg->length;
	}
	scp->resid = req_len - act_len;
#endif

	return 0;
}

/* evpd => Enable Vital product Data */
static const char *inq_vendor_id_1 = "QUANTUM ";
static const char *inq_product_id_1 = "SDLT600         ";

static const char *inq_vendor1_id_1 = "IBM     ";
static const char *inq_product1_id_1 = "ULT3580-TD3     ";

static const char *inq_vendor2_id_1 = "SONY    ";
static const char *inq_product2_id_1 = "SDX-900V        ";

static const char *inq_vendor_id_8 = "STK     ";
static const char *inq_product_id_8 = "L700            ";

static int inquiry_evpd_83(unsigned char *arr, int dev_id_num,
				const char *dev_id_str, int dev_id_str_len,
				struct vtl_dev_info *devip)
{
	int num;

	/* Two identification descriptors: */
	/* T10 vendor identifier field format (faked) */
	arr[0] = 0x2;	/* ASCII */
	arr[1] = 0x1;
	arr[2] = 0x0;

	if (devip->ptype == TYPE_TAPE) {
		switch(devip->lun) {
		case 3:
		case 4:
		case 5:
			memcpy(&arr[4], inq_vendor1_id_1, 8);
			memcpy(&arr[12], inq_product1_id_1, 16);
			memcpy(&arr[28], dev_id_str, dev_id_str_len);
			break;
		case 6:
		case 7:
		case 8:
			memcpy(&arr[4], inq_vendor2_id_1, 8);
			memcpy(&arr[12], inq_product2_id_1, 16);
			memcpy(&arr[28], dev_id_str, dev_id_str_len);
			break;
		default:
			memcpy(&arr[4], inq_vendor_id_1, 8);
			memcpy(&arr[12], inq_product_id_1, 16);
			memcpy(&arr[28], dev_id_str, dev_id_str_len);
		break;
		}
	} else {
		memcpy(&arr[4], inq_vendor_id_8, 8);
		memcpy(&arr[12], inq_product_id_8, 16);
		memcpy(&arr[28], dev_id_str, dev_id_str_len);
	}
	num = 8 + 16 + dev_id_str_len;
	arr[3] = num;
	num += 4;
	/* NAA IEEE registered identifier (faked) */
	arr[num] = 0x1;	/* binary */
	arr[num + 1] = 0x3;
	arr[num + 2] = 0x0;
	arr[num + 3] = 0x8;
	arr[num + 4] = 0x51;	/* ieee company id=0x123456 (faked) */
	arr[num + 5] = 0x23;
	arr[num + 6] = 0x45;
	arr[num + 7] = 0x60;
	arr[num + 8] = (dev_id_num >> 24);
	arr[num + 9] = (dev_id_num >> 16) & 0xff;
	arr[num + 10] = (dev_id_num >> 8) & 0xff;
	arr[num + 11] = dev_id_num & 0xff;
	return num + 12;
}

#define SDEBUG_LONG_INQ_SZ 96
#define SDEBUG_MAX_INQ_ARR_SZ 128

static int resp_inquiry(struct scsi_cmnd *scp, int target,
			struct vtl_dev_info *devip)
{
	unsigned char pq_pdt;
	unsigned char arr[SDEBUG_MAX_INQ_ARR_SZ];
	unsigned char *cmd = (unsigned char *)scp->cmnd;
	int alloc_len;

	alloc_len = (cmd[3] << 8) + cmd[4];
	memset(arr, 0, SDEBUG_MAX_INQ_ARR_SZ);
	pq_pdt = (devip->ptype & 0x1f);
	arr[0] = pq_pdt;
	if (0x2 & cmd[1]) {  /* CMDDT bit set */
		mk_sense_buffer(devip,ILLEGAL_REQUEST,INVALID_FIELD_IN_CDB,0);
		return check_condition_result;
	} else if (0x1 & cmd[1]) {  /* EVPD bit set */
		int dev_id_num, len, host;
		char dev_id_str[6];

		if (devip->serial_no) {
			dev_id_num = 0;
			len = strlen(devip->serial_no);
			strncpy(dev_id_str, devip->serial_no, 10);
		} else {
			host = devip->vtl_host->shost->host_no;
			dev_id_num = host * 2000 + (devip->target * 1000)
						+ devip->lun;
			len = scnprintf(dev_id_str, 10, "%3s%06d",
				(vtl_serial_prefix) ? vtl_serial_prefix : "SN",
				dev_id_num);
			if (VTL_OPT_NOISE & vtl_opts)
				printk("Host: %d, target: %d, lun: %d"
					" => SN: %s\n",
						host,
						devip->target,
						devip->lun,
						dev_id_str);
		}

		if (0 == cmd[2]) { /* supported vital product data pages */
			if (devip->lun > 0)
				arr[3] = 5;
			else
				arr[3] = 3;
			arr[4] = 0x0; /* this page */
			arr[5] = 0x80; /* unit serial number */
			arr[6] = 0x83; /* device identification */
			if (devip->lun > 0) {
				arr[7] = 0xb0;	// SSC VPD page code
				arr[8] = 0xc0;	// F/w build information page
			}
		} else if (0x80 == cmd[2]) { /* unit serial number */
			arr[1] = 0x80;
			arr[3] = len;
			memcpy(&arr[4], dev_id_str, len);
		} else if (0x83 == cmd[2]) { /* device identification */
			arr[1] = 0x83;
			arr[3] = inquiry_evpd_83(&arr[4], dev_id_num,
						 dev_id_str, len, devip);
		} else if (0xb0 == cmd[2]) { // SSC VPD page
			arr[1] = 0xb0;
			arr[2] = 0;
			arr[3] = 2;	// Page len
			arr[4] = 1;	// Set WORM bit
		} else if (0xc0 == cmd[2]) { // Firmware Build Informaiton Page
			arr[1] = 0xc0;
			// Reserved, however SDLT seem to take this as 'WORM'
			arr[2] = 1;
			arr[3] = 0x28;	// Page len
			strncpy(&arr[20], "10-03-2008 19:38:00", 20);
		} else {
			/* Illegal request, invalid field in cdb */
			mk_sense_buffer(devip, ILLEGAL_REQUEST,
					INVALID_FIELD_IN_CDB, 0);
			return check_condition_result;
		}
		return fill_from_dev_buffer(scp, arr,
					    min(alloc_len,
					    SDEBUG_MAX_INQ_ARR_SZ),
					    NULL);
	}
	/* drops through here for a standard inquiry */
	arr[1] = DEV_REMOVEABLE(target) ? 0x80 : 0;	/* Removable disk */
	if (VTL_OPT_NOISE & vtl_opts)
		printk("Media Removeable: %s\n",
					DEV_REMOVEABLE(target) ? "Yes":"No");
	arr[2] = vtl_scsi_level;
	arr[3] = 2;    /* response_data_format==2 */
	arr[4] = SDEBUG_LONG_INQ_SZ - 5;
	arr[6] = 0x1; /* claim: ADDR16 */
	/* arr[6] |= 0x40; ... claim: EncServ (enclosure services) */
//	arr[7] = 0x3a; /* claim: WBUS16, SYNC, LINKED + CMDQUE */
	arr[7] = 0x32; /* claim: WBUS16, SYNC, CMDQUE */
	if (devip->ptype == TYPE_TAPE) {
		switch(devip->lun) {
		case 3:
		case 4:
		case 5:
			memcpy(&arr[8], inq_vendor1_id_1, 8);
			memcpy(&arr[16], inq_product1_id_1, 16);
			break;
		case 6:
		case 7:
		case 8:
			memcpy(&arr[8], inq_vendor2_id_1, 8);
			memcpy(&arr[16], inq_product2_id_1, 16);
			break;
		default:
			memcpy(&arr[8], inq_vendor_id_1, 8);
			memcpy(&arr[16], inq_product_id_1, 16);
			break;
		}
	} else {
		memcpy(&arr[8], inq_vendor_id_8, 8);
		memcpy(&arr[16], inq_product_id_8, 16);
	}
	/* Add devices will have the same product revision... */
	if (vtl_firmware)
		memcpy(&arr[32], vtl_firmware, 4);
	else
		memcpy(&arr[32], inq_product_rev, 4);

	/* version descriptors (2 bytes each) follow */
	arr[58] = 0x0; arr[59] = 0x40; /* SAM-2 */
	arr[60] = 0x3; arr[61] = 0x0;  /* SPC-3 */
	if (devip->ptype == TYPE_DISK) {
		arr[62] = 0x1; arr[63] = 0x80; /* SBC */
	} else if (devip->ptype == TYPE_TAPE) {
		arr[62] = 0x2; arr[63] = 0x00; /* SSC */
	}
	return fill_from_dev_buffer(scp, arr,
			    min(alloc_len, SDEBUG_LONG_INQ_SZ), NULL);
}

static int resp_requests(struct scsi_cmnd *scp,
			 struct vtl_dev_info *devip)
{
	unsigned char *sbuff;
	unsigned char *cmd = (unsigned char *)scp->cmnd;
	unsigned char arr[SDEBUG_SENSE_LEN];
	int len = 18;

	memset(arr, 0, SDEBUG_SENSE_LEN);
	if (devip->reset == 1)
		mk_sense_buffer(devip, 0, NO_ADDED_SENSE, 0);
	sbuff = devip->sense_buff;
	if ((cmd[1] & 1) && (! vtl_dsense)) {
		/* DESC bit set and sense_buff in fixed format */
		arr[0] = 0x72;
		arr[1] = sbuff[2];     /* sense key */
		arr[2] = sbuff[12];    /* asc */
		arr[3] = sbuff[13];    /* ascq */
		len = 8;
	} else
		memcpy(arr, sbuff, SDEBUG_SENSE_LEN);
	mk_sense_buffer(devip, 0, NO_ADDED_SENSE, 0);
	return fill_from_dev_buffer(scp, arr, len, NULL);
}

#define SDEBUG_READBLOCKLIMITS_ARR_SZ 6
static int resp_readblocklimits(struct scsi_cmnd *scp,
			struct vtl_dev_info *devip)
{
	unsigned char arr[SDEBUG_READBLOCKLIMITS_ARR_SZ];
	int errsts;
	unsigned long size = vtl_ssc_buffer_sz;

	if ((errsts = check_reset(scp, devip)))
		return errsts;

	memset(arr, 0, SDEBUG_READBLOCKLIMITS_ARR_SZ);
	arr[1] = (size >> 16);
	arr[2] = (size >> 8) & 0xff;
	arr[3] = size & 0xff;
	arr[5] = 0x4;	/* minimum block size */
	return fill_from_dev_buffer(scp, arr, SDEBUG_READBLOCKLIMITS_ARR_SZ,
				NULL);
}

#define SDEBUG_RLUN_ARR_SZ 128

static int resp_report_luns(struct scsi_cmnd *scp, struct vtl_dev_info *devip)
{
	unsigned int alloc_len;
	int lun_cnt, i, upper;
	unsigned char *cmd = (unsigned char *)scp->cmnd;
	int select_report = (int)cmd[2];
	struct scsi_lun *one_lun;
	unsigned char arr[SDEBUG_RLUN_ARR_SZ];

	alloc_len = cmd[9] + (cmd[8] << 8) + (cmd[7] << 16) + (cmd[6] << 24);
	if ((alloc_len < 16) || (select_report > 2)) {
		mk_sense_buffer(devip, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB,0);
		return check_condition_result;
	}
	/* can produce response with up to 16k luns (lun 0 to lun 16383) */
	memset(arr, 0, SDEBUG_RLUN_ARR_SZ);
	lun_cnt = vtl_max_luns;
	arr[2] = ((sizeof(struct scsi_lun) * lun_cnt) >> 8) & 0xff;
	arr[3] = (sizeof(struct scsi_lun) * lun_cnt) & 0xff;
	lun_cnt = min((int)((SDEBUG_RLUN_ARR_SZ - 8) /
			    sizeof(struct scsi_lun)), lun_cnt);
	one_lun = (struct scsi_lun *) &arr[8];
	for (i = 0; i < lun_cnt; i++) {
		upper = (i >> 8) & 0x3f;
		if (upper)
			one_lun[i].scsi_lun[0] =
			    (upper | (SAM2_LUN_ADDRESS_METHOD << 6));
		one_lun[i].scsi_lun[1] = i & 0xff;
	}
	return fill_from_dev_buffer(scp, arr,
					min((int)alloc_len,
					SDEBUG_RLUN_ARR_SZ),
					NULL);
}

/* When timer goes off this function is called. */
static void timer_intr_handler(unsigned long indx)
{
	struct vtl_queued_cmd *sqcp;
	unsigned long iflags;

	if (indx >= VTL_CANQUEUE) {
		printk(KERN_ERR "vtl:timer_intr_handler: indx too "
		       "large\n");
		return;
	}
	spin_lock_irqsave(&queued_arr_lock, iflags);
	sqcp = &queued_arr[(int)indx];
	if (! sqcp->in_use) {
		spin_unlock_irqrestore(&queued_arr_lock, iflags);
		printk(KERN_ERR "vtl:timer_intr_handler: Unexpected "
		       "interrupt\n");
		return;
	}
	sqcp->in_use = 0;
	if (sqcp->done_funct) {
		sqcp->a_cmnd->result = sqcp->scsi_result;
		sqcp->done_funct(sqcp->a_cmnd); /* callback to mid level */
	}
	sqcp->done_funct = NULL;
	spin_unlock_irqrestore(&queued_arr_lock, iflags);
}

static int vtl_slave_alloc(struct scsi_device *sdp)
{
	struct vtl_host_info *vtl_host;
	struct vtl_dev_info *open_devip = NULL;
	struct vtl_dev_info *devip = (struct vtl_dev_info *)sdp->hostdata;
	unsigned long sz = 0;
	unsigned char *base_p;
	struct kfifo *base_fifo = NULL;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: slave_alloc <%u %u %u %u>\n",
		       sdp->host->host_no, sdp->channel, sdp->id, sdp->lun);

	if (devip)
		return 0;

	vtl_host = *(struct vtl_host_info **) sdp->host->hostdata;
	if (! vtl_host) {
		printk(KERN_ERR "Host info NULL\n");
		return 0;
	}

	list_for_each_entry(devip, &vtl_host->dev_info_list, dev_list) {
		if ((devip->used) && (devip->channel == sdp->channel) &&
				(devip->target == sdp->id) &&
				(devip->lun == sdp->lun))
			return -1;
		else {
			if ((!devip->used) && (!open_devip))
				open_devip = devip;
		}
	}
	if (NULL == open_devip) { /* try and make a new one */
		open_devip = kmalloc(sizeof(*open_devip),GFP_KERNEL);
		if (NULL == open_devip) {
			printk(KERN_ERR "%s(): out of memory at line %d\n",
				__func__, __LINE__);
			return -1;
		}
		memset(open_devip, 0, sizeof(*open_devip));
		open_devip->vtl_host = vtl_host;
		list_add_tail(&open_devip->dev_list,
		&vtl_host->dev_info_list);
	}
	if (open_devip) {
		open_devip->minor = allocate_minor_no(open_devip);
		open_devip->channel = sdp->channel;
		open_devip->target = sdp->id;
		open_devip->lun = sdp->lun;
		open_devip->vtl_host = vtl_host;
		open_devip->reset = 0;
		open_devip->used = 1;
		/* Unit not ready by default */
		open_devip->device_offline = 1;
		if (open_devip->minor == 0) {
			/* Set unit type up as Library */
			open_devip->ptype = TYPE_MEDIUM_CHANGER;
			sz = MEDIUM_CHANGER_SZ;	/* 512k buffer */
		} else {
			/* Set unit type up as Tape */
			open_devip->ptype = TYPE_TAPE;
			sz = vtl_ssc_buffer_sz;	/* 256k buffer */
		}
		open_devip->status = 0;
		open_devip->status_argv = 0;
		open_devip->serial_no = (char *)NULL;

		base_p = vmalloc(sz);
		if (NULL == base_p) {
			printk(KERN_ERR
			"%s(): Allocate %ldbytes failed, out of memory\n",
					__func__, (unsigned long)sz);
			return -1;
		}
		memset(base_p, 0, sz);
		base_fifo = (struct kfifo *)kfifo_init( base_p, sz, GFP_KERNEL,
						 &open_devip->spin_in_progress);
		if (IS_ERR(base_fifo)) {
			printk(KERN_ERR
			"%s: Can not initialise kfifo memory at line %d\n",
				__func__, __LINE__);
			return -1;
		}
		/* Allocate memory for header buffer */
		open_devip->vtl_header = vmalloc(sizeof(struct vtl_header));
		if (NULL == open_devip->vtl_header) {
			printk(KERN_ERR
		"vtl_init: out of memory, Can not allocate header buffer\n");
			return -1;
		}
		/* Allocate default 64k read buffer */
		open_devip->rw_buf_sz = 65536;
		open_devip->rw_buf = vmalloc(open_devip->rw_buf_sz);
		if (NULL == open_devip->rw_buf) {
			printk(KERN_ERR
		"vtl_init: out of memory, Can not allocate read buffer\n");
			return -1;
		}

		/* Now set the storage pointer in the struct */
		open_devip->fifo = base_fifo;

		/* Make the current pointer to the start */
		open_devip->spin_in_progress = SPIN_LOCK_UNLOCKED;

		init_MUTEX(&open_devip->lock);

		if (VTL_OPT_NOISE & vtl_opts)
			printk("vtl: Allocated %ld bytes for fifo buffer\n",
								 (long)sz);

		memset(open_devip->sense_buff, 0, SDEBUG_SENSE_LEN);

		if (vtl_dsense)
			open_devip->sense_buff[0] = 0x72;
		else {
			open_devip->sense_buff[0] = 0x70;
			open_devip->sense_buff[7] = 0xa;
		}
		return 0;
	}
	return -1;
}

static int vtl_slave_configure(struct scsi_device *sdp)
{
	struct vtl_dev_info *devip;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: slave_configure <%u %u %u %u>\n",
		       sdp->host->host_no, sdp->channel, sdp->id, sdp->lun);
	if (sdp->host->max_cmd_len != VTL_MAX_CMD_LEN)
		sdp->host->max_cmd_len = VTL_MAX_CMD_LEN;
	devip = devInfoReg(sdp);
	sdp->hostdata = devip;
	if (sdp->host->cmd_per_lun)
		scsi_adjust_queue_depth(sdp, VTL_TAGGED_QUEUING,
					sdp->host->cmd_per_lun);
	return 0;
}

static void vtl_slave_destroy(struct scsi_device *sdp)
{
	struct vtl_dev_info *devip =
				(struct vtl_dev_info *)sdp->hostdata;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: slave_destroy <%u %u %u %u>\n",
		       sdp->host->host_no, sdp->channel, sdp->id, sdp->lun);
	if (devip) {
		/* make this slot avaliable for re-use */
		/* A little work to clean up after kfifo allocation */
		vfree(devip->serial_no);
		vfree(devip->fifo->buffer);
		kfree(devip->fifo);
		vfree(devip->vtl_header);

		devip->used = 0;
		sdp->hostdata = NULL;
	}
}

static struct vtl_dev_info *devInfoReg(struct scsi_device *sdev)
{
	struct vtl_host_info *vtl_host;
	struct vtl_dev_info *devip =
			(struct vtl_dev_info *)sdev->hostdata;

	if (devip)
		return devip;

	vtl_host = *(struct vtl_host_info **) sdev->host->hostdata;
	if (!vtl_host) {
		printk(KERN_ERR "Host info NULL\n");
		return NULL;
	}

	list_for_each_entry(devip, &vtl_host->dev_info_list, dev_list) {
		if ((devip->used) && (devip->channel == sdev->channel) &&
				(devip->target == sdev->id) &&
				(devip->lun == sdev->lun))
			return devip;
	}

	return NULL;
}

static void mk_sense_buffer(struct vtl_dev_info *devip, int key,
			    int asc, int asq)
{
	unsigned char *sbuff;

	sbuff = devip->sense_buff;
	memset(sbuff, 0, SDEBUG_SENSE_LEN);
	if (vtl_dsense) {
		sbuff[0] = 0x72;  /* descriptor, current */
		sbuff[1] = key;
		sbuff[2] = asc;
		sbuff[3] = asq;
	} else {
		sbuff[0] = 0x70;  /* fixed, current */
		sbuff[2] = key;
		sbuff[7] = 0xa;	  /* implies 18 byte sense buffer */
		sbuff[12] = asc;
		sbuff[13] = asq;
	}
	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl:    [sense_key,asc,ascq]: "
		      "[0x%x,0x%x,0x%x]\n", key, asc, asq);
}

static int vtl_abort(struct scsi_cmnd *SCpnt)
{
	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: abort\n");
	++num_aborts;
	stop_queued_cmnd(SCpnt);
	return SUCCESS;
}

static int vtl_device_reset(struct scsi_cmnd *SCpnt)
{
	struct vtl_dev_info *devip;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: device_reset\n");
	++num_dev_resets;
	if (SCpnt) {
		devip = devInfoReg(SCpnt->device);
		if (devip) {
			kfifo_reset(devip->fifo);
			devip->reset = 1;
		}
	}
	return SUCCESS;
}

static int vtl_bus_reset(struct scsi_cmnd *SCpnt)
{
	struct vtl_host_info *vtl_host;
	struct vtl_dev_info *dev_info;
	struct scsi_device *sdp;
	struct Scsi_Host *hp;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: bus_reset\n");
	++num_bus_resets;
	if (SCpnt && ((sdp = SCpnt->device)) && ((hp = sdp->host))) {
		vtl_host = *(struct vtl_host_info **) hp->hostdata;
		if (vtl_host) {
			list_for_each_entry(dev_info,
						&vtl_host->dev_info_list,
						dev_list)
			dev_info->reset = 1;
		}
	}
	return SUCCESS;
}

static int vtl_host_reset(struct scsi_cmnd *SCpnt)
{
	struct vtl_host_info *vtl_host;
	struct vtl_dev_info *dev_info;

	if (VTL_OPT_NOISE & vtl_opts)
	printk(KERN_INFO "vtl: host_reset\n");
	++num_host_resets;
	spin_lock(&vtl_host_list_lock);
	list_for_each_entry(vtl_host, &vtl_host_list, host_list) {
		list_for_each_entry(dev_info, &vtl_host->dev_info_list,
							dev_list)
		dev_info->reset = 1;
	}
	spin_unlock(&vtl_host_list_lock);
	stop_all_queued();
	return SUCCESS;
}

/* Returns 1 if found 'cmnd' and deleted its timer. else returns 0 */
static int stop_queued_cmnd(struct scsi_cmnd *cmnd)
{
	unsigned long iflags;
	int k;
	struct vtl_queued_cmd *sqcp;

	spin_lock_irqsave(&queued_arr_lock, iflags);
	for (k = 0; k < VTL_CANQUEUE; ++k) {
		sqcp = &queued_arr[k];
		if (sqcp->in_use && (cmnd == sqcp->a_cmnd)) {
			del_timer_sync(&sqcp->cmnd_timer);
			sqcp->in_use = 0;
			sqcp->a_cmnd = NULL;
			break;
		}
	}
	spin_unlock_irqrestore(&queued_arr_lock, iflags);
	return (k < VTL_CANQUEUE) ? 1 : 0;
}

/* Deletes (stops) timers of all queued commands */
static void stop_all_queued(void)
{
	unsigned long iflags;
	int k;
	struct vtl_queued_cmd *sqcp;

	spin_lock_irqsave(&queued_arr_lock, iflags);
	for (k = 0; k < VTL_CANQUEUE; ++k) {
		sqcp = &queued_arr[k];
		if (sqcp->in_use && sqcp->a_cmnd) {
			del_timer_sync(&sqcp->cmnd_timer);
			sqcp->in_use = 0;
			sqcp->a_cmnd = NULL;
		}
	}
	spin_unlock_irqrestore(&queued_arr_lock, iflags);
}

/* Initializes timers in queued array */
static void __init init_all_queued(void)
{
	unsigned long iflags;
	int k;
	struct vtl_queued_cmd *sqcp;

	spin_lock_irqsave(&queued_arr_lock, iflags);
	for (k = 0; k < VTL_CANQUEUE; ++k) {
		sqcp = &queued_arr[k];
		init_timer(&sqcp->cmnd_timer);
		sqcp->in_use = 0;
		sqcp->a_cmnd = NULL;
	}
	spin_unlock_irqrestore(&queued_arr_lock, iflags);
}

/* Set 'perm' (4th argument) to 0 to disable module_param's definition
 * of sysfs parameters (which module_param doesn't yet support).
 * Sysfs parameters defined explicitly below.
 */
module_param_named(add_host, vtl_add_host, int, 0); /* perm=0644 */
module_param_named(set_serial, vtl_set_serial_num, int, 0); /* perm=0644 */
module_param_named(set_firmware, vtl_set_firmware, int, 0); /* perm=0644 */
module_param_named(ssc_buffer_sz, vtl_ssc_buffer_sz, int, 0); /* perm=0644 */
module_param_named(delay, vtl_delay, int, 0); /* perm=0644 */
module_param_named(dsense, vtl_dsense, int, 0);
module_param_named(every_nth, vtl_every_nth, int, 0);
module_param_named(max_luns, vtl_max_luns, int, 0);
module_param_named(num_tgts, vtl_num_tgts, int, 0);
module_param_named(opts, vtl_opts, int, 0); /* perm=0644 */
module_param_named(scsi_level, vtl_scsi_level, int, 0);

MODULE_AUTHOR("Eric Youngdale + Douglas Gilbert + Mark Harvey");
MODULE_DESCRIPTION("SCSI vtl adapter driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(VTL_VERSION);

MODULE_PARM_DESC(add_host, "0..127 hosts allowed(def=1)");
MODULE_PARM_DESC(set_serial, "num SerialNum");
MODULE_PARM_DESC(set_firmware, "num firmware");
MODULE_PARM_DESC(ssc_buffer_sz, "ssc buffer size(def=262144)");
MODULE_PARM_DESC(delay, "# of jiffies to delay response(def=1)");
MODULE_PARM_DESC(dsense, "use descriptor sense format(def: fixed)");
MODULE_PARM_DESC(every_nth, "timeout every nth command(def=100)");
MODULE_PARM_DESC(max_luns, "number of SCSI LUNs per target to simulate");
MODULE_PARM_DESC(num_tgts, "number of SCSI targets per host to simulate");
MODULE_PARM_DESC(opts, "1->noise, 2->medium_error, 4->...");
MODULE_PARM_DESC(scsi_level, "SCSI level to simulate(def=5[SPC-3])");


static char vtl_parm_info[256];

static const char *vtl_info(struct Scsi_Host *shp)
{
	sprintf(vtl_parm_info, "vtl: version %s [%s], "
		"opts=0x%x", VTL_VERSION,
		vtl_version_date, vtl_opts);
	return vtl_parm_info;
}

/* vtl_proc_info
 * Used if the driver currently has no own support for /proc/scsi
 */
static int vtl_proc_info(struct Scsi_Host *host, char *buffer,
			 char **start, off_t offset, int length, int inout)
{
	int len, pos, begin;
	int orig_length;

	orig_length = length;

	if (inout == 1) {
		char arr[16];
		int minLen = length > 15 ? 15 : length;

		if (!capable(CAP_SYS_ADMIN) || !capable(CAP_SYS_RAWIO))
			return -EACCES;
		memcpy(arr, buffer, minLen);
		arr[minLen] = '\0';
		if (1 != sscanf(arr, "%d", &pos))
			return -EINVAL;
		vtl_opts = pos;
		if (vtl_every_nth != 0)
			vtl_cmnd_count = 0;
		return length;
	}
	begin = 0;
	pos = len = sprintf(buffer, "vtl adapter driver, version "
	    "%s [%s]\n"
	    "num_tgts=%d, opts=0x%x, "
	    "every_nth=%d(curr:%d)\n"
	    "delay=%d, max_luns=%d\n"
            "firmware=%d, scsi_level=%d\n"
	    "number of aborts=%d, device_reset=%d, bus_resets=%d, "
	    "host_resets=%d\n",
	    VTL_VERSION, vtl_version_date, vtl_num_tgts,
	    vtl_opts, vtl_every_nth,
	    vtl_cmnd_count, vtl_delay,
	    vtl_max_luns,
	    vtl_set_firmware, vtl_scsi_level,
	    num_aborts, num_dev_resets, num_bus_resets, num_host_resets);
	if (pos < offset) {
		len = 0;
		begin = pos;
	}
	*start = buffer + (offset - begin);	/* Start of wanted data */
	len -= (offset - begin);
	if (len > length)
		len = length;
	return len;
}

static ssize_t vtl_delay_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_delay);
}

static ssize_t vtl_delay_store(struct device_driver *ddp,
				  const char *buf, size_t count)
{
	int delay;
	char work[20];

	if (1 == sscanf(buf, "%10s", work)) {
		if ((1 == sscanf(work, "%d", &delay)) && (delay >= 0)) {
			vtl_delay = delay;
			return count;
		}
	}
	return -EINVAL;
}
DRIVER_ATTR(delay, S_IRUGO | S_IWUSR, vtl_delay_show,
	    vtl_delay_store);

static ssize_t vtl_opts_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%x\n", vtl_opts);
}

static ssize_t vtl_opts_store(struct device_driver *ddp,
				 const char *buf, size_t count)
{
	int opts;
	char work[20];

	if (1 == sscanf(buf, "%10s", work)) {
		if (0 == strnicmp(work,"0x", 2)) {
			if (1 == sscanf(&work[2], "%x", &opts))
				goto opts_done;
		} else {
			if (1 == sscanf(work, "%d", &opts))
				goto opts_done;
		}
	}
	return -EINVAL;
opts_done:
	vtl_opts = opts;
	vtl_cmnd_count = 0;
	return count;
}
DRIVER_ATTR(opts, S_IRUGO | S_IWUSR, vtl_opts_show,
	    vtl_opts_store);

static ssize_t vtl_dsense_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_dsense);
}
static ssize_t vtl_dsense_store(struct device_driver *ddp,
				  const char *buf, size_t count)
{
	int n;

	if ((count > 0) && (1 == sscanf(buf, "%d", &n)) && (n >= 0)) {
		vtl_dsense = n;
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(dsense, S_IRUGO | S_IWUSR, vtl_dsense_show,
	    vtl_dsense_store);

static ssize_t vtl_num_tgts_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_num_tgts);
}
static ssize_t vtl_num_tgts_store(struct device_driver *ddp,
				     const char *buf, size_t count)
{
	int n;

	if ((count > 0) && (1 == sscanf(buf, "%d", &n)) && (n >= 0)) {
		vtl_num_tgts = n;
		vtl_max_tgts_luns();
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(num_tgts, S_IRUGO | S_IWUSR, vtl_num_tgts_show,
	    vtl_num_tgts_store);


static ssize_t vtl_every_nth_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_every_nth);
}
static ssize_t vtl_every_nth_store(struct device_driver *ddp,
				      const char *buf, size_t count)
{
	int nth;

	if ((count > 0) && (1 == sscanf(buf, "%d", &nth))) {
		vtl_every_nth = nth;
		vtl_cmnd_count = 0;
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(every_nth, S_IRUGO | S_IWUSR, vtl_every_nth_show,
	    vtl_every_nth_store);

static ssize_t vtl_max_luns_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_max_luns);
}
static ssize_t vtl_max_luns_store(struct device_driver *ddp,
				     const char *buf, size_t count)
{
	int n;

	if ((count > 0) && (1 == sscanf(buf, "%d", &n)) && (n >= 0)) {
		vtl_max_luns = n;
		vtl_max_tgts_luns();
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(max_luns, S_IRUGO | S_IWUSR, vtl_max_luns_show,
	    vtl_max_luns_store);

static ssize_t vtl_scsi_level_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_scsi_level);
}
DRIVER_ATTR(scsi_level, S_IRUGO, vtl_scsi_level_show, NULL);

static ssize_t vtl_show_ssc_buffer_sz(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_ssc_buffer_sz);
}
static ssize_t vtl_set_ssc_buffer_sz(struct device_driver *ddp,
				     const char *buf, size_t count)
{
	int buffer_sz;
	int retval;

	retval = sscanf(buf, "%d", &buffer_sz);

	if (retval == 1) {
		if ((buffer_sz < 65536) || (buffer_sz > 512000)) {
			printk("Buffersize out of range: %d", buffer_sz);
			return -EINVAL;
		}
		printk("Setting buffer size %d\n", buffer_sz);
		vtl_ssc_buffer_sz = buffer_sz;
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(ssc_buffer_sz, S_IRUGO | S_IWUSR, vtl_show_ssc_buffer_sz, 
	    vtl_set_ssc_buffer_sz);


static ssize_t vtl_serial_num_show(struct device_driver *ddp, char *buf)
{
	if (vtl_serial_prefix)
		return scnprintf(buf, PAGE_SIZE, "%s\n", vtl_serial_prefix);
	return scnprintf(buf, PAGE_SIZE, "%s\n", "Dynamic");
}

static ssize_t vtl_serial_num_store(struct device_driver *ddp,
				     const char *buf, size_t count)
{
	int retval;
	char work[20];

	if (count > 20) {
		printk("Serial number too long\n");
		return -EINVAL;
	}

	retval = sscanf(buf, "%10s", work);

	if (retval == 1) {
		if (vtl_serial_prefix) {
			printk("Serial prefix already set to %s\n",
							vtl_serial_prefix);
			return -EINVAL;
		}
		vtl_serial_prefix = kmalloc(strlen(work) + 1, GFP_KERNEL);
		strcpy(vtl_serial_prefix, work);
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(serial_prefix, S_IRUGO | S_IWUSR, vtl_serial_num_show, 
	    vtl_serial_num_store);

static ssize_t vtl_firmware_show(struct device_driver *ddp, char *buf)
{
	if (vtl_firmware)
		return scnprintf(buf, PAGE_SIZE, "%s\n", vtl_firmware);
	return scnprintf(buf, 4, "%s\n", inq_product_rev);
}

static ssize_t vtl_firmware_store(struct device_driver *ddp,
				     const char *buf, size_t count)
{
	int retval;
	char work[8];

	if (count > 6) {
		printk("Firmware number too long\n");
		return -EINVAL;
	}

	retval = sscanf(buf, "%6s", work);

	if (retval == 1) {
		if (vtl_firmware) {
			printk("Serial prefix already set to %s\n",
							vtl_firmware);
			return -EINVAL;
		}
		vtl_firmware = kmalloc(strlen(work) + 1, GFP_KERNEL);
		strcpy(vtl_firmware, work);
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(firmware, S_IRUGO | S_IWUSR, vtl_firmware_show, 
	    vtl_firmware_store);

static ssize_t vtl_add_host_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_add_host);
}

static ssize_t vtl_add_host_store(struct device_driver *ddp,
				     const char *buf, size_t count)
{
	int delta_hosts;
	char work[20];

	if (1 != sscanf(buf, "%10s", work))
		return -EINVAL;
	{	/* temporary hack around sscanf() problem with -ve nums */
		int neg = 0;

		if ('-' == *work)
			neg = 1;
		if (1 != sscanf(work + neg, "%d", &delta_hosts))
			return -EINVAL;
		if (neg)
			delta_hosts = -delta_hosts;
	}
	if (delta_hosts > 0) {
		do {
			vtl_add_adapter();
		} while (--delta_hosts);
	} else if (delta_hosts < 0) {
		do {
			vtl_remove_adapter();
		} while (++delta_hosts);
	}
	return count;
}
DRIVER_ATTR(add_host, S_IRUGO | S_IWUSR, vtl_add_host_show, 
	    vtl_add_host_store);

static int do_create_driverfs_files(void)
{
	int	ret;
	ret = driver_create_file(&vtl_driverfs_driver, &driver_attr_add_host);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_firmware);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_serial_prefix);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_ssc_buffer_sz);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_delay);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_every_nth);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_max_luns);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_num_tgts);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_opts);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_scsi_level);
	return ret;
}

static void do_remove_driverfs_files(void)
{
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_scsi_level);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_opts);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_num_tgts);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_max_luns);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_every_nth);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_delay);
	kfree(vtl_firmware);
	kfree(vtl_serial_prefix);
	vtl_firmware = NULL;
	vtl_serial_prefix = NULL;
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_ssc_buffer_sz);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_serial_prefix);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_firmware);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_add_host);
}

static int allocate_minor_no(struct vtl_dev_info *devip)
{
	int a = 0;

	for(a=0; a < DEF_MAX_MINOR_NO; a++) {
		if (devp[a] == 0) {
			devp[a] = devip;
			break;
		}
	}
	return a;
}

static int __init vtl_init(void)
{
	int host_to_add;
	int k;
	int	ret;

	vtl_Major = register_chrdev(vtl_Major, "vtl", &vtl_fops);
	if (vtl_Major < 0) {
		printk(KERN_WARNING "vtl: can't get major number\n");
		return vtl_Major;
	}

	init_all_queued();

	ret = device_register(&pseudo_primary);
	if (ret < 0) {
		printk(KERN_WARNING "vtl: device_register error: %d\n", ret);
		goto dev_unreg;
	}
	ret = bus_register(&pseudo_lld_bus);
	if (ret < 0) {
		printk(KERN_WARNING "vtl: bus_register error: %d\n", ret);
		goto bus_unreg;
	}
	ret = driver_register(&vtl_driverfs_driver);
	if (ret < 0) {
		printk(KERN_WARNING "vtl: driver_register error: %d\n", ret);
		goto driver_unreg;
	}
	ret = do_create_driverfs_files();
	if (ret < 0) {
		printk(KERN_WARNING "vtl: driver_create_file error: %d\n", ret);
		goto del_files;
	}

	vtl_driver_template.proc_name = (char *)vtl_driver_name;

	host_to_add = vtl_add_host;
	vtl_add_host = 0;

	if (vtl_set_firmware > 0xffff) {
		printk(KERN_ERR "VTL Firmware larger the 0xffff - setting to default\n");
		vtl_set_firmware = VTL_FIRMWARE;
	}
	snprintf(inq_product_rev, 5, "%04x", vtl_set_firmware);
	for (k = 0; k < host_to_add; k++) {
		if (vtl_add_adapter()) {
			printk(KERN_ERR "vtl_init: "
				"vtl_add_adapter failed k=%d\n", k);
			break;
		}
	}

	if (VTL_OPT_NOISE & vtl_opts) {
		printk(KERN_INFO "vtl_init: built %d host(s)\n",
		       vtl_add_host);
	}
	return 0;
del_files:
	do_remove_driverfs_files();
driver_unreg:
	driver_unregister(&vtl_driverfs_driver);
bus_unreg:
	bus_unregister(&pseudo_lld_bus);
dev_unreg:
	device_unregister(&pseudo_primary);

	return ret;
}

static void __exit vtl_exit(void)
{
	int k = vtl_add_host;

	stop_all_queued();
	for (; k; k--)
		vtl_remove_adapter();
	do_remove_driverfs_files();
	driver_unregister(&vtl_driverfs_driver);
	bus_unregister(&pseudo_lld_bus);
	device_unregister(&pseudo_primary);
	unregister_chrdev(vtl_Major, "vtl");
}

device_initcall(vtl_init);
module_exit(vtl_exit);

void pseudo_0_release(struct device *dev)
{
	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: pseudo_0_release() called\n");
}

static struct device pseudo_primary = {
	.bus_id		= "pseudo_0",
	.release	= pseudo_0_release,
};

static int pseudo_lld_bus_match(struct device *dev,
				struct device_driver *dev_driver)
{
	return 1;
}

static struct bus_type pseudo_lld_bus = {
	.name = "pseudo",
	.match = pseudo_lld_bus_match,
};

static void vtl_release_adapter(struct device *dev)
{
	struct vtl_host_info *vtl_host;

	vtl_host = to_vtl_host(dev);
	kfree(vtl_host);
}

static int vtl_add_adapter(void)
{
	int k, devs_per_host;
	int error = 0;
	struct vtl_host_info *vtl_host;
	struct vtl_dev_info *vtl_devinfo;
	struct list_head *lh, *lh_sf;

	vtl_host = kmalloc(sizeof(*vtl_host),GFP_KERNEL);

	if (NULL == vtl_host) {
		printk(KERN_ERR "%s: out of memory at line %d\n",
						__func__, __LINE__);
	return -ENOMEM;
	}

	memset(vtl_host, 0, sizeof(*vtl_host));
	INIT_LIST_HEAD(&vtl_host->dev_info_list);

	devs_per_host = vtl_num_tgts * vtl_max_luns;
	for (k = 0; k < devs_per_host; k++) {
		vtl_devinfo = kmalloc(sizeof(*vtl_devinfo),GFP_KERNEL);
		if (NULL == vtl_devinfo) {
			printk(KERN_ERR "%s: out of memory at line %d\n",
						__func__, __LINE__);
			error = -ENOMEM;
			goto clean;
		}
		memset(vtl_devinfo, 0, sizeof(*vtl_devinfo));
		vtl_devinfo->vtl_host = vtl_host;
		list_add_tail(&vtl_devinfo->dev_list,
						&vtl_host->dev_info_list);
	}

	spin_lock(&vtl_host_list_lock);
	list_add_tail(&vtl_host->host_list, &vtl_host_list);
	spin_unlock(&vtl_host_list_lock);

	vtl_host->dev.bus = &pseudo_lld_bus;
	vtl_host->dev.parent = &pseudo_primary;
	vtl_host->dev.release = &vtl_release_adapter;
	sprintf(vtl_host->dev.bus_id, "adapter%d", vtl_add_host);

	error = device_register(&vtl_host->dev);

	if (error)
		goto clean;

	++vtl_add_host;
	return error;

	clean:
	list_for_each_safe(lh, lh_sf, &vtl_host->dev_info_list) {
		vtl_devinfo = list_entry(lh, struct vtl_dev_info, dev_list);
		list_del(&vtl_devinfo->dev_list);
		kfree(vtl_devinfo);
	}

	kfree(vtl_host);
	return error;
}

static void vtl_remove_adapter(void)
{
	struct vtl_host_info *vtl_host = NULL;

	spin_lock(&vtl_host_list_lock);
	if (!list_empty(&vtl_host_list)) {
		vtl_host = list_entry(vtl_host_list.prev,
					struct vtl_host_info, host_list);
		list_del(&vtl_host->host_list);
	}
	spin_unlock(&vtl_host_list_lock);

	if (!vtl_host)
		return;

	device_unregister(&vtl_host->dev);
	--vtl_add_host;
}

static int vtl_driver_probe(struct device *dev)
{
	int error = 0;
	struct vtl_host_info *vtl_host;
	struct Scsi_Host *hpnt;

	vtl_host = to_vtl_host(dev);

	hpnt = scsi_host_alloc(&vtl_driver_template, sizeof(vtl_host));
	if (NULL == hpnt) {
		printk(KERN_ERR "%s: scsi_register failed\n", __func__);
		error = -ENODEV;
		return error;
	}

	vtl_host->shost = hpnt;
	*((struct vtl_host_info **)hpnt->hostdata) = vtl_host;
	if ((hpnt->this_id >= 0) && (vtl_num_tgts > hpnt->this_id))
		hpnt->max_id = vtl_num_tgts + 1;
	else
		hpnt->max_id = vtl_num_tgts;
	hpnt->max_lun = vtl_max_luns;

	error = scsi_add_host(hpnt, &vtl_host->dev);
	if (error) {
		printk(KERN_ERR "%s: scsi_add_host failed\n", __func__);
		error = -ENODEV;
		scsi_host_put(hpnt);
	} else
		scsi_scan_host(hpnt);

	return error;
}

static int vtl_driver_remove(struct device *dev)
{
	struct list_head *lh, *lh_sf;
	struct vtl_host_info *vtl_host;
	struct vtl_dev_info *vtl_devinfo;

	vtl_host = to_vtl_host(dev);

	if (!vtl_host) {
		printk(KERN_ERR "%s: Unable to locate host info\n",
		       __func__);
		return -ENODEV;
	}

	scsi_remove_host(vtl_host->shost);

	list_for_each_safe(lh, lh_sf, &vtl_host->dev_info_list) {
		vtl_devinfo = list_entry(lh, struct vtl_dev_info,
					dev_list);
		list_del(&vtl_devinfo->dev_list);
		kfree(vtl_devinfo);
	}

	scsi_host_put(vtl_host->shost);
	return 0;
}

static void vtl_max_tgts_luns(void)
{
	struct vtl_host_info *vtl_host;
	struct Scsi_Host *hpnt;

	spin_lock(&vtl_host_list_lock);
	list_for_each_entry(vtl_host, &vtl_host_list, host_list) {
		hpnt = vtl_host->shost;
		if ((hpnt->this_id >= 0) &&
		    (vtl_num_tgts > hpnt->this_id))
			hpnt->max_id = vtl_num_tgts + 1;
		else
			hpnt->max_id = vtl_num_tgts;
		hpnt->max_lun = vtl_max_luns;
	}
	spin_unlock(&vtl_host_list_lock);
}

/*
 *******************************************************************
 * Char device driver routines
 *******************************************************************
 */

/*
 * char device ioctl entry point
 */
static int vtl_c_ioctl(struct inode *inode, struct file *file,
					unsigned int cmd, unsigned long arg)
{
	unsigned int minor = iminor(inode);
	struct vtl_queued_cmd *sqcp = NULL;
	unsigned int  k;
	int ret = 0;
	unsigned char s[4];	/* Serial Number & sense data buffer */
	unsigned char valid_sense;
	unsigned long serial_no;
	unsigned int  count;
	char *sn;
	struct vtl_header *vheadp;

	if (minor > DEF_MAX_MINOR_NO) {	/* Check limit minor no. */
		return -ENODEV;
	}

	if (NULL == devp[minor]) {
		return -ENODEV;
	}

	if (down_interruptible(&devp[minor]->lock))
		return -ERESTARTSYS;

	ret = 0;

	switch (cmd) {
	/* Online */
	case 0x80:
		get_user(devp[minor]->status_argv, (unsigned int *)arg);
		/* Only like types (SSC / medium changer) */
		if (devp[minor]->status_argv != devp[minor]->ptype) {
			printk("devp[%d]->ptype: %d\n",
					 	minor, devp[minor]->ptype);
			ret = -ENODEV;
			goto give_up;
		}
		devp[minor]->device_offline = 0;
		printk(KERN_INFO "vtl%d: Online ptype(%d)\n",
					minor, devp[minor]->status_argv);
		break;

	/* Offline */
	case 0x81:
		devp[minor]->device_offline = 1;
		printk(KERN_INFO "vtl%d: Offline\n", minor);
		break;

	/* read size of fifo buffer */
	case 0x82:
		put_user(devp[minor]->fifo->size, (unsigned int *)arg);
		printk(KERN_INFO "vtl%d: fifo buffer %d bytes\n",
					    minor, devp[minor]->fifo->size);
		break;

	/* ioctl poll -> return status of vtl driver */
	case 0x83:
		/*
		 SCSI code updates status
			- which this ioctl passes to user space
		*/
		put_user(devp[minor]->status_argv, (unsigned int *)arg);
		ret = devp[minor]->status;
		break;

	/* Ack status poll */
	case 0x84:
		get_user(devp[minor]->status_argv, (unsigned int *)arg);
		devp[minor]->status = 0;
		break;

	/*
	 * Finish up the SCSI command.
	 * - 1. Total number of bytes to read from user space is passed
	 *	in status_argv.
	 * - 2. Read S/No. of the SCSI cmd (4 bytes).
	 * - 3. Read 1 byte - 1 = Request Sense, 0 = No Sense
	 * - 4. Read any more data and place into SCSI data buffer
	 * - 5. Delete the timer & clear the queued_arr[] entry.
	 */
	case 0x184:	/* VX_FINISH_SCSI_COMMAND */
		get_user(count, (unsigned int *)arg);
		/* Read in SCSI cmd s/no. */
		kfifo_get(devp[minor]->fifo, s, 4);
		serial_no =	(s[0] << 24) +
				(s[1] << 16) +
				(s[2] <<  8) +
				 s[3];

		/* Valid sense ?? */
		kfifo_get(devp[minor]->fifo, &valid_sense, 1);

		count -= 5;

		/* While size of SCSI read is larger than our buffer */
		if (count > devp[minor]->rw_buf_sz) {
			while(count > devp[minor]->rw_buf_sz)
				/* Double size */
			       devp[minor]->rw_buf_sz += devp[minor]->rw_buf_sz;

			/* Free old buffer */
			vfree(devp[minor]->rw_buf);
			devp[minor]->rw_buf = vmalloc(devp[minor]->rw_buf_sz);
			if (NULL == devp[minor]->rw_buf) {
				ret = -ENOMEM;
				goto give_up;
			}
			printk("New read/write buffer size: %d\n",
							devp[minor]->rw_buf_sz);
		}

		for (k = 0; k < VTL_CANQUEUE; ++k) {
			sqcp = &queued_arr[k];
			if (sqcp->in_use)
				if (sqcp->a_cmnd->serial_number == serial_no)
					break;
		}
		if (k >= VTL_CANQUEUE) {
			printk(KERN_WARNING "c_ioctl: callback function not"
						" found.: k = %d\n", k);
			ret = 1;	/* report busy to mid level */
			goto give_up;
		}
		if (NULL == sqcp) {
			printk("FATAL %s, line %d: sqcp is NULL\n",
							__func__, __LINE__);
		} else {
			if (count)
				fill_from_dev_buffer(sqcp->a_cmnd, NULL, count,
							devp[minor]->fifo);
			del_timer_sync(&sqcp->cmnd_timer);
			sqcp->in_use = 0;

			if (VTL_OPT_NOISE & vtl_opts)
				printk("%s(), line %d: auto sense: %s\n",
						__func__, __LINE__,
						(valid_sense) ? "Yes" : "No");
			if (valid_sense) {
				sqcp->a_cmnd->result = check_condition_result;
				// Automagically grab SENSE data
				kfifo_get(devp[minor]->fifo,
						sqcp->a_cmnd->sense_buffer,
						SENSE_BUF_SIZE);
			} else {
				sqcp->a_cmnd->result = DID_OK << 16;
			}

			if (NULL != sqcp->done_funct)
				sqcp->done_funct(sqcp->a_cmnd);
			else
				printk("FATAL %s, line %d: SCSI done_funct"
						" callback => NULL\n",
						__func__, __LINE__);
		}
		devp[minor]->status = 0;
		break;

	/* Clear 'command pending' status
	 *        i.e. - userspace daemon acknowledged it has read
	 *		 SCSI cmnd
	 */
	case 0x185:	/* VX_ACK_SCSI_CDB */
		get_user(devp[minor]->status_argv, (unsigned int *)arg);
		devp[minor]->status = 0;
		break;

	/*
	 * c_ioctl > 200 are new & improved interface
	 * where we no longer depend on the kfifo (and hence remove an
	 * extra copy of data)
	 */
	case 0x200:	/* VTL_GET_HEADER - Read SCSI header + S/No. */
		vheadp = (struct vtl_header *)devp[minor]->vtl_header;
		if (copy_to_user((u8 *)arg, (u8 *)vheadp,
					 sizeof(struct vtl_header))) {
			ret = -EFAULT;
			goto give_up;
		}
		break;
	case 0x201:	/* VTL_GET_DATA */
		ret = -ENOTTY;
		goto give_up;
	case 0x202:	/* Copy 'Serial Number' from userspace */
		sn = vmalloc(32);
		if (!sn) {
			printk("%s out of memory\n", __func__);
			ret = -ENOMEM;
			goto give_up;
		}
		vfree(devp[minor]->serial_no);
		devp[minor]->serial_no = sn;
		if (copy_from_user(sn, (u8 *)arg, 32)) {
			ret = -EFAULT;
			goto give_up;
		}
		if (strlen(sn) < 2) {
			printk("Serial number too short. Removing\n");
			vfree(sn);
			devp[minor]->serial_no = (char *)NULL;
		}
		if (VTL_OPT_NOISE & vtl_opts)
			printk("Setting serial number to %s\n", sn);
		break;
	case 0x203:	/* VTL_PUT_DATA */
		ret = -ENOTTY;
		goto give_up;
	default:
		ret = -ENOTTY;
		goto give_up;
	}

give_up:
	up(&devp[minor]->lock);
	return ret;
}

static ssize_t vtl_read(struct file *filp, char *buf, size_t count,
						loff_t *ppos)
{
	unsigned int minor = iminor(filp->f_dentry->d_inode);
	size_t cnt = 0;
	unsigned char c;
	struct kfifo *p;

	p = devp[minor]->fifo;
	if (down_interruptible(&devp[minor]->lock))
		return -ERESTARTSYS;

	while(cnt < count &&
		(kfifo_get(p, &c, sizeof(c)) == sizeof(c))) {
// FIXME: Should use 'copy_from_user()/copy_to_user() instead of char at a time
			if (put_user(c, buf++)) {
				up(&devp[minor]->lock);
				return -EFAULT;
			}
		cnt++;
	}
	up(&devp[minor]->lock);
	return cnt;
}

static ssize_t vtl_write(struct file *filp, const char *buf, size_t count,
						loff_t *ppos)
{
	unsigned int minor = iminor(filp->f_dentry->d_inode);
	size_t	cnt = 0;
	unsigned char c;
	int	ret;

	if (down_interruptible(&devp[minor]->lock))
		return -ERESTARTSYS;

	while(cnt < count) {
		if (get_user(c, buf++))
			return -EFAULT;
		ret = kfifo_put(devp[minor]->fifo, &c, sizeof(c));
		if (ret != sizeof(c)) {
			up(&devp[minor]->lock);
			return -EFAULT;
		}
		cnt++;
	}
	up(&devp[minor]->lock);
	return cnt;
}

static int vtl_release(struct inode *inode, struct file *filp)
{
	unsigned int minor = iminor(inode);
	if (VTL_OPT_NOISE & vtl_opts)
		printk("vtl%d: Release\n", minor);
	devp[minor]->device_offline = 1;
	return 0;
}

static int vtl_open(struct inode *inode, struct file *filp)
{
	unsigned int minor = iminor(inode);
	if (devp[minor] == 0) {
		printk("Attempt to open vtl%d failed: No such device\n", minor);
		return -EBUSY;
	} else
		return 0;
}


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,10)
#include "kfifo.c"
#endif
