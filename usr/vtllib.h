/*
 * The shared library libvxshared.so function defination
 *
 * $Id: vxshared.h,v 1.3.2.1 2006-08-06 07:58:44 markh Exp $
 *
 * Copyright (C) 2005 Mark Harvey markh794 at gmail dot com
 *                                mark_harvey at symantec dot com
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
#ifndef Solaris
  #include <endian.h>
  #include <byteswap.h>
#endif

#define HOME_CONFIG_PATH "/etc/mhvtl"

#if __BYTE_ORDER == __BIG_ENDIAN

#define ntohll(x)	(x)
#define ntohl(x)	(x)
#define ntohs(x)	(x)
#define htonll(x)	(x)
#define htonl(x)	(x)
#define htons(x)	(x)
#else
# if __BYTE_ORDER == __LITTLE_ENDIAN
#  define ntohll(x)	__bswap_64 (x)
#  define ntohl(x)	__bswap_32 (x)
#  define ntohs(x)	__bswap_16 (x)
#  define htonll(x)	__bswap_64 (x)
#  define htonl(x)	__bswap_32 (x)
#  define htons(x)	__bswap_16 (x)
# endif
#endif

#define EOM_FLAG 0x40

#define STATUS_OK 0

#define STATUS_QUEUE_CMD 0xfe

#define SENSE_BUF_SIZE 38

#define SCSI_SN_LEN 16

/* Where all the tape data files belong */
#define HOME_PATH "/opt/vtl"

/*
 * Process the LOG_SENSE page definations
 */
#define BUFFER_UNDER_OVER_RUN 0x01
#define WRITE_ERROR_COUNTER 0x02
#define READ_ERROR_COUNTER 0x03
#define READ_REVERSE_ERROR_COUNTER 0x04
#define VERIFY_ERROR_COUNTER 0x05
#define NON_MEDIUM_ERROR_COUNTER 0x06
#define LAST_n_ERROR 0x07
#define FORMAT_STATUS 0x08
#define LAST_n_DEFERRED_ERROR 0x0b
#define SEQUENTIAL_ACCESS_DEVICE 0x0c
#define TEMPERATURE_PAGE 0x0d
#define START_STOP_CYCLE_COUNTER 0x0e
#define APPLICATION_CLIENT 0x0f
#define SELFTEST_RESULTS 0x10
#define TAPE_ALERT 0x2e
#define INFORMATIONAL_EXCEPTIONS 0x2f
#define TAPE_USAGE 0x30
#define TAPE_CAPACITY 0x31
#define DATA_COMPRESSION 0x32

#define MAX_INQ_ARR_SZ 64

/*
 * Medium Type Definations
 */
#define MEDIA_TYPE_DATA 0
#define MEDIA_TYPE_WORM 1
#define MEDIA_TYPE_CLEAN 6

// Log Page header
struct	log_pg_header {
	uint8_t pcode;
	uint8_t res;
	uint16_t len;
	};

// Page Code header struct.
struct	pc_header {
	uint8_t head0;
	uint8_t head1;
	uint8_t flags;
	uint8_t len;
	};

// Vendor Specific : 0x32 (Taken from IBM Ultrium doco)
struct	DataCompression {
	struct log_pg_header pcode_head;

	struct pc_header h_ReadCompressionRatio;
	uint16_t ReadCompressionRatio;
	struct pc_header h_WriteCompressionRatio;
	uint16_t WriteCompressionRatio;
	struct pc_header h_MBytesToServer;
	uint32_t MBytesToServer;
	struct pc_header h_BytesToServer;
	uint32_t BytesToServer;
	struct pc_header h_MBytesReadFromTape;
	uint32_t MBytesReadFromTape;
	struct pc_header h_BytesReadFromTape;
	uint32_t BytesReadFromTape;

	struct pc_header h_MBytesFromServer;
	uint32_t MBytesFromServer;
	struct pc_header h_BytesFromServer;
	uint32_t BytesFromServer;
	struct pc_header h_MBytesWrittenToTape;
	uint32_t MBytesWrittenToTape;
	struct pc_header h_BytesWrittenToTape;
	uint32_t BytesWrittenToTape;
	};

// Buffer Under/Over Run log page - 0x01 : SPC-3 (7.2.3)
struct	BufferUnderOverRun {
	struct log_pg_header	pcode_head;
	};

struct	TapeUsage {
	struct log_pg_header pcode_head;
	struct pc_header flagNo01;
	uint32_t value01;
	struct pc_header flagNo02;
	uint64_t value02;
	struct pc_header flagNo03;
	uint32_t value03;
	struct pc_header flagNo04;
	uint16_t value04;
	struct pc_header flagNo05;
	uint16_t value05;
	struct pc_header flagNo06;
	uint16_t value06;
	struct pc_header flagNo07;
	uint64_t value07;
	struct pc_header flagNo08;
	uint32_t value08;
	struct pc_header flagNo09;
	uint16_t value09;
	struct pc_header flagNo10;
	uint16_t value10;
	struct pc_header flagNo11;
	uint16_t value11;
	};

struct	TapeCapacity {
	struct log_pg_header pcode_head;
	struct pc_header flagNo01;
	uint32_t value01;
	struct pc_header flagNo02;
	uint32_t value02;
	struct pc_header flagNo03;
	uint32_t value03;
	struct pc_header flagNo04;
	uint32_t value04;
	};

struct TapeAlert_pg {
	struct pc_header flag;
	uint8_t value;
	};

// Tape Alert Log Page - 0x2E : SSC-3 (8.2.3)
struct	TapeAlert_page {
	struct log_pg_header pcode_head;

	struct TapeAlert_pg TapeAlert[64];
	};

// Temperature Log Page - 0x0d : SPC-3 (7.2.13)
struct	Temperature_page {
	struct log_pg_header pcode_head;
	struct pc_header header;
	uint16_t temperature;
	};

// Write/Read/Read Reverse
// Error Counter log page - 0x02, 0x03, 0x04 : SPC-3 (7.2.4)
struct	error_counter {
	struct log_pg_header pcode_head;

	struct pc_header h_err_correctedWODelay;
	uint32_t err_correctedWODelay;
	struct pc_header h_err_correctedWDelay;
	uint32_t err_correctedWDelay;
	struct pc_header h_totalReTry;
	uint32_t totalReTry;
	struct pc_header h_totalErrorsCorrected;
	uint32_t totalErrorsCorrected;
	struct pc_header h_correctAlgorithm;
	uint32_t correctAlgorithm;
	struct pc_header h_bytesProcessed;
	uint64_t bytesProcessed;
	struct pc_header h_uncorrectedErrors;
	uint32_t uncorrectedErrors;
	struct pc_header h_readErrorsSinceLast;
	uint32_t readErrorsSinceLast;
	struct pc_header h_totalRawReadError;
	uint32_t totalRawReadError;
	struct pc_header h_totalDropoutError;
	uint32_t totalDropoutError;
	struct pc_header h_totalServoTracking;
	uint32_t totalServoTracking;
	};

// Sequential-Access
// Device log page - 0x0C : SSC-3 (Ch 8.2.2)
struct	seqAccessDevice {
	struct log_pg_header	pcode_head;

	struct pc_header h_writeDataB4;
	uint64_t writeDataB4Compression;
	struct pc_header h_writeData_Af;
	uint64_t writeDataAfCompression;

	struct pc_header h_readData_b4;
	uint64_t readDataB4Compression;
	struct pc_header h_readData_Af;
	uint64_t readDataAfCompression;

	struct pc_header h_cleaning;
	uint64_t TapeAlert;

	struct pc_header h_mbytes_processed;
	uint32_t mbytes_processed;

	struct pc_header h_load_cycle;
	uint32_t load_cycle;

	struct pc_header h_clean;	// Header of clean
	uint32_t clean_cycle;

	};


struct report_luns {
	uint32_t size;
	uint32_t reserved;
	uint32_t LUN_0;
	};

struct mode {
	uint8_t pcode;		// Page code
	uint8_t subpcode;	// Sub page code
	int32_t pcodeSize;	// Size of page code data.
	uint8_t *pcodePointer;	// Pointer to page code - NULL end of array..
	};

/* v2 of the tape media
 * Between BOT & blk #1, is the MAM (Medium Auxiliary Memory)
 */
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
	uint8_t MediumType;	// 0 -> Data, 1 -> WORM, 6 -> Clean
	uint64_t MAMCapacity;
	uint16_t MediumTypeInformation;	// If Clean, max mount

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
	uint8_t pad[1024 - 888];

};

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
	drive_3592_J1A,
	drive_3592_E05,
	drive_3592_E06,
	drive_AIT4,
	drive_10K_A,
	drive_10K_B,
	drive_SDLT600,
	drive_UNKNOWN /* Always last */
};

/* Logical Unit information */
struct lu_phy_attr {
	char ptype;
	char removable;
	uint8_t drive_type;
	int drive_native_write_density[drive_UNKNOWN + 1];
	char vendor_id[VENDOR_ID_LEN + 1];
	char product_id[PRODUCT_ID_LEN + 1];
	char product_rev[PRODUCT_REV_LEN + 1];
	char lu_serial_no[SCSI_SN_LEN];
	uint16_t version_desc[3];
	uint8_t supported_density[8];
	void *lu_private;	/* Private data struct per lu */
	struct vpd *lu_vpd[1 << PCODE_SHIFT];
};

int send_msg(char *, int);
void logSCSICommand(uint8_t *);
int check_reset(uint8_t *);
void mkSenseBuf(uint8_t, uint32_t, uint8_t *);
void resp_allow_prevent_removal(uint8_t *, uint8_t *);
void resp_log_select(uint8_t *, uint8_t *);
int resp_read_position_long(loff_t, uint8_t *, uint8_t *);
int resp_read_position(loff_t, uint8_t *, uint8_t *);
int resp_report_lun(struct report_luns *, uint8_t *, uint8_t *);
int resp_read_media_serial(uint8_t *, uint8_t *, uint8_t *);
int resp_mode_sense(uint8_t *, uint8_t *, struct mode *, uint8_t, uint8_t *);
//int32_t resp_write_attribute(uint8_t *, uint64_t, struct MAM *, uint8_t *);
struct mode *find_pcode(uint8_t, struct mode *);
struct mode *alloc_mode_page(uint8_t, struct mode *, int);
int resp_read_block_limits(struct vtl_ds *dbuf_p, int sz);

void setTapeAlert(struct TapeAlert_page *, uint64_t);
void initTapeAlert(struct TapeAlert_page *);

void hex_dump(uint8_t *, int);
int chrdev_open(char *name, uint8_t);
int oom_adjust(void);

void log_opcode(char *opcode, uint8_t *SCpnt, uint8_t *sam_stat);
int resp_a3_service_action(uint8_t *cdb, uint8_t *sam_stat);
int resp_a4_service_action(uint8_t *cdb, uint8_t *sam_stat);
int ProcessSendDiagnostic(uint8_t *cdb, int sz, uint8_t *buf, uint32_t block_size, uint8_t *sam_stat);
int ProcessReceiveDiagnostic(uint8_t *cdb, uint8_t *buf, uint8_t *sam_stat);

struct vpd *alloc_vpd(uint16_t sz);
pid_t add_lu(int minor, struct vtl_ctl *ctl);

void completeSCSICommand(int, struct vtl_ds *ds);
void getCommand(int, struct vtl_header *);
int retrieve_CDB_data(int cdev, struct vtl_ds *dbuf_p);
void get_sn_inquiry(int, struct vtl_sn_inquiry *);
int check_for_running_daemons(int minor);

