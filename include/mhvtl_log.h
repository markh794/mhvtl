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

#ifndef MHVTL_LOG_H
#define MHVTL_LOG_H

#include <stdint.h>
#include "mhvtl_list.h"

struct lu_phy_attr;
typedef void (*init_pg_fn)(void *log_ptr);

/*
 * Process the LOG_SENSE page definitions
 */
#define SUPPORTED_LOG_PAGES		   0x00
#define BUFFER_UNDER_OVER_RUN	   0x01
#define WRITE_ERROR_COUNTER		   0x02
#define READ_ERROR_COUNTER		   0x03
#define READ_REVERSE_ERROR_COUNTER 0x04
#define VERIFY_ERROR_COUNTER	   0x05
#define NON_MEDIUM_ERROR_COUNTER   0x06
#define LAST_n_ERROR			   0x07
#define FORMAT_STATUS			   0x08
#define LAST_n_DEFERRED_ERROR	   0x0b
#define SEQUENTIAL_ACCESS_DEVICE   0x0c
#define TEMPERATURE_PAGE		   0x0d
#define START_STOP_CYCLE_COUNTER   0x0e
#define APPLICATION_CLIENT		   0x0f
#define SELFTEST_RESULTS		   0x10
#define DEVICE_STATUS			   0x11
#define VOLUME_STATISTICS		   0x17
#define TAPE_ALERT				   0x2e
#define INFORMATIONAL_EXCEPTIONS   0x2f
#define TAPE_USAGE				   0x30
#define TAPE_CAPACITY			   0x31
#define DATA_COMPRESSION		   0x32

#define NO_SUBPAGE 0x00

struct log_pg_list {
	struct list_head siblings;
	char			*description;
	int				 log_page_num;
	int				 log_subpage_num;
	int				 size;
	void			*p;
};

/* Log Page header */
struct log_pg_header {
	uint8_t	 pcode;
	uint8_t	 res;
	uint16_t len;
} __attribute__((packed));

/* Page Code header struct. */
struct pc_header {
	uint8_t head0;
	uint8_t head1;
	uint8_t flags;
	uint8_t len;
} __attribute__((packed));

/* Vendor Specific : 0x32 (Taken from IBM Ultrium doco) */
struct DataCompression_pg {
	struct log_pg_header pcode_head;

	struct pc_header h_ReadCompressionRatio;
	uint16_t		 ReadCompressionRatio;
	struct pc_header h_WriteCompressionRatio;
	uint16_t		 WriteCompressionRatio;
	struct pc_header h_MBytesToServer;
	uint32_t		 MBytesToServer;
	struct pc_header h_BytesToServer;
	uint32_t		 BytesToServer;
	struct pc_header h_MBytesReadFromTape;
	uint32_t		 MBytesReadFromTape;
	struct pc_header h_BytesReadFromTape;
	uint32_t		 BytesReadFromTape;

	struct pc_header h_MBytesFromServer;
	uint32_t		 MBytesFromServer;
	struct pc_header h_BytesFromServer;
	uint32_t		 BytesFromServer;
	struct pc_header h_MBytesWrittenToTape;
	uint32_t		 MBytesWrittenToTape;
	struct pc_header h_BytesWrittenToTape;
	uint32_t		 BytesWrittenToTape;
} __attribute__((packed));

/* Buffer Under/Over Run log page - 0x01 : SPC-3 (7.2.3) */
struct BufferUnderOverRun_pg {
	struct log_pg_header pcode_head;
} __attribute__((packed));

struct TapeUsage_pg {
	struct log_pg_header pcode_head;
	struct pc_header	 flagNo01;
	uint32_t			 volumeMounts;
	struct pc_header	 flagNo02;
	uint64_t			 volumeDatasetsWritten;
	struct pc_header	 flagNo03;
	uint32_t			 volWriteRetries;
	struct pc_header	 flagNo04;
	uint16_t			 volWritePerms;
	struct pc_header	 flagNo05;
	uint16_t			 volSuspendedWrites;
	struct pc_header	 flagNo06;
	uint16_t			 volFatalSuspendedWrites;
	struct pc_header	 flagNo07;
	uint64_t			 volDatasetsRead;
	struct pc_header	 flagNo08;
	uint32_t			 volReadRetries;
	struct pc_header	 flagNo09;
	uint16_t			 volReadPerms;
	struct pc_header	 flagNo10;
	uint16_t			 volSuspendedReads;
	struct pc_header	 flagNo11;
	uint16_t			 volFatalSuspendedReads;
} __attribute__((packed));

struct DeviceStatus_pg {
	struct log_pg_header pcode_head;

	struct pc_header h_vhf;
	struct __attribute__((packed)) {
		struct __attribute__((packed)) {
			uint8_t DINIT : 1; /* Device Initialized - 0 not initialised*/
			uint8_t CRQRD : 1; /* Cleaning required - before media is mounted (required) */
			uint8_t CRQST : 1; /* Cleaning required - non urgent (request) */
			uint8_t WRTP : 1;  /* Physical Write Protect */
			uint8_t CMPR : 1;  /* Compression enabled */
			uint8_t MACC : 1;  /* MAM accessible */
			uint8_t HIU : 1;   /* Host Initiated Unload */
			uint8_t PAMR : 1;  /* Prevent/Allow Media Removal */
		} b4;

		struct __attribute__((packed)) {
			uint8_t MOUNTED : 1; /* Medium mounted */
			uint8_t MTHRD : 1;	 /* Medium Threaded */
			uint8_t MSTD : 1;	 /* Medium Seated */
			uint8_t RSVD_2 : 1;
			uint8_t MPRSNT : 1; /* Medium Present */
			uint8_t RAA : 1;	/* Robotic access allowed */
			uint8_t RSVD_1 : 1;
			uint8_t INXTN : 1; /* In Transition - other bits in byte 5 not stable */
		} b5;

		uint8_t b6; /* DT Device Activity */

		struct __attribute__((packed)) {
			uint8_t TAFC : 1;	/* TapeAlert state flag changed */
			uint8_t INITFC : 1; /* Interface changed */
			uint8_t RRQST : 1;	/* Recovery requested */
			uint8_t ESR : 1;	/* Encryption Service Requested */
			uint8_t EPP : 1;	/* Encryption Parameters Present */
			uint8_t TDDEC : 1;	/* Tape Diagnostic data entry created */
			uint8_t RSVD : 1;
			uint8_t VS : 1; /* Always 0 */
		} b7;

	} vhf; /* Very High Frequency data */
} __attribute__((packed));

struct TapeCapacity_pg {
	struct log_pg_header pcode_head;
	struct pc_header	 flagNo01;
	uint32_t			 partition0remaining;
	struct pc_header	 flagNo02;
	uint32_t			 partition1remaining;
	struct pc_header	 flagNo03;
	uint32_t			 partition0maximum;
	struct pc_header	 flagNo04;
	uint32_t			 partition1maximum;
} __attribute__((packed));

/* Volume Statistics - 0x17
 * SSC-4
 */
struct partition_record_header {
	uint8_t	 len;
	uint8_t	 reserved;
	uint16_t partition_no;

} __attribute__((packed));

struct partition_record_size4 {
	struct partition_record_header header;
	uint32_t					   data;
} __attribute__((packed));

struct partition_record_size6 {
	struct partition_record_header header;
	uint8_t						   data[6];
} __attribute__((packed));

struct VolumeStatistics_pg {
	struct log_pg_header pcode_head;

	struct pc_header h_PageValid;
	uint8_t			 PageValid;

	struct pc_header h_VolumeMounts;
	uint32_t		 VolumeMounts;

	struct pc_header h_VolumeDatasetsWritten;
	uint64_t		 VolumeDatasetsWritten;

	struct pc_header h_RecoveredWriteDataErrors;
	uint32_t		 RecoveredWriteDataErrors;

	struct pc_header h_UnrecoveredWriteDataErrors;
	uint16_t		 UnrecoveredWriteDataErrors;

	struct pc_header h_WriteServoErrors;
	uint16_t		 WriteServoErrors;

	struct pc_header h_UnrecoveredWriteServoErrors;
	uint16_t		 UnrecoveredWriteServoErrors;

	struct pc_header h_VolumeDatasetsRead;
	uint64_t		 VolumeDatasetsRead;

	struct pc_header h_RecoveredReadErrors;
	uint32_t		 RecoveredReadErrors;

	struct pc_header h_UnrecoveredReadErrors;
	uint16_t		 UnrecoveredReadErrors;

	struct pc_header h_LastMountUnrecoveredWriteErrors;
	uint16_t		 LastMountUnrecoveredWriteErrors;

	struct pc_header h_LastMountUnrecoveredReadErrors;
	uint16_t		 LastMountUnrecoveredReadErrors;

	struct pc_header h_LastMountMBWritten;
	uint32_t		 LastMountMBWritten;

	struct pc_header h_LastMountMBRead;
	uint32_t		 LastMountMBRead;

	struct pc_header h_LifetimeMBWritten;
	uint64_t		 LifetimeMBWritten;

	struct pc_header h_LifetimeMBRead;
	uint64_t		 LifetimeMBRead;

	struct pc_header h_LastLoadWriteCompressionRatio;
	uint16_t		 LastLoadWriteCompressionRatio;

	struct pc_header h_LastLoadReadCompressionRatio;
	uint16_t		 LastLoadReadCompressionRatio;

	struct pc_header h_MediumMountTime;
	uint8_t			 MediumMountTime[6];

	struct pc_header h_MediumReadyTime;
	uint8_t			 MediumReadyTime[6];

	struct pc_header h_TotalNativeCapacity;
	uint32_t		 TotalNativeCapacity;

	struct pc_header h_TotalUsedNativeCapacity;
	uint32_t		 TotalUsedNativeCapacity;

	struct pc_header h_AppDesignCapacity;
	uint32_t		 AppDesignCapacity;

	struct pc_header h_VolumeLifetimeRemaining;
	uint8_t			 VolumeLifetimeRemaining;

	struct pc_header h_VolumeSerialNumber;
	uint8_t			 VolumeSerialNumber[32];

	struct pc_header h_TapeLotIdentifier;
	uint8_t			 TapeLotIdentifier[8];

	struct pc_header h_VolumeBarcode;
	uint8_t			 VolumeBarcode[32];

	struct pc_header h_VolumeManufacturer;
	uint8_t			 VolumeManufacturer[8];

	struct pc_header h_VolumeLicenseCode;
	uint8_t			 VolumeLicenseCode[4];

	struct pc_header h_VolumePersonality;
	uint8_t			 VolumePersonality[9];

	struct pc_header h_WriteProtect;
	uint8_t			 WriteProtect;

	struct pc_header h_WORM;
	uint8_t			 WORM;

	struct pc_header h_TempExceeded;
	uint8_t			 TempExceeded;

	struct pc_header h_BOMPasses;
	uint32_t		 BOMPasses;

	struct pc_header h_MOTPasses;
	uint32_t		 MOTPasses;

	/* size depends on actual nb of partitions
	=> we put max size by default and adapt dynamically later */
	struct pc_header			  h_FirstEncryptedLogicalObj;
	struct partition_record_size6 FirstEncryptedLogicalObj[MAX_PARTITIONS];

	struct pc_header			  h_FirstUnencryptedLogicalObj;
	struct partition_record_size6 FirstUnencryptedLogicalObj[MAX_PARTITIONS];

	struct pc_header			  h_ApproxNativeCapacityPartition;
	struct partition_record_size4 ApproxNativeCapacityPartition[MAX_PARTITIONS];

	struct pc_header			  h_ApproxUsedNativeCapacityPartition;
	struct partition_record_size4 ApproxUsedNativeCapacityPartition[MAX_PARTITIONS];

	struct pc_header			  h_RemainingCapacityToEWPartition;
	struct partition_record_size4 RemainingCapacityToEWPartition[MAX_PARTITIONS];

} __attribute__((packed));

struct TapeAlert_flag {
	struct pc_header flag;
	uint8_t			 value;
} __attribute__((packed));

/* Tape Alert Log Page - 0x2E
 * SSC-3 (8.2.3)
 */
struct TapeAlert_pg {
	struct log_pg_header pcode_head;

	struct TapeAlert_flag TapeAlert[64];
} __attribute__((packed));

/* Temperature Log Page - 0x0d
 * SPC-3 (7.2.13)
 */
struct Temperature_pg {
	struct log_pg_header pcode_head;
	struct pc_header	 header;
	uint16_t			 temperature;
} __attribute__((packed));

/* Write/Read/Read Reverse
 * Error Counter log page - 0x02, 0x03, 0x04
 * SPC-3 (7.2.4)
 */
struct ErrorCounter_pg {
	struct log_pg_header pcode_head;

	struct pc_header h_err_correctedWODelay;
	uint32_t		 err_correctedWODelay;
	struct pc_header h_err_correctedWDelay;
	uint32_t		 err_correctedWDelay;
	struct pc_header h_totalReTry;
	uint32_t		 totalReTry;
	struct pc_header h_totalErrorsCorrected;
	uint32_t		 totalErrorsCorrected;
	struct pc_header h_correctAlgorithm;
	uint32_t		 correctAlgorithm;
	struct pc_header h_bytesProcessed;
	uint64_t		 bytesProcessed;
	struct pc_header h_uncorrectedErrors;
	uint32_t		 uncorrectedErrors;
	struct pc_header h_readErrorsSinceLast;
	uint32_t		 readErrorsSinceLast;
	struct pc_header h_totalRawReadError;
	uint32_t		 totalRawReadError;
	struct pc_header h_totalDropoutError;
	uint32_t		 totalDropoutError;
	struct pc_header h_totalServoTracking;
	uint32_t		 totalServoTracking;
} __attribute__((packed));

/* Sequential-Access
 * Device log page - 0x0C
 * SSC-3 (Ch 8.2.2)
 */
struct SequentialAccessDevice_pg {
	struct log_pg_header pcode_head;

	struct pc_header h_writeDataB4;
	uint64_t		 writeDataB4Compression; /* Write. Bytes from initiator */
	struct pc_header h_writeData_Af;
	uint64_t		 writeDataAfCompression; /* Write. Bytes written to media */

	struct pc_header h_readData_b4;
	uint64_t		 readDataB4Compression; /* Read. Bytes read from media */
	struct pc_header h_readData_Af;
	uint64_t		 readDataAfCompression; /* Read. Bytes to initiator */

	struct pc_header h_bop_eod;
	uint32_t		 capacity_bop_eod; /* Native capacity BOT to EOD */

	struct pc_header h_bop_ew;
	uint32_t		 capacity_bop_ew; /* Native capacity BOP to EW */

	struct pc_header h_ew_leop;
	uint32_t		 capacity_ew_leop; /* Native capacity EW and
										* Logical End Of Partition */

	struct pc_header h_bop_curr;
	uint32_t		 capacity_bop_curr; /* Native capacity BOP to curr pos */

	struct pc_header h_buffer;
	uint32_t		 capacity_buffer; /* Native capacity in buffer */

	struct pc_header h_cleaning;
	uint64_t		 TapeAlert;

	struct pc_header h_mbytes_processed;
	uint32_t		 mbytes_processed; /* MB since cleaning */

	struct pc_header h_load_cycle;
	uint32_t		 load_cycle; /* Total number of load cycles over drive lifetime*/

	struct pc_header h_clean;
	uint32_t		 clean_cycle; /* Total number of cleans over drive lifetime */

} __attribute__((packed));

void set_current_state(int s);
int	 get_tape_load_status();
void set_tape_load_status(int s);

void set_lp_11_macc(int flag);
void set_lp11_medium_present(int flag); /* Update LogPage 11 'Medium Present' bit */
void set_lp11_compression(int flag);	/* Update LogPage 11 compression bit */
void set_lp_11_wp(int flag);
void setTapeAlert(struct TapeAlert_pg *, uint64_t); /* in vtllib.c, never used */
void initTapeAlert(struct TapeAlert_pg *);
void dealloc_all_log_pages(struct lu_phy_attr *lu);

int	 update_TapeAlert(uint64_t flags);
int	 set_TapeAlert(uint64_t flags);
void update_TapeUsage(struct TapeUsage_pg *b);
void update_TapeCapacity(struct TapeCapacity_pg *pg);
void update_SequentialAccessDevice(struct SequentialAccessDevice_pg *sa);

struct log_pg_list *lookup_log_pg(struct list_head *l, uint8_t page, uint8_t subpage);
int					alloc_log_page(struct lu_phy_attr *lu,
								   uint8_t page, uint8_t subpage,
								   init_pg_fn init_log_pg,
								   size_t	  pg_size);

int add_log_write_err_counter(struct lu_phy_attr *lu);
int add_log_read_err_counter(struct lu_phy_attr *lu);
int add_log_sequential_access(struct lu_phy_attr *lu);
int add_log_temperature_page(struct lu_phy_attr *lu);
int add_log_volume_statistics(struct lu_phy_attr *lu);
int add_log_tape_alert(struct lu_phy_attr *lu);
int add_log_tape_usage(struct lu_phy_attr *lu);
int add_log_tape_capacity(struct lu_phy_attr *lu);
int add_log_data_compression(struct lu_phy_attr *lu);
int add_log_device_status(struct lu_phy_attr *lu);

extern const char *log_page_desc[0x38];

#endif /* MHVTL_LOG_H */