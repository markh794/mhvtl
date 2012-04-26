/*
 * The shared library libvtlscsi.so function defs
 *
 * Copyright (C) 2005-2012 Mark Harvey markh794@gmail.com
 *                                  mark_harvey@symantec.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#ifndef _VTLLIB_H_
#define _VTLLIB_H_

#ifndef Solaris
  #include <endian.h>
  #include <byteswap.h>
#endif
#include <inttypes.h>
#include <sys/types.h>

#include "vtl_common.h"

#ifndef MHVTL_CONFIG_PATH
#define MHVTL_CONFIG_PATH "/etc/mhvtl"
#endif

#ifndef MHVTL_HOME_PATH
/* Where all the tape data files belong */
#define MHVTL_HOME_PATH "/opt/vtl"
#endif

#if __BYTE_ORDER == __BIG_ENDIAN

#define htonll(x)	(x)
#define htonl(x)	(x)
#define htons(x)	(x)
#else
# if __BYTE_ORDER == __LITTLE_ENDIAN
#  define htonll(x)	__bswap_64 (x)
#  define htonl(x)	__bswap_32 (x)
#  define htons(x)	__bswap_16 (x)
# endif
#endif

#define min(x,y) ({		\
	typeof(x) _x = (x);	\
	typeof(y) _y = (y);	\
	(void) (&_x == &_y);	\
	_x < _y ? _x : _y; })

#define max(x,y) ({		\
	typeof(x) _x = (x);	\
	typeof(y) _y = (y);	\
	(void) (&_x == &_y);	\
	_x > _y ? _x : _y; })

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define STATUS_OK 0

#define STATUS_QUEUE_CMD 0xfe

#define SCSI_SN_LEN 16

#define MAX_BARCODE_LEN	16
#define LEFT_JUST_16_STR "%-16s"

#define MAX_INQ_ARR_SZ 64
#define MALLOC_SZ 512

#define TAPE_UNLOADED 0
#define TAPE_LOADED 1

/*
 * Medium Type Definations
 */
#define MEDIA_TYPE_DATA 0
#define MEDIA_TYPE_WORM 1
#define MEDIA_TYPE_CLEAN 6

#define MHVTL_NO_COMPRESSION 0

/* status definitions (byte[2] in the element descriptor) */
#define STATUS_Full      0x01
#define STATUS_ImpExp    0x02
#define STATUS_Except    0x04
#define STATUS_Access    0x08
#define STATUS_ExEnab    0x10
#define STATUS_InEnab    0x20
#define STATUS_Reserved6 0x40
#define STATUS_Reserved7 0x80
/* internal_status definitions: */
#define INSTATUS_NO_BARCODE 0x01

#define	VOLTAG_LEN	36	/* size of voltag area in RES descriptor */

#define VPD_83_SZ 50
#define VPD_86_SZ 0x3c
#define VPD_B0_SZ 4
#define VPD_B1_SZ SCSI_SN_LEN
#define VPD_B2_SZ 8
#define VPD_C0_SZ 0x28

struct report_luns {
	uint32_t size;
	uint32_t reserved;
	uint32_t LUN_0;
	};

struct mode {
	struct list_head siblings;
	uint8_t pcode;		/* Page code */
	uint8_t subpcode;	/* Sub page code */
	int32_t pcodeSize;	/* Size of page code data. */
	uint8_t *pcodePointerBitMap;	/* bitmap for changeable data */
	uint8_t *pcodePointer;	/* Pointer to page code data */
	char *description;	/* ASCII text 'description' */
	};

/* v2 of the tape media
 * Between BOT & blk #1, is the MAM (Medium Auxiliary Memory)
 */
#define MAM_VERSION 2
struct MAM {
	uint32_t tape_fmt_version;
	uint32_t mam_fmt_version;

	uint64_t remaining_capacity;
	uint64_t max_capacity;
	uint64_t TapeAlert;
	uint64_t LoadCount;
	uint64_t MAMSpaceRemaining;
	uint8_t AssigningOrganization_1[8];
	uint8_t InitializationCount[2];
	uint8_t DevMakeSerialLastLoad[40];
	uint8_t DevMakeSerialLastLoad1[40];
	uint8_t DevMakeSerialLastLoad2[40];
	uint8_t DevMakeSerialLastLoad3[40];
	uint64_t WrittenInMediumLife;
	uint64_t ReadInMediumLife;
	uint64_t WrittenInLastLoad;
	uint64_t ReadInLastLoad;

	uint8_t MediumManufacturer[8];
	uint8_t MediumSerialNumber[32];
	uint32_t MediumLength;
	uint32_t MediumWidth;
	uint8_t AssigningOrganization_2[8];
	uint8_t MediumManufactureDate[12];
	uint8_t FormattedDensityCode;
	uint8_t MediumDensityCode;
	uint8_t MediumType;	/* 0 -> Data, 1 -> WORM, 6 -> Clean */
	uint8_t MediaType;	/* LTO1, LTO2, AIT etc (Media_Type_list) */
	uint64_t MAMCapacity;
	uint16_t MediumTypeInformation;	/* If Clean, max mount */

	uint8_t ApplicationVendor[8];
	uint8_t ApplicationName[32];
	uint8_t ApplicationVersion[8];
	uint8_t UserMediumTextLabel[160];
	uint8_t DateTimeLastWritten[12];
	uint8_t LocalizationIdentifier;
	uint8_t Barcode[32];
	uint8_t OwningHostTextualName[80];
	uint8_t MediaPool[160];

	uint8_t record_dirty; /* 0 = Record clean, non-zero umount failed. */
	uint16_t Flags;

	struct uniq_media_info {
		uint32_t bits_per_mm;
		uint16_t tracks;
		char density_name[8];
		char description[32];
	} media_info;

	/* Pad to keep MAM to 1024 bytes */
	uint8_t pad[1024 - 876];
} __attribute__((packed));

#define MAM_FLAGS_ENCRYPTION_FORMAT 0x0001

#define PCODE_SHIFT 7
#define PCODE_OFFSET(x) (x & ((1 << PCODE_SHIFT) - 1))

struct lu_phy_attr;

struct vpd {
	uint16_t sz;
	void (*vpd_update)(struct lu_phy_attr *lu, void *data);
	uint8_t data[0];
};

enum drive_type_list {
	drive_undefined,
	drive_LTO1,
	drive_LTO2,
	drive_LTO3,
	drive_LTO4,
	drive_LTO5,
	drive_LTO6,
	drive_3592_J1A,
	drive_3592_E05,
	drive_3592_E06,
	drive_DDS1,
	drive_DDS2,
	drive_DDS3,
	drive_DDS4,
	drive_DDS5,
	drive_AIT1,
	drive_AIT2,
	drive_AIT3,
	drive_AIT4,
	drive_10K_A,
	drive_10K_B,
	drive_10K_C,
	drive_DLT7K,
	drive_DLT8K,
	drive_SDLT,
	drive_SDLT220,
	drive_SDLT320,
	drive_SDLT600,
	drive_SDLT_S4,
	drive_9840_A,
	drive_9840_B,
	drive_9840_C,
	drive_9840_D,
	drive_9940_A,
	drive_9940_B,
	drive_UNKNOWN /* Always last */
};

/* Uniquely define each media type known */
enum Media_Type_list {
	Media_undefined,
	Media_LTO1,
	Media_LTO1_CLEAN,
	Media_LTO2,
	Media_LTO2_CLEAN,
	Media_LTO3,
	Media_LTO3_CLEAN,
	Media_LTO3_WORM,
	Media_LTO4,
	Media_LTO4_CLEAN,
	Media_LTO4_WORM,
	Media_LTO5,
	Media_LTO5_CLEAN,
	Media_LTO5_WORM,
	Media_LTO6,
	Media_LTO6_CLEAN,
	Media_LTO6_WORM,
	Media_3592_JA,
	Media_3592_JA_CLEAN,
	Media_3592_JW,
	Media_3592_JB,
	Media_3592_JB_CLEAN,
	Media_3592_JX,
	Media_3592_JX_CLEAN,
	Media_AIT1,
	Media_AIT1_CLEAN,
	Media_AIT2,
	Media_AIT2_CLEAN,
	Media_AIT3,
	Media_AIT3_CLEAN,
	Media_AIT4,
	Media_AIT4_CLEAN,
	Media_AIT4_WORM,
	Media_T10KA,
	Media_T10KA_CLEAN,
	Media_T10KA_WORM,
	Media_T10KB,
	Media_T10KB_CLEAN,
	Media_T10KB_WORM,
	Media_T10KC,
	Media_T10KC_CLEAN,
	Media_T10KC_WORM,
	Media_DLT2,
	Media_DLT2_CLEAN,
	Media_DLT3,
	Media_DLT3_CLEAN,
	Media_DLT4,
	Media_DLT4_CLEAN,
	Media_SDLT,
	Media_SDLT_CLEAN,
	Media_SDLT220,
	Media_SDLT220_CLEAN,
	Media_SDLT320,
	Media_SDLT320_CLEAN,
	Media_SDLT600,
	Media_SDLT600_CLEAN,
	Media_SDLT600_WORM,
	Media_SDLT_S4,
	Media_SDLT_S4_CLEAN,
	Media_SDLT_S4_WORM,
	Media_DDS1,
	Media_DDS1_CLEAN,
	Media_DDS2,
	Media_DDS2_CLEAN,
	Media_DDS3,
	Media_DDS3_CLEAN,
	Media_DDS4,
	Media_DDS4_CLEAN,
	Media_DDS5,
	Media_DDS5_CLEAN,
	Media_9840A,
	Media_9840A_CLEAN,
	Media_9840B,
	Media_9840B_CLEAN,
	Media_9840C,
	Media_9840C_CLEAN,
	Media_9840D,
	Media_9840D_CLEAN,
	Media_9940A,
	Media_9940A_CLEAN,
	Media_9940B,
	Media_9940B_CLEAN,
	Media_UNKNOWN /* always last */
};

struct scsi_cmd {
	uint8_t *scb;	/* SCSI Command Block */
	int scb_len;
	int cdev;	/* filepointer to char dev */
	struct vtl_ds *dbuf_p;
	struct lu_phy_attr *lu;
};

struct device_type_operations {
	uint8_t (*cmd_perform)(struct scsi_cmd *cmd);
	int (*pre_cmd_perform)(struct scsi_cmd *cmd, void *p);
	int (*post_cmd_perform)(struct scsi_cmd *cmd, void *p);
};

struct device_type_template {
	struct device_type_operations ops[256];
};

/* Logical Unit information */
struct lu_phy_attr {
	char ptype;
	char removable;
	char mode_media_type;
	char online;
	char vendor_id[VENDOR_ID_LEN + 1];
	char product_id[PRODUCT_ID_LEN + 1];
	char product_rev[PRODUCT_REV_LEN + 1];
	char lu_serial_no[SCSI_SN_LEN];
	uint16_t version_desc[3];

	struct list_head den_list;

	struct list_head mode_pg;

	struct list_head log_pg;

	struct device_type_template *scsi_ops;

	uint8_t *naa;
	struct vpd *lu_vpd[1 << PCODE_SHIFT];
	void *lu_private;	/* Private data struct per lu */

	struct report_luns report_luns;
	FILE *fifo_fd;
	char *fifoname;
	int fifo_flag;
};

/* Drive Info */
struct d_info {
	struct list_head siblings;
	char inq_vendor_id[10];
	char inq_product_id[18];
	char inq_product_rev[6];
	char inq_product_sno[12];
	long drv_id;		/* drive's send_msg queue ID */
	char online;		/* Physical status of drive */
	int SCSI_BUS;
	int SCSI_ID;
	int SCSI_LUN;
	char tapeLoaded;	/* Tape is 'loaded' by drive */
	struct s_info *slot;
};

struct m_info { /* Media Info */
	struct list_head siblings;
	uint32_t last_location;
	char barcode[MAX_BARCODE_LEN + 2];
	uint8_t media_domain;
	uint8_t media_type;
	uint8_t cart_type;
	uint8_t internal_status; /* internal states */
};

struct s_info { /* Slot Info */
	struct list_head siblings;
	uint32_t slot_location;
	uint32_t last_location;
	struct d_info *drive;
	struct m_info *media;
	/* Additional Sense Code & Additional Sense Code Qualifier */
	uint16_t asc_ascq;
	uint8_t	status;	/* Used for MAP status. */
	/* 1 Media Transport, 2 Storage, 3 MAP, 4 Data transfer */
	uint8_t element_type;
	uint8_t media_domain;	/* L700 */
	uint8_t media_type;	/* L700 */
};

#define DEF_SMC_PRIV_STATE_MSG_LENGTH 64

struct smc_priv {
	uint32_t bufsize;
	struct list_head drive_list;
	struct list_head slot_list;
	struct list_head media_list;
	int commandtimeout;	/* Timeout for 'movecommand' */
	int num_drives;
	int num_picker;
	int num_map;
	int num_storage;
	int dvcid_len;
	char dvcid_serial_only;
	char cap_closed;
	char *state_msg;	/* Custom State message */
	char *movecommand;	/* 3rd party command to call */
};

struct density_info {
	uint32_t bits_per_mm;
	uint16_t media_width;
	uint16_t tracks;
	uint32_t capacity;
	uint16_t density;
	char assigning_org[9];
	char density_name[9];
	char description[20];
};

struct supported_density_list {
	struct list_head siblings;
	struct density_info *density_info;
	int rw;
};

extern uint8_t sense[SENSE_BUF_SIZE];

/* Used by Mode Sense - if set, return block descriptor */
extern uint8_t modeBlockDescriptor[8];

enum MHVTL_STATE {
	MHVTL_STATE_INIT,
	MHVTL_STATE_IDLE,
/* Drive operation states */
	MHVTL_STATE_UNLOADED,
	MHVTL_STATE_LOADING,
	MHVTL_STATE_LOADING_CLEAN,
	MHVTL_STATE_LOADING_WORM,
	MHVTL_STATE_LOADED,
	MHVTL_STATE_LOADED_IDLE,
	MHVTL_STATE_LOAD_FAILED,
	MHVTL_STATE_REWIND,
	MHVTL_STATE_POSITIONING,
	MHVTL_STATE_LOCATE,
	MHVTL_STATE_READING,
	MHVTL_STATE_WRITING,
	MHVTL_STATE_UNLOADING,
	MHVTL_STATE_ERASE,

/* Library operation states */
	MHVTL_STATE_MOVING_DRIVE_2_SLOT,
	MHVTL_STATE_MOVING_SLOT_2_DRIVE,
	MHVTL_STATE_MOVING_DRIVE_2_MAP,
	MHVTL_STATE_MOVING_MAP_2_DRIVE,
	MHVTL_STATE_MOVING_SLOT_2_MAP,
	MHVTL_STATE_MOVING_MAP_2_SLOT,
	MHVTL_STATE_MOVING_DRIVE_2_DRIVE,
	MHVTL_STATE_MOVING_SLOT_2_SLOT,
	MHVTL_STATE_OPENING_MAP,
	MHVTL_STATE_CLOSING_MAP,
	MHVTL_STATE_INVENTORY,
	MHVTL_STATE_INITIALISE_ELEMENTS,
	MHVTL_STATE_ONLINE,
	MHVTL_STATE_OFFLINE,
	MHVTL_STATE_UNKNOWN,
};

int check_reset(uint8_t *);
void reset_device(void);
void mkSenseBuf(uint8_t, uint32_t, uint8_t *);
void resp_log_select(uint8_t *, uint8_t *);
int resp_read_position_long(loff_t, uint8_t *, uint8_t *);
int resp_read_position(loff_t, uint8_t *, uint8_t *);
int resp_report_lun(struct report_luns *, uint8_t *, uint8_t *);
int resp_read_media_serial(uint8_t *, uint8_t *, uint8_t *);
int resp_mode_sense(uint8_t *, uint8_t *, struct mode *, uint8_t, uint8_t *);
struct mode *lookup_pcode(struct list_head *l, uint8_t pcode, uint8_t subpcode);
int resp_read_block_limits(struct vtl_ds *dbuf_p, int sz);

void hex_dump(uint8_t *, int);
int chrdev_open(char *name, uint8_t);
int chrdev_create(uint8_t minor);
int chrdev_chown(uint8_t minor, uid_t uid, gid_t gid);
int oom_adjust(void);
int open_fifo(FILE **fifo_fd, char *fifoname);
void status_change(FILE *fifo_fd, int current_status, int my_id, char **msg);

char *readline(char *s, int len, FILE *f);
void blank_fill(uint8_t *dest, char *src, int len);

void log_opcode(char *opcode, uint8_t *SCpnt, struct vtl_ds *dbuf_p);

struct vpd *alloc_vpd(uint16_t sz);
pid_t add_lu(int minor, struct vtl_ctl *ctl);

void completeSCSICommand(int, struct vtl_ds *ds);
void getCommand(int, struct vtl_header *);
int retrieve_CDB_data(int cdev, struct vtl_ds *dbuf_p);
void get_sn_inquiry(int, struct vtl_sn_inquiry *);
int check_for_running_daemons(int minor);

void mhvtl_prt_cdb(int l, uint64_t sn, uint8_t * cdb);
void checkstrlen(char *s, unsigned int len);
extern int device_type_register(struct lu_phy_attr *lu,
					struct device_type_template *t);

void process_fifoname(struct lu_phy_attr *lu, char *s, int flag);

uint8_t clear_WORM(struct list_head *l);
uint8_t set_WORM(struct list_head *l);
uint8_t clear_compression_mode_pg(struct list_head *l);
uint8_t set_compression_mode_pg(struct list_head *l, int lvl);

void rmnl(char *s, unsigned char c, int len);
void truncate_spaces(char *s, int maxlen);
char *get_version(void);

void update_vpd_86(struct lu_phy_attr *lu, void *p);
void update_vpd_b0(struct lu_phy_attr *lu, void *p);
void update_vpd_b1(struct lu_phy_attr *lu, void *p);
void update_vpd_b2(struct lu_phy_attr *lu, void *p);
void update_vpd_c0(struct lu_phy_attr *lu, void *p);
void update_vpd_c1(struct lu_phy_attr *lu, void *p);

int get_fifo_count(char *path);
int dec_fifo_count(char *path);
int inc_fifo_count(char *path);
void cleanup_msg(void);

int add_density_support(struct list_head *l, struct density_info *di, int rw);
int add_drive_media_list(struct lu_phy_attr *lu, int status, char *s);

#endif /*  _VTLLIB_H_ */
