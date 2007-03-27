/* 
 *  linux/kernel/vtl.c
 *
 *  Copyright (C) 1992  Eric Youngdale
 *  Simulate a host adapter with 2 disks attached.  Do a lot of checking
 *  to make sure that we are not getting blocks mixed up, and PANIC if
 *  anything out of the ordinary is seen.
 *
 *  This version is more generic, simulating a variable number of disk 
 *  (or disk like devices) sharing a common amount of RAM (default 8 MB
 *  but can be set at driver/module load time).
 *
 *  For documentation see http://www.torque.net/sg/sdebug.html
 *
 *   D. Gilbert (dpg) work for Magneto-Optical device test [20010421]
 *   dpg: work for devfs large number of disks [20010809]
 *        use vmalloc() more inquiry+mode_sense [20020302]
 *        add timers for delayed responses [20020721]
 *
 *   Mark Harvey. Modified as a Virtual Tape Library.
 *
 *      Hack-rip-tear Eric & Dougs good work...
 *
 *      Modified by removing most of the kernel code used to return the
 *      scsi responses, attached a char device back-end and passing
 *      the scsi commands to user-space daemons instead.
 *
 *      These user daemons act as the SCSI targets - SSC and SMC targets
 *      working at the moment.
 *
 *	$Id: vtl.c,v 1.11.2.1 2006-08-06 07:59:11 markh Exp $
 */

#include <linux/config.h>
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

#include <asm/io.h>
#include <asm/uaccess.h>

#include <linux/blk.h>
#include "scsi.h"
#include "hosts.h"

#include <linux/stat.h>

#ifndef LINUX_VERSION_CODE
#include <linux/version.h>
#endif

#include "vtl.h"

// static const char * vtl_version_str = "Version: 0.61 (20020815)";
static const char * vtl_version_str = "Version: 0.01 (20060423-5)";

/* Dynamically assigned Major number */
unsigned int	major = 0;
static char vtl_driver_name[] = "vtl";

#define SAM_STAT_CHECK_CONDITION 0x2

static const int check_condition_result =
	(DRIVER_SENSE << 24) | SAM_STAT_CHECK_CONDITION;

#ifndef SCSI_CMD_READ_16
#define SCSI_CMD_READ_16 0x88
#endif
#ifndef SCSI_CMD_WRITE_16
#define SCSI_CMD_WRITE_16 0x8a
#endif
#ifndef REPORT_LUNS
#define REPORT_LUNS 0xa0
#endif

/* A few options that we want selected */
#define DEF_NR_FAKE_DEVS   1
#define DEF_DEV_SIZE_MB   8
#define DEF_FAKE_BLK0   0
#define DEF_EVERY_NTH   100
#define DEF_DELAY   1

#define DEF_OPTS   1
#define VTL_OPT_NOISE   1
#define VTL_OPT_MEDIUM_ERR   2
#define VTL_OPT_EVERY_NTH   4

#define OPT_MEDIUM_ERR_ADDR   0x1234

static int vtl_num_devs = DEF_NR_FAKE_DEVS;
static int vtl_opts = DEF_OPTS;
static int vtl_every_nth = DEF_EVERY_NTH;
static int vtl_cmnd_count = 0;
static int vtl_delay = DEF_DELAY;

#define NR_HOSTS_PRESENT (((vtl_num_devs - 1) / 7) + 1)
#define N_HEAD          8
#define N_SECTOR        32
#define DEV_READONLY(TGT)      (0)
#define DEV_REMOVEABLE(TGT)    (0)
#define PERIPH_DEVICE_TYPE(TGT) (TYPE_TAPE);

static int vtl_dev_size_mb = DEF_DEV_SIZE_MB;
#define STORE_SIZE (vtl_dev_size_mb * 1024 * 1024)
#define DEF_MAX_MINOR_NO 255

/* default sector size is 512 bytes, 2**9 bytes */
#define POW2_SECT_SIZE 9
#define SECT_SIZE (1 << POW2_SECT_SIZE)

#define N_CYLINDER (STORE_SIZE / (SECT_SIZE * N_SECTOR * N_HEAD))

/* Time to wait before completing a command */
#define CAPACITY (N_HEAD * N_SECTOR * N_CYLINDER)
#define SECT_SIZE_PER(TGT) SECT_SIZE

#define SDEBUG_SENSE_LEN 32

struct target_data {
	u8	* SCp;
	u8	* buf;
};

struct sdebug_dev_info {
	Scsi_Device	* sdp;
	unsigned char	sense_buff[SDEBUG_SENSE_LEN];	/* weak nexus */
	char		reset;
	char		device_offline;
	unsigned int	minor;
	unsigned int	ptype;
	unsigned int	status;
	unsigned int	status_argv;

	struct target_data target;
	spinlock_t	spin_in_progress;
};
static struct sdebug_dev_info * devInfop;

typedef void (* done_funct_t) (Scsi_Cmnd *);

struct sdebug_queued_cmd {
	int in_use;
	struct timer_list cmnd_timer;
	done_funct_t done_funct;
	struct scsi_cmnd * a_cmnd;
	int scsi_result;
};
static struct sdebug_queued_cmd queued_arr[VTL_CANQUEUE];

static unsigned char * fake_storep;	/* ramdisk storage */

static unsigned char broken_buff[SDEBUG_SENSE_LEN];

static int num_aborts = 0;
static int num_dev_resets = 0;
static int num_bus_resets = 0;
static int num_host_resets = 0;

static spinlock_t queued_arr_lock = SPIN_LOCK_UNLOCKED;
static rwlock_t atomic_rw = RW_LOCK_UNLOCKED;


/* function declarations */
static int resp_inquiry(unsigned char * cmd, int target, unsigned char * buff,
			int bufflen, struct sdebug_dev_info * devip);
static int resp_mode_sense(unsigned char * cmd, int target,
			   unsigned char * buff, int bufflen,
			   struct sdebug_dev_info * devip);
static int resp_read(Scsi_Cmnd * SCpnt, int upper_blk, int block, 
		     int num, struct sdebug_dev_info * devip);
static int resp_write(Scsi_Cmnd * SCpnt, int upper_blk, int block, int num,
		      struct sdebug_dev_info * devip);
static int resp_report_luns(unsigned char * cmd, unsigned char * buff,
			    int bufflen, struct sdebug_dev_info * devip);
static void timer_intr_handler(unsigned long);
static struct sdebug_dev_info * devInfoReg(Scsi_Device * sdp);
static void mk_sense_buffer(struct sdebug_dev_info * devip, int key, 
			    int asc, int asq, int inbandLen);
static int check_reset(Scsi_Cmnd * SCpnt, struct sdebug_dev_info * devip);
static int schedule_resp(struct scsi_cmnd * cmnd, 
			 struct sdebug_dev_info * devip, 
			 done_funct_t done, int scsi_result, int delta_jiff);
static void init_all_queued(void);
static void stop_all_queued(void);
static int stop_queued_cmnd(struct scsi_cmnd * cmnd);
static int inquiry_evpd_83(unsigned char * arr, int dev_id_num,
                           const char * dev_id_str, int dev_id_str_len);

static Scsi_Host_Template driver_template = VTL_TEMPLATE;

static struct file_operations vtl_fops = {
	write:	vtl_write,
	read:	vtl_read,
	ioctl:	vtl_c_ioctl,
	open:	vtl_open,
	release: vtl_c_release,
};


static
int vtl_queuecommand(Scsi_Cmnd * SCpnt, done_funct_t done)
{
	unsigned char *cmd = (unsigned char *) SCpnt->cmnd;
	int block;
	int upper_blk;
	unsigned char *buff;
	int errsts = 0;
	int target = SCpnt->target;
	int bufflen = SCpnt->request_bufflen;
	int num, capac;
	struct sdebug_dev_info * devip = NULL;
	unsigned char * sbuff;

	if (done == NULL)
		return 0;	/* assume mid level reprocessing command */

	if (SCpnt->use_sg) { /* just use first element */
		struct scatterlist *sgpnt = (struct scatterlist *)
						SCpnt->request_buffer;

		buff = sgpnt[0].address;
		bufflen = sgpnt[0].length;
		/* READ and WRITE process scatterlist themselves */
	}
	else 
		buff = (unsigned char *) SCpnt->request_buffer;
	if (NULL == buff) {
		printk(KERN_WARNING "vtl:qc: buff was NULL??\n");
		buff = broken_buff;	/* just point at dummy */
		bufflen = SDEBUG_SENSE_LEN;
	}

        if(target == driver_template.this_id) {
                printk(KERN_WARNING 
		       "vtl: initiator's id used as target!\n");
		return schedule_resp(SCpnt, NULL, done, 0, 0);
        }

	if ((target > driver_template.this_id) || (SCpnt->lun != 0))
		return schedule_resp(SCpnt, NULL, done, 
				     DID_NO_CONNECT << 16, 0);
// #if 0
	printk(KERN_INFO "sdebug:qc: host_no=%d, id=%d, sdp=%p, cmd=0x%x\n",
	       (int)SCpnt->device->host->host_no, (int)SCpnt->device->id,
	       SCpnt->device, (int)*cmd);
// #endif
	if (NULL == SCpnt->device->hostdata) {
		devip = devInfoReg(SCpnt->device);
		if (NULL == devip)
			return schedule_resp(SCpnt, NULL, done, 
					     DID_NO_CONNECT << 16, 0);
		SCpnt->device->hostdata = devip;
	}
	devip = SCpnt->device->hostdata;

        if ((VTL_OPT_EVERY_NTH & vtl_opts) &&
            (vtl_every_nth > 0) &&
            (++vtl_cmnd_count >= vtl_every_nth)) {
                vtl_cmnd_count =0;
                return 0; /* ignore command causing timeout */
        }

	switch (*cmd) {
	case INQUIRY:     /* mandatory */
		/* assume INQUIRY called first so setup max_cmd_len */
		if (SCpnt->host->max_cmd_len != VTL_MAX_CMD_LEN)
			SCpnt->host->max_cmd_len = VTL_MAX_CMD_LEN;
		errsts = resp_inquiry(cmd, target, buff, bufflen, devip);
		break;
	case REQUEST_SENSE:	/* mandatory */
		/* Since this driver indicates autosense by placing the
		 * sense buffer in the scsi_cmnd structure in the response
		 * (when CHECK_CONDITION is set), the mid level shouldn't
		 * need to call REQUEST_SENSE */
		if (devip) {
			sbuff = devip->sense_buff;
			memcpy(buff, sbuff, (bufflen < SDEBUG_SENSE_LEN) ? 
					     bufflen : SDEBUG_SENSE_LEN);
			mk_sense_buffer(devip, 0, 0x0, 0, 7);
		} else {
			memset(buff, 0, bufflen);
			buff[0] = 0x70;
		}
		break;
	case START_STOP:
		errsts = check_reset(SCpnt, devip);
		break;
	case ALLOW_MEDIUM_REMOVAL:
		if ((errsts = check_reset(SCpnt, devip)))
			break;
		if (VTL_OPT_NOISE & vtl_opts)
			printk("\tMedium removal %s\n",
			       cmd[4] ? "inhibited" : "enabled");
		break;
	case SEND_DIAGNOSTIC:     /* mandatory */
		memset(buff, 0, bufflen);
		break;
	case TEST_UNIT_READY:     /* mandatory */
		memset(buff, 0, bufflen);
		break;
        case RESERVE:
		errsts = check_reset(SCpnt, devip);
		memset(buff, 0, bufflen);
                break;
        case RESERVE_10:
		errsts = check_reset(SCpnt, devip);
		memset(buff, 0, bufflen);
                break;
        case RELEASE:
		errsts = check_reset(SCpnt, devip);
		memset(buff, 0, bufflen);
                break;
        case RELEASE_10:
		errsts = check_reset(SCpnt, devip);
		memset(buff, 0, bufflen);
                break;
	case READ_CAPACITY:
		errsts = check_reset(SCpnt, devip);
		memset(buff, 0, bufflen);
		if (bufflen > 7) {
			capac = CAPACITY - 1;
			buff[0] = (capac >> 24);
			buff[1] = (capac >> 16) & 0xff;
			buff[2] = (capac >> 8) & 0xff;
			buff[3] = capac & 0xff;
			buff[6] = (SECT_SIZE_PER(target) >> 8) & 0xff;
			buff[7] = SECT_SIZE_PER(target) & 0xff;
		}
		break;
	case SCSI_CMD_READ_16:	/* SBC-2 */
	case READ_12:
	case READ_10:
	case READ_6:
		if ((errsts = check_reset(SCpnt, devip)))
			break;
		upper_blk = 0;
		if ((*cmd) == SCSI_CMD_READ_16) {
			upper_blk = cmd[5] + (cmd[4] << 8) + 
				    (cmd[3] << 16) + (cmd[2] << 24);
			block = cmd[9] + (cmd[8] << 8) + 
				(cmd[7] << 16) + (cmd[6] << 24);
			num = cmd[13] + (cmd[12] << 8) + 
				(cmd[11] << 16) + (cmd[10] << 24);
		} else if ((*cmd) == READ_12) {
			block = cmd[5] + (cmd[4] << 8) + 
				(cmd[3] << 16) + (cmd[2] << 24);
			num = cmd[9] + (cmd[8] << 8) + 
				(cmd[7] << 16) + (cmd[6] << 24);
		} else if ((*cmd) == READ_10) {
			block = cmd[5] + (cmd[4] << 8) + 
				(cmd[3] << 16) + (cmd[2] << 24);
			num = cmd[8] + (cmd[7] << 8);
		} else {
			block = cmd[3] + (cmd[2] << 8) + 
				((cmd[1] & 0x1f) << 16);
			num = cmd[4];
		}
		errsts = resp_read(SCpnt, upper_blk, block, num, devip);
		break;
	case REPORT_LUNS:
		errsts = resp_report_luns(cmd, buff, bufflen, devip);
		break;
	case SCSI_CMD_WRITE_16:	/* SBC-2 */
	case WRITE_12:
	case WRITE_10:
	case WRITE_6:
		if ((errsts = check_reset(SCpnt, devip)))
			break;
		upper_blk = 0;
		if ((*cmd) == SCSI_CMD_WRITE_16) {
			upper_blk = cmd[5] + (cmd[4] << 8) + 
				    (cmd[3] << 16) + (cmd[2] << 24);
			block = cmd[9] + (cmd[8] << 8) + 
				(cmd[7] << 16) + (cmd[6] << 24);
			num = cmd[13] + (cmd[12] << 8) + 
				(cmd[11] << 16) + (cmd[10] << 24);
		} else if ((*cmd) == WRITE_12) {
			block = cmd[5] + (cmd[4] << 8) + 
				(cmd[3] << 16) + (cmd[2] << 24);
			num = cmd[9] + (cmd[8] << 8) + 
				(cmd[7] << 16) + (cmd[6] << 24);
		} else if ((*cmd) == WRITE_10) {
			block = cmd[5] + (cmd[4] << 8) + 
				(cmd[3] << 16) + (cmd[2] << 24);
			num = cmd[8] + (cmd[7] << 8);
		} else {
			block = cmd[3] + (cmd[2] << 8) + 
				((cmd[1] & 0x1f) << 16);
			num = cmd[4];
		}
		errsts = resp_write(SCpnt, upper_blk, block, num, devip);
		break;
	case MODE_SENSE:
	case MODE_SENSE_10:
		errsts = resp_mode_sense(cmd, target, buff, bufflen, devip);
		break;
	default:
//#if 0
		printk(KERN_INFO "vtl: Unsupported command, "
		       "opcode=0x%x\n", (int)cmd[0]);
//#endif
		if ((errsts = check_reset(SCpnt, devip)))
			break;
		mk_sense_buffer(devip, ILLEGAL_REQUEST, 0x20, 0, 14);
		errsts = (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
		break;
	}
	return schedule_resp(SCpnt, devip, done, errsts, vtl_delay);
}

static int vtl_ioctl(Scsi_Device *dev, int cmd, void *arg)
{
	if (VTL_OPT_NOISE & vtl_opts) {
		printk(KERN_INFO "vtl: ioctl: cmd=0x%x\n", cmd);
	}
	return -ENOTTY;
}

static int check_reset(Scsi_Cmnd * SCpnt, struct sdebug_dev_info * devip)
{
	if (devip->reset) {
		devip->reset = 0;
		mk_sense_buffer(devip, UNIT_ATTENTION, 0x29, 0, 14);
		return (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
	}
	return 0;
}

#define SDEBUG_LONG_INQ_SZ 58
#define SDEBUG_MAX_INQ_ARR_SZ 128

static const char * vendor_id = "Linux   ";
static const char * product_id = "vtl             ";
static const char * product_rev = "0004";

static int inquiry_evpd_83(unsigned char * arr, int dev_id_num, 
			   const char * dev_id_str, int dev_id_str_len)
{
	int num;

	/* Two identification descriptors: */
	/* T10 vendor identifier field format (faked) */
	arr[0] = 0x2;	/* ASCII */
	arr[1] = 0x1;
	arr[2] = 0x0;
	memcpy(&arr[4], vendor_id, 8);
	memcpy(&arr[12], product_id, 16);
	memcpy(&arr[28], dev_id_str, dev_id_str_len);
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

static int resp_inquiry(unsigned char * cmd, int target, unsigned char * buff,
			int bufflen, struct sdebug_dev_info * devip)
{
	unsigned char pq_pdt;
	unsigned char arr[SDEBUG_MAX_INQ_ARR_SZ];
	int min_len = bufflen > SDEBUG_MAX_INQ_ARR_SZ ? 
			SDEBUG_MAX_INQ_ARR_SZ : bufflen;

	if (bufflen < cmd[4])
		printk(KERN_INFO "vtl: inquiry: bufflen=%d "
		       "< alloc_length=%d\n", bufflen, (int)cmd[4]);
	memset(buff, 0, bufflen);
	memset(arr, 0, SDEBUG_MAX_INQ_ARR_SZ);
	pq_pdt = PERIPH_DEVICE_TYPE(target);
	devip->ptype = PERIPH_DEVICE_TYPE(target);
	arr[0] = pq_pdt;
	if (0x2 & cmd[1]) {  /* CMDDT bit set */
		mk_sense_buffer(devip, ILLEGAL_REQUEST, 0x24, 0, 14);
		return (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
	} else if (0x1 & cmd[1]) {  /* EVPD bit set */
		int dev_id_num, len;
		char dev_id_str[6];
		
		dev_id_num = ((devip->sdp->host->host_no + 1) * 1000) + 
			      devip->sdp->id;
		len = snprintf(dev_id_str, 6, "%d", dev_id_num);
		len = (len > 6) ? 6 : len;
		if (0 == cmd[2]) { /* supported vital product data pages */
			arr[3] = 3;
			arr[4] = 0x0; /* this page */
			arr[5] = 0x80; /* unit serial number */
			arr[6] = 0x83; /* device identification */
		} else if (0x80 == cmd[2]) { /* unit serial number */
			arr[1] = 0x80;
			arr[3] = len;
			memcpy(&arr[4], dev_id_str, len);
		} else if (0x83 == cmd[2]) { /* device identification */
			arr[1] = 0x83;
			arr[3] = inquiry_evpd_83(&arr[4], dev_id_num,
						 dev_id_str, len);
		} else {
			/* Illegal request, invalid field in cdb */
			mk_sense_buffer(devip, ILLEGAL_REQUEST, 0x24, 0, 14);
			return (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
		}
		memcpy(buff, arr, min_len); 
		return 0;
	}
	/* drops through here for a standard inquiry */
	arr[1] = DEV_REMOVEABLE(target) ? 0x80 : 0;	/* Removable disk */
	arr[2] = 3;	/* claim SCSI 3 */
	arr[4] = SDEBUG_LONG_INQ_SZ - 5;
	arr[7] = 0x3a; /* claim: WBUS16, SYNC, LINKED + CMDQUE */
	memcpy(&arr[8], vendor_id, 8);
	memcpy(&arr[16], product_id, 16);
	memcpy(&arr[32], product_rev, 4);
	memcpy(buff, arr, min_len);
	return 0;
}

/* <<Following mode page info copied from ST318451LW>> */ 

static int resp_err_recov_pg(unsigned char * p, int pcontrol, int target)
{	/* Read-Write Error Recovery page for mode_sense */
	unsigned char err_recov_pg[] = {0x1, 0xa, 0xc0, 11, 240, 0, 0, 0, 
					5, 0, 0xff, 0xff};

	memcpy(p, err_recov_pg, sizeof(err_recov_pg));
	if (1 == pcontrol)
		memset(p + 2, 0, sizeof(err_recov_pg) - 2);
	return sizeof(err_recov_pg);
}

static int resp_disconnect_pg(unsigned char * p, int pcontrol, int target)
{ 	/* Disconnect-Reconnect page for mode_sense */
	unsigned char disconnect_pg[] = {0x2, 0xe, 128, 128, 0, 10, 0, 0, 
					 0, 0, 0, 0, 0, 0, 0, 0};

	memcpy(p, disconnect_pg, sizeof(disconnect_pg));
	if (1 == pcontrol)
		memset(p + 2, 0, sizeof(disconnect_pg) - 2);
	return sizeof(disconnect_pg);
}

static int resp_format_pg(unsigned char * p, int pcontrol, int target)
{       /* Format device page for mode_sense */
        unsigned char format_pg[] = {0x3, 0x16, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0, 0, 0x40, 0, 0, 0};

        memcpy(p, format_pg, sizeof(format_pg));
        p[10] = (N_SECTOR >> 8) & 0xff;
        p[11] = N_SECTOR & 0xff;
        p[12] = (SECT_SIZE >> 8) & 0xff;
        p[13] = SECT_SIZE & 0xff;
        if (DEV_REMOVEABLE(target))
                p[20] |= 0x20; /* should agree with INQUIRY */
        if (1 == pcontrol)
                memset(p + 2, 0, sizeof(format_pg) - 2);
        return sizeof(format_pg);
}

static int resp_caching_pg(unsigned char * p, int pcontrol, int target)
{ 	/* Caching page for mode_sense */
	unsigned char caching_pg[] = {0x8, 18, 0x14, 0, 0xff, 0xff, 0, 0, 
		0xff, 0xff, 0xff, 0xff, 0x80, 0x14, 0, 0,     0, 0, 0, 0};

	memcpy(p, caching_pg, sizeof(caching_pg));
	if (1 == pcontrol)
		memset(p + 2, 0, sizeof(caching_pg) - 2);
	return sizeof(caching_pg);
}

static int resp_ctrl_m_pg(unsigned char * p, int pcontrol, int target)
{ 	/* Control mode page for mode_sense */
	unsigned char ctrl_m_pg[] = {0xa, 10, 2, 0, 0, 0, 0, 0,
				     0, 0, 0x2, 0x4b};

	memcpy(p, ctrl_m_pg, sizeof(ctrl_m_pg));
	if (1 == pcontrol)
		memset(p + 2, 0, sizeof(ctrl_m_pg) - 2);
	return sizeof(ctrl_m_pg);
}


#define SDEBUG_MAX_MSENSE_SZ 256

static int resp_mode_sense(unsigned char * cmd, int target,
			   unsigned char * buff, int bufflen,
			   struct sdebug_dev_info * devip)
{
	unsigned char dbd;
	int pcontrol, pcode;
	unsigned char dev_spec;
	int alloc_len, msense_6, offset, len;
	unsigned char * ap;
	unsigned char arr[SDEBUG_MAX_MSENSE_SZ];
	int min_len = bufflen > SDEBUG_MAX_MSENSE_SZ ? 
			SDEBUG_MAX_MSENSE_SZ : bufflen;

	SCSI_LOG_LLQUEUE(3, printk("Mode sense ...(%p %d)\n", buff, bufflen));
	dbd = cmd[1] & 0x8;
	pcontrol = (cmd[2] & 0xc0) >> 6;
	pcode = cmd[2] & 0x3f;
	msense_6 = (MODE_SENSE == cmd[0]);
	alloc_len = msense_6 ? cmd[4] : ((cmd[7] << 8) | cmd[6]);
	/* printk(KERN_INFO "msense: dbd=%d pcontrol=%d pcode=%d "
		"msense_6=%d alloc_len=%d\n", dbd, pcontrol, pcode, "
		"msense_6, alloc_len); */
	if (bufflen < alloc_len)
		printk(KERN_INFO "vtl: mode_sense: bufflen=%d "
		       "< alloc_length=%d\n", bufflen, alloc_len);
	memset(buff, 0, bufflen);
	memset(arr, 0, SDEBUG_MAX_MSENSE_SZ);
	if (0x3 == pcontrol) {  /* Saving values not supported */
		mk_sense_buffer(devip, ILLEGAL_REQUEST, 0x39, 0, 14);
		return (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
	}
	dev_spec = DEV_READONLY(target) ? 0x80 : 0x0;
	if (msense_6) {
		arr[2] = dev_spec;
		offset = 4;
	} else {
		arr[3] = dev_spec;
		offset = 8;
	}
	ap = arr + offset;

	switch (pcode) {
	case 0x1:	/* Read-Write error recovery page, direct access */
		len = resp_err_recov_pg(ap, pcontrol, target);
		offset += len;
		break;
	case 0x2:	/* Disconnect-Reconnect page, all devices */
		len = resp_disconnect_pg(ap, pcontrol, target);
		offset += len;
		break;
        case 0x3:       /* Format device page, direct access */
                len = resp_format_pg(ap, pcontrol, target);
                offset += len;
                break;
	case 0x8:	/* Caching page, direct access */
		len = resp_caching_pg(ap, pcontrol, target);
		offset += len;
		break;
	case 0xa:	/* Control Mode page, all devices */
		len = resp_ctrl_m_pg(ap, pcontrol, target);
		offset += len;
		break;
	case 0x3f:	/* Read all Mode pages */
		len = resp_err_recov_pg(ap, pcontrol, target);
		len += resp_disconnect_pg(ap + len, pcontrol, target);
		len += resp_caching_pg(ap + len, pcontrol, target);
		len += resp_ctrl_m_pg(ap + len, pcontrol, target);
		offset += len;
		break;
	default:
		mk_sense_buffer(devip, ILLEGAL_REQUEST, 0x24, 0, 14);
		return (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
	}
	if (msense_6)
		arr[0] = offset - 1;
	else {
		offset -= 2;
		arr[0] = (offset >> 8) & 0xff; 
		arr[1] = offset & 0xff; 
	}
	memcpy(buff, arr, min_len);
	return 0;
}

static int resp_read(Scsi_Cmnd * SCpnt, int upper_blk, int block, int num, 
		     struct sdebug_dev_info * devip)
{
        unsigned char *buff = (unsigned char *) SCpnt->request_buffer;
        int nbytes, sgcount;
        struct scatterlist *sgpnt = NULL;
        int bufflen = SCpnt->request_bufflen;
	unsigned long iflags;

	if (upper_blk || (block + num > CAPACITY)) {
		mk_sense_buffer(devip, ILLEGAL_REQUEST, 0x21, 0, 14);
		return (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
	}
	if ((VTL_OPT_MEDIUM_ERR & vtl_opts) &&
	    (block >= OPT_MEDIUM_ERR_ADDR) && 
	    (block < (OPT_MEDIUM_ERR_ADDR + num))) {
		mk_sense_buffer(devip, MEDIUM_ERROR, 0x11, 0, 14);
		/* claim unrecoverable read error */
		return (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
	}
	read_lock_irqsave(&atomic_rw, iflags);
        sgcount = 0;
	nbytes = bufflen;
	/* printk(KERN_INFO "vtl_read: block=%d, tot_bufflen=%d\n", 
	       block, bufflen); */
	if (SCpnt->use_sg) {
		sgcount = 0;
		sgpnt = (struct scatterlist *) buff;
		buff = sgpnt[sgcount].address;
		bufflen = sgpnt[sgcount].length;
	}
	do {
		memcpy(buff, fake_storep + (block * SECT_SIZE), bufflen);
		nbytes -= bufflen;
		if (SCpnt->use_sg) {
			block += bufflen >> POW2_SECT_SIZE;
			sgcount++;
			if (nbytes) {
				buff = sgpnt[sgcount].address;
				bufflen = sgpnt[sgcount].length;
			}
		} else if (nbytes > 0)
			printk(KERN_WARNING "vtl:resp_read: unexpected "
			       "nbytes=%d\n", nbytes);
	} while (nbytes);
	read_unlock_irqrestore(&atomic_rw, iflags);
	return 0;
}

static int resp_write(Scsi_Cmnd * SCpnt, int upper_blk, int block, int num, 
		      struct sdebug_dev_info * devip)
{
        unsigned char *buff = (unsigned char *) SCpnt->request_buffer;
        int nbytes, sgcount;
        struct scatterlist *sgpnt = NULL;
        int bufflen = SCpnt->request_bufflen;
	unsigned long iflags;

	if (upper_blk || (block + num > CAPACITY)) {
		mk_sense_buffer(devip, ILLEGAL_REQUEST, 0x21, 0, 14);
		return (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
	}

	write_lock_irqsave(&atomic_rw, iflags);
        sgcount = 0;
	nbytes = bufflen;
	if (SCpnt->use_sg) {
		sgcount = 0;
		sgpnt = (struct scatterlist *) buff;
		buff = sgpnt[sgcount].address;
		bufflen = sgpnt[sgcount].length;
	}
	do {
		memcpy(fake_storep + (block * SECT_SIZE), buff, bufflen);

		nbytes -= bufflen;
		if (SCpnt->use_sg) {
			block += bufflen >> POW2_SECT_SIZE;
			sgcount++;
			if (nbytes) {
				buff = sgpnt[sgcount].address;
				bufflen = sgpnt[sgcount].length;
			}
		} else if (nbytes > 0)
			printk(KERN_WARNING "vtl:resp_write: "
			       "unexpected nbytes=%d\n", nbytes);
	} while (nbytes);
	write_unlock_irqrestore(&atomic_rw, iflags);
	return 0;
}

static int resp_report_luns(unsigned char * cmd, unsigned char * buff,
			    int bufflen, struct sdebug_dev_info * devip)
{
	unsigned int alloc_len;
	int select_report = (int)cmd[2];

	alloc_len = cmd[9] + (cmd[8] << 8) + (cmd[7] << 16) + (cmd[6] << 24);
	if ((alloc_len < 16) || (select_report > 2)) {
		mk_sense_buffer(devip, ILLEGAL_REQUEST, 0x24, 0, 14);
		return (COMMAND_COMPLETE << 8) | (CHECK_CONDITION << 1);
	}
	if (bufflen > 3) {
		memset(buff, 0, bufflen);
		buff[3] = 8;
	}
	return 0;
}

/* When timer goes off this function is called. */
static void timer_intr_handler(unsigned long indx)
{
	struct sdebug_queued_cmd * sqcp;
	unsigned int iflags;

	if (indx >= VTL_CANQUEUE) {
		printk(KERN_ERR "vtl:timer_intr_handler: indx too "
		       "large\n");
		return;
	}
	spin_lock_irqsave(&queued_arr_lock, iflags);
	sqcp = &queued_arr[(int)indx];
	if (! sqcp->in_use) {
		printk(KERN_ERR "vtl:timer_intr_handler: Unexpected "
		       "interrupt\n");
		spin_unlock_irqrestore(&queued_arr_lock, iflags);
		return;
	}
	sqcp->in_use = 0;
	if (sqcp->done_funct)
		sqcp->done_funct(sqcp->a_cmnd); /* callback to mid level */
	sqcp->done_funct = NULL;
	spin_unlock_irqrestore(&queued_arr_lock, iflags);
}

static int initialized = 0;
static int num_present = 0;

static int vtl_detect(Scsi_Host_Template * tpnt)
{
	int	k, sz;
	int	stat;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: detect\n");
	if (0 == initialized) {
		++initialized;
		// Initialise one more struct than required - NULL terminated.
		sz = sizeof(struct sdebug_dev_info) * (vtl_num_devs + 1);
		devInfop = vmalloc(sz);
		if (NULL == devInfop) {
			printk(KERN_ERR "vtl_detect: out of "
			       "memory, 0.5\n");
			return 0;
		}
		memset(devInfop, 0, sz);
		sz = STORE_SIZE;
		fake_storep = vmalloc(sz);
		if (NULL == fake_storep) {
			printk(KERN_ERR "vtl_detect: out of memory"
			       ", 0\n");
			return 0;
		}
		memset(fake_storep, 0, sz);

	// FIXME: OK, they all point to same buffer (at the moment)...
		devInfop->rw_buf_sz = sz;
		devInfop->rw_buf = fake_storep;

		init_all_queued();
		tpnt->proc_name = (char *)vtl_driver_name;
		for (num_present = 0, k = 0; k < NR_HOSTS_PRESENT; k++) {
			if (NULL == scsi_register(tpnt, 0))
				printk(KERN_ERR "vtl_detect: "
					"scsi_register failed k=%d\n", k);
			else
				++num_present;
		}

		stat = register_chrdev( major, vtl_driver_name, &vtl_fops );
		if(stat < 0)
			printk(KERN_ERR "Unable to register vtl char dev");
		else
			printk(KERN_ERR "Registered char() dev");

		return num_present;
	} else {
		printk(KERN_WARNING "vtl_detect: called again\n");
		return 0;
	}
}


static int num_releases = 0;

static int vtl_release(struct Scsi_Host * hpnt)
{
	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: release\n");
	stop_all_queued();
	scsi_unregister(hpnt);
	if (++num_releases == num_present) {
		vfree(fake_storep);
		vfree(devInfop);
	}
	unregister_chrdev( major, vtl_driver_name);
	return 0;
}

static struct sdebug_dev_info * devInfoReg(Scsi_Device * sdp)
{
	int k;
	struct sdebug_dev_info * devip;

	for (k = 0; k < vtl_num_devs; ++k) {
		devip = &devInfop[k];
		if (devip->sdp == sdp)
			return devip;
	}
	for (k = 0; k < vtl_num_devs; ++k) {
		devip = &devInfop[k];
		if (NULL == devip->sdp) {
			devip->sdp = sdp;
			devip->reset = 1;
			devip->status = 0; // No queued cmd
			memset(devip->sense_buff, 0, SDEBUG_SENSE_LEN);
			devip->sense_buff[0] = 0x70;
			return devip;
		}
	}
	return NULL;
}

static void mk_sense_buffer(struct sdebug_dev_info * devip, int key, 
			    int asc, int asq, int inbandLen)
{
	unsigned char * sbuff;

	sbuff = devip->sense_buff;
	memset(sbuff, 0, SDEBUG_SENSE_LEN);
	if (inbandLen > SDEBUG_SENSE_LEN)
		inbandLen = SDEBUG_SENSE_LEN;
	sbuff[0] = 0x70;
	sbuff[2] = key;
	sbuff[7] = (inbandLen > 7) ? (inbandLen - 8) : 0;
	sbuff[12] = asc;
	sbuff[13] = asq;
}

static int vtl_abort(Scsi_Cmnd * SCpnt)
{
	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: abort\n");
	++num_aborts;
	stop_queued_cmnd(SCpnt);
	return SUCCESS;
}

static int vtl_biosparam(Disk * disk, kdev_t dev, int *info)
{
	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: biosparam\n");
	/* int size = disk->capacity; */
	info[0] = N_HEAD;
	info[1] = N_SECTOR;
	info[2] = N_CYLINDER;
	if (info[2] >= 1024)
		info[2] = 1024;
	return 0;
}

static int vtl_device_reset(Scsi_Cmnd * SCpnt)
{
	Scsi_Device * sdp;
	int k;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: device_reset\n");
	++num_dev_resets;
	if (SCpnt && ((sdp = SCpnt->device))) {
		for (k = 0; k < vtl_num_devs; ++k) {
			if (sdp->hostdata == (devInfop + k))
				break;
		}
		if (k < vtl_num_devs)
			devInfop[k].reset = 1;
	}
	return SUCCESS;
}

static int vtl_bus_reset(Scsi_Cmnd * SCpnt)
{
	Scsi_Device * sdp;
	struct Scsi_Host * hp;
	int k;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: bus_reset\n");
	++num_bus_resets;
	if (SCpnt && ((sdp = SCpnt->device)) && ((hp = sdp->host))) {
		for (k = 0; k < vtl_num_devs; ++k) {
			if (hp == devInfop[k].sdp->host)
				devInfop[k].reset = 1;
		}
	}
	return SUCCESS;
}

static int vtl_host_reset(Scsi_Cmnd * SCpnt)
{
	int k;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "vtl: host_reset\n");
	++num_host_resets;
	for (k = 0; k < vtl_num_devs; ++k)
		devInfop[k].reset = 1;
	stop_all_queued();

	return SUCCESS;
}

/* Returns 1 if found 'cmnd' and deleted its timer. else returns 0 */
static int stop_queued_cmnd(struct scsi_cmnd * cmnd)
{
	unsigned long iflags;
	int k;
	struct sdebug_queued_cmd * sqcp;

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
	struct sdebug_queued_cmd * sqcp;

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
static void init_all_queued(void)
{
	unsigned long iflags;
	int k;
	struct sdebug_queued_cmd * sqcp;

	spin_lock_irqsave(&queued_arr_lock, iflags);
	for (k = 0; k < VTL_CANQUEUE; ++k) {
		sqcp = &queued_arr[k];
		init_timer(&sqcp->cmnd_timer);
		sqcp->in_use = 0;
		sqcp->a_cmnd = NULL;
	}
	spin_unlock_irqrestore(&queued_arr_lock, iflags);
}

static int schedule_resp(struct scsi_cmnd * cmnd, 
			 struct sdebug_dev_info * devip,
			 done_funct_t done, int scsi_result, int delta_jiff)
{
	int k, num; 

	if (VTL_OPT_NOISE & vtl_opts) {
		printk(KERN_INFO "vtl: cmd ");
		for (k = 0, num = cmnd->cmd_len; k < num; ++k)
	            printk("%02x ", (int)cmnd->cmnd[k]);
		printk("result=0x%x\n", scsi_result);
	}
	if (cmnd && devip) {
		/* simulate autosense by this driver */
		if (CHECK_CONDITION == status_byte(scsi_result))
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
		struct sdebug_queued_cmd * sqcp = NULL;

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
		return 0;
	}
}

#ifndef MODULE
static int __init num_devs_setup(char *str)
{   
    int tmp; 
    
    if (get_option(&str, &tmp) == 1) {
        if (tmp > 0)
            vtl_num_devs = tmp;
        return 1;
    } else {
        printk(KERN_INFO "vtl_num_devs: usage vtl_num_devs=<n> "
               "(<n> can be from 1 to around 2000)\n");
        return 0;
    }
}
__setup("vtl_num_devs=", num_devs_setup);

static int __init dev_size_mb_setup(char *str)
{   
    int tmp; 
    
    if (get_option(&str, &tmp) == 1) {
        if (tmp > 0)
            vtl_dev_size_mb = tmp;
        return 1;
    } else {
        printk(KERN_INFO "vtl_dev_size_mb: usage "
	       "vtl_dev_size_mb=<n>\n"
               "    (<n> is number of MB ram shared by all devs\n");
        return 0;
    }
}
__setup("vtl_dev_size_mb=", dev_size_mb_setup);

static int __init opts_setup(char *str)
{   
    int tmp; 
    
    if (get_option(&str, &tmp) == 1) {
        if (tmp > 0)
            vtl_opts = tmp;
        return 1;
    } else {
        printk(KERN_INFO "vtl_opts: usage "
	       "vtl_opts=<n>\n"
               "    (1->noise, 2->medium_error, 4->... (can be or-ed)\n");
        return 0;
    }
}
__setup("vtl_opts=", opts_setup);

static int __init every_nth_setup(char *str)
{
    int tmp;

    if (get_option(&str, &tmp) == 1) {
        if (tmp > 0)
            vtl_every_nth = tmp;
        return 1;
    } else {
        printk(KERN_INFO "vtl_every_nth: usage "
               "vtl_every_nth=<n>\n"
               "    timeout every nth command (when ...)\n");
        return 0;
    }
}
__setup("vtl_every_nth=", every_nth_setup);

static int __init delay_setup(char *str)
{
    int tmp;

    if (get_option(&str, &tmp) == 1) {
	vtl_delay = tmp;
        return 1;
    } else {
        printk(KERN_INFO "vtl_delay: usage "
               "vtl_delay=<n>\n"
               "    delay response <n> jiffies\n");
        return 0;
    }
}
__setup("vtl_delay=", delay_setup);

#endif

MODULE_AUTHOR("Eric Youngdale + Douglas Gilbert + Mark Harvey");
MODULE_DESCRIPTION("vtl based on SCSI debug adapter driver");
MODULE_PARM(vtl_num_devs, "i");
MODULE_PARM_DESC(vtl_num_devs, "number of SCSI devices to simulate");
MODULE_PARM(vtl_dev_size_mb, "i");
MODULE_PARM_DESC(vtl_dev_size_mb, "size in MB of ram shared by devs");
MODULE_PARM(vtl_opts, "i");
MODULE_PARM_DESC(vtl_opts, "1->noise, 2->medium_error, 4->...");
MODULE_PARM(vtl_every_nth, "i");
MODULE_PARM_DESC(vtl_every_nth, "timeout every nth command(def=100)");
MODULE_PARM(vtl_delay, "i");
MODULE_PARM_DESC(vtl_delay, "# of jiffies to delay response(def=1)");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

static char sdebug_info[256];

static const char * vtl_info(struct Scsi_Host * shp)
{
	sprintf(sdebug_info, "vtl, %s, num_devs=%d, "
		"dev_size_mb=%d, opts=0x%x", vtl_version_str,
		vtl_num_devs, vtl_dev_size_mb,
		vtl_opts);
	return sdebug_info;
}

/* vtl_proc_info
 * Used if the driver currently has no own support for /proc/scsi
 */
static int vtl_proc_info(char *buffer, char **start, off_t offset,
				int length, int inode, int inout)
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
		if (VTL_OPT_EVERY_NTH & vtl_opts)
                        vtl_cmnd_count = 0;
		return length;
	}
	begin = 0;
	pos = len = sprintf(buffer, "vtl adapter driver, %s\n"
	    "num_devs=%d, shared (ram) size=%d MB, opts=0x%x, "
	    "every_nth=%d(curr:%d)\n"
	    "sector_size=%d bytes, cylinders=%d, heads=%d, sectors=%d, "
	    "delay=%d\nnumber of aborts=%d, device_reset=%d, bus_resets=%d, " 
	    "host_resets=%d\n",
	    vtl_version_str, vtl_num_devs, 
	    vtl_dev_size_mb, vtl_opts, vtl_every_nth,
	    vtl_cmnd_count,
	    SECT_SIZE, N_CYLINDER, N_HEAD, N_SECTOR, vtl_delay,
	    num_aborts, num_dev_resets, num_bus_resets, num_host_resets);
	if (pos < offset) {
		len = 0;
		begin = pos;
	}
	*start = buffer + (offset - begin);	/* Start of wanted data */
	len -= (offset - begin);
	if (len > length)
		len = length;

	return (len);
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
        unsigned int minor = MINOR(inode->i_rdev);
        struct sdebug_queued_cmd * sqcp = NULL;
        unsigned int  k;
        unsigned long iflags;
        unsigned char s[4];     /* Serial Number & sense data buffer */
        unsigned char valid_sense;
        u32 serial_no;
        u32 count = 0;
	struct sdebug_dev_info * devip;

        if(minor > vtl_num_devs)	/* Check limit minor no. */
                return -ENODEV;

	devip = &devInfop[minor];
	if(NULL == devip->sdp)
                return -ENODEV;

        switch (cmd) {
        /* Online */
        case 0x80:
                copy_from_user(&devip->status_argv, (void *)arg,
						sizeof(devip->status_argv));
                /* Only like types (SSC / medium changer) */
                if(devip->status_argv != devip->ptype) {
                        printk("vtl%d: devip->ptype: %d,"
				" requested type: %d\n",
                                                minor,
						devip->ptype,
						devip->status_argv);
                        return -ENODEV;
                }
                devip->device_offline = 0;
                printk(KERN_INFO "vtl%d: Online ptype(%d)\n",
                                        minor, devip->status_argv);
                break;

        /* Offline */
        case 0x81:
                devip->device_offline = 1;
                printk(KERN_INFO "vtl%d: Offline\n", minor);
                break;

        /* read size of fifo buffer */
        case 0x82:
                copy_to_user((void *)arg, &devip->rw_buf_sz, sizeof(u32));
                printk(KERN_INFO "vtl%d: fifo buffer %d bytes\n",
                                            minor, devip->rw_buf_sz);
                break;

        /* ioctl poll -> return status of vtl driver */
        case 0x83:
                /*
                 SCSI code updates status
                        - which this ioctl passes to user space
                */
                copy_to_user((void *)arg, (u32 *)devip->status_argv, sizeof(u32));
                return devip->status;
                break;

        /* Ack status poll */
        case 0x84:
                copy_from_user((u32 *)devip->status_argv, (void *)arg, sizeof(u32));
                devip->status = 0;
                break;

        /*
         * Finish up the SCSI command.
         * - 1. Total number of bytes to read from user space is passed
         *      in status_argv.
         * - 2. Read S/No. of the SCSI cmd (4 bytes).
         * - 3. Read 1 byte - 1 = Request Sense, 0 = No Sense
         * - 4. Read any more data and place into SCSI data buffer
         * - 5. Delete the timer & clear the queued_arr[] entry.
         */
        case 0x184:     /* VX_FINISH_SCSI_COMMAND */
                copy_from_user((u32 *)count, (void *)arg, sizeof(u32));
		arg++;
                /* Read in SCSI cmd s/no. */
                copy_from_user((u8 *)s, &arg, 4);
                serial_no = (u32)htonl(s);

		arg += 4;

                /* Valid sense ?? */
                copy_from_user(&valid_sense, &arg, sizeof(valid_sense));

                count -= 5;

                /* While size of SCSI read is larger than our buffer */
                if(count > devip->rw_buf_sz) {
                        while(count > devip->rw_buf_sz)
                                /* Double size */
                               devip->rw_buf_sz += devip->rw_buf_sz;
                        /* Free old buffer */
                        vfree(devip->rw_buf);
                        devip->rw_buf = vmalloc(devip->rw_buf_sz);
                        if(NULL == devip->rw_buf)
                                return -ENOMEM;
                        printk("New read/write buffer size: %d\n",
							 devip->rw_buf_sz);
		} 
		// Find the SCSI cmd s/no. so we can complete correct cmd.
                spin_lock_irqsave(&queued_arr_lock, iflags);
                for (k = 0; k < VTL_CANQUEUE; ++k) {
                        sqcp = &queued_arr[k];
                        if (sqcp->in_use)
                                if(sqcp->a_cmnd->serial_number == serial_no)
                                        break;
                }
                if (k >= VTL_CANQUEUE) {
                        spin_unlock_irqrestore(&queued_arr_lock, iflags);
                        printk(KERN_WARNING "c_ioctl: callback function not"
                                                " found.: k = %d\n", k);
                        return 1;       /* report busy to mid level */
                }
                if(NULL == sqcp) {
                        printk("FATAL %s, line %d: sqcp is NULL\n",
                                                        __FUNCTION__, __LINE__);                } else {
                        if(count) // FIXME: Need to make sure below is correct.
				copy_to_user((void *)arg, sqcp->a_cmnd, count);
//                               fill_from_dev_buffer(sqcp->a_cmnd, NULL, count,
//                                                        devip->fifo);
                        del_timer_sync(&sqcp->cmnd_timer);
                        sqcp->in_use = 0;

/* FIXME: Should implement auto_sense, later when I get the rest working :-) */
                        if(valid_sense)
                                sqcp->a_cmnd->result = check_condition_result;
                        else
                                sqcp->a_cmnd->result = DID_OK << 16;

                        sqcp->a_cmnd->sense_buffer[0] = 0x0;

                        if(NULL != sqcp->done_funct)
                                sqcp->done_funct(sqcp->a_cmnd);
                        else
                                printk("FATAL %s, line %d: SCSI done_funct"
                                                " callback => NULL\n",
                                                __FUNCTION__, __LINE__);
                }
                spin_unlock_irqrestore(&queued_arr_lock, iflags);
                devip->status = 0;
                break;

        /* Clear 'command pending' status
         *        i.e. - userspace daemon acknowledged it has read
         *               SCSI cmnd
         */
        case 0x185:     /* VX_ACK_SCSI_CDB */
                spin_lock_irqsave(&queued_arr_lock, iflags);
                copy_from_user((void *)devip->status_argv, (void *)arg,
					sizeof(devip->status_argv));
                devip->status = 0;
                spin_unlock_irqrestore(&queued_arr_lock, iflags);
                break;

        default:
                return -ENOTTY;
        }
return 0;
}


static ssize_t vtl_write(struct file *filp, const char *buf, size_t count,
                                                loff_t * ppos)
{
        unsigned int minor = MINOR(filp->f_dentry->d_inode->i_rdev);
        loff_t	cnt = 0;
        loff_t	ret = 0;
	loff_t	n = 0;
	struct	sdebug_dev_info * devip;
	unsigned char	fifo[16];

	MOD_INC_USE_COUNT;

	devip = &devInfop[minor];

	cnt = 0;
	printk("vtl%d write: buf 0x%lx, count %d, cnt %d\n",
				minor, buf, count, cnt);

        while((cnt >= 0) && (cnt < count)) {
		n = (count - cnt) > sizeof(fifo) ? sizeof(fifo) : count - cnt;
                ret = copy_from_user((u8 *)fifo, buf, n);
		if (ret < 0) {
			MOD_DEC_USE_COUNT;
                        return -EFAULT;
		}
/*
		printk("cnt: %d, n: %d\n",
				cnt, n);
		printk(" 0x%02x 0x%02x 0x%02x 0x%02x ",
				fifo[0], fifo[1], fifo[2], fifo[3]);
		printk(" 0x%02x 0x%02x 0x%02x 0x%02x\n",
				fifo[4], fifo[5], fifo[6], fifo[7]);
		printk(" 0x%02x 0x%02x 0x%02x 0x%02x ",
				fifo[8], fifo[9], fifo[10], fifo[11]);
		printk(" 0x%02x 0x%02x 0x%02x 0x%02x\n",
				fifo[12], fifo[13], fifo[14], fifo[15]);
*/

                cnt += n;
        }
	MOD_DEC_USE_COUNT;
	if(cnt < 0)
		return -EFAULT;
        return cnt;
}

static ssize_t vtl_read(struct file *filp, char *buf, size_t count,
                                                loff_t * ppos)
{
        unsigned int minor = MINOR(filp->f_dentry->d_inode->i_rdev);
        size_t	cnt = 0;
	size_t	ret;
	struct	sdebug_dev_info * devip;
	void * fifo;

	devip = &devInfop[minor];

	printk("vtl%d read: buf 0x%x, count %ld\n", minor, buf, count);
	return -EFAULT;

        while(cnt < count) {
		ret = copy_to_user(fifo, buf, count);
		if (ret < 0)
			return -EFAULT;
                cnt += ret;
        }
        return cnt;
}

static int vtl_c_release(struct inode *inode, struct file *filp)
{
        unsigned int minor = MINOR(inode->i_rdev);
	struct sdebug_dev_info * devip;
	devip = &devInfop[minor];
        printk("vtl%d: Release\n", minor);
        devip->device_offline = 1;
        return 0;
}

static int vtl_open(struct inode *inode, struct file *filp)
{
        unsigned int minor = MINOR(inode->i_rdev);
	struct sdebug_dev_info * devip;
	devip = &devInfop[minor];

	printk("Minor no. %d\n", minor);

        if(minor > vtl_num_devs) {	// Exceeded number of devices
                printk("Attempt to open vtl%d failed: No such device\n", minor);
                return -EBUSY;
        }
	if(NULL == devip->sdp) {	// No target..
                printk("Attempt to open vtl%d failed: No such device\n", minor);
                return -EBUSY;
	} else {
                printk("vtl%d: Opened OK\n", minor);
                return 0;
        }
}

#include "scsi_module.c"
