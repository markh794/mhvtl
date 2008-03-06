#ifndef _VTL_H
/* $Id: vtl.h,v 1.3.2.1 2006-08-06 07:59:13 markh Exp $ */

#include <linux/types.h>

static int vtl_slave_alloc(struct scsi_device *);
static int vtl_slave_configure(struct scsi_device *);
static void vtl_slave_destroy(struct scsi_device *);
static int vtl_queuecommand(struct scsi_cmnd *,
				   void (*done) (struct scsi_cmnd *));
static int vtl_b_ioctl(struct scsi_device *, int, void __user *);
static int vtl_c_ioctl(struct inode *, struct file *, unsigned int, unsigned long);
static int vtl_abort(struct scsi_cmnd *);
static int vtl_bus_reset(struct scsi_cmnd *);
static int vtl_device_reset(struct scsi_cmnd *);
static int vtl_host_reset(struct scsi_cmnd *);
static int vtl_proc_info(struct Scsi_Host *, char *, char **, off_t, int, int);
static const char * vtl_info(struct Scsi_Host *);
static ssize_t vtl_read(struct file *filp, char __user *buf, size_t count, loff_t *offp);
static ssize_t vtl_write(struct file *filp, const char __user *buf, size_t count, loff_t *offp);
static int vtl_open(struct inode *, struct file *);
static int vtl_release(struct inode *, struct file *);

#define VTL_CANQUEUE  1 	/* needs to be >= 1 */

#define VTL_MAX_CMD_LEN 16

#define VTL_ONLINE 0x80
#define VTL_OFFLINE 0x81
#define VTL_FIFO_SIZE 0x82
#define VTL_CHECK_STATUS 0x83

#define VTL_STATUS_OK 0
#define VTL_STATUS_READ 0x8
#define VTL_STATUS_WRITE 0xa
#define VTL_STATUS_WRITE_FILEMARK 0x10
#define VTL_STATUS_REWIND 0x1
#define VTL_STATUS_LOAD_UNLOAD 0x1b
#define VTL_STATUS_SPACE 0x11

#define VTL_QUEUE_CMD 0xfe
#define VTL_ACK_USER_STATUS 0x185

#endif // _VTL_H
