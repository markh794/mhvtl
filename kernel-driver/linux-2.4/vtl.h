#ifndef _VTL_H

/* $Id: vtl.h,v 1.4.2.1 2006-08-06 07:59:11 markh Exp $ */

#include <linux/types.h>
#include <linux/kdev_t.h>

static int vtl_detect(Scsi_Host_Template *);
static int vtl_release(struct Scsi_Host *);
/* static int vtl_command(Scsi_Cmnd *); */
static int vtl_queuecommand(Scsi_Cmnd *, void (*done) (Scsi_Cmnd *));
static int vtl_ioctl(Scsi_Device *, int, void *);
static int vtl_biosparam(Disk *, kdev_t, int[]);
static int vtl_abort(Scsi_Cmnd *);
static int vtl_bus_reset(Scsi_Cmnd *);
static int vtl_device_reset(Scsi_Cmnd *);
static int vtl_host_reset(Scsi_Cmnd *);
static int vtl_proc_info(char *, char **, off_t, int, int, int);
static const char * vtl_info(struct Scsi_Host *);

static int vtl_c_ioctl(struct inode *, struct file *, unsigned int, unsigned long);
static ssize_t vtl_read(struct file *, char *, size_t, loff_t *);
static ssize_t vtl_write(struct file *, const char *, size_t, loff_t *);
static int vtl_open(struct inode *, struct file *);
static int vtl_c_release(struct inode *, struct file *);


/*
 * This driver is written for the lk 2.4 series
 */
#define VTL_CANQUEUE  255 	/* needs to be >= 1 */

#define VTL_MAX_CMD_LEN 16

#define VTL_TEMPLATE \
		   {proc_info:         vtl_proc_info,	\
		    name:              "SCSI DEBUG",		\
		    info:              vtl_info,		\
		    detect:            vtl_detect,	\
		    release:           vtl_release,	\
		    ioctl:             vtl_ioctl,	\
		    queuecommand:      vtl_queuecommand, \
		    eh_abort_handler:  vtl_abort,	\
		    eh_bus_reset_handler: vtl_bus_reset,	\
		    eh_device_reset_handler: vtl_device_reset,	\
		    eh_host_reset_handler: vtl_host_reset,	\
		    bios_param:        vtl_biosparam,	\
		    can_queue:         VTL_CANQUEUE,	\
		    this_id:           7,			\
		    sg_tablesize:      64,			\
		    cmd_per_lun:       1,			\
		    unchecked_isa_dma: 0,			\
		    use_clustering:    ENABLE_CLUSTERING,	\
		    use_new_eh_code:   1,			\
}

#endif
