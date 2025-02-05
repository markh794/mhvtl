/*
 * This handles any SCSI OP 'log sense / log select'
 *
 * Copyright (C) 2005 - 2025 Mark Harvey markh794 at gmail dot com
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
 *
 * See comments in vtltape.c for a more complete version release...
 *
 */

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
#define DEVICE_STATUS 0x11
#define TAPE_ALERT 0x2e
#define INFORMATIONAL_EXCEPTIONS 0x2f
#define TAPE_USAGE 0x30
#define TAPE_CAPACITY 0x31
#define DATA_COMPRESSION 0x32

struct vhf_data_4 {
	uint8_t DINIT:1;	/* Device Initialized - 0 not initialised*/
	uint8_t CRQRD:1;	/* Cleaning required - before media is mounted (required) */
	uint8_t CRQST:1;	/* Cleaning required - non urgent (request) */
	uint8_t WRTP:1;		/* Physical Write Protect */
	uint8_t CMPR:1;		/* Compression enabled */
	uint8_t MACC:1;		/* MAM accessible */
	uint8_t HIU:1;		/* Host Initiated Unload */
	uint8_t	PAMR:1;		/* Prevent/Allow Media Removal */
	} __attribute__((packed));

struct vhf_data_5 {
	uint8_t MOUNTED:1;	/* Medium mounted */
	uint8_t MTHRD:1;	/* Medium Threaded */
	uint8_t MSTD:1;		/* Medium Seated */
	uint8_t RSVD_2:1;
	uint8_t MPRSNT:1;	/* Medium Present */
	uint8_t RAA:1;		/* Robotic access allowed */
	uint8_t RSVD_1:1;
	uint8_t	INXTN:1;	/* In Transition - other bits in byte 5 not stable */
	} __attribute__((packed));

struct vhf_data_7 {
	uint8_t TAFC:1;		/* TapeAlert state flag changed */
	uint8_t INITFC:1;	/* Interface changed */
	uint8_t RRQST:1;	/* Recovery requested */
	uint8_t ESR:1;		/* Encryption Service Requested */
	uint8_t EPP:1;		/* Encryption Parameters Present */
	uint8_t TDDEC:1;	/* Tape Diagnostic data entry created */
	uint8_t RSVD:1;
	uint8_t	VS:1;		/* Always 0 */
	} __attribute__((packed));

struct log_pg_list {
	struct list_head siblings;
	char *description;
	int log_page_num;
	int size;
	void *p;
};

/* Log Page header */
struct	log_pg_header {
	uint8_t pcode;
	uint8_t res;
	uint16_t len;
	} __attribute__((packed));

/* Page Code header struct. */
struct	pc_header {
	uint8_t head0;
	uint8_t head1;
	uint8_t flags;
	uint8_t len;
	} __attribute__((packed));

/* Vendor Specific : 0x32 (Taken from IBM Ultrium doco) */
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
	} __attribute__((packed));

/* Buffer Under/Over Run log page - 0x01 : SPC-3 (7.2.3) */
struct	BufferUnderOverRun {
	struct log_pg_header	pcode_head;
	} __attribute__((packed));

struct	TapeUsage {
	struct log_pg_header pcode_head;
	struct pc_header flagNo01;
	uint32_t volumeMounts;
	struct pc_header flagNo02;
	uint64_t volumeDatasetsWritten;
	struct pc_header flagNo03;
	uint32_t volWriteRetries;
	struct pc_header flagNo04;
	uint16_t volWritePerms;
	struct pc_header flagNo05;
	uint16_t volSuspendedWrites;
	struct pc_header flagNo06;
	uint16_t volFatalSuspendedWrites;
	struct pc_header flagNo07;
	uint64_t volDatasetsRead;
	struct pc_header flagNo08;
	uint32_t volReadRetries;
	struct pc_header flagNo09;
	uint16_t volReadPerms;
	struct pc_header flagNo10;
	uint16_t volSuspendedReads;
	struct pc_header flagNo11;
	uint16_t volFatalSuspendedReads;
	} __attribute__((packed));

struct DeviceStatus {
	struct log_pg_header pcode_head;
	struct pc_header vhf_data;
	uint8_t byte4;
	uint8_t byte5;
	uint8_t byte6;
	uint8_t byte7;
	} __attribute__((packed));

struct	TapeCapacity {
	struct log_pg_header pcode_head;
	struct pc_header flagNo01;
	uint32_t partition0remaining;
	struct pc_header flagNo02;
	uint32_t partition1remaining;
	struct pc_header flagNo03;
	uint32_t partition0maximum;
	struct pc_header flagNo04;
	uint32_t partition1maximum;
	} __attribute__((packed));

struct TapeAlert_pg {
	struct pc_header flag;
	uint8_t value;
	} __attribute__((packed));

/* Tape Alert Log Page - 0x2E
 * SSC-3 (8.2.3)
 */
struct	TapeAlert_page {
	struct log_pg_header pcode_head;

	struct TapeAlert_pg TapeAlert[64];
	} __attribute__((packed));

/* Temperature Log Page - 0x0d
 * SPC-3 (7.2.13)
 */
struct	Temperature_page {
	struct log_pg_header pcode_head;
	struct pc_header header;
	uint16_t temperature;
	} __attribute__((packed));

/* Write/Read/Read Reverse
 * Error Counter log page - 0x02, 0x03, 0x04
 * SPC-3 (7.2.4)
 */
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
	} __attribute__((packed));

/* Sequential-Access
 * Device log page - 0x0C
 * SSC-3 (Ch 8.2.2)
 */
struct	seqAccessDevice {
	struct log_pg_header	pcode_head;

	struct pc_header h_writeDataB4;
	uint64_t writeDataB4Compression; /* Write. Bytes from initiator */
	struct pc_header h_writeData_Af;
	uint64_t writeDataAfCompression; /* Write. Bytes written to media */

	struct pc_header h_readData_b4;
	uint64_t readDataB4Compression; /* Read. Bytes read from media */
	struct pc_header h_readData_Af;
	uint64_t readDataAfCompression; /* Read. Bytes to initiator */

	struct pc_header h_bop_eod;
	uint32_t capacity_bop_eod; /* Native capacity BOT to EOD */

	struct pc_header h_bop_ew;
	uint32_t capacity_bop_ew; /* Native capacity BOP to EW */

	struct pc_header h_ew_leop;
	uint32_t capacity_ew_leop; /* Native capacity EW and
				    * Logical End Of Partition */

	struct pc_header h_bop_curr;
	uint32_t capacity_bop_curr; /* Native capacity BOP to curr pos */

	struct pc_header h_buffer;
	uint32_t capacity_buffer; /* Native capacity in buffer */

	struct pc_header h_cleaning;
	uint64_t TapeAlert;

	struct pc_header h_mbytes_processed;
	uint32_t mbytes_processed;	/* MB since cleaning */

	struct pc_header h_load_cycle;
	uint32_t load_cycle;		/* Total number of load cycles over drive lifetime*/

	struct pc_header h_clean;
	uint32_t clean_cycle;		/* Total number of cleans over drive lifetime */

	} __attribute__((packed));

void setTapeAlert(struct TapeAlert_page *, uint64_t);
void initTapeAlert(struct TapeAlert_page *);
void dealloc_all_log_pages(struct lu_phy_attr *lu);

int update_TapeAlert(struct lu_phy_attr *lu, uint64_t flags);
int set_TapeAlert(struct lu_phy_attr *lu, uint64_t flags);
void *get_vhf_byte(struct lu_phy_attr *lu, int offset);

struct log_pg_list *lookup_log_pg(struct list_head *l, uint8_t page);
struct log_pg_list *alloc_log_page(struct list_head *l, uint8_t page, int size);

int add_log_write_err_counter(struct lu_phy_attr *lu);
int add_log_read_err_counter(struct lu_phy_attr *lu);
int add_log_sequential_access(struct lu_phy_attr *lu);
int add_log_temperature_page(struct lu_phy_attr *lu);
int add_log_tape_alert(struct lu_phy_attr *lu);
int add_log_tape_usage(struct lu_phy_attr *lu);
int add_log_tape_capacity(struct lu_phy_attr *lu);
int add_log_data_compression(struct lu_phy_attr *lu);
int add_log_device_status(struct lu_phy_attr *lu);
