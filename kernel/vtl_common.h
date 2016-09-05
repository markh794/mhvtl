/* Common stuff for kernel and usr programs */
#ifndef VTL_COMMON_H
#define VTL_COMMON_H

#define SENSE_BUF_SIZE	96
/* Max cdb size */
#define MAX_COMMAND_SIZE	16

#define VTL_IDLE		0x00
#define VTL_QUEUE_CMD		0xfe

/* ioctl defines */
#define VX_TAPE_ONLINE		0x80
#define VTL_POLL_AND_GET_HEADER	0x200
#define VTL_GET_DATA		0x201
#define VTL_PUT_DATA		0x203
#define VTL_REMOVE_LU		0x205

#define VENDOR_ID_LEN	8
#define PRODUCT_ID_LEN	16
#define PRODUCT_REV_LEN	4

struct	vtl_header {
	unsigned long long serialNo;
	unsigned char cdb[MAX_COMMAND_SIZE];
	unsigned char *buf;
};

struct vtl_ds {
	void *data;
	unsigned int sz;
	unsigned long long serialNo;
	void *sense_buf;
	unsigned char sam_stat;
};

struct vtl_sn_inquiry {
	char sn[32];
	char vendor_id[VENDOR_ID_LEN + 2];
	char product_id[PRODUCT_ID_LEN + 2];
};

struct vtl_ctl {
	unsigned int channel;
	unsigned int id;
	unsigned int lun;
};

#if !defined(FALSE)
  #define FALSE 0
#endif

#if !defined(TRUE)
  #define TRUE 1
#endif

#endif /* VTL_COMMON_H */

