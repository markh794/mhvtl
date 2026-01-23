/*
 * This handles any SCSI OP 'mode sense / mode select'
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include "mhvtl_list.h"
#include "logging.h"
#include "vtllib.h"
#include "ssc.h"
#include "be_byteshift.h"
#include "mhvtl_log.h"

#define LOG_PG_HEADER(pageCode) \
	{(uint8_t)(pageCode), 0x00, 0x00}
#define LOG_PARAM(paramCode, flags, name) \
	{                                     \
		(uint8_t)((paramCode) >> 8),      \
		(uint8_t)((paramCode) & 0xFF),    \
		(flags),                          \
		sizeof(pg->name),                 \
	},                                    \
		.name

const char *log_page_desc[0x38] = {
	[0x00 ... 0x37]				 = "Unsupported Log page",
	[SUPPORTED_LOG_PAGES]		 = "Supported Log pages",
	[BUFFER_UNDER_OVER_RUN]		 = "Buffer Under/Over Run",
	[WRITE_ERROR_COUNTER]		 = "Write Error Counter",
	[READ_ERROR_COUNTER]		 = "Read Error Counter",
	[READ_REVERSE_ERROR_COUNTER] = "Read Reverse Error Counter",
	[VERIFY_ERROR_COUNTER]		 = "Verify Error Counter",
	[NON_MEDIUM_ERROR_COUNTER]	 = "Non-Medium Error Counter",
	[LAST_n_ERROR]				 = "Last N Error",
	[FORMAT_STATUS]				 = "Format Status",
	[LAST_n_DEFERRED_ERROR]		 = "Last N Deferred Error",
	[SEQUENTIAL_ACCESS_DEVICE]	 = "Sequential Access Device",
	[TEMPERATURE_PAGE]			 = "Temperature Page",
	[START_STOP_CYCLE_COUNTER]	 = "Start/Stop Cycle Counter",
	[APPLICATION_CLIENT]		 = "Application Client",
	[SELFTEST_RESULTS]			 = "Selftest Results",
	[VOLUME_STATISTICS]			 = "Volume Statistics",
	[DEVICE_STATUS]				 = "VHF Device Status",
	[TAPE_ALERT]				 = "Tape Alert",
	[INFORMATIONAL_EXCEPTIONS]	 = "Informational Exceptions",
	[TAPE_USAGE]				 = "Tape Usage",
	[TAPE_CAPACITY]				 = "Tape Capacity",
	[DATA_COMPRESSION]			 = "Data Compression",
};

struct log_pg_list *lookup_log_pg(struct list_head *l, uint8_t page, uint8_t subpage) {
	struct log_pg_list *log_pg;

	MHVTL_DBG(3, "fetching log page (0x%02x - 0x%02x)", page, subpage);

	list_for_each_entry(log_pg, l, siblings) {
		if (log_pg->log_page_num == page && log_pg->log_subpage_num == subpage) {
			MHVTL_DBG(2, "log page (0x%02x - 0x%02x) found : %s", page, subpage, log_page_desc[page]);
			return log_pg;
		}
	}

	/* If no page found, ignore subpage */
	list_for_each_entry(log_pg, l, siblings) {
		if (log_pg->log_page_num == page) {
			MHVTL_DBG(2, "log page (0x%02x - 0x%02x) found but wrong supbage: %s", page, subpage, log_page_desc[page]);
			return log_pg;
		}
	}

	MHVTL_DBG(3, "log page (0x%02x - 0x%02x) not found", page, subpage);

	return NULL;
}

int alloc_log_page(struct lu_phy_attr *lu,
				   uint8_t page, uint8_t subpage,
				   init_pg_fn init_log_pg,
				   size_t	  pg_size) {
	struct log_pg_list *log_pg;
	int					creation = 0;

	MHVTL_DBG(3, "%p : Allocate log page (0x%02x - 0x%02x), size %d",
			  &lu->log_pg, page, subpage, (int)pg_size);

	/* Getting the entry, create it if does not exist */
	log_pg = lookup_log_pg(&lu->log_pg, page, subpage);
	if (!log_pg) {
		creation = 1;
		log_pg	 = malloc(sizeof(struct log_pg_list));
	}

	if (log_pg) {
		log_pg->log_page_num	= page;
		log_pg->log_subpage_num = subpage;
		log_pg->size			= pg_size;
		log_pg->p				= malloc(pg_size);
		if (log_pg->p) {
			init_log_pg(log_pg->p);
			put_unaligned_be16(pg_size - sizeof(struct log_pg_header), /* Setting the real len of the log page */
							   &((struct log_pg_header *)log_pg->p)->len);
			if (creation)
				list_add_tail(&log_pg->siblings, &lu->log_pg);
			return 0;
		} else {
			MHVTL_ERR("Unable to malloc log page buffer (%zu)", pg_size);
			free(log_pg);
			return -ENOMEM;
		}
	}

	MHVTL_ERR("Unable to malloc log page entry (%zu)", pg_size);
	return -ENOMEM;
}

void dealloc_all_log_pages(struct lu_phy_attr *lu) {
	struct log_pg_list *lp, *ln;

	list_for_each_entry_safe(lp, ln, &lu->log_pg, siblings) {
		MHVTL_DBG(2, "Removing %s", lp->description);
		free(lp->p);
		list_del(&lp->siblings);
		free(lp);
	}
}

static void init_log_write_err_counter(void *log_ptr) {
	struct ErrorCounter_pg *pg = log_ptr;
	*pg						   = (struct ErrorCounter_pg){
		   LOG_PG_HEADER(WRITE_ERROR_COUNTER),
		   LOG_PARAM(0x0000, 0x60, err_correctedWODelay) = 0x00,
		   LOG_PARAM(0x0001, 0x60, err_correctedWDelay)	 = 0x00,
		   LOG_PARAM(0x0002, 0x60, totalReTry)			 = 0x00,
		   LOG_PARAM(0x0003, 0x60, totalErrorsCorrected) = 0x00,
		   LOG_PARAM(0x0004, 0x60, correctAlgorithm)	 = 0x00,
		   LOG_PARAM(0x0005, 0x60, bytesProcessed)		 = 0x00,
		   LOG_PARAM(0x0006, 0x60, uncorrectedErrors)	 = 0x00,
		   LOG_PARAM(0x8000, 0x60, readErrorsSinceLast)	 = 0x00,
		   LOG_PARAM(0x8001, 0x60, totalRawReadError)	 = 0x00,
		   LOG_PARAM(0x8002, 0x60, totalDropoutError)	 = 0x00,
		   LOG_PARAM(0x8003, 0x60, totalServoTracking)	 = 0x00,
	   };
}
int add_log_write_err_counter(struct lu_phy_attr *lu) {
	return alloc_log_page(lu, WRITE_ERROR_COUNTER, NO_SUBPAGE,
						  init_log_write_err_counter, sizeof(struct ErrorCounter_pg));
}

static void init_log_read_err_counter(void *log_ptr) {
	struct ErrorCounter_pg *pg = log_ptr;
	*pg						   = (struct ErrorCounter_pg){
		   LOG_PG_HEADER(READ_ERROR_COUNTER),
		   LOG_PARAM(0x0000, 0x60, err_correctedWODelay) = 0x00,
		   LOG_PARAM(0x0001, 0x60, err_correctedWDelay)	 = 0x00,
		   LOG_PARAM(0x0002, 0x60, totalReTry)			 = 0x00,
		   LOG_PARAM(0x0003, 0x60, totalErrorsCorrected) = 0x00,
		   LOG_PARAM(0x0004, 0x60, correctAlgorithm)	 = 0x00,
		   LOG_PARAM(0x0005, 0x60, bytesProcessed)		 = 0x00,
		   LOG_PARAM(0x0006, 0x60, uncorrectedErrors)	 = 0x00,
		   LOG_PARAM(0x8000, 0x60, readErrorsSinceLast)	 = 0x00,
		   LOG_PARAM(0x8001, 0x60, totalRawReadError)	 = 0x00,
		   LOG_PARAM(0x8002, 0x60, totalDropoutError)	 = 0x00,
		   LOG_PARAM(0x8003, 0x60, totalServoTracking)	 = 0x00,
	   };
}
int add_log_read_err_counter(struct lu_phy_attr *lu) {
	return alloc_log_page(lu, READ_ERROR_COUNTER, NO_SUBPAGE,
						  init_log_read_err_counter, sizeof(struct ErrorCounter_pg));
}

static void init_log_sequential_access(void *log_ptr) {
	struct SequentialAccessDevice_pg *pg = log_ptr;
	*pg									 = (struct SequentialAccessDevice_pg){
		 LOG_PG_HEADER(SEQUENTIAL_ACCESS_DEVICE),
		 LOG_PARAM(0x0000, 0x40, writeDataB4Compression) = 0x00,
		 LOG_PARAM(0x0001, 0x40, writeDataAfCompression) = 0x00,
		 LOG_PARAM(0x0002, 0x40, readDataB4Compression)	 = 0x00,
		 LOG_PARAM(0x0003, 0x40, readDataAfCompression)	 = 0x00,
		 LOG_PARAM(0x0004, 0x40, capacity_bop_eod)		 = 0x00,
		 LOG_PARAM(0x0005, 0x40, capacity_bop_ew)		 = 0x00,
		 LOG_PARAM(0x0006, 0x40, capacity_ew_leop)		 = 0x00,
		 LOG_PARAM(0x0007, 0x40, capacity_bop_curr)		 = 0x00,
		 LOG_PARAM(0x0008, 0x40, capacity_buffer)		 = 0x00,
		 LOG_PARAM(0x0100, 0x40, TapeAlert)				 = 0x00,
		 LOG_PARAM(0x8000, 0x40, mbytes_processed)		 = 0x00,
		 LOG_PARAM(0x8001, 0x40, load_cycle)			 = 0x00,
		 LOG_PARAM(0x8002, 0x40, clean_cycle)			 = 0x00,
	 };
}
int add_log_sequential_access(struct lu_phy_attr *lu) {
	return alloc_log_page(lu, SEQUENTIAL_ACCESS_DEVICE, NO_SUBPAGE,
						  init_log_sequential_access, sizeof(struct SequentialAccessDevice_pg));
}

static void init_log_temperature_page(void *log_ptr) {
	struct Temperature_pg *pg = log_ptr;
	*pg						  = (struct Temperature_pg){
		  LOG_PG_HEADER(TEMPERATURE_PAGE),
		  LOG_PARAM(0x0000, 0x60, temperature) = 0x00,
	  };
}
int add_log_temperature_page(struct lu_phy_attr *lu) {
	return alloc_log_page(lu, TEMPERATURE_PAGE, NO_SUBPAGE,
						  init_log_temperature_page, sizeof(struct Temperature_pg));
}

static void init_log_volume_statistics(void *log_ptr) {
	struct VolumeStatistics_pg *pg = log_ptr;
	*pg							   = (struct VolumeStatistics_pg){
		   LOG_PG_HEADER(VOLUME_STATISTICS),
		   LOG_PARAM(0x0000, 0x03, PageValid)						  = 0x01,
		   LOG_PARAM(0x0001, 0x03, VolumeMounts)					  = 0x00,
		   LOG_PARAM(0x0002, 0x03, VolumeDatasetsWritten)			  = 0x00,
		   LOG_PARAM(0x0003, 0x03, RecoveredWriteDataErrors)		  = 0x00,
		   LOG_PARAM(0x0004, 0x03, UnrecoveredWriteDataErrors)		  = 0x00,
		   LOG_PARAM(0x0005, 0x03, WriteServoErrors)				  = 0x00,
		   LOG_PARAM(0x0006, 0x03, UnrecoveredWriteServoErrors)		  = 0x00,
		   LOG_PARAM(0x0007, 0x03, VolumeDatasetsRead)				  = 0x00,
		   LOG_PARAM(0x0008, 0x03, RecoveredReadErrors)				  = 0x00,
		   LOG_PARAM(0x0009, 0x03, UnrecoveredReadErrors)			  = 0x00,
		   LOG_PARAM(0x000C, 0x03, LastMountUnrecoveredWriteErrors)	  = 0x00,
		   LOG_PARAM(0x000D, 0x03, LastMountUnrecoveredReadErrors)	  = 0x00,
		   LOG_PARAM(0x000E, 0x03, LastMountMBWritten)				  = 0x00,
		   LOG_PARAM(0x000F, 0x03, LastMountMBRead)					  = 0x00,
		   LOG_PARAM(0x0010, 0x03, LifetimeMBWritten)				  = 0x00,
		   LOG_PARAM(0x0011, 0x03, LifetimeMBRead)					  = 0x00,
		   LOG_PARAM(0x0012, 0x03, LastLoadWriteCompressionRatio)	  = 0x00,
		   LOG_PARAM(0x0013, 0x03, LastLoadReadCompressionRatio)	  = 0x00,
		   LOG_PARAM(0x0014, 0x03, MediumMountTime)					  = {0},
		   LOG_PARAM(0x0015, 0x03, MediumReadyTime)					  = {0},
		   LOG_PARAM(0x0016, 0x03, TotalNativeCapacity)				  = 0xfffffffe,
		   LOG_PARAM(0x0017, 0x03, TotalUsedNativeCapacity)			  = 0xfffffffe,
		   LOG_PARAM(0x0018, 0x03, AppDesignCapacity)				  = 0x00,
		   LOG_PARAM(0x0019, 0x03, VolumeLifetimeRemaining)			  = 0x00,
		   LOG_PARAM(0x0040, 0x01, VolumeSerialNumber)				  = {0},
		   LOG_PARAM(0x0041, 0x01, TapeLotIdentifier)				  = {0},
		   LOG_PARAM(0x0042, 0x01, VolumeBarcode)					  = {0},
		   LOG_PARAM(0x0043, 0x01, VolumeManufacturer)				  = {0},
		   LOG_PARAM(0x0044, 0x01, VolumeLicenseCode)				  = {0},
		   LOG_PARAM(0x0045, 0x01, VolumePersonality)				  = {0},
		   LOG_PARAM(0x0080, 0x03, WriteProtect)					  = 0x00,
		   LOG_PARAM(0x0081, 0x03, WORM)							  = 0x00,
		   LOG_PARAM(0x0082, 0x03, TempExceeded)					  = 0x00,
		   LOG_PARAM(0x0101, 0x03, BOMPasses)						  = 0x00,
		   LOG_PARAM(0x0102, 0x03, MOTPasses)						  = 0x00,
		   LOG_PARAM(0x0200, 0x03, FirstEncryptedLogicalObj)		  = {{{0}}},
		   LOG_PARAM(0x0201, 0x03, FirstUnencryptedLogicalObj)		  = {{{0}}},
		   LOG_PARAM(0x0202, 0x03, ApproxNativeCapacityPartition)	  = {{{0}}},
		   LOG_PARAM(0x0203, 0x03, ApproxUsedNativeCapacityPartition) = {{{0}}},
		   LOG_PARAM(0x0204, 0x03, RemainingCapacityToEWPartition)	  = {{{0}}},
	   };
	for (int k = 0; k < MAX_PARTITIONS; k++) {
		pg->FirstEncryptedLogicalObj[k].header.len = sizeof(struct partition_record_size6) - 1;
		put_unaligned_be16(k, &pg->FirstEncryptedLogicalObj[k].header.partition_no);
		pg->FirstUnencryptedLogicalObj[k].header.len = sizeof(struct partition_record_size6) - 1;
		put_unaligned_be16(k, &pg->FirstUnencryptedLogicalObj[k].header.partition_no);
		pg->ApproxNativeCapacityPartition[k].header.len = sizeof(struct partition_record_size4) - 1;
		put_unaligned_be16(k, &pg->ApproxNativeCapacityPartition[k].header.partition_no);
		pg->ApproxUsedNativeCapacityPartition[k].header.len = sizeof(struct partition_record_size4) - 1;
		put_unaligned_be16(k, &pg->ApproxUsedNativeCapacityPartition[k].header.partition_no);
		pg->RemainingCapacityToEWPartition[k].header.len = sizeof(struct partition_record_size4) - 1;
		put_unaligned_be16(k, &pg->RemainingCapacityToEWPartition[k].header.partition_no);
	};
}
int add_log_volume_statistics(struct lu_phy_attr *lu) {
	return alloc_log_page(lu, VOLUME_STATISTICS, NO_SUBPAGE,
						  init_log_volume_statistics, sizeof(struct VolumeStatistics_pg));
}

static void init_log_tape_alert(void *log_ptr) {
	struct TapeAlert_pg *pg = log_ptr;
	*pg						= (struct TapeAlert_pg){
		LOG_PG_HEADER(TAPE_ALERT),
	};
	for (int i = 0; i < 64; i++) {
		pg->TapeAlert[i] = (struct TapeAlert_flag){{0x00, i + 1, 0xc0, 1}, 0x00};
	}
}
int add_log_tape_alert(struct lu_phy_attr *lu) {
	return alloc_log_page(lu, TAPE_ALERT, NO_SUBPAGE,
						  init_log_tape_alert, sizeof(struct TapeAlert_pg));
}

static void init_log_tape_usage(void *log_ptr) {
	struct TapeUsage_pg *pg = log_ptr;
	*pg						= (struct TapeUsage_pg){
		LOG_PG_HEADER(TAPE_USAGE),
		LOG_PARAM(0x0001, 0xc0, volumeMounts)			 = 0x00,
		LOG_PARAM(0x0002, 0xc0, volumeDatasetsWritten)	 = 0x00,
		LOG_PARAM(0x0003, 0xc0, volWriteRetries)		 = 0x00,
		LOG_PARAM(0x0004, 0xc0, volWritePerms)			 = 0x00,
		LOG_PARAM(0x0005, 0xc0, volSuspendedWrites)		 = 0x00,
		LOG_PARAM(0x0006, 0xc0, volFatalSuspendedWrites) = 0x00,
		LOG_PARAM(0x0007, 0xc0, volDatasetsRead)		 = 0x00,
		LOG_PARAM(0x0008, 0xc0, volReadRetries)			 = 0x00,
		LOG_PARAM(0x0009, 0xc0, volReadPerms)			 = 0x00,
		LOG_PARAM(0x000a, 0xc0, volSuspendedReads)		 = 0x00,
		LOG_PARAM(0x000b, 0xc0, volFatalSuspendedReads)	 = 0x00,
	};
}
int add_log_tape_usage(struct lu_phy_attr *lu) {
	return alloc_log_page(lu, TAPE_USAGE, NO_SUBPAGE,
						  init_log_tape_usage, sizeof(struct TapeUsage_pg));
}

static void init_log_device_status(void *log_ptr) {
	struct DeviceStatus_pg *pg = log_ptr;
	*pg						   = (struct DeviceStatus_pg){
		   LOG_PG_HEADER(DEVICE_STATUS),
		   LOG_PARAM(0x0000, 0x03, vhf) = {{0x00, 0x00, 0x00, 0x01}},
	   };
}
int add_log_device_status(struct lu_phy_attr *lu) {
	return alloc_log_page(lu, DEVICE_STATUS, NO_SUBPAGE,
						  init_log_device_status, sizeof(struct DeviceStatus_pg));
}

static void init_log_tape_capacity(void *log_ptr) {
	struct TapeCapacity_pg *pg = log_ptr;
	*pg						   = (struct TapeCapacity_pg){
		   LOG_PG_HEADER(TAPE_CAPACITY),
		   LOG_PARAM(0x0001, 0xc0, partition0remaining) = 0x00,
		   LOG_PARAM(0x0002, 0xc0, partition1remaining) = 0x00,
		   LOG_PARAM(0x0003, 0xc0, partition0maximum)	= 0x00,
		   LOG_PARAM(0x0004, 0xc0, partition1maximum)	= 0x00,
	   };
}
int add_log_tape_capacity(struct lu_phy_attr *lu) {
	return alloc_log_page(lu, TAPE_CAPACITY, NO_SUBPAGE,
						  init_log_tape_capacity, sizeof(struct TapeCapacity_pg));
}

static void init_log_data_compression(void *log_ptr) {
	struct DataCompression_pg *pg = log_ptr;
	*pg							  = (struct DataCompression_pg){
		  LOG_PG_HEADER(DATA_COMPRESSION),
		  LOG_PARAM(0x0000, 0x40, ReadCompressionRatio)	 = 0x00,
		  LOG_PARAM(0x0001, 0x40, WriteCompressionRatio) = 0x00,
		  LOG_PARAM(0x0002, 0x40, MBytesToServer)		 = 0x00,
		  LOG_PARAM(0x0003, 0x40, BytesToServer)		 = 0x00,
		  LOG_PARAM(0x0004, 0x40, MBytesReadFromTape)	 = 0x00,
		  LOG_PARAM(0x0005, 0x40, BytesReadFromTape)	 = 0x00,
		  LOG_PARAM(0x0006, 0x40, MBytesFromServer)		 = 0x00,
		  LOG_PARAM(0x0007, 0x40, BytesFromServer)		 = 0x00,
		  LOG_PARAM(0x0008, 0x40, MBytesWrittenToTape)	 = 0x00,
		  LOG_PARAM(0x0009, 0x40, BytesWrittenToTape)	 = 0x00,
	  };
}
int add_log_data_compression(struct lu_phy_attr *lu) {
	return alloc_log_page(lu, DATA_COMPRESSION, NO_SUBPAGE,
						  init_log_data_compression, sizeof(struct DataCompression_pg));
}

/* Update MAM Accessible bit in LogPage 0x11 */
void set_lp_11_macc(int flag) {
	struct DeviceStatus_pg *lp = lookup_log_pg(&lunit.log_pg, DEVICE_STATUS, NO_SUBPAGE)->p;
	if (lp)
		lp->vhf.b4.MACC = flag;
}

void set_lp11_compression(int flag) {
	struct DeviceStatus_pg *lp = lookup_log_pg(&lunit.log_pg, DEVICE_STATUS, NO_SUBPAGE)->p;
	if (lp)
		lp->vhf.b4.CMPR = flag;
}

void set_lp_11_crqst(int flag) {
	struct DeviceStatus_pg *lp = lookup_log_pg(&lunit.log_pg, DEVICE_STATUS, NO_SUBPAGE)->p;
	if (lp)
		lp->vhf.b4.CRQST = flag;
}

void set_lp_11_crqrd(int flag) {
	struct DeviceStatus_pg *lp = lookup_log_pg(&lunit.log_pg, DEVICE_STATUS, NO_SUBPAGE)->p;
	if (lp)
		lp->vhf.b4.CRQRD = flag;
}

/* Update WriteProtect bit in LogPage 0x11 */
void set_lp_11_wp(int flag) {
	struct DeviceStatus_pg *lp = lookup_log_pg(&lunit.log_pg, DEVICE_STATUS, NO_SUBPAGE)->p;
	if (lp)
		lp->vhf.b4.WRTP = flag;
}

void set_lp11_medium_present(int flag) {
	struct DeviceStatus_pg *lp = lookup_log_pg(&lunit.log_pg, DEVICE_STATUS, NO_SUBPAGE)->p;
	if (!lp)
		return;

	lp->vhf.b5.MPRSNT = flag;

	if (!flag) {		   /* Clearing bit - also set state to unloaded */
		set_lp_11_macc(0); /* MAM Accessible */
		set_current_state(MHVTL_STATE_UNLOADED);
	}
}

/* Only valid for SSC devices */
int update_TapeAlert(uint64_t flags) {
	struct SequentialAccessDevice_pg *sad;
	struct log_pg_list				 *l;
	uint64_t						  ta;

	l = lookup_log_pg(&lunit.log_pg, SEQUENTIAL_ACCESS_DEVICE, NO_SUBPAGE);
	if (l) {
		sad = (struct SequentialAccessDevice_pg *)l->p;
		ta	= get_unaligned_be64(&sad->TapeAlert);
		MHVTL_DBG(2, "Adding flags: %.8x %.8x to %.8x %.8x",
				  (uint32_t)(flags >> 32) & 0xffffffff,
				  (uint32_t)flags & 0xffffffff,
				  (uint32_t)(ta >> 32) & 0xffffffff,
				  (uint32_t)ta & 0xffffffff);
		set_TapeAlert(ta | flags);
		if (flags & 1L << 19) /* Clean Now (required) */
			set_lp_11_crqrd(1);
		else
			set_lp_11_crqrd(0);
		if (flags & 1L << 20) /* Clean Periodic (requested) */
			set_lp_11_crqst(1);
		else
			set_lp_11_crqst(0);

		return 0;
	}
	return -1;
}

int set_TapeAlert(uint64_t flags) {
	struct SequentialAccessDevice_pg *sad;
	struct TapeAlert_pg				 *ta;
	struct DeviceStatus_pg			 *ds;
	struct log_pg_list				 *l;
	int								  i;

	/* Set LP 0x11 'TAFC' bit (TapeAlert Flag Changed) */
	l = lookup_log_pg(&lunit.log_pg, DEVICE_STATUS, NO_SUBPAGE)->p;
	if (!l)
		return -1;

	ds = (struct DeviceStatus_pg *)l->p;
	if (flags) {
		ds->vhf.b7.TAFC = 1;
		MHVTL_DBG(2, "Setting TAFC bit true");
	} else {
		ds->vhf.b7.TAFC = 0;
		MHVTL_DBG(3, "Not setting TAFC bit as flags is zero");
	}

	l = lookup_log_pg(&lunit.log_pg, TAPE_ALERT, NO_SUBPAGE);
	if (!l)
		return -1;

	ta = (struct TapeAlert_pg *)l->p;

	MHVTL_DBG(2, "Setting TapeAlert flags 0x%.8x %.8x",
			  (uint32_t)(flags >> 32) & 0xffffffff,
			  (uint32_t)flags & 0xffffffff);

	for (i = 0; i < 64; i++)
		ta->TapeAlert[i].value = (flags & (1ull << i)) ? 1 : 0;

	/* Don't treat not having a SEQUENTIAL ACCESS DEVICE log page
	 * as fatal (e.g. SMC devices)
	 */
	l = lookup_log_pg(&lunit.log_pg, SEQUENTIAL_ACCESS_DEVICE, NO_SUBPAGE);
	if (l) {
		sad = (struct SequentialAccessDevice_pg *)l->p;
		put_unaligned_be64(flags, &sad->TapeAlert);
	}
	if (flags & 1L << 19) /* Clean Now (required) */
		set_lp_11_crqrd(1);
	else
		set_lp_11_crqrd(0);
	if (flags & 1L << 20) /* Clean Periodic (requested) */
		set_lp_11_crqst(1);
	else
		set_lp_11_crqst(0);

	return 0;
}

void update_TapeUsage(struct TapeUsage_pg *b) {
	uint64_t datasets = count_filemarks(-1);
	uint64_t load_count;

	/* if we have more than 1 filemark,
	 * most apps write 2 filemarks to flag EOD
	 * So, lets subtract one from the filemark count to
	 * present a more accurate 'Data Set' count
	 */
	if (datasets > 1)
		datasets--;

	load_count = get_unaligned_be64(&lu_ssc.mamp->LoadCount);
	put_unaligned_be32(load_count, &b->volumeMounts);

	put_unaligned_be64(datasets, &b->volumeDatasetsWritten);
}

void update_TapeCapacity(struct TapeCapacity_pg *pg) {
	if (get_tape_load_status() == TAPE_LOADED) {
		uint64_t cap;

		cap = get_unaligned_be64(&mam.remaining_capacity);
		cap /= lu_ssc.capacity_unit;
		put_unaligned_be32(cap, &pg->partition0remaining);

		cap = get_unaligned_be64(&mam.max_capacity);
		cap /= lu_ssc.capacity_unit;
		put_unaligned_be32(cap, &pg->partition0maximum);
	} else {
		pg->partition0remaining = 0;
		pg->partition0maximum	= 0;
	}
}

void update_SequentialAccessDevice(struct SequentialAccessDevice_pg *sa) {
	put_unaligned_be64(lu_ssc.bytesWritten_I,
					   &sa->writeDataB4Compression);
	put_unaligned_be64(lu_ssc.bytesWritten_M,
					   &sa->writeDataAfCompression);
	put_unaligned_be64(lu_ssc.bytesRead_M,
					   &sa->readDataB4Compression);
	put_unaligned_be64(lu_ssc.bytesRead_I,
					   &sa->readDataAfCompression);

	/* Values in MBytes */
	if (get_tape_load_status() == TAPE_LOADED) {
		put_unaligned_be32(lu_ssc.max_capacity >> 20,
						   &sa->capacity_bop_eod);
		put_unaligned_be32(lu_ssc.early_warning_position >> 20,
						   &sa->capacity_bop_ew);
		put_unaligned_be32(lu_ssc.early_warning_sz >> 20,
						   &sa->capacity_ew_leop);
		put_unaligned_be32(current_tape_offset() >> 20,
						   &sa->capacity_bop_curr);
	} else {
		put_unaligned_be32(0xffffffff, &sa->capacity_bop_eod);
		put_unaligned_be32(0xffffffff, &sa->capacity_bop_ew);
		put_unaligned_be32(0xffffffff, &sa->capacity_ew_leop);
		put_unaligned_be32(0xffffffff, &sa->capacity_bop_curr);
	}
}

void set_current_state(int s) {
	struct DeviceStatus_pg *lp = lookup_log_pg(&lunit.log_pg, DEVICE_STATUS, NO_SUBPAGE)->p;

	current_state = s;

	/* Now translate the 'mhVTL' state into DT Device Activity values */
	if (lp) {
		switch (s) {
		case MHVTL_STATE_UNLOADED:
			lp->vhf.b6 = 0;
			break;
		case MHVTL_STATE_LOADING:
			lp->vhf.b6 = 2;
			break;
		case MHVTL_STATE_LOADING_CLEAN:
			lp->vhf.b6 = 1;
			break;
		case MHVTL_STATE_LOADING_WORM:
			lp->vhf.b6 = 2;
			break;
		case MHVTL_STATE_LOADED:
			lp->vhf.b6 = 0;
			break;
		case MHVTL_STATE_LOADED_IDLE:
			lp->vhf.b6 = 0;
			break;
		case MHVTL_STATE_LOAD_FAILED:
			lp->vhf.b6 = 0;
			set_lp_11_macc(0); /* MAM Accessible - False */
			break;
		case MHVTL_STATE_REWIND:
			lp->vhf.b6 = 0x8;
			break;
		case MHVTL_STATE_POSITIONING:
			lp->vhf.b6 = 0x7;
			break;
		case MHVTL_STATE_LOCATE:
			lp->vhf.b6 = 0x7;
			break;
		case MHVTL_STATE_READING:
			lp->vhf.b6 = 0x5;
			break;
		case MHVTL_STATE_WRITING:
			lp->vhf.b6 = 0x6;
			break;
		case MHVTL_STATE_UNLOADING:
			lp->vhf.b6 = 0x3;
			break;
		case MHVTL_STATE_ERASE:
			lp->vhf.b6 = 0x9;
			break;
		case MHVTL_STATE_VERIFY:
			lp->vhf.b6 = 0x4;
			break;
		}
	}
}

/* FIXME: Add VHF log page stuff here */
int get_tape_load_status(void) {
	return lu_ssc.load_status;
}

void set_tape_load_status(int s) {
	struct DeviceStatus_pg *lp = lookup_log_pg(&lunit.log_pg, DEVICE_STATUS, NO_SUBPAGE)->p;

	lu_ssc.load_status = s;

	if (lp) {
		switch (s) {
		case TAPE_UNLOADED:
			lp->vhf.b5.INXTN   = 1; /* In transition */
			lp->vhf.b5.MSTD	   = 0; /* Medium seated */
			lp->vhf.b5.MTHRD   = 0; /* Medium threaded */
			lp->vhf.b5.MOUNTED = 0; /* Medium mounted */
			lp->vhf.b5.MPRSNT  = 0; /* Medium Present */
			lp->vhf.b5.RAA	   = 1; /* Robotic access allowed */
			lp->vhf.b5.INXTN   = 0; /* Completed updates */
			set_lp_11_macc(0);		/* MAM Accessible */
			break;
		case TAPE_LOADED:
			lp->vhf.b5.INXTN   = 1; /* In transition */
			lp->vhf.b5.MSTD	   = 1; /* Medium seated */
			lp->vhf.b5.MTHRD   = 1; /* Medium threaded */
			lp->vhf.b5.MOUNTED = 1; /* Medium mounted */
			lp->vhf.b5.MPRSNT  = 1; /* Medium Present */
			lp->vhf.b5.RAA	   = 1; /* Robotic access allowed */
			lp->vhf.b5.INXTN   = 0; /* Completed updates */
			set_lp_11_macc(1);		/* MAM Accessible */
			break;
		case TAPE_LOADING:
			lp->vhf.b5.INXTN   = 1; /* In transition */
			lp->vhf.b5.MSTD	   = 1; /* Medium seated */
			lp->vhf.b5.MTHRD   = 0; /* Medium threaded */
			lp->vhf.b5.MOUNTED = 0; /* Medium mounted */
			lp->vhf.b5.MPRSNT  = 1; /* Medium Present */
			lp->vhf.b5.RAA	   = 1; /* Robotic access allowed */
			lp->vhf.b5.INXTN   = 0; /* Completed updates */
			set_lp_11_macc(0);		/* MAM Accessible */
			break;
		}
	}
}