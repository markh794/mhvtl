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
#include <sys/types.h>
#include <string.h>
#include <syslog.h>
#include <inttypes.h>
#include <errno.h>
#include "mhvtl_scsi.h"
#include "mhvtl_list.h"
#include "logging.h"
#include "vtllib.h"
#include "q.h"
#include "ssc.h"
#include "smc.h"
#include "be_byteshift.h"
#include "mhvtl_log.h"

static char *write_error_counter = "WRITE ERROR Counter";
static char *read_error_counter = "READ ERROR Counter";
static char *sequential_access_device = "Sequential Access";
static char *temperature_page = "Temperature";
static char *tape_alert = "Tape Alert";
static char *tape_usage = "Tape Usage";
static char *device_status = "Device Status";
static char *tape_capacity = "Tape Capacity";
static char *data_compression = "Data Compression";

struct log_pg_list *lookup_log_pg(struct list_head *l, uint8_t page)
{
	struct log_pg_list *log_pg;

	MHVTL_DBG(3, "Looking for: log page 0x%02x", page);

	list_for_each_entry(log_pg, l, siblings) {
		if (log_pg->log_page_num == page) {
			MHVTL_DBG(2, "%s (0x%02x)", log_pg->description, page);
			return log_pg;
		}
	}

	MHVTL_DBG(3, "Log page 0x%02x not found", page);

	return NULL;
}

/*
 * Used by log sense/mode select struct.
 *
 * Allocate 'size' bytes & init to 0
 *
 * Return pointer to log structure being init. or NULL if alloc failed
 */
struct log_pg_list *alloc_log_page(struct list_head *l, uint8_t page, int size)
{
	struct log_pg_list *log_page;

	MHVTL_DBG(3, "%p : Allocate log page 0x%02x, size %d", l, page, size);

	log_page = lookup_log_pg(l, page);
	if (!log_page) {	/* Create a new entry */
		log_page = zalloc(sizeof(struct log_pg_list));
	}
	if (log_page) {
		log_page->p = zalloc(size);
		MHVTL_DBG(3, "log page pointer: %p for log page 0x%02x",
			log_page->p, page);
		if (log_page->p) {	/* If ! null, set size of data */
			log_page->log_page_num = page;
			log_page->size = size;
			list_add_tail(&log_page->siblings, l);
			return log_page;
		} else {
			MHVTL_ERR("Unable to malloc(%d)", size);
			free(log_page);
		}
	}
	return NULL;
}

void dealloc_all_log_pages(struct lu_phy_attr *lu)
{
	struct log_pg_list *lp, *ln;

	list_for_each_entry_safe(lp, ln, &lu->log_pg, siblings) {
		MHVTL_DBG(2, "Removing %s", lp->description);
		free(lp->p);
		list_del(&lp->siblings);
		free(lp);
	}
}

int add_log_write_err_counter(struct lu_phy_attr *lu)
{
	struct log_pg_list *log_pg;
	struct error_counter tp = {
	{ WRITE_ERROR_COUNTER, 0x00, 0x00, },
	{ 0x00, 0x00, 0x60, sizeof(tp.err_correctedWODelay), }, 0x00,	/* {02h:0000h} Errors corrected with/o delay */
	{ 0x00, 0x01, 0x60, sizeof(tp.err_correctedWDelay), }, 0x00,	/* {02h:0001h} Errors corrected with delay */
	{ 0x00, 0x02, 0x60, sizeof(tp.totalReTry), }, 0x00,		/* {02h:0002h} Total rewrites */
	{ 0x00, 0x03, 0x60, sizeof(tp.totalErrorsCorrected), }, 0x00,	/* {02h:0003h} Total errors corrected */
	{ 0x00, 0x04, 0x60, sizeof(tp.correctAlgorithm), }, 0x00,	/* {02h:0004h} total times correct algorithm */
	{ 0x00, 0x05, 0x60, sizeof(tp.bytesProcessed), }, 0x00,		/* {02h:0005h} Total bytes processed */
	{ 0x00, 0x06, 0x60, sizeof(tp.uncorrectedErrors), }, 0x00,	/* {02h:0006h} Total uncorrected errors */
	{ 0x80, 0x00, 0x60, sizeof(tp.readErrorsSinceLast), }, 0x00,	/* {02h:8000h} Write errors since last read */
	{ 0x80, 0x01, 0x60, sizeof(tp.totalRawReadError), }, 0x00,	/* {02h:8001h} Total raw write error flags */
	{ 0x80, 0x02, 0x60, sizeof(tp.totalDropoutError), }, 0x00,	/* {02h:8002h} Total dropout error count */
	{ 0x80, 0x03, 0x60, sizeof(tp.totalServoTracking), }, 0x00,	/* {02h:8003h} Total servo tracking */
	};

	log_pg = alloc_log_page(&lu->log_pg, WRITE_ERROR_COUNTER, sizeof(tp));
	if (!log_pg)
		return -ENOMEM;

	log_pg->description = write_error_counter;

	put_unaligned_be16(sizeof(tp) - sizeof(tp.pcode_head),
				 &tp.pcode_head.len);

	memcpy(log_pg->p, &tp, sizeof(tp));

	return 0;
}

int add_log_read_err_counter(struct lu_phy_attr *lu)
{
	struct log_pg_list *log_pg;
	struct error_counter tp = {
	{ READ_ERROR_COUNTER, 0x00, 0x00, },
	{ 0x00, 0x00, 0x60, sizeof(tp.err_correctedWODelay), }, 0x00,	/* (03h:0000h} Errors corrected with/o delay */
	{ 0x00, 0x01, 0x60, sizeof(tp.err_correctedWDelay), }, 0x00,	/* {03h:0001h} Errors corrected with delay */
	{ 0x00, 0x02, 0x60, sizeof(tp.totalReTry), }, 0x00,		/* {03h:0002h} Total rewrites/rereads */
	{ 0x00, 0x03, 0x60, sizeof(tp.totalErrorsCorrected), }, 0x00,	/* {03h:0003h} Total errors corrected */
	{ 0x00, 0x04, 0x60, sizeof(tp.correctAlgorithm), }, 0x00,	/* {03h:0004h} total times correct algorithm */
	{ 0x00, 0x05, 0x60, sizeof(tp.bytesProcessed), }, 0x00,		/* {03h:0005h} Total bytes processed */
	{ 0x00, 0x06, 0x60, sizeof(tp.uncorrectedErrors), }, 0x00,	/* {03h:0006h} Total uncorrected errors */
	{ 0x80, 0x00, 0x60, sizeof(tp.readErrorsSinceLast), }, 0x00,	/* {03h:8009h} r/w errors since last read */
	{ 0x80, 0x01, 0x60, sizeof(tp.totalRawReadError), }, 0x00,	/* {03h:8001h} Total raw write error flags */
	{ 0x80, 0x02, 0x60, sizeof(tp.totalDropoutError), }, 0x00,	/* {03h:8002h} Total dropout error count */
	{ 0x80, 0x03, 0x60, sizeof(tp.totalServoTracking), }, 0x00,	/* {03h:8003h} Total servo tracking */
	};

	log_pg = alloc_log_page(&lu->log_pg, READ_ERROR_COUNTER, sizeof(tp));
	if (!log_pg)
		return -ENOMEM;

	log_pg->description = read_error_counter;

	put_unaligned_be16(sizeof(tp) - sizeof(tp.pcode_head),
				 &tp.pcode_head.len);

	memcpy(log_pg->p, &tp, sizeof(tp));

	return 0;
}

int add_log_sequential_access(struct lu_phy_attr *lu)
{
	struct log_pg_list *log_pg;
	struct seqAccessDevice tp = {
	{ SEQUENTIAL_ACCESS_DEVICE, 0x00, 0x54, },
	{ 0x00, 0x00, 0x40, sizeof(tp.writeDataB4Compression), }, 0x00, /* {0C:0000h} Write. Bytes from initiator */
	{ 0x00, 0x01, 0x40, sizeof(tp.writeDataAfCompression), }, 0x00, /* {0C:0001h} Write. Bytes written to media */
	{ 0x00, 0x02, 0x40, sizeof(tp.readDataB4Compression), }, 0x00,	/* {0C:0002h} Read. Bytes read from media */
	{ 0x00, 0x03, 0x40, sizeof(tp.readDataAfCompression), }, 0x00,	/* {0C:0003h} Read. Bytes to initialtor */
	{ 0x00, 0x04, 0x40, sizeof(tp.capacity_bop_eod), }, 0x00,	/* {0C:0004h} Native capacity BOT to EOD */
	{ 0x00, 0x05, 0x40, sizeof(tp.capacity_bop_ew), }, 0x00,	/* {0C:0005h} Native capacity BOP to EW */
	{ 0x00, 0x06, 0x40, sizeof(tp.capacity_ew_leop), }, 0x00,	/* {0C:0006h} Native capacity EW & LEOP */
	{ 0x00, 0x07, 0x40, sizeof(tp.capacity_bop_curr), }, 0x00,	/* {0C:0007h} Native capacity BOP to curr pos */
	{ 0x00, 0x08, 0x40, sizeof(tp.capacity_buffer), }, 0x00,	/* {0C:0008h} Native capacity in buffer */
	{ 0x01, 0x00, 0x40, sizeof(tp.TapeAlert), }, 0x00,		/* {0C:0100h} Cleaning required (TapeAlert) */
	{ 0x80, 0x00, 0x40, sizeof(tp.mbytes_processed), }, 0x00,	/* {0C:8000h} MBytes processed since clean */
	{ 0x80, 0x01, 0x40, sizeof(tp.load_cycle), }, 0x00,		/* {0C:8001h} Lifetime load cycle */
	{ 0x80, 0x02, 0x40, sizeof(tp.clean_cycle), }, 0x00,		/* {0C:8002h} Lifetime cleaning cycles */
	};

	log_pg = alloc_log_page(&lu->log_pg, SEQUENTIAL_ACCESS_DEVICE,
				sizeof(tp));
	if (!log_pg)
		return -ENOMEM;

	log_pg->description = sequential_access_device;

	put_unaligned_be16(sizeof(tp) - sizeof(tp.pcode_head),
				 &tp.pcode_head.len);

	memcpy(log_pg->p, &tp, sizeof(tp));

	return 0;
}

int add_log_temperature_page(struct lu_phy_attr *lu)
{
	struct log_pg_list *log_pg;
	struct Temperature_page tp =  {
		{ TEMPERATURE_PAGE, 0x00, 0x06, },
		{ 0x00, 0x00, 0x60, 0x02, }, 0x00,	/* Temperature */
	};

	log_pg = alloc_log_page(&lu->log_pg, TEMPERATURE_PAGE, sizeof(tp));
	if (!log_pg)
		return -ENOMEM;

	log_pg->description = temperature_page;

	put_unaligned_be16(sizeof(tp) - sizeof(tp.pcode_head),
				 &tp.pcode_head.len);

	/* Pre-fill temperature at 35C */
	put_unaligned_be16(35, &tp.temperature);

	memcpy(log_pg->p, &tp, sizeof(tp));

	return 0;
}

int add_log_tape_alert(struct lu_phy_attr *lu)
{
	struct log_pg_list *log_pg;
	struct TapeAlert_page tp = {
	{ TAPE_ALERT, 0x00, 0x00, },
	};
	int i;

	log_pg = alloc_log_page(&lu->log_pg, TAPE_ALERT, sizeof(tp));
	if (!log_pg)
		return -ENOMEM;

	log_pg->description = tape_alert;

	tp.pcode_head.pcode = TAPE_ALERT;
	tp.pcode_head.res = 0;
	for (i = 0; i < 64; i++) {
		tp.TapeAlert[i].flag.head0 = 0;
		tp.TapeAlert[i].flag.head1 = i + 1;
		tp.TapeAlert[i].flag.flags = 0xc0;
		tp.TapeAlert[i].flag.len = 1;
		tp.TapeAlert[i].value = 0;
	}
	put_unaligned_be16(sizeof(tp) - sizeof(tp.pcode_head),
				 &tp.pcode_head.len);

	memcpy(log_pg->p, &tp, sizeof(tp));

	return 0;
}

int add_log_tape_usage(struct lu_phy_attr *lu)
{
	struct log_pg_list *log_pg;
	struct TapeUsage tp = {
	{ TAPE_USAGE, 0x00, 0x54, },
	{ 0x00, 0x01, 0xc0, sizeof(tp.volumeMounts), }, 0x00,		/* {30h:0001h} Thread count */
	{ 0x00, 0x02, 0xc0, sizeof(tp.volumeDatasetsWritten), }, 0x00,	/* {30h:0002h} Total data sets written */
	{ 0x00, 0x03, 0xc0, sizeof(tp.volWriteRetries), }, 0x00,	/* {30h:0003h} Total write retries */
	{ 0x00, 0x04, 0xc0, sizeof(tp.volWritePerms), }, 0x00,		/* {30h:0004h} Total Unrecovered write error */
	{ 0x00, 0x05, 0xc0, sizeof(tp.volSuspendedWrites), }, 0x00,	/* {30h:0005h} Total Suspended writes */
	{ 0x00, 0x06, 0xc0, sizeof(tp.volFatalSuspendedWrites), }, 0x00, /* {30h:0006h} Total Fatal suspended writes */
	{ 0x00, 0x07, 0xc0, sizeof(tp.volDatasetsRead), }, 0x00,	/* {30h:0007h} Total data sets read */
	{ 0x00, 0x08, 0xc0, sizeof(tp.volReadRetries), }, 0x00,		/* {30h:0008h} Total read retries */
	{ 0x00, 0x09, 0xc0, sizeof(tp.volReadPerms), }, 0x00,		/* {30h:0009h} Total unrecovered read errors */
	{ 0x00, 0x0a, 0xc0, sizeof(tp.volSuspendedReads), }, 0x00,	/* {30h:000ah} Total suspended reads */
	{ 0x00, 0x0b, 0xc0, sizeof(tp.volFatalSuspendedReads), }, 0x00,	/* {30h:000bh} Total Fatal suspended reads */
	};

	log_pg = alloc_log_page(&lu->log_pg, TAPE_USAGE, sizeof(tp));
	if (!log_pg)
		return -ENOMEM;

	log_pg->description = tape_usage;

	put_unaligned_be16(sizeof(tp) - sizeof(tp.pcode_head),
				 &tp.pcode_head.len);

	memcpy(log_pg->p, &tp, sizeof(tp));

	return 0;
}

int add_log_device_status(struct lu_phy_attr *lu)
{
	struct log_pg_list *log_pg;
	struct DeviceStatus tp = {
		{ DEVICE_STATUS, 0x00, 0x08, },
		{ 0x00, 0x00, 0x03, 0x04, }, 0x00, 0x00, 0x00, 0x01, /* {11h:0000h} VHF parameter code  - 0000h */
	};

	log_pg = alloc_log_page(&lu->log_pg, DEVICE_STATUS, sizeof(tp));
	if (!log_pg)
		return -ENOMEM;

	log_pg->description = device_status;

	put_unaligned_be16(sizeof(tp) - sizeof(tp.pcode_head), &tp.pcode_head.len);

	memcpy(log_pg->p, &tp, sizeof(tp));

	return 0;
}

int add_log_tape_capacity(struct lu_phy_attr *lu)
{
	struct log_pg_list *log_pg;
	struct TapeCapacity tp = {
	{ TAPE_CAPACITY, 0x00, 0x54, },
	{ 0x00, 0x01, 0xc0, sizeof(tp.partition0remaining), }, 0x00,	/* {31h:0001h} main partition remaining cap */
	{ 0x00, 0x02, 0xc0, sizeof(tp.partition1remaining), }, 0x00,	/* {31h:0002h} Alt. partition remaining cap */
	{ 0x00, 0x03, 0xc0, sizeof(tp.partition0maximum), }, 0x00,	/* {31h:0003h} main partition max cap */
	{ 0x00, 0x04, 0xc0, sizeof(tp.partition1maximum), }, 0x00,	/* {31h:0004h} Alt. partition max cap */
	};

	log_pg = alloc_log_page(&lu->log_pg, TAPE_CAPACITY, sizeof(tp));
	if (!log_pg)
		return -ENOMEM;

	log_pg->description = tape_capacity;

	put_unaligned_be16(sizeof(tp) - sizeof(tp.pcode_head),
				 &tp.pcode_head.len);

	memcpy(log_pg->p, &tp, sizeof(tp));

	return 0;
}

int add_log_data_compression(struct lu_phy_attr *lu)
{
	struct log_pg_list *log_pg;
	struct DataCompression tp = {
	{ DATA_COMPRESSION, 0x00, 0x54, },
	{ 0x00, 0x00, 0x40, sizeof(tp.ReadCompressionRatio), }, 0x00,	/* {32h:0000h} Read Compression Ratio */
	{ 0x00, 0x01, 0x40, sizeof(tp.WriteCompressionRatio), }, 0x00,	/* {32h:0001h} Write Compression Ratio */
	{ 0x00, 0x02, 0x40, sizeof(tp.MBytesToServer), }, 0x00,		/* {32h:0002h} MBytes transferred to server */
	{ 0x00, 0x03, 0x40, sizeof(tp.BytesToServer), }, 0x00,		/* {32h:0003h} Bytes transferred to server */
	{ 0x00, 0x04, 0x40, sizeof(tp.MBytesReadFromTape), }, 0x00,	/* {32h:0004h} MBytes read from tape */
	{ 0x00, 0x05, 0x40, sizeof(tp.BytesReadFromTape), }, 0x00,	/* {32h:0005h} Bytes read from tape */
	{ 0x00, 0x06, 0x40, sizeof(tp.MBytesFromServer), }, 0x00,	/* {32h:0006h} MBytes transferred from server */
	{ 0x00, 0x07, 0x40, sizeof(tp.BytesFromServer), }, 0x00,	/* {32h:0007h} Bytes transferred from server */
	{ 0x00, 0x08, 0x40, sizeof(tp.MBytesWrittenToTape), }, 0x00,	/* {32h:0008h} MBytes written to tape */
	{ 0x00, 0x09, 0x40, sizeof(tp.BytesWrittenToTape), }, 0x00,	/* {32h:0009h} Bytes written to tape */
	};

	log_pg = alloc_log_page(&lu->log_pg, DATA_COMPRESSION, sizeof(tp));
	if (!log_pg)
		return -ENOMEM;

	log_pg->description = data_compression;

	put_unaligned_be16(sizeof(tp) - sizeof(tp.pcode_head),
				 &tp.pcode_head.len);

	memcpy(log_pg->p, &tp, sizeof(tp));

	return 0;
}

void set_lp_11_crqst(struct lu_phy_attr *lu, int flag)
{
	struct vhf_data_4 *vhf4;

	vhf4 = (struct vhf_data_4 *)get_vhf_byte(lu, 4);
	if (!vhf4)
		return;
	vhf4->CRQST = (flag) ? 1 : 0;
}

void set_lp_11_crqrd(struct lu_phy_attr *lu, int flag)
{
	struct vhf_data_4 *vhf4;

	vhf4 = (struct vhf_data_4 *)get_vhf_byte(lu, 4);
	if (!vhf4)
		return;
	vhf4->CRQRD = (flag) ? 1 : 0;
}

/* Only valid for SSC devices */
int update_TapeAlert(struct lu_phy_attr *lu, uint64_t flags)
{
	struct seqAccessDevice *sad;
	struct log_pg_list *l;
	uint64_t ta;

	l = lookup_log_pg(&lu->log_pg, SEQUENTIAL_ACCESS_DEVICE);
	if (l) {
		sad = (struct seqAccessDevice *)l->p;
		ta = get_unaligned_be64(&sad->TapeAlert);
		MHVTL_DBG(2, "Adding flags: %.8x %.8x to %.8x %.8x",
					(uint32_t)(flags >> 32) & 0xffffffff,
					(uint32_t)flags & 0xffffffff,
					(uint32_t)(ta >> 32) & 0xffffffff,
					(uint32_t)ta & 0xffffffff);
		set_TapeAlert(lu, ta | flags);
		if (flags & 1L << 19)	/* Clean Now (required) */
			set_lp_11_crqrd(lu, 1);
		else
			set_lp_11_crqrd(lu, 0);
		if (flags & 1L << 20)	/* Clean Periodic (requested) */
			set_lp_11_crqst(lu, 1);
		else
			set_lp_11_crqst(lu, 0);

		return 0;
	}
	return -1;
}

int set_TapeAlert(struct lu_phy_attr *lu, uint64_t flags)
{
	struct seqAccessDevice *sad;
	struct TapeAlert_page *ta;
	struct log_pg_list *l;
	int i;

	struct vhf_data_7 *p;

	/* Set LP 0x11 'TAFC' bit (TapeAlert Flag Changed) */
	p = get_vhf_byte(lu, 7);
	if (p) {
		if (flags) {
			p->TAFC = 1;
			MHVTL_DBG(2, "Setting TAFC bit true");
		} else {
			p->TAFC = 0;
			MHVTL_DBG(3, "Not setting TAFC bit as flags is zero");
		}
	}

	l = lookup_log_pg(&lu->log_pg, TAPE_ALERT);
	if (!l)
		return -1;

	ta = (struct TapeAlert_page *)l->p;

	MHVTL_DBG(2, "Setting TapeAlert flags 0x%.8x %.8x",
				(uint32_t)(flags >> 32) & 0xffffffff,
				(uint32_t)flags & 0xffffffff);

	for (i = 0; i < 64; i++)
		ta->TapeAlert[i].value = (flags & (1ull << i)) ? 1 : 0;

	/* Don't treat not having a SEQUENTIAL ACCESS DEVICE log page
	 * as fatal (e.g. SMC devices)
	 */
	l = lookup_log_pg(&lu->log_pg, SEQUENTIAL_ACCESS_DEVICE);
	if (l) {
		sad = (struct seqAccessDevice *)l->p;
		put_unaligned_be64(flags, &sad->TapeAlert);
	}
	if (flags & 1L << 19)	/* Clean Now (required) */
		set_lp_11_crqrd(lu, 1);
	else
		set_lp_11_crqrd(lu, 0);
	if (flags & 1L << 20)	/* Clean Periodic (requested) */
		set_lp_11_crqst(lu, 1);
	else
		set_lp_11_crqst(lu, 0);

	return 0;
}

/*
 * offset is the byte offset into the VHF data structure - 4/5/6/7
 */
void * get_vhf_byte(struct lu_phy_attr *lu, int offset)
{
	struct log_pg_list *l;
	uint8_t *p;
	int pg_header = 4;

	l = lookup_log_pg(&lu->log_pg, DEVICE_STATUS);
	if (!l)
		return NULL;

	p = l->p;

	return p + offset + pg_header;
}
