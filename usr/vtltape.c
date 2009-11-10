/*
 * This daemon is the SCSI SSC target (Sequential device - tape drive)
 * portion of the vtl package.
 *
 * The vtl package consists of:
 *   a kernel module (vlt.ko) - Currently on 2.6.x Linux kernel support.
 *   SCSI target daemons for both SMC and SSC devices.
 *
 * Copyright (C) 2005 - 2009 Mark Harvey       markh794@gmail.com
 *                                          mark_harvey@symantec.com
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

 * v0.1 -> Proof (proof of concept) that this may actually work (just)
 * v0.2 -> Get queueCommand() callback working -
 *         (Note to self: Sleeping in kernel is bad!)
 * v0.3 -> Message queues + make into daemon
 *	   changed lseek to lseek64
 * v0.4 -> First copy given to anybody,
 * v0.10 -> First start of a Solaris x86 port.. Still underway.
 * v0.11 -> First start of a Linux 2.4 kernel port.. Still underway.
 *	    However I'm scrapping this kfifo stuff and passing a pointer
 *	    and using copy{to|from}_user routines instead.
 * v0.12 -> Forked into 'stable' (0.12) and 'devel' (0.13).
 *          My current thinking : This is a dead end anyway.
 *          An iSCSI target done in user-space is now my perferred solution.
 *          This means I don't have to do any kernel level drivers
 *          and leaverage the hosts native iSCSI initiator.
 * 0.14 13 Feb 2008
 *	Since ability to define device serial number, increased ver from
 *	0.12 to 0.14
 *
 * 0.16 Jun 2009
 * 	Moved SCSI Inquiry into user-space.
 * 	SCSI lu are created/destroyed as the daemon is started/shutdown
 */

#define _XOPEN_SOURCE 500

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <strings.h>
#include <syslog.h>
#include <inttypes.h>
#include <pwd.h>
#include "be_byteshift.h"
#include "vtl_common.h"
#include "scsi.h"
#include "q.h"
#include "vtltape.h"
#include "vtllib.h"
#include "spc.h"

char vtl_driver_name[] = "vtltape";

/* Variables for simple, single initiator, SCSI Reservation system */
static int I_am_SPC_2_Reserved;
uint32_t SPR_Reservation_Generation;
uint8_t SPR_Reservation_Type;
uint64_t SPR_Reservation_Key;

/* Variables for simple, logical only SCSI Encryption system */
static uint32_t KEY_INSTANCE_COUNTER;
static uint32_t DECRYPT_MODE;
static uint32_t ENCRYPT_MODE;
static uint32_t KEY_LENGTH;
static uint32_t UKAD_LENGTH;
static uint32_t AKAD_LENGTH;
static uint8_t KEY[32];
static uint8_t UKAD[32];
static uint8_t AKAD[32];

#include <zlib.h>

/* Suppress Incorrect Length Indicator */
#define SILI  0x2
/* Fixed block format */
#define FIXED 0x1

/* Sense Data format bits & pieces */
/* Incorrect Length Indicator */
#define SD_VALID 0x80
#define SD_FILEMARK 0x80
#define SD_EOM 0x40
#define SD_ILI 0x20

#ifndef Solaris
  /* I'm sure there must be a header where lseek64() is defined */
  loff_t lseek64(int, loff_t, int);
//  int open64(char *, int);
  int ioctl(int, int, void *);
#endif

int send_msg(char *, int);

int verbose = 0;
int debug = 0;
int reset = 1;		/* Tape drive has been 'reset' */

#define TAPE_UNLOADED 0
#define TAPE_LOADED 1
#define TAPE_LOAD_BAD 2

static int bufsize = 1024 * 1024;
static loff_t max_tape_capacity;	/* Max capacity of media */
static int tapeLoaded = 0;	/* Default to Off-line */
static int inLibrary = 0;	/* Default to stand-alone drive */
static int datafile;		/* Global file handle - This goes against the
			grain, however the handle is passed to every function
			anyway. */
static char *currentMedia;	/* filename of 'datafile' */
static uint8_t sam_status = 0;	/* Non-zero if Sense-data is valid */
static uint8_t MediaType = 0;	/* 0 = Data, 1 WORM, 6 = Cleaning. */
static uint8_t MediaWriteProtect = 0;	/* True if virtual "write protect" switch is set */
static int OK_to_write = 1;	// True if in correct position to start writing
static int compressionFactor = 0;

static uint64_t bytesRead = 0;
static uint64_t bytesWritten = 0;
static unsigned char mediaSerialNo[34];	// Currently mounted media S/No.

uint8_t sense[SENSE_BUF_SIZE]; /* Request sense buffer */

struct lu_phy_attr lunit;

static struct MAM mam;

enum Media_Type_list {
	Media_undefined,
	Media_LTO1,
	Media_LTO2,
	Media_LTO3,
	Media_LTO3W,
	Media_LTO4,
	Media_LTO4W,
	Media_3592_JA,
	Media_3592_JW,
	Media_3592_JB,
	Media_3592_JX,
	Media_AIT4,
	Media_AIT4W,
	Media_10K,
	Media_10KW,
	Media_SDLT600,
	Media_UNKNOWN /* always last */
} Media_Type;

static int Drive_Native_Write_Density[drive_UNKNOWN + 1];
static int Media_Native_Write_Density[Media_UNKNOWN + 1];

struct MAM_Attributes_table {
	int attribute;
	int length;
	int read_only;
	int format;
	void *value;
} MAM_Attributes[] = {
	{0x000, 8, 1, 0, &mam.remaining_capacity },
	{0x001, 8, 1, 0, &mam.max_capacity },
	{0x002, 8, 1, 0, &mam.TapeAlert },
	{0x003, 8, 1, 0, &mam.LoadCount },
	{0x004, 8, 1, 0, &mam.MAMSpaceRemaining },
	{0x005, 8, 1, 1, &mam.AssigningOrganization_1 },
	{0x006, 1, 1, 0, &mam.FormattedDensityCode },
	{0x007, 2, 1, 0, &mam.InitializationCount },
	{0x20a, 40, 1, 1, &mam.DevMakeSerialLastLoad },
	{0x20b, 40, 1, 1, &mam.DevMakeSerialLastLoad1 },
	{0x20c, 40, 1, 1, &mam.DevMakeSerialLastLoad2 },
	{0x20d, 40, 1, 1, &mam.DevMakeSerialLastLoad3 },
	{0x220, 8, 1, 0, &mam.WrittenInMediumLife },
	{0x221, 8, 1, 0, &mam.ReadInMediumLife },
	{0x222, 8, 1, 0, &mam.WrittenInLastLoad },
	{0x223, 8, 1, 0, &mam.ReadInLastLoad },
	{0x400, 8, 1, 1, &mam.MediumManufacturer },
	{0x401, 32, 1, 1, &mam.MediumSerialNumber },
	{0x402, 4, 1, 0, &mam.MediumLength },
	{0x403, 4, 1, 0, &mam.MediumWidth },
	{0x404, 8, 1, 1, &mam.AssigningOrganization_2 },
	{0x405, 1, 1, 0, &mam.MediumDensityCode },
	{0x406, 8, 1, 1, &mam.MediumManufactureDate },
	{0x407, 8, 1, 0, &mam.MAMCapacity },
	{0x408, 1, 0, 0, &mam.MediumType },
	{0x409, 2, 1, 0, &mam.MediumTypeInformation },
	{0x800, 8, 0, 1, &mam.ApplicationVendor },
	{0x801, 32, 0, 1, &mam.ApplicationName },
	{0x802, 8, 0, 1, &mam.ApplicationVersion },
	{0x803, 160, 0, 2, &mam.UserMediumTextLabel },
	{0x804, 12, 0, 1, &mam.DateTimeLastWritten },
	{0x805, 1, 0, 0, &mam.LocalizationIdentifier },
	{0x806, 32, 0, 1, &mam.Barcode },
	{0x807, 80, 0, 2, &mam.OwningHostTextualName },
	{0x808, 160, 0, 2, &mam.MediaPool },
	{0xbff, 0, 1, 0, NULL }
};

/* Log pages */
static struct	Temperature_page Temperature_pg = {
	{ TEMPERATURE_PAGE, 0x00, 0x06, },
	{ 0x00, 0x00, 0x60, 0x02, }, 0x00,	// Temperature
	};

static struct error_counter pg_write_err_counter = {
	{ WRITE_ERROR_COUNTER, 0x00, 0x00, },
	{ 0x00, 0x00, 0x60, 0x04, }, 0x00, // Errors corrected with/o delay
	{ 0x00, 0x01, 0x60, 0x04, }, 0x00, // Errors corrected with delay
	{ 0x00, 0x02, 0x60, 0x04, }, 0x00, // Total rewrites
	{ 0x00, 0x03, 0x60, 0x04, }, 0x00, // Total errors corrected
	{ 0x00, 0x04, 0x60, 0x04, }, 0x00, // total times correct algorithm
	{ 0x00, 0x05, 0x60, 0x08, }, 0x00, // Total bytes processed
	{ 0x00, 0x06, 0x60, 0x04, }, 0x00, // Total uncorrected errors
	{ 0x80, 0x00, 0x60, 0x04, }, 0x00, // Write errors since last read
	{ 0x80, 0x01, 0x60, 0x04, }, 0x00, // Total raw write error flags
	{ 0x80, 0x02, 0x60, 0x04, }, 0x00, // Total dropout error count
	{ 0x80, 0x03, 0x60, 0x04, }, 0x00, // Total servo tracking
	};
static struct error_counter pg_read_err_counter = {
	{ READ_ERROR_COUNTER, 0x00, 0x00, },
	{ 0x00, 0x00, 0x60, 0x04, }, 0x00, // Errors corrected with/o delay
	{ 0x00, 0x01, 0x60, 0x04, }, 0x00, // Errors corrected with delay
	{ 0x00, 0x02, 0x60, 0x04, }, 0x00, // Total rewrites/rereads
	{ 0x00, 0x03, 0x60, 0x04, }, 0x00, // Total errors corrected
	{ 0x00, 0x04, 0x60, 0x04, }, 0x00, // total times correct algorithm
	{ 0x00, 0x05, 0x60, 0x08, }, 0x00, // Total bytes processed
	{ 0x00, 0x06, 0x60, 0x04, }, 0x00, // Total uncorrected errors
	{ 0x80, 0x00, 0x60, 0x04, }, 0x00, // r/w errors since last read
	{ 0x80, 0x01, 0x60, 0x04, }, 0x00, // Total raw write error flags
	{ 0x80, 0x02, 0x60, 0x04, }, 0x00, // Total dropout error count
	{ 0x80, 0x03, 0x60, 0x04, }, 0x00, // Total servo tracking
	};

static struct seqAccessDevice seqAccessDevice = {
	{ SEQUENTIAL_ACCESS_DEVICE, 0x00, 0x54, },
	{ 0x00, 0x00, 0x40, 0x08, }, 0x00,	// Write data b4 compression
	{ 0x00, 0x01, 0x40, 0x08, }, 0x00,	// Write data after compression
	{ 0x00, 0x02, 0x40, 0x08, }, 0x00,	// Read data b4 compression
	{ 0x00, 0x03, 0x40, 0x08, }, 0x00,	// Read data after compression
	{ 0x01, 0x00, 0x40, 0x08, }, 0x00,	// Cleaning required (TapeAlert)
	{ 0x80, 0x00, 0x40, 0x04, }, 0x00,	// MBytes processed since clean
	{ 0x80, 0x01, 0x40, 0x04, }, 0x00,	// Lifetime load cycle
	{ 0x80, 0x02, 0x40, 0x04, }, 0x00,	// Lifetime cleaning cycles
	};

static struct TapeAlert_page TapeAlert;

static struct DataCompression DataCompression = {
	{ DATA_COMPRESSION, 0x00, 0x54, },
	{ 0x00, 0x00, 0x40, 0x02, }, 0x00,	// Read Compression Ratio
	{ 0x00, 0x00, 0x40, 0x02, }, 0x00,	// Write Compression Ratio
	{ 0x00, 0x00, 0x40, 0x04, }, 0x00,	// MBytes transferred to server
	{ 0x00, 0x00, 0x40, 0x04, }, 0x00,	// Bytes transferred to server
	{ 0x00, 0x00, 0x40, 0x04, }, 0x00,	// MBytes read from tape
	{ 0x00, 0x00, 0x40, 0x04, }, 0x00,	// Bytes read from tape
	{ 0x00, 0x00, 0x40, 0x04, }, 0x00,    // MBytes transferred from server
	{ 0x00, 0x00, 0x40, 0x04, }, 0x00,	// Bytes transferred from server
	{ 0x00, 0x00, 0x40, 0x04, }, 0x00,	// MBytes written to tape
	{ 0x00, 0x00, 0x40, 0x04, }, 0x00,	// Bytes written to tape
	};

static struct TapeUsage TapeUsage = {
	{ TAPE_USAGE, 0x00, 0x54, },
	{ 0x00, 0x01, 0xc0, 0x04, }, 0x00,	// Thread count
	{ 0x00, 0x02, 0xc0, 0x08, }, 0x00,	// Total data sets written
	{ 0x00, 0x03, 0xc0, 0x04, }, 0x00,	// Total write retries
	{ 0x00, 0x04, 0xc0, 0x02, }, 0x00,	// Total Unrecovered write error
	{ 0x00, 0x05, 0xc0, 0x02, }, 0x00,	// Total Suspended writes
	{ 0x00, 0x06, 0xc0, 0x02, }, 0x00,	// Total Fatal suspended writes
	{ 0x00, 0x07, 0xc0, 0x08, }, 0x00,	// Total data sets read
	{ 0x00, 0x08, 0xc0, 0x04, }, 0x00,	// Total read retries
	{ 0x00, 0x09, 0xc0, 0x02, }, 0x00,	// Total unrecovered read errors
	{ 0x00, 0x0a, 0xc0, 0x02, }, 0x00,	// Total suspended reads
	{ 0x00, 0x0b, 0xc0, 0x02, }, 0x00,	// Total Fatal suspended reads
	};

static struct TapeCapacity TapeCapacity = {
	{ TAPE_CAPACITY, 0x00, 0x54, },
	{ 0x00, 0x01, 0xc0, 0x04, }, 0x00,	// main partition remaining cap
	{ 0x00, 0x02, 0xc0, 0x04, }, 0x00,	// Alt. partition remaining cap
	{ 0x00, 0x03, 0xc0, 0x04, }, 0x00,	// main partition max cap
	{ 0x00, 0x04, 0xc0, 0x04, }, 0x00,	// Alt. partition max cap
	};

static struct report_luns report_luns = {
	0x00, 0x00, 0x00,			// 16 bytes in length..
	};

/*
 * Mode Pages defined for SSC-3 devices..
 */

// Used by Mode Sense - if set, return block descriptor
uint8_t blockDescriptorBlock[8] = {0x40, 0, 0, 0, 0, 0, 0, 0, };

static struct mode sm[] = {
//	Page,  subpage, len, 'pointer to data struct'
	{0x01, 0x00, 0x00, NULL, },	// RW error recovery - SSC3-8.3.5
	{0x02, 0x00, 0x00, NULL, },	// Disconnect Reconnect - SPC3
	{0x0a, 0x00, 0x00, NULL, },	// Control Extension - SPC3
	{0x0f, 0x00, 0x00, NULL, },	// Data Compression - SSC3-8.3.3
	{0x10, 0x00, 0x00, NULL, },	// Device config - SSC3-8.3.3
	{0x11, 0x00, 0x00, NULL, },	// Medium Partition - SSC3-8.3.4
	{0x1a, 0x00, 0x00, NULL, },	// Power condition - SPC3
	{0x1c, 0x00, 0x00, NULL, },  // Informational Exception Ctrl SSC3-8.3.6
	{0x1d, 0x00, 0x00, NULL, },	// Medium configuration - SSC3-8.3.7
	{0x00, 0x00, 0x00, NULL, },	// NULL terminator
	};

//static loff_t	currentPosition = 0;
static struct blk_header c_pos;

static void usage(char *progname) {
	printf("Usage: %s -q <Q number> [-d] [-v] [-f file]\n",
						 progname);
	printf("       Where file == data file\n");
	printf("              'q number' is the queue priority number\n");
	printf("              'd' == debug\n");
	printf("              'v' == verbose\n");
}

static void
mk_sense_short_block(uint32_t requested, uint32_t processed, uint8_t *sense_valid)
{
	int difference = (int)requested - (int)processed;

	/* No sense, ILI bit set */
	mkSenseBuf(SD_ILI, NO_ADDITIONAL_SENSE, sense_valid);

	MHVTL_DBG(2, "Short block read: Requested: %d, Read: %d,"
			" short by %d bytes",
					requested, processed, difference);

	/* Now fill in the datablock with number of bytes not read/written */
	put_unaligned_be32(difference, &sense[3]);
}

static loff_t read_header(struct blk_header *h, int size, uint8_t *sam_stat)
{
	loff_t nread;

	nread = read(datafile, h, size);
	if (nread < 0) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		nread = 0;
	} else if (nread == 0) {
		mkSenseBuf(MEDIUM_ERROR, E_END_OF_DATA, sam_stat);
		nread = 0;
	}
	return nread;
}

static loff_t position_to_curr_header(uint8_t *sam_stat)
{
	return (lseek64(datafile, c_pos.curr_blk, SEEK_SET));
}

static int skip_to_next_header(uint8_t *sam_stat)
{
	loff_t nread;

	if (c_pos.blk_type == B_EOD) {
		mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
		MHVTL_DBG(1, "End of data detected while forward SPACEing!!");
		return -1;
	}

	if (c_pos.next_blk != lseek64(datafile, c_pos.next_blk, SEEK_SET)) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sam_stat);
		MHVTL_DBG(1, "Unable to seek to next block header");
		return -1;
	}
	nread = read_header(&c_pos, sizeof(c_pos), sam_stat);
	if (nread == 0) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sam_stat);
		MHVTL_DBG(1, "Unable to read next block header");
		return -1;
	}
	if (nread == -1) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sam_stat);
		MHVTL_DBG(1, "Unable to read next block header: %m");
		return -1;
	}
	// Position to start of header (rewind over header)
	if (c_pos.curr_blk != position_to_curr_header(sam_stat)) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sam_stat);
		MHVTL_DBG(1, "Error positing in datafile. Offset: %" PRId64,
				c_pos.curr_blk);
		return -1;
	}
	return 0;
}

static int skip_to_prev_header(uint8_t *sam_stat)
{
	loff_t nread;

	// Position to previous header
	MHVTL_DBG(3, "Positioning to c_pos.prev_blk: %" PRId64,
				c_pos.prev_blk);
	if (c_pos.prev_blk != lseek64(datafile, c_pos.prev_blk, SEEK_SET)) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		MHVTL_DBG(1, "Error position in datafile !!");
		return -1;
	}
	// Read in header
	MHVTL_DBG(3, "Reading in header: %d bytes", (int)sizeof(c_pos));

	nread = read_header(&c_pos, sizeof(c_pos), sam_stat);
	if (nread == 0) {
		MHVTL_DBG(1, "Error reading datafile while reverse SPACEing");
		return -1;
	}
	if (c_pos.blk_type == B_BOT) {
		MHVTL_DBG(3, "Found Beginning Of Tape, "
				"Skipping to next header..");
		skip_to_next_header(sam_stat);
		mkSenseBuf(MEDIUM_ERROR, E_BOM, sam_stat);
		MHVTL_DBG(3, "Found BOT!!");
		return -1;
	}

	// Position to start of header (rewind over header)
	if (c_pos.curr_blk != position_to_curr_header(sam_stat)) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR,sam_stat);
		MHVTL_DBG(1, "Error position in datafile !!");
		return -1;
	}
	MHVTL_DBG(3, "Rewinding over header just read in: "
			"curr_position: %" PRId64, c_pos.curr_blk);
	return 0;
}

/*
 * Create & write a new block header
 */
static int mkNewHeader(char type, int size, int comp_size, uint8_t *sam_stat)
{
	struct blk_header h;
	loff_t nwrite;

	memset(&h, 0, sizeof(h));

	h.blk_type = type;	/* Header type */
	h.blk_size = size;	/* Size of uncompressed data */
	h.disk_blk_size = comp_size; /* For when I do compression.. */
	h.curr_blk = lseek64(datafile, 0, SEEK_CUR); // Update current position
	h.blk_number = c_pos.blk_number;

	/* If we are writing a new EOD marker,
	 *  - then set next pointer to itself
	 * else
	 *  - Set pointer to next header (header size + size of data)
	 */
	if (type == B_EOD)
		h.next_blk = h.curr_blk;
	else
		h.next_blk = h.curr_blk + comp_size + sizeof(h);

	if (h.curr_blk == c_pos.curr_blk) {
	/* If current pos == last header read in we are about to overwrite the
	 * current header block
	 */
		h.prev_blk = c_pos.prev_blk;
		h.blk_number = c_pos.blk_number;
	} else if (h.curr_blk == c_pos.next_blk) {
	/* New header block at end of data file.. */
		h.prev_blk = c_pos.curr_blk;
		h.blk_number = c_pos.blk_number + 1;
	} else {
		MHVTL_DBG(1, "Position error blk No: %" PRId64
			 ", Pos: %" PRId64
			", Exp: %" PRId64,
				h.blk_number, h.curr_blk, c_pos.curr_blk);
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sam_stat);
		return 0;
	}
        /* handle encryption processing */
	if ((type == B_DATA) && (ENCRYPT_MODE == 2)) {
		int i;

		h.blk_flags |= BLKHDR_FLG_ENCRYPTED;
		h.encryption_ukad_length = UKAD_LENGTH;

		for (i = 0; i < UKAD_LENGTH; ++i) {
			h.encryption_ukad[i] = UKAD[i];
		}

		h.encryption_akad_length = AKAD_LENGTH;
		for (i = 0; i < AKAD_LENGTH; ++i) {
			h.encryption_akad[i] = AKAD[i];
		}

		h.encryption_key_length = KEY_LENGTH;
		for (i = 0; i < KEY_LENGTH; ++i) {
			h.encryption_key[i] = KEY[i];
		}
	}

	if ((type == B_DATA) && compressionFactor)
		h.blk_flags |= BLKHDR_FLG_COMPRESSED;

	nwrite = write(datafile, &h, sizeof(h));

	/*
	 * If write was successful, update c_pos with this header block.
	 */
	if (nwrite <= 0) {
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
		MHVTL_DBG(1, "Write failure, pos: %" PRId64 ": %m",
						h.curr_blk);
		return nwrite;
	}
	memcpy(&c_pos, &h, sizeof(h)); // Update where we think we are..

	return nwrite;
}

static int mkEODHeader(uint8_t *sam_stat)
{
	loff_t nwrite;

	nwrite = mkNewHeader(B_EOD, 0, 0, sam_stat);
	if (MediaType == MEDIA_TYPE_WORM)
		OK_to_write = 1;

	/* If we have just written a END OF DATA marker,
	 * rewind to just before it. */
	// Position to start of header (rewind over header)
	if (c_pos.curr_blk != position_to_curr_header(sam_stat)) {
		mkSenseBuf(MEDIUM_ERROR,E_SEQUENTIAL_POSITION_ERR,sam_stat);
		MHVTL_DBG(1, "Failed to write EOD header");
		return -1;
	}
	return nwrite;
}

/*
 *
 */

static int skip_prev_filemark(uint8_t *sam_stat)
{

	if (c_pos.blk_type == B_FILEMARK)
		c_pos.blk_type = B_NOOP;
	while (c_pos.blk_type != B_FILEMARK) {
		if (c_pos.blk_type == B_BOT) {
			mkSenseBuf(NO_SENSE, E_BOM, sam_stat);
			MHVTL_DBG(2, "Found Beginning of tape");
			return -1;
		}
		if (skip_to_prev_header(sam_stat))
			return -1;
	}
	return 0;
}

/*
 *
 */
static int skip_next_filemark(uint8_t *sam_stat)
{
	// While blk header is NOT a filemark, keep skipping to next header
	while (c_pos.blk_type != B_FILEMARK) {
		// END-OF-DATA -> Treat this as an error - return..
		if (c_pos.blk_type == B_EOD) {
			mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
			MHVTL_DBG(2, "%s", "Found end of media");
			if (MediaType == MEDIA_TYPE_WORM)
				OK_to_write = 1;
			return -1;
		}
		if (skip_to_next_header(sam_stat))
			return -1;	// On error
	}
	// Position to header AFTER the FILEMARK..
	if (skip_to_next_header(sam_stat))
		return -1;

	return 0;
}

/***********************************************************************/

/*
 * Set TapeAlert status in seqAccessDevice
 */
static void
setSeqAccessDevice(struct seqAccessDevice * seqAccessDevicep, uint64_t flg) {

	seqAccessDevicep->TapeAlert = htonll(flg);
}

/*
 * Check for any write restrictions - e.g. WORM, or Clean Cartridge mounted.
 * Return 1 = OK to write, zero -> Can't write.
 */
static int checkRestrictions(uint8_t *sam_stat)
{

	// Check that there is a piece of media loaded..
	switch (tapeLoaded) {
	case TAPE_LOADED:	/* Do nothing */
		break;
	case TAPE_UNLOADED:
		mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		OK_to_write = 0;
		return OK_to_write;
		break;
	default:
		mkSenseBuf(NOT_READY, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		OK_to_write = 0;
		return OK_to_write;
		break;
	}

	switch(MediaType) {
	case MEDIA_TYPE_CLEAN:
		mkSenseBuf(NOT_READY, E_CLEANING_CART_INSTALLED, sam_stat);
		MHVTL_DBG(2, "Can not write - Cleaning cart");
		OK_to_write = 0;
		break;
	case MEDIA_TYPE_WORM:
		/* If we are not at end of data for a write
		 * and media is defined as WORM, fail...
		 */
		if (c_pos.blk_type == B_EOD)
			OK_to_write = 1;	// OK to append to end of 'tape'
		if (!OK_to_write) {
			MHVTL_DBG(1, "Failed attempt to overwrite WORM data");
			mkSenseBuf(DATA_PROTECT,
				E_MEDIUM_OVERWRITE_ATTEMPTED, sam_stat);
		}
		break;
	case MEDIA_TYPE_DATA:
		OK_to_write = 1;
		break;
	}

	/* over-ride the above IF the virtual write protect switch is on */
	if (OK_to_write && MediaWriteProtect) {
		OK_to_write = 0;
		mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT, sam_stat);
	}

	MHVTL_DBG(2, "returning %s",
				(OK_to_write) ? "Writable" : "Non-writable");
	return OK_to_write;
}

/*
 * Set WORM mode sense flg
 */
static void setWORM(void) {
	struct mode *m;
	uint8_t *p;

	// Find pointer to Medium Configuration Page
	m = find_pcode(0x1d, sm);
	if (m) {
		p = m->pcodePointer;
		p[2] = 1;	/* Set WORMM bit */
		MHVTL_DBG(2, "WORM mode page set");
	}
}

/*
 * Clears WORM mode sense flg
 */
static void clearWORM(void) {
	struct mode *m;
	uint8_t *p;

	// Find pointer to Medium Configuration Page
	m = find_pcode(0x1d, sm);
	if (m) {
		p = m->pcodePointer;
		p[2] = 0;
		MHVTL_DBG(2, "WORM mode page cleared");
	}
}


/*
 * Report density of media loaded.

FIXME:
 -  Need to return full list of media support.
    e.g. AIT-4 should return AIT1, AIT2 AIT3 & AIT4 data.
 */

#define REPORT_DENSITY_LEN 56
static int resp_report_density(uint8_t media, struct vtl_ds *dbuf_p)
{
	uint8_t *buf = dbuf_p->data;
	int len = dbuf_p->sz;

	// Zero out buf
	memset(buf, 0, len);

	put_unaligned_be16(REPORT_DENSITY_LEN - 4, &buf[0]);

	buf[2] = 0;	// Reserved
	buf[3] = 0;	// Reserved

	buf[4] = 0x40;	// Primary Density Code
	buf[5] = 0x40;	// Secondary Density Code
	buf[6] = 0xa0;	// WRTOK = 1, DUP = 0, DEFLT = 1: 1010 0000b
	buf[7] = 0;


	// Bits per mm (only 24bits in len MS Byte should be 0).
	put_unaligned_be32(mam.media_info.bits_per_mm, &buf[8]);

	// Media Width (tenths of mm)
	put_unaligned_be32(mam.MediumWidth, &buf[12]);

	// Tracks
	put_unaligned_be16(mam.MediumLength, &buf[14]);

	// Capacity
	put_unaligned_be32(mam.max_capacity, &buf[16]);

	// Assigning Oranization (8 chars long)
	if (tapeLoaded == TAPE_LOADED) {
		snprintf((char *)&buf[20], 8, "%-8s",
					mam.AssigningOrganization_1);
		// Density Name (8 chars long)
		snprintf((char *)&buf[28], 8, "%-8s",
					mam.media_info.density_name);
		// Description (18 chars long)
		snprintf((char *)&buf[36], 18, "%-18s",
					mam.media_info.description);
	} else {
		snprintf((char *)&buf[20], 8, "%-8s", "LTO-CVE");
		// Density Name (8 chars long)
		snprintf((char *)&buf[28], 8, "%-8s", "U-18");
		// Description (18 chars long)
		snprintf((char *)&buf[36], 18, "%-18s", "Ultrium 1/8T");
	}

return(REPORT_DENSITY_LEN);
}


/*
 * Process the MODE_SELECT command
 */
static int resp_mode_select(int cdev, uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	uint8_t *sam_stat = &dbuf_p->sam_stat;
	uint8_t *buf = dbuf_p->data;
	int block_descriptor_sz;
	uint8_t *bdb = NULL;
	int long_lba = 0;
	int count;

	count = retrieve_CDB_data(cdev, dbuf_p);

	/* This should be more selective..
	 * Only needed for cmds that alter the partitioning or format..

	if (!checkRestrictions(sam_stat))
		return 0;
	 */

	if (cdb[0] == MODE_SELECT) {
		block_descriptor_sz = buf[3];
		if (block_descriptor_sz)
			bdb = &buf[4];
	} else {
		block_descriptor_sz = get_unaligned_be16(&buf[6]);
		long_lba = buf[4] & 1;
		if (block_descriptor_sz)
			bdb = &buf[8];
	}

	if (bdb) {
		if (!long_lba) {
			memcpy(blockDescriptorBlock, bdb, block_descriptor_sz);
		} else {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
							sam_stat);
			MHVTL_DBG(1, "Warning can not "
				"handle long descriptor block (long_lba bit)");
		}
	}

	/*
		FIXME: Need to add code here to set/reset compression
		MODE PAGE 0x0f
	*/
	if (debug)
		hex_dump(buf, dbuf_p->sz);

	return 0;
}


static int resp_log_sense(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	uint8_t	*b = dbuf_p->data;
	int retval = 0;
	uint16_t *sp;
	uint16_t alloc_len = dbuf_p->sz;

	uint8_t supported_pages[] = {	0x00, 0x00, 0x00, 0x08,
					0x00,
					WRITE_ERROR_COUNTER,
					READ_ERROR_COUNTER,
					SEQUENTIAL_ACCESS_DEVICE,
					TEMPERATURE_PAGE,
					TAPE_ALERT,
					TAPE_USAGE,
					TAPE_CAPACITY,
					DATA_COMPRESSION,
					};

	switch (cdb[2] & 0x3f) {
	case 0:	/* Send supported pages */
		MHVTL_DBG(1, "LOG SENSE: Sending supported pages");
		sp = (uint16_t *)&supported_pages[2];
		*sp = htons(sizeof(supported_pages) - 4);
		b = memcpy(b, supported_pages, sizeof(supported_pages));
		retval = sizeof(supported_pages);
		break;
	case WRITE_ERROR_COUNTER:	/* Write error page */
		MHVTL_DBG(1, "LOG SENSE: Write error page");
		pg_write_err_counter.pcode_head.len =
				htons((sizeof(pg_write_err_counter)) -
				sizeof(pg_write_err_counter.pcode_head));
		b = memcpy(b, &pg_write_err_counter,
					sizeof(pg_write_err_counter));
		retval += sizeof(pg_write_err_counter);
		break;
	case READ_ERROR_COUNTER:	/* Read error page */
		MHVTL_DBG(1, "LOG SENSE: Read error page");
		pg_read_err_counter.pcode_head.len =
				htons((sizeof(pg_read_err_counter)) -
				sizeof(pg_read_err_counter.pcode_head));
		b = memcpy(b, &pg_read_err_counter,
					sizeof(pg_read_err_counter));
		retval += sizeof(pg_read_err_counter);
		break;
	case SEQUENTIAL_ACCESS_DEVICE:
		MHVTL_DBG(1, "LOG SENSE: Sequential Access Device Log page");
		seqAccessDevice.pcode_head.len = htons(sizeof(seqAccessDevice) -
					sizeof(seqAccessDevice.pcode_head));
		b = memcpy(b, &seqAccessDevice, sizeof(seqAccessDevice));
		retval += sizeof(seqAccessDevice);
		break;
	case TEMPERATURE_PAGE:	/* Temperature page */
		MHVTL_DBG(1, "LOG SENSE: Temperature page");
		Temperature_pg.pcode_head.len = htons(sizeof(Temperature_pg) -
					sizeof(Temperature_pg.pcode_head));
		Temperature_pg.temperature = htons(35);
		b = memcpy(b, &Temperature_pg, sizeof(Temperature_pg));
		retval += sizeof(Temperature_pg);
		break;
	case TAPE_ALERT:	/* TapeAlert page */
		MHVTL_DBG(1, "LOG SENSE: TapeAlert page");
		MHVTL_DBG(2, " Returning TapeAlert flags: 0x%" PRIx64,
				get_unaligned_be64(&seqAccessDevice.TapeAlert));

		TapeAlert.pcode_head.len = htons(sizeof(TapeAlert) -
					sizeof(TapeAlert.pcode_head));
		b = memcpy(b, &TapeAlert, sizeof(TapeAlert));
		retval += sizeof(TapeAlert);
		/* Clear flags after value read. */
		if (alloc_len > 4) {
			setTapeAlert(&TapeAlert, 0);
			setSeqAccessDevice(&seqAccessDevice, 0);
		} else
			MHVTL_DBG(1, "TapeAlert : Alloc len short -"
				" Not clearing TapeAlert flags.");
		break;
	case TAPE_USAGE:	/* Tape Usage Log */
		MHVTL_DBG(1, "LOG SENSE: Tape Usage page");
		TapeUsage.pcode_head.len = htons(sizeof(TapeUsage) -
					sizeof(TapeUsage.pcode_head));
		b = memcpy(b, &TapeUsage, sizeof(TapeUsage));
		retval += sizeof(TapeUsage);
		break;
	case TAPE_CAPACITY:	/* Tape Capacity page */
		MHVTL_DBG(1, "LOG SENSE: Tape Capacity page");
		TapeCapacity.pcode_head.len = htons(sizeof(TapeCapacity) -
					sizeof(TapeCapacity.pcode_head));
		if (tapeLoaded == TAPE_LOADED) {
			TapeCapacity.value01 =
				htonl(max_tape_capacity - c_pos.curr_blk);
			TapeCapacity.value03 = htonl(max_tape_capacity);
		} else {
			TapeCapacity.value01 = 0;
			TapeCapacity.value03 = 0;
		}
		b = memcpy(b, &TapeCapacity, sizeof(TapeCapacity));
		retval += sizeof(TapeCapacity);
		break;
	case DATA_COMPRESSION:	/* Data Compression page */
		MHVTL_DBG(1, "LOG SENSE: Data Compression page");
		DataCompression.pcode_head.len = htons(sizeof(DataCompression) -
					sizeof(DataCompression.pcode_head));
		b = memcpy(b, &DataCompression, sizeof(DataCompression));
		retval += sizeof(DataCompression);
		break;
	default:
		MHVTL_DBG(1, "LOG SENSE: Unknown code: 0x%x", cdb[2] & 0x3f);
		retval = 2;
		break;
	}
	return retval;
}

/*
 * Read Attribute
 *
 * Fill in 'buf' with data and return number of bytes
 */
static int resp_read_attribute(uint8_t *cdb, uint8_t *buf, uint8_t *sam_stat)
{
	uint16_t attribute;
	uint32_t alloc_len;
	int ret_val = 0;
	int byte_index = 4;
	int indx, found_attribute;

	attribute = get_unaligned_be16(&cdb[8]);
	alloc_len = get_unaligned_be32(&cdb[10]);
	MHVTL_DBG(2, "Read Attribute: 0x%x, allocation len: %d",
							attribute, alloc_len);

	memset(buf, 0, alloc_len);	// Clear memory

	if (cdb[1] == 0) {
		/* Attribute Values */
		for (indx = found_attribute = 0; MAM_Attributes[indx].length; indx++) {
			if (attribute == MAM_Attributes[indx].attribute) {
				found_attribute = 1;
			}
			if (found_attribute) {
				/* calculate available data length */
				ret_val += MAM_Attributes[indx].length + 5;
				if (ret_val < alloc_len) {
					/* add it to output */
					buf[byte_index++] = MAM_Attributes[indx].attribute >> 8;
					buf[byte_index++] = MAM_Attributes[indx].attribute;
					buf[byte_index++] = (MAM_Attributes[indx].read_only << 7) |
					                    MAM_Attributes[indx].format;
					buf[byte_index++] = MAM_Attributes[indx].length >> 8;
					buf[byte_index++] = MAM_Attributes[indx].length;
					memcpy(&buf[byte_index], MAM_Attributes[indx].value,
					       MAM_Attributes[indx].length);
					byte_index += MAM_Attributes[indx].length;
				}
			}
		}
		if (!found_attribute) {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
			return 0;
		}
	} else {
		/* Attribute List */
		for (indx = found_attribute = 0; MAM_Attributes[indx].length; indx++) {
			/* calculate available data length */
			ret_val += 2;
			if (ret_val <= alloc_len) {
				/* add it to output */
				buf[byte_index++] = MAM_Attributes[indx].attribute >> 8;
				buf[byte_index++] = MAM_Attributes[indx].attribute;
			}
		}
	}

	put_unaligned_be32(ret_val, &buf[0]);

	if (ret_val > alloc_len)
		ret_val = alloc_len;

	return ret_val;
}

/*
 * Process WRITE ATTRIBUTE scsi command
 * Returns 0 if OK
 *         or 1 if MAM needs to be written.
 *         or -1 on failure.
 */
static int resp_write_attribute(uint8_t *cdb, struct vtl_ds *dbuf_p, struct MAM *mamp)
{
	uint32_t alloc_len;
	int byte_index;
	int indx, attribute, attribute_length, found_attribute = 0;
	struct MAM mam_backup;
	uint8_t *buf = dbuf_p->data;
	uint8_t *sam_stat = &dbuf_p->sam_stat;

	alloc_len = get_unaligned_be32(&cdb[10]);

	memcpy(&mam_backup, &mamp, sizeof(struct MAM));
	for (byte_index = 4; byte_index < alloc_len; ) {
		attribute = ((uint16_t)buf[byte_index++] << 8);
		attribute += buf[byte_index++];
		for (indx = found_attribute = 0; MAM_Attributes[indx].length; indx++) {
			if (attribute == MAM_Attributes[indx].attribute) {
				found_attribute = 1;
				byte_index += 1;
				attribute_length = ((uint16_t)buf[byte_index++] << 8);
				attribute_length += buf[byte_index++];
				if ((attribute == 0x408) &&
					(attribute_length == 1) &&
						(buf[byte_index] == 0x80)) {
					/* set media to worm */
					MHVTL_DBG(1, "Converted media to WORM");
					mamp->MediumType = MEDIA_TYPE_WORM;
				} else {
					memcpy(MAM_Attributes[indx].value,
						&buf[byte_index],
						MAM_Attributes[indx].length);
				}
				byte_index += attribute_length;
				break;
			} else {
				found_attribute = 0;
			}
		}
		if (!found_attribute) {
			memcpy(&mamp, &mam_backup, sizeof(mamp));
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_PARMS, sam_stat);
			return 0;
		}
	}
	return found_attribute;
}

/*
 *
 * Return number of bytes read.
 *        0 on error with sense[] filled in...
 */
static int readBlock(int cdev, uint8_t *buf, uint8_t *sam_stat, uint32_t request_sz)
{
	loff_t nread = 0;
	uint8_t	*cbuf = NULL;
	uLongf uncompress_sz;
	int z;
	uint32_t save_sense;

	MHVTL_DBG(3, "Request to read: %d bytes", request_sz);

	/* check for a zero length read
	 * This is not an error, and shouldn't change the tape position */
	if (request_sz == 0) {
		return 0;
	}

	/* Read in block of data */
	switch(c_pos.blk_type) {
	case B_FILEMARK:
		MHVTL_DBG(1, "Expected to find hdr type: %d, found: %d",
					B_DATA, c_pos.blk_type);
		skip_to_next_header(sam_stat);
		mk_sense_short_block(request_sz, 0, sam_stat);
		save_sense = get_unaligned_be32(&sense[3]);
		mkSenseBuf(NO_SENSE | SD_FILEMARK, E_MARK, sam_stat);
		put_unaligned_be32(save_sense, &sense[3]);
		return nread;
		break;
	case B_EOD:
		mk_sense_short_block(request_sz, 0, sam_stat);
		save_sense = get_unaligned_be32(&sense[3]);
		mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
		put_unaligned_be32(save_sense, &sense[3]);
		return nread;
		break;
	case B_BOT:
		skip_to_next_header(sam_stat);
		/* Re-exec function. */
		return readBlock(cdev, buf, sam_stat, request_sz);
		break;
	case B_DATA:
		/* If we are positioned at beginning of header, read it in. */
		if (c_pos.curr_blk == lseek64(datafile, 0, SEEK_CUR)) {
			nread = read_header(&c_pos, sizeof(c_pos), sam_stat);
			if (nread == 0) {	// Error
				MHVTL_DBG(1, "%m");
				mkSenseBuf(MEDIUM_ERROR,E_UNRECOVERED_READ,
								sam_stat);
				return 0;
			}
		}

		if (c_pos.blk_flags & BLKHDR_FLG_COMPRESSED) {
			cbuf = malloc(compressBound(c_pos.blk_size));
			if (!cbuf) {
				MHVTL_DBG(1, "Out of memory");
				mkSenseBuf(MEDIUM_ERROR, E_DECOMPRESSION_CRC,
								sam_stat);
				return 0;
			}
			nread = read(datafile, cbuf, c_pos.disk_blk_size);
		} else
			nread = read(datafile, buf, c_pos.disk_blk_size);

		if (nread == 0) {	/* End of data - no more to read */
			MHVTL_DBG(1, "%s",
				"End of data detected when reading from file");
			mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
			return nread;
		} else if (nread < 0) {	/* Error */
			MHVTL_DBG(1, "%m");
			mkSenseBuf(MEDIUM_ERROR, E_UNRECOVERED_READ, sam_stat);
			return 0;
		}
		if (c_pos.blk_flags & BLKHDR_FLG_COMPRESSED) {
			uncompress_sz = c_pos.blk_size;

			z = uncompress((uint8_t *)buf, &uncompress_sz,
					cbuf, nread);
			nread = 0;
			if (z != Z_OK)
				mkSenseBuf(MEDIUM_ERROR, E_DECOMPRESSION_CRC,
							sam_stat);
			switch (z) {
			case Z_OK:
				nread = uncompress_sz;
				MHVTL_DBG(2, "Read %u (%u) bytes of compressed"
					" data, have %u bytes for result",
					(uint32_t)nread, c_pos.disk_blk_size,
					c_pos.blk_size);
				break;
			case Z_MEM_ERROR:
				MHVTL_DBG(1, "Not enough memory to decompress");
				break;
			case Z_DATA_ERROR:
				MHVTL_DBG(1, "Block corrupt or incomplete");
				break;
			case Z_BUF_ERROR:
				MHVTL_DBG(1, "Not enough memory in destination buf");
				break;
			}
			nread = uncompress_sz;
			free(cbuf);
		}
		/* requested block and actual block size different */
		if (nread != request_sz)
			mk_sense_short_block(request_sz, nread, sam_stat);
		break;
	default:
		MHVTL_DBG(1, "Unknown blk header at offset %" PRId64
				" - Abort read cmd", c_pos.curr_blk);
		mkSenseBuf(MEDIUM_ERROR, E_UNRECOVERED_READ, sam_stat);
		return 0;
		break;
	}
	// Now read in subsequent header
	skip_to_next_header(sam_stat);

	return nread;
}

/*
 * Writes data in struct mam back to beginning of datafile..
 * Returns 0 if nothing written or -1 on error
 */
static int rewriteMAM(struct MAM *mamp, uint8_t *sam_stat)
{
	loff_t nwrite = 0;

	// Rewrite MAM data
	nwrite = pwrite(datafile, mamp, sizeof(mam), sizeof(struct blk_header));
	if (nwrite != sizeof(mam)) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		return -1;
	}

	return 0;
}


/*
 * Return number of bytes written to 'file'
 */
static int writeBlock(uint8_t *src_buf, uint32_t src_sz,  uint8_t *sam_stat)
{
	loff_t	nwrite = 0;
	Bytef *dest_buf;
	uLong dest_len = src_sz;
	uLong src_len = src_sz;
	uint32_t local_ENCRYPT_MODE = 0;
	int z;

	if (c_pos.blk_number == 0) {
		/* 3590 media must be formatted to allow encryption.
		 * This is done by writting an ANSI like label
		 * (NBU label is close enough) to the tape while
		 * an encryption key is in place. The drive doesn't
		 * actually use the key, but sets the tape format
		 */
		if (lunit.drive_type == drive_3592_E06) {
			if (ENCRYPT_MODE == 2) {
				local_ENCRYPT_MODE = ENCRYPT_MODE;
				ENCRYPT_MODE = 0;
				mam.Flags |= MAM_FLAGS_ENCRYPTION_FORMAT;
			} else
				mam.Flags &= ~MAM_FLAGS_ENCRYPTION_FORMAT;
		}
		if (Media_Native_Write_Density[Media_Type])
			blockDescriptorBlock[0] = Media_Native_Write_Density[Media_Type];
		else
			blockDescriptorBlock[0] = Drive_Native_Write_Density[lunit.drive_type];

		mam.MediumDensityCode = blockDescriptorBlock[0];
		mam.FormattedDensityCode = blockDescriptorBlock[0];
		rewriteMAM(&mam, sam_stat);
	} else {
		/* Extra check for 3592 to be sure the cartridge is
		 * formatted for encryption
		 */
		if ((lunit.drive_type == drive_3592_E06) && ENCRYPT_MODE &&
				!(mam.Flags & MAM_FLAGS_ENCRYPTION_FORMAT)) {
			mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT, sam_stat);
			return 0;
		}

		if ((Media_Native_Write_Density[Media_Type] == -1) &&
			(mam.MediumDensityCode != Drive_Native_Write_Density[lunit.drive_type])) {
			switch(lunit.drive_type) {
			case drive_3592_E05:
				if (mam.MediumDensityCode == Drive_Native_Write_Density[drive_3592_J1A])
					break;
				mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT, sam_stat);
				return 0;
				break;
			case drive_3592_E06:
				if (mam.MediumDensityCode == Drive_Native_Write_Density[drive_3592_E05])
					break;
				mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT, sam_stat);
				return 0;
				break;
			default:
				mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT, sam_stat);
				return 0;
				break;
			}
		}
	}

	if (compressionFactor) {
		int dest_sz;

		dest_sz = compressBound(src_sz);
		dest_buf = malloc(dest_sz);
		if (!dest_buf) {
			MHVTL_DBG(1, "malloc(%d) failed", dest_sz);
			mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
			return 0;
		}
		z = compress2(dest_buf, &dest_len, src_buf, src_sz,
							compressionFactor);
		if (z != Z_OK) {
			mkSenseBuf(HARDWARE_ERROR, E_COMPRESSION_CHECK,
							sam_stat);
			switch (z) {
			case Z_MEM_ERROR:
				MHVTL_DBG(1, "Not enough memory to compress "
						"data");
				break;
			case Z_BUF_ERROR:
				MHVTL_DBG(1, "Not enough memory in destination "
						" buf to compress data");
				break;
			case Z_DATA_ERROR:
				MHVTL_DBG(1, "Input data corrupt / incomplete");
				break;
			}
		}
		MHVTL_DBG(2, "Compression: Orig %d, after comp: %ld",
					src_sz, (unsigned long)dest_len);
	} else
		dest_buf = src_buf;

	nwrite = mkNewHeader(B_DATA, src_len, dest_len, sam_stat);
	if (local_ENCRYPT_MODE)
		ENCRYPT_MODE = local_ENCRYPT_MODE;

	if (nwrite <= 0) {
		MHVTL_DBG(1, "Failed to write header");
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
		return 0;
	}

	// now write the block of data..
	nwrite = write(datafile, dest_buf, dest_len);
	if (nwrite <= 0) {
		MHVTL_DBG(1, "%m: failed to write %ld bytes", dest_len);
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
	} else if (nwrite != dest_len) {
		MHVTL_DBG(1, "Did not write all data");
		mk_sense_short_block(src_len, nwrite, sam_stat);
	}
	if (c_pos.curr_blk >= max_tape_capacity) {
		MHVTL_DBG(2, "End of Medium - Setting EOM flag");
		mkSenseBuf(NO_SENSE|SD_EOM, NO_ADDITIONAL_SENSE, sam_stat);
	}

	if (compressionFactor)
		free(dest_buf);

	/* Write END-OF-DATA marker */
	nwrite = mkEODHeader(sam_stat);
	if (nwrite <= 0)
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);

	return src_len;
}

/*
 * Rewind 'tape'.
 */
static int rawRewind(uint8_t *sam_stat)
{
	off64_t retval;
	int val = 0;

	// Start at beginning of datafile..
	retval = lseek64(datafile, 0L, SEEK_SET);
	if (retval < 0) {
		MHVTL_DBG(1, "Can't seek to beginning of file: %m");
		val = 1;
	}

	/*
	 * Read header..
	 * If this is not the BOT header we are in trouble
	 */
	retval = read(datafile, &c_pos, sizeof(c_pos));
	if (retval != sizeof(c_pos)) {
		MHVTL_DBG(1, "Can't read header: %m");
		val = 1;
	}

	return val;
}

/*
 * Rewind to beginning of data file and the position to first data header.
 *
 * Return 0 -> Not loaded.
 *        1 -> Load OK
 *        2 -> format corrupt.
 */
static int resp_rewind(uint8_t *sam_stat)
{
	loff_t nread = 0;

	if (rawRewind(sam_stat)) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		return 2;
	}

	if (c_pos.blk_type != B_BOT) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		return 2;
	}
	nread = read(datafile, &mam, sizeof(struct MAM));
	if (nread != sizeof(struct MAM)) {
		MHVTL_DBG(1, "read MAM short - corrupt");
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		memset(&mam, 0, sizeof(struct MAM));
		return 2;
	}

	if (mam.tape_fmt_version != TAPE_FMT_VERSION) {
		MHVTL_DBG(1, "Incorrect media format");
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		return 2;
	}

	MHVTL_DBG(2, "MAM: media S/No. %s", mam.MediumSerialNumber);

	if (skip_to_next_header(sam_stat))
		return 2;

	MediaType = mam.MediumType;
	switch(MediaType) {
	case MEDIA_TYPE_CLEAN:
		OK_to_write = 0;
		break;
	case MEDIA_TYPE_WORM:
		/* Special condition...
		* If we
		* - rewind,
		* - write filemark
		* - EOD
		* We set this as writable media as the tape is blank.
		*/
		if (c_pos.blk_type != B_EOD)
			OK_to_write = 0;

		// Check that this header is a filemark and the next header
		//  is End of Data. If it is, we are OK to write
		if (c_pos.blk_type == B_FILEMARK) {
			skip_to_next_header(sam_stat);
			if (c_pos.blk_type == B_EOD)
				OK_to_write = 1;
		}
		// Now we have to go thru thru the rewind again..
		if (rawRewind(sam_stat)) {
			mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
			return 2;
		}

		// No need to do all previous error checking...
		skip_to_next_header(sam_stat);
		break;
	case MEDIA_TYPE_DATA:
		OK_to_write = 1;	// Reset flag to OK.
		break;
	}

	MHVTL_DBG(1, "Media is %s",
				(OK_to_write) ? "writable" : "not writable");

	return 1;
}

/*
 * Space over (to) x filemarks. Setmarks not supported as yet.
 */
static void resp_space(uint32_t count, int code, uint8_t *sam_stat)
{

	switch(code) {
	// Space 'count' blocks
	case 0:
		MHVTL_DBG(1, "SCSI space 0x%02x blocks **", count);
		if (count > 0xff000000) {
			// Moved backwards. Disable writing..
			if (MediaType == MEDIA_TYPE_WORM)
				OK_to_write = 0;
			for (;count > 0; count++)
				if (skip_to_prev_header(sam_stat))
					return;
		} else {
			for (;count > 0; count--)
				if (skip_to_next_header(sam_stat))
					return;
		}
		break;
	// Space 'count' filemarks
	case 1:
		MHVTL_DBG(1, "SCSI space 0x%02x filemarks **", count);
		if (count > 0xff000000) {	// skip backwards..
			// Moved backwards. Disable writing..
			if (MediaType == MEDIA_TYPE_WORM)
				OK_to_write = 0;
			for (;count > 0; count++)
				if (skip_prev_filemark(sam_stat))
					return;
		} else {
			for (;count > 0; count--)
				if (skip_next_filemark(sam_stat))
					return;
		}
		break;
	// Space to end-of-data - Ignore 'count'
	case 3:
		MHVTL_DBG(1, "%s", "SCSI space to end-of-data **");
		while (c_pos.blk_type != B_EOD)
			if (skip_to_next_header(sam_stat)) {
				if (MediaType == MEDIA_TYPE_WORM)
					OK_to_write = 1;
				return;
			}
		break;

	default:
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB, sam_stat);
		break;
	}
}

#ifdef MHVTL_DEBUG
static char *sps_pg0 = "Tape Data Encyrption in Support page";
static char *sps_pg1 = "Tape Data Encyrption Out Support Page";
static char *sps_pg16 = "Data Encryption Capabilities page";
static char *sps_pg17 = "Supported key formats page";
static char *sps_pg18 = "Data Encryption management capabilities page";
static char *sps_pg32 = "Data Encryption Status page";
static char *sps_pg33 = "Next Block Encryption Status Page";
static char *sps_pg48 = "Random Number Page";
static char *sps_pg49 = "Device Server Key Wrapping Public Key page";
static char *sps_reserved = "Security Protcol Specific : reserved value";

static char *lookup_sp_specific(uint16_t field)
{
	MHVTL_DBG(3, "Lookup %d", field);
	switch (field) {
	case 0:	return sps_pg0;
	case 1: return sps_pg1;
	case 16: return sps_pg16;
	case 17: return sps_pg17;
	case 18: return sps_pg18;
	case 32: return sps_pg32;
	case 33: return sps_pg33;
	case 48: return sps_pg48;
	case 49: return sps_pg49;
	default: return sps_reserved;
	break;
	}
}
#endif

#define SUPPORTED_SECURITY_PROTOCOL_LIST 0
#define CERTIFICATE_DATA		 1

/* FIXME:
 * Took this certificate from my Ubuntu install
 *          /usr/share/libssl-dev/demos/sign/key.pem
 * Need to insert a certificate of my own here...
 */
static char *certificate =
"-----BEGIN CERTIFICATE-----\n"
"MIICLDCCAdYCAQAwDQYJKoZIhvcNAQEEBQAwgaAxCzAJBgNVBAYTAlBUMRMwEQYD\n"
"VQQIEwpRdWVlbnNsYW5kMQ8wDQYDVQQHEwZMaXNib2ExFzAVBgNVBAoTDk5ldXJv\n"
"bmlvLCBMZGEuMRgwFgYDVQQLEw9EZXNlbnZvbHZpbWVudG8xGzAZBgNVBAMTEmJy\n"
"dXR1cy5uZXVyb25pby5wdDEbMBkGCSqGSIb3DQEJARYMc2FtcG9AaWtpLmZpMB4X\n"
"DTk2MDkwNTAzNDI0M1oXDTk2MTAwNTAzNDI0M1owgaAxCzAJBgNVBAYTAlBUMRMw\n"
"EQYDVQQIEwpRdWVlbnNsYW5kMQ8wDQYDVQQHEwZMaXNib2ExFzAVBgNVBAoTDk5l\n"
"dXJvbmlvLCBMZGEuMRgwFgYDVQQLEw9EZXNlbnZvbHZpbWVudG8xGzAZBgNVBAMT\n"
"EmJydXR1cy5uZXVyb25pby5wdDEbMBkGCSqGSIb3DQEJARYMc2FtcG9AaWtpLmZp\n"
"MFwwDQYJKoZIhvcNAQEBBQADSwAwSAJBAL7+aty3S1iBA/+yxjxv4q1MUTd1kjNw\n"
"L4lYKbpzzlmC5beaQXeQ2RmGMTXU+mDvuqItjVHOK3DvPK7lTcSGftUCAwEAATAN\n"
"BgkqhkiG9w0BAQQFAANBAFqPEKFjk6T6CKTHvaQeEAsX0/8YHPHqH/9AnhSjrwuX\n"
"9EBc0n6bVGhN7XaXd6sJ7dym9sbsWxb+pJdurnkxjx4=\n"
"-----END CERTIFICATE-----\n";

/*
 * Returns number of bytes in struct
 */
static int resp_spin_page_0(uint8_t *buf, uint16_t sps, uint32_t alloc_len, uint8_t *sam_stat)
{
	int ret = 0;

	MHVTL_DBG(2, "%s", lookup_sp_specific(sps));

	switch(sps) {
	case SUPPORTED_SECURITY_PROTOCOL_LIST:
		memset(buf, 0, alloc_len);
		buf[6] = 0;	/* list length (MSB) */
		buf[7] = 2;	/* list length (LSB) */
		buf[8] = SUPPORTED_SECURITY_PROTOCOL_LIST;
		buf[9] = CERTIFICATE_DATA;
		buf[10] = 0x20;
		ret = 11;
		break;

	case CERTIFICATE_DATA:
		memset(buf, 0, alloc_len);
		put_unaligned_be16(sizeof(certificate), &buf[2]);
		strncpy((char *)&buf[4], certificate, alloc_len - 4);
		if (strlen(certificate) >= alloc_len - 4)
			ret = alloc_len;
		else
			ret = strlen(certificate) + 4;
		break;

	default:
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
	}
	return ret;
}

#define ENCR_IN_SUPPORT_PAGES		0
#define ENCR_OUT_SUPPORT_PAGES		1
#define ENCR_CAPABILITIES		0x10
#define ENCR_KEY_FORMATS		0x11
#define ENCR_KEY_MGT_CAPABILITIES	0x12
#define ENCR_DATA_ENCR_STATUS		0x20
#define ENCR_NEXT_BLK_ENCR_STATUS	0x21

#define ENCR_SET_DATA_ENCRYPTION	0x10

/*
 * Return number of valid bytes in data structure
 */
static int resp_spin_page_20(uint8_t *buf, uint16_t sps, uint32_t alloc_len, uint8_t *sam_stat)
{
	int ret = 0;
	int indx, count, correct_key;

	MHVTL_DBG(2, "%s", lookup_sp_specific(sps));

	memset(buf, 0, alloc_len);
	switch(sps) {
	case ENCR_IN_SUPPORT_PAGES:
		put_unaligned_be16(ENCR_IN_SUPPORT_PAGES, &buf[0]);
		put_unaligned_be16(16, &buf[2]); /* List length */
		put_unaligned_be16(ENCR_IN_SUPPORT_PAGES, &buf[4]);
		put_unaligned_be16(ENCR_OUT_SUPPORT_PAGES, &buf[6]);
		put_unaligned_be16(ENCR_CAPABILITIES, &buf[8]);
		put_unaligned_be16(ENCR_KEY_FORMATS, &buf[10]);
		put_unaligned_be16(ENCR_KEY_MGT_CAPABILITIES, &buf[12]);
		put_unaligned_be16(ENCR_DATA_ENCR_STATUS, &buf[14]);
		put_unaligned_be16(ENCR_NEXT_BLK_ENCR_STATUS, &buf[16]);
		ret = 18;
		break;

	case ENCR_OUT_SUPPORT_PAGES:
		put_unaligned_be16(ENCR_OUT_SUPPORT_PAGES, &buf[0]);
		put_unaligned_be16(2, &buf[2]); /* List length */
		put_unaligned_be16(ENCR_SET_DATA_ENCRYPTION, &buf[4]);
		ret = 6;
		break;

	case ENCR_CAPABILITIES:
		put_unaligned_be16(ENCR_CAPABILITIES, &buf[0]);
		put_unaligned_be16(40, &buf[2]); /* List length */

		buf[20] = 1;	/* Algorithm index */
		buf[21] = 0;	/* Reserved */
		put_unaligned_be16(0x14, &buf[22]); /* Descriptor length */
		buf[24] = 0x3a;	/* MAC C/DED_C DECRYPT_C = 2 ENCRYPT_C = 2 */
		buf[25] = 0x10;	/* NONCE_C = 1 */
		/* Max unauthenticated key data */
		put_unaligned_be16(0x20, &buf[26]);
		/* Max authenticated  key data */
		put_unaligned_be16(0x0c, &buf[28]);
		/* Key size */
		put_unaligned_be16(0x20, &buf[30]);
		buf[32] = 0x01;	/* EAREM */
		/* buf 12 - 19 reserved */

		buf[40] = 0;	/* Encryption Algorithm Id */
		buf[41] = 0x01;	/* Encryption Algorithm Id */
		buf[42] = 0;	/* Encryption Algorithm Id */
		buf[43] = 0x14;	/* Encryption Algorithm Id */
		ret = 44;

		MHVTL_DBG(2, "Drive type: %d, Media type: %d",
				lunit.drive_type, Media_Type);

		/* adjustments for each emulated drive type */
		switch (lunit.drive_type) {
		case drive_10K_A:
		case drive_10K_B:
			buf[4] = 0x1; /* CFG_P == 01b */
			if (tapeLoaded == TAPE_LOADED)
				buf[24] |= 0x80; /* AVFMV */
				buf[27] = 0x1e; /* Max unauthenticated key data */
				buf[29] = 0x00; /* Max authenticated key data */
				buf[32] |= 0x42; /* DKAD_C == 1, RDMC_C == 1 */
				buf[40] = 0x80; /* Encryption Algorithm Id */
				buf[43] = 0x10; /* Encryption Algorithm Id */
			break;
		case drive_3592_E06:
			if (tapeLoaded == TAPE_LOADED)
				buf[24] |= 0x80; /* AVFMV */
				buf[27] = 0x00; /* Max unauthenticated key data */
				buf[32] |= 0x0e; /* RDMC_C == 7 */
			break;
		case drive_LTO4:
			MHVTL_DBG(1, "LTO4 drive");
			buf[4] = 0x1; /* CFG_P == 01b */
			if (tapeLoaded == TAPE_LOADED) {
				if (Media_Type == Media_LTO4) {
					MHVTL_DBG(1, "LTO4 Medium");
					buf[24] |= 0x80; /* AVFMV */
				}
			}
			buf[32] |= 0x08; /* RDMC_C == 4 */
			break;
		default:
			break;
		}
		break;

	case ENCR_KEY_FORMATS:
		put_unaligned_be16(ENCR_KEY_FORMATS, &buf[0]);
		put_unaligned_be16(1, &buf[2]); /* List length */
		buf[4] = 0;	/* Plain text */
		ret = 5;
		break;

	case ENCR_KEY_MGT_CAPABILITIES:
		put_unaligned_be16(ENCR_KEY_MGT_CAPABILITIES, &buf[0]);
		put_unaligned_be16(0x0c, &buf[2]); /* List length */
		buf[4] = 1;	/* LOCK_C */
		buf[5] = 7;	/* CKOD_C, DKOPR_C, CKORL_C */
		buf[6] = 0;	/* Reserved */
		buf[7] = 7;	/* AITN_C, LOCAL_C, PUBLIC_C */
		/* buf 8 - 15 reserved */
		ret = 16;
		break;

	case ENCR_DATA_ENCR_STATUS:
		put_unaligned_be16(ENCR_DATA_ENCR_STATUS, &buf[0]);
		put_unaligned_be16(0x20, &buf[2]); /* List length */
		buf[4] = 0x21;	/* I_T Nexus scope and Key Scope */
		buf[5] = ENCRYPT_MODE;
		buf[6] = DECRYPT_MODE;
		buf[7] = 0x01;	/* Algorithm Index */
		put_unaligned_be32(KEY_INSTANCE_COUNTER, &buf[8]);
		ret = 24;
		indx = 24;
		if (UKAD_LENGTH) {
			buf[3] += 4 + UKAD_LENGTH;
			buf[indx++] = 0x00;
			buf[indx++] = 0x00;
			buf[indx++] = 0x00;
			buf[indx++] = UKAD_LENGTH;
			for (count = 0; count < UKAD_LENGTH; ++count) {
				buf[indx++] = UKAD[count];
			}
			ret += 4 + UKAD_LENGTH;
		}
		if (AKAD_LENGTH) {
			buf[3] += 4 + AKAD_LENGTH;
			buf[indx++] = 0x01;
			buf[indx++] = 0x00;
			buf[indx++] = 0x00;
			buf[indx++] = AKAD_LENGTH;
			for (count = 0; count < AKAD_LENGTH; ++count) {
				buf[indx++] = AKAD[count];
			}
			ret += 4 + AKAD_LENGTH;
		}
		break;

	case ENCR_NEXT_BLK_ENCR_STATUS:
		if (tapeLoaded != TAPE_LOADED) {
			mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
			break;
		}
		/* c_pos contains the NEXT block's header info already */
		put_unaligned_be16(ENCR_NEXT_BLK_ENCR_STATUS, &buf[0]);
		buf[2] = 0;	/* List length (MSB) */
		buf[3] = 12;	/* List length (MSB) */
		if (sizeof(loff_t) > 32)
			put_unaligned_be64(c_pos.blk_number, &buf[4]);
		else
			put_unaligned_be32(c_pos.blk_number, &buf[8]);
		if (c_pos.blk_type != B_DATA)
			buf[12] = 0x2; /* not a logical block */
		else
			buf[12] = 0x3; /* not encrypted */
		buf[13] = 0x01; /* Algorithm Index */
		ret = 16;
		if (c_pos.blk_flags & BLKHDR_FLG_ENCRYPTED) {
			correct_key = TRUE;
			indx = 16;
			if (c_pos.encryption_ukad_length) {
				buf[3] += 4 + c_pos.encryption_ukad_length;
				buf[indx++] = 0x00;
				buf[indx++] = 0x01;
				buf[indx++] = 0x00;
				buf[indx++] = c_pos.encryption_ukad_length;
				for (count = 0; count < c_pos.encryption_ukad_length; ++count) {
					buf[indx++] = c_pos.encryption_ukad[count];
				}
				ret += 4 + c_pos.encryption_ukad_length;
			}
			if (c_pos.encryption_akad_length) {
				buf[3] += 4 + c_pos.encryption_akad_length;
				buf[indx++] = 0x01;
				buf[indx++] = 0x03;
				buf[indx++] = 0x00;
				buf[indx++] = c_pos.encryption_akad_length;
				for (count = 0; count < c_pos.encryption_akad_length; ++count) {
					buf[indx++] = c_pos.encryption_akad[count];
				}
				ret += 4 + c_pos.encryption_akad_length;
			}
			/* compare the keys */
			if (correct_key) {
				if (c_pos.encryption_key_length != KEY_LENGTH)
					correct_key = FALSE;
				for (count = 0; count < c_pos.encryption_key_length; ++count) {
					if (c_pos.encryption_key[count] != KEY[count]) {
						correct_key = FALSE;
						break;
					}
				}
			}
			if (correct_key)
				buf[12] = 0x5; /* encrypted, correct key */
			else
				buf[12] = 0x6; /* encrypted, need key */
		}
		break;

	default:
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
	}
	return ret;
}

/*
 * Retrieve Security Protocol Information
 */
#define SECURITY_PROTOCOL_INFORMATION 0
#define TAPE_DATA_ENCRYPTION 0x20

static int resp_spin(uint8_t *cdb, uint8_t *buf, uint8_t *sam_stat)
{
	uint16_t sps = get_unaligned_be16(&cdb[2]);
	uint32_t alloc_len = get_unaligned_be32(&cdb[6]);
	uint8_t inc_512 = (cdb[4] & 0x80) ? 1 : 0;

	if (inc_512)
		alloc_len = alloc_len * 512;

	switch(cdb[1]) {
	case SECURITY_PROTOCOL_INFORMATION:
		return resp_spin_page_0(buf, sps, alloc_len, sam_stat);
		break;

	case TAPE_DATA_ENCRYPTION:
		return resp_spin_page_20(buf, sps, alloc_len, sam_stat);
		break;
	}

	MHVTL_DBG(1, "Security protocol 0x%04x unknown", cdb[1]);

	mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
	return 0;
}

static int resp_spout(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
#ifdef MHVTL_DEBUG
	uint16_t sps = get_unaligned_be16(&cdb[2]);
	uint8_t inc_512 = (cdb[4] & 0x80) ? 1 : 0;
#endif
	uint8_t *sam_stat = &dbuf_p->sam_stat;
	int count;
	uint8_t	*buf = dbuf_p->data;

	if (cdb[1] != TAPE_DATA_ENCRYPTION) {
		MHVTL_DBG(1, "Security protocol 0x%02x unknown", cdb[1]);
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return 0;
	}
	MHVTL_DBG(2, "Tape Data Encryption, %s, "
			" alloc len: 0x%02x, inc_512: %s",
				lookup_sp_specific(sps),
				dbuf_p->sz, (inc_512) ? "Set" : "Unset");

	/* check for a legal "set data encryption page" */
	if ((buf[0] != 0x00) || (buf[1] != 0x10) ||
	    (buf[2] != 0x00) || (buf[3] < 16) ||
	    (buf[8] != 0x01) || (buf[9] != 0x00)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return 0;
	}

	KEY_INSTANCE_COUNTER++;
	ENCRYPT_MODE = buf[6];
	DECRYPT_MODE = buf[7];
	UKAD_LENGTH = 0;
	AKAD_LENGTH = 0;
	KEY_LENGTH = get_unaligned_be16(&buf[18]);
	for (count = 0; count < KEY_LENGTH; ++count) {
		KEY[count] = buf[20 + count];
	}

	MHVTL_DBG(2, "Encrypt mode: %d Decrypt mode: %d, "
			"ukad len: %d akad len: %d",
				ENCRYPT_MODE, DECRYPT_MODE,
				UKAD_LENGTH, AKAD_LENGTH);

	if (dbuf_p->sz > (19 + KEY_LENGTH + 4)) {
		if (buf[20 + KEY_LENGTH] == 0x00) {
			UKAD_LENGTH = get_unaligned_be16(&buf[22 + KEY_LENGTH]);
			for (count = 0; count < UKAD_LENGTH; ++count) {
				UKAD[count] = buf[24 + KEY_LENGTH + count];
			}
		} else if (buf[20 + KEY_LENGTH] == 0x01) {
			AKAD_LENGTH = get_unaligned_be16(&buf[22 + KEY_LENGTH]);
			for (count = 0; count < AKAD_LENGTH; ++count) {
				AKAD[count] = buf[24 + KEY_LENGTH + count];
			}
		}
	}

	count = FALSE;

	switch (lunit.drive_type) {
	case drive_10K_A:
	case drive_10K_B:
		if ((UKAD_LENGTH > 30) || (AKAD_LENGTH > 0))
			count = TRUE;
		/* This drive requires the KAD to decrypt */
		if (UKAD_LENGTH == 0)
			count = TRUE;
		break;
	case drive_3592_E06:
		if ((UKAD_LENGTH > 0) || (AKAD_LENGTH > 12))
			count = TRUE;
		/* This drive will not accept a KAD if not encrypting */
		if (!ENCRYPT_MODE && (UKAD_LENGTH || AKAD_LENGTH))
			count = TRUE;
		break;
	case drive_LTO4:
		if ((UKAD_LENGTH > 32) || (AKAD_LENGTH > 12))
			count = TRUE;
		/* This drive will not accept a KAD if not encrypting */
		if (!ENCRYPT_MODE && (UKAD_LENGTH || AKAD_LENGTH))
			count = TRUE;
		break;
	default:
		break;
	}

	/* For some reason, this command needs to be failed */
	if (count) {
		KEY_INSTANCE_COUNTER--;
		ENCRYPT_MODE = 0;
		DECRYPT_MODE = buf[7];
		UKAD_LENGTH = 0;
	        AKAD_LENGTH = 0;
		KEY_LENGTH = 0;
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
	}

	return 0;
}

/*
 * Update MAM contents with current counters
 */
static void updateMAM(struct MAM *mamp, uint8_t *sam_stat, int loadCount)
{
	uint64_t bw;		// Bytes Written
	uint64_t br;		// Bytes Read
	uint64_t load;	// load count

	MHVTL_DBG(2, "updateMAM(%d)", loadCount);

	// Update bytes written this load.
	put_unaligned_be64(bytesWritten, &mamp->WrittenInLastLoad);
	put_unaligned_be64(bytesRead, &mamp->ReadInLastLoad);

	// Update total bytes read/written
	bw = get_unaligned_be64(&mamp->WrittenInMediumLife);
	bw += bytesWritten;
	put_unaligned_be64(bw, &mamp->WrittenInMediumLife);

	br = get_unaligned_be64(&mamp->ReadInMediumLife);
	br += bytesRead;
	put_unaligned_be64(br, &mamp->ReadInMediumLife);

	// Update load count
	if (loadCount) {
		load = get_unaligned_be64(&mamp->LoadCount);
		load++;
		put_unaligned_be64(load, &mamp->LoadCount);
	}

	rewriteMAM(mamp, sam_stat);
}

/*
 * Returns true if blk header has correct encryption key data
 */
int valid_encryption_blk(uint8_t *sam_stat)
{
	int correct_key;
	int i;

	/* decryption logic */
	correct_key = TRUE;
	if (c_pos.blk_flags & BLKHDR_FLG_ENCRYPTED) {
		/* compare the keys */
		if (DECRYPT_MODE > 1) {
			if (c_pos.encryption_key_length != KEY_LENGTH) {
				mkSenseBuf(DATA_PROTECT, E_INCORRECT_KEY, sam_stat);
				correct_key = FALSE;
			}
			for (i = 0; i < c_pos.encryption_key_length; ++i) {
				if (c_pos.encryption_key[i] != KEY[i]) {
					mkSenseBuf(DATA_PROTECT, E_INCORRECT_KEY,
						           sam_stat);
					correct_key = FALSE;
					break;
				}
			}
			/* STK requires the UKAD back to decrypt */
			if ((lunit.drive_type == drive_10K_A) ||
				    (lunit.drive_type == drive_10K_B)) {
				if (c_pos.encryption_ukad_length != UKAD_LENGTH) {
					mkSenseBuf(DATA_PROTECT, E_INCORRECT_KEY, sam_stat);
					correct_key = FALSE;
					return correct_key;
				}
				for (i = 0; i < c_pos.encryption_ukad_length; ++i) {
					if (c_pos.encryption_ukad[i] != UKAD[i]) {
						mkSenseBuf(DATA_PROTECT, E_INCORRECT_KEY,
						           sam_stat);
						correct_key = FALSE;
						break;
					}
				}
			} /* End if STK 10K */
		} else {
			mkSenseBuf(DATA_PROTECT, E_UNABLE_TO_DECRYPT, sam_stat);
			correct_key = FALSE;
		}
	} else if (DECRYPT_MODE == 2) {
		mkSenseBuf(DATA_PROTECT, E_UNENCRYPTED_DATA, sam_stat);
		correct_key = FALSE;
	}
	return correct_key;
}

/*
 *
 * Process the SCSI command
 *
 * Called with:
 *	cdev     -> Char dev file handle,
 *	cdb      -> SCSI Command buffer pointer,
 *	dbuf     -> struct vtl_ds *
 *
 *	Return	 -> vtl_ds->sz contains number of bytes to return.
 */
static void processCommand(int cdev, uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	uint32_t sz = 0;
	uint32_t count;
	uint32_t retval = 0;
	int k;
	int code;
	int service_action;
	struct mode *smp = sm;
	uint8_t *sam_stat = &dbuf_p->sam_stat;
	uint8_t *buf;
	loff_t nread;
	static	uint8_t last_cmd;

	dbuf_p->sz = 0;

	if ((cdb[0] == READ_6 || cdb[0] == WRITE_6) && cdb[0] == last_cmd) {
		MHVTL_DBG_PRT_CDB(2, dbuf_p->serialNo, cdb);
	} else {
		MHVTL_DBG_PRT_CDB(1, dbuf_p->serialNo, cdb);
	}

	/* Limited subset of commands don't need to check for power-on reset */
	switch (cdb[0]) {
	case REPORT_LUN:
	case REQUEST_SENSE:
	case MODE_SELECT:
	case INQUIRY:
		break;
	default:
		if (check_reset(sam_stat))
			return;
	}

	// Now process SCSI command.
	switch (cdb[0]) {
	case ALLOW_MEDIUM_REMOVAL:
		MHVTL_DBG(1, "%s MEDIA removal (%ld) **",
					(cdb[4]) ? "Prevent" : "Allow",
					(long)dbuf_p->serialNo);
		resp_allow_prevent_removal(cdb, sam_stat);
		break;

	case INQUIRY:
		MHVTL_DBG(1, "INQUIRY (%ld) **", (long)dbuf_p->serialNo);
		dbuf_p->sz = spc_inquiry(cdb, dbuf_p, &lunit);
		break;

	case FORMAT_UNIT:	// That's FORMAT_MEDIUM for an SSC device...
		MHVTL_DBG(1, "Format Medium (%ld) **",
						(long)dbuf_p->serialNo);

		if (!checkRestrictions(sam_stat))
			break;

		if (c_pos.blk_number != 0) {
			MHVTL_DBG(2, "Not at beginning **");
			mkSenseBuf(ILLEGAL_REQUEST,E_POSITION_PAST_BOM,
								 sam_stat);
			break;
		}
		mkEODHeader(sam_stat);
		break;

	case SEEK_10:	// Thats LOCATE_BLOCK for SSC devices...
		MHVTL_DBG(1, "Fast Block Locate (%ld) **",
						(long)dbuf_p->serialNo);
		count = get_unaligned_be32(&cdb[3]);

		/* If we want to seek closer to beginning of file than
		 * we currently are, rewind and seek from there
		 */
		MHVTL_DBG(2, "Current blk: %" PRId64 ", seek: %d",
					c_pos.blk_number, count);
		if (count < c_pos.blk_number &&
					c_pos.blk_number - count > count)
			resp_rewind(sam_stat);

		if (MediaType == MEDIA_TYPE_WORM)
			OK_to_write = 0;
		while (c_pos.blk_number != count) {
			if (c_pos.blk_number > count) {
				if (skip_to_prev_header(sam_stat) == -1)
					break;
			} else {
				if (skip_to_next_header(sam_stat) == -1)
					break;
			}
		}
		break;

	case LOG_SELECT:	// Set or reset LOG stats.
		MHVTL_DBG(1, "LOG SELECT (%ld) **", (long)dbuf_p->serialNo);
		resp_log_select(cdb, sam_stat);
		break;

	case LOG_SENSE:
		MHVTL_DBG(1, "LOG SENSE (%ld) **", (long)dbuf_p->serialNo);
		dbuf_p->sz = get_unaligned_be16(&cdb[7]);
		retval = resp_log_sense(cdb, dbuf_p);
		/* Only return number of bytes allocated by initiator */
		if (dbuf_p->sz > retval)
			dbuf_p->sz = retval;
		break;

	case MODE_SELECT:
	case MODE_SELECT_10:
		MHVTL_DBG(1, "MODE SELECT (%ld) **", (long)dbuf_p->serialNo);
		dbuf_p->sz = (MODE_SELECT == cdb[0]) ? cdb[4] :
						((cdb[7] << 8) | cdb[8]);
		if (cdb[1] & 0x10) { /* Page Format: 1 - SPC, 0 - vendor uniq */
			/* FIXME: Need to add something here */
		}
		dbuf_p->sz = resp_mode_select(cdev, cdb, dbuf_p);
		break;

	case MODE_SENSE:
	case MODE_SENSE_10:
		MHVTL_DBG(1, "MODE SENSE (%ld) **", (long)dbuf_p->serialNo);
		dbuf_p->sz = resp_mode_sense(cdb, dbuf_p->data, smp, MediaWriteProtect, sam_stat);
		break;

	case READ_10:
		MHVTL_DBG(1, "READ_10 (%ld) op code not currently supported",
						(long)dbuf_p->serialNo);
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case READ_12:
		MHVTL_DBG(1, "READ_12 (%ld) op code not currently supported",
						(long)dbuf_p->serialNo);
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case READ_16:
		MHVTL_DBG(1, "READ_16 (%ld) op code not currently supported",
						(long)dbuf_p->serialNo);
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case READ_6:
		if (cdb[1] & FIXED) { /* If - Fixed block read */
			count = get_unaligned_be24(&cdb[2]);
			sz = get_unaligned_be24(&blockDescriptorBlock[5]);
			MHVTL_DBG(last_cmd == READ_6 ? 2 : 1,
				"READ_6 \"Fixed block read\" "
				"under development - "
				" Read %d blocks of %d size", count, sz);
		} else { /* else - Variable block read */
			sz = get_unaligned_be24(&cdb[2]);
			count = 1;
			MHVTL_DBG(last_cmd == READ_6 ? 2 : 1,
				"READ_6 (%ld) : %d bytes **",
					(long)dbuf_p->serialNo,
					sz);
		}

		/* If both FIXED & SILI bits set, invalid combo.. */
		if ((cdb[1] & (SILI | FIXED)) == (SILI | FIXED)) {
			MHVTL_DBG(1, "Supress ILI and Fixed block "
					"read not allowed by SSC3");
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sam_stat);
			reset = 0;
			break;
		}
		if (tapeLoaded == TAPE_LOADED) {
			if (MediaType == MEDIA_TYPE_CLEAN) {
				MHVTL_DBG(3, "Cleaning cart loaded");
				mkSenseBuf(NOT_READY, E_CLEANING_CART_INSTALLED,
								sam_stat);
				break;
			}
		} else if (tapeLoaded == TAPE_UNLOADED) {
			MHVTL_DBG(3, "No media loaded");
			mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
			break;
		} else {
			MHVTL_DBG(1, "Media format corrupt");
			mkSenseBuf(NOT_READY, E_MEDIUM_FORMAT_CORRUPT,sam_stat);
			break;
		}

		buf = dbuf_p->data;
		for (k = 0; k < count; k++) {
			if (!valid_encryption_blk(sam_stat))
				break;
			retval = readBlock(cdev, buf, sam_stat, sz);
			buf += retval;
			dbuf_p->sz += retval;
		}
		/* Fix this for fixed block reads */
		if (retval > (sz * count))
			retval = sz * count;
		bytesRead += retval;
		pg_read_err_counter.bytesProcessed = bytesRead;
//		dbuf_p->sz += retval;
		break;

	case READ_ATTRIBUTE:
		MHVTL_DBG(1, "Read Attribute (%ld) **",
						(long)dbuf_p->serialNo);
		if (tapeLoaded == TAPE_UNLOADED) {
			MHVTL_DBG(1, "Failed due to \"no media loaded\"");
			mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
			break;
		} else if (tapeLoaded > TAPE_LOADED) {
			MHVTL_DBG(1, "Failed due to \"media corrupt\"");
			mkSenseBuf(NOT_READY,E_MEDIUM_FORMAT_CORRUPT,sam_stat);
			break;
		}
		/* Only support Service Action - Attribute Values */
		if (cdb[1] < 2)
			dbuf_p->sz = resp_read_attribute(cdb, dbuf_p->data, sam_stat);
		else {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sam_stat);
			break;
		}
		{
#ifdef MHVTL_DEBUG
			uint8_t *p = dbuf_p->data;
#endif
			MHVTL_DBG(3,
				" dump return data, length: %d", dbuf_p->sz);
			for (k = 0; k < dbuf_p->sz; k += 8) {
				MHVTL_DBG(3, " 0x%02x 0x%02x 0x%02x 0x%02x"
					" 0x%02x 0x%02x 0x%02x 0x%02x",
					p[k+0], p[k+1], p[k+2], p[k+3],
					p[k+4], p[k+5], p[k+6], p[k+7]);
			}
		}
		break;

	case READ_BLOCK_LIMITS:
		MHVTL_DBG(1, "Read block limits (%ld) **",
						(long)dbuf_p->serialNo);
		if (tapeLoaded == TAPE_LOADED || tapeLoaded == TAPE_UNLOADED)
			dbuf_p->sz = resp_read_block_limits(dbuf_p, bufsize);
		else
			mkSenseBuf(NOT_READY,E_MEDIUM_FORMAT_CORRUPT,sam_stat);
		break;

	case READ_MEDIA_SERIAL_NUMBER:
		MHVTL_DBG(1, "Read Medium Serial No. (%ld) **",
						(long)dbuf_p->serialNo);
		if (tapeLoaded == TAPE_UNLOADED)
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sam_stat);
		else if (tapeLoaded == TAPE_LOADED) {
			dbuf_p->sz = resp_read_media_serial(mediaSerialNo,
								dbuf_p->data,
								sam_stat);
		} else
			mkSenseBuf(NOT_READY,E_MEDIUM_FORMAT_CORRUPT,sam_stat);
		break;

	case READ_POSITION:
		MHVTL_DBG(1, "Read Position (%ld) **",
						(long)dbuf_p->serialNo);
		service_action = cdb[1] & 0x1f;
/* service_action == 0 or 1 -> Returns 20 bytes of data (short) */
		if (tapeLoaded == TAPE_UNLOADED) {
			mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		} else if ((service_action == 0) || (service_action == 1)) {
			dbuf_p->sz = resp_read_position(c_pos.blk_number,
							dbuf_p->data, sam_stat);
		} else {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sam_stat);
		}
		break;

	case RELEASE:
	case RELEASE_10:
		MHVTL_DBG(1, "Release (%ld) **", (long)dbuf_p->serialNo);
		if (!SPR_Reservation_Type && SPR_Reservation_Key)
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		I_am_SPC_2_Reserved = 0;
		break;

	case REPORT_DENSITY:
		MHVTL_DBG(1, "Report Density (%ld) **", (long)dbuf_p->serialNo);
		dbuf_p->sz = get_unaligned_be16(&cdb[7]);
		dbuf_p->sz = resp_report_density((cdb[1] & 0x01), dbuf_p);
		break;

	case REPORT_LUN:
		MHVTL_DBG(1, "Report LUNs (%ld) **", (long)dbuf_p->serialNo);
		// Minimum allocation length is 16 bytes.
		if (get_unaligned_be32(&cdb[6]) < 16) {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sam_stat);
			break;
		}
		report_luns.size = htonl(sizeof(report_luns) - 8);
		resp_report_lun(&report_luns, dbuf_p->data, sam_stat);
		break;

	case REQUEST_SENSE:
		MHVTL_DBG(1, "Request Sense (%ld) : key/ASC/ASCQ "
				"[0x%02x 0x%02x 0x%02x]"
				" Filemark: %s, EOM: %s, ILI: %s",
					(long)dbuf_p->serialNo,
					sense[2] & 0x0f, sense[12], sense[13],
					(sense[2] & SD_FILEMARK) ? "yes" : "no",
					(sense[2] & SD_EOM) ? "yes" : "no",
					(sense[2] & SD_ILI) ? "yes" : "no");
		sz = (cdb[4] < sizeof(sense)) ? cdb[4] : sizeof(sense);
		memcpy(dbuf_p->data, sense, sz);
		/* Clear out the request sense flag */
		*sam_stat = 0;
		memset(sense, 0, sizeof(sense));
		dbuf_p->sz = sz;
		break;

	case RESERVE:
	case RESERVE_10:
		MHVTL_DBG(1, "Reserve (%ld) **", (long)dbuf_p->serialNo);
		if (!SPR_Reservation_Type && !SPR_Reservation_Key)
			I_am_SPC_2_Reserved = 1;
		if (!SPR_Reservation_Type & SPR_Reservation_Key)
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		break;

	case REZERO_UNIT:	/* Rewind */
		MHVTL_DBG(1, "Rewinding (%ld) **", (long)dbuf_p->serialNo);
		resp_rewind(sam_stat);
		sleep(1);
		break;

	case ERASE_6:
		MHVTL_DBG(1, "Erasing (%ld) **", (long)dbuf_p->serialNo);

		if (!checkRestrictions(sam_stat))
			break;

		/* Rewind and postition just after the first header. */
		resp_rewind(sam_stat);

		if (ftruncate(datafile, c_pos.curr_blk)) {
			MHVTL_DBG(1, "Failed to truncate datafile");
		}

		/* Position to just before first header. */
		position_to_curr_header(sam_stat);

		/* Write EOD header */
		mkEODHeader(sam_stat);
		sleep(2);
		break;

	case SPACE:
		MHVTL_DBG(1, "SPACE (%ld) **", (long)dbuf_p->serialNo);
		count = get_unaligned_be24(&cdb[2]);
		code = cdb[1] & 0x07;

		/* Can return a '2s complement' to seek backwards */
		if (cdb[2] & 0x80)
			count += (0xff << 24);

		resp_space(count, code, sam_stat);
		break;

	case START_STOP:	/* Load/Unload cmd */
		if (cdb[4] && 0x1) {
			MHVTL_DBG(1, "Loading Tape (%ld) **",
						(long)dbuf_p->serialNo);
			tapeLoaded = resp_rewind(sam_stat);
		} else if (tapeLoaded == TAPE_UNLOADED) {
			mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		} else {
			mam.record_dirty = 0;
			// Don't update load count on unload -done at load time
			updateMAM(&mam, sam_stat, 0);
			close(datafile);
			tapeLoaded = TAPE_UNLOADED;
			OK_to_write = 0;
			clearWORM();
			MHVTL_DBG(1, "Unloading Tape (%ld)  **",
						(long)dbuf_p->serialNo);
			close(datafile);
		}
		break;

	case TEST_UNIT_READY:	// Return OK by default
		MHVTL_DBG(1, "Test Unit Ready (%ld) : %s",
				(long)dbuf_p->serialNo,
				(tapeLoaded == TAPE_UNLOADED) ? "No" : "Yes");
		if (tapeLoaded == TAPE_UNLOADED)
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sam_stat);
		if (tapeLoaded > TAPE_LOADED)
			mkSenseBuf(NOT_READY,E_MEDIUM_FORMAT_CORRUPT,sam_stat);

		break;

	case WRITE_10:
		MHVTL_DBG(1, "WRITE_10 op code not currently supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case WRITE_12:
		MHVTL_DBG(1, "WRITE_12 op code not currently supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case WRITE_16:
		MHVTL_DBG(1, "WRITE_16 op code not currently supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case WRITE_6:
		/* If Fixed block writes */
		if (cdb[1] & FIXED) {
			count = get_unaligned_be24(&cdb[2]);
			sz = get_unaligned_be24(&blockDescriptorBlock[5]);
			MHVTL_DBG(last_cmd == WRITE_6 ? 2 : 1,
				"WRITE_6: %d blks of %d bytes (%ld) **",
						count,
						sz,
						(long)dbuf_p->serialNo);
		/* else - Variable Block writes */
		} else {
			count = 1;
			sz = get_unaligned_be24(&cdb[2]);
			MHVTL_DBG(last_cmd == WRITE_6 ? 2 : 1,
				"WRITE_6: %d bytes (%ld) **",
						sz,
						(long)dbuf_p->serialNo);
		}

		/* FIXME: Should handle this instead of 'check & warn' */
		if ((sz * count) > bufsize)
			MHVTL_DBG(1,
			"Fatal: bufsize %d, requested write of %d bytes",
							bufsize, sz);

		/* Retrieve data from kernel */
		dbuf_p->sz = sz * count;
		nread = retrieve_CDB_data(cdev, dbuf_p);

		if (!checkRestrictions(sam_stat))
			break;

		if (OK_to_write) {
			buf = dbuf_p->data;
			for (k = 0; k < count; k++) {
				retval = writeBlock(buf, sz, sam_stat);
				bytesWritten += retval;
				buf += retval;
				pg_write_err_counter.bytesProcessed =
							bytesWritten;
			}
		}
		break;

	case WRITE_ATTRIBUTE:
		MHVTL_DBG(1, "Write Attributes (%ld) **",
						(long)dbuf_p->serialNo);

		if (tapeLoaded == TAPE_UNLOADED) {
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sam_stat);
			break;
		} else if (tapeLoaded > TAPE_LOADED) {
			mkSenseBuf(NOT_READY,E_MEDIUM_FORMAT_CORRUPT, sam_stat);
			break;
		}

		dbuf_p->sz = get_unaligned_be32(&cdb[10]);
		sz = retrieve_CDB_data(cdev, dbuf_p);
		MHVTL_DBG(1, "  --> Expected to read %d bytes"
				", read %d", dbuf_p->sz, sz);
		if (resp_write_attribute(cdb, dbuf_p, &mam))
			rewriteMAM(&mam, sam_stat);
		break;

	case WRITE_FILEMARKS:
		sz = get_unaligned_be24(&cdb[2]);
		MHVTL_DBG(1, "Write %d filemarks (%ld) **",
						sz,
						(long)dbuf_p->serialNo);
		if (!checkRestrictions(sam_stat))
			break;

		if (sz > 0) {
			while (sz > 0) {
				sz--;
				mkNewHeader(B_FILEMARK, 0, 0, sam_stat);
			}
			mkEODHeader(sam_stat);
		}
		break;

	case RECEIVE_DIAGNOSTIC:
		MHVTL_DBG(1, "Receive Diagnostic (%ld) **",
						(long)dbuf_p->serialNo);
		dbuf_p->sz = ProcessReceiveDiagnostic(cdb, dbuf_p->data,
						dbuf_p);
		break;

	case SEND_DIAGNOSTIC:
		MHVTL_DBG(1, "Send Diagnostic (%ld) **",
						(long)dbuf_p->serialNo);
		count = get_unaligned_be16(&cdb[3]);
		if (count) {
			dbuf_p->sz = count;
			sz = retrieve_CDB_data(cdev, dbuf_p);
			ProcessSendDiagnostic(cdb, 16, dbuf_p->data, sz,
						dbuf_p);
		}
		break;

	case PERSISTENT_RESERVE_IN:
		MHVTL_DBG(1, "PERSISTENT RESERVE IN (%ld) **",
						(long)dbuf_p->serialNo);
		if (I_am_SPC_2_Reserved)
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		else
			dbuf_p->sz = resp_spc_pri(cdb, dbuf_p);
		break;

	case PERSISTENT_RESERVE_OUT:
		MHVTL_DBG(1, "PERSISTENT RESERVE OUT (%ld) **",
						(long)dbuf_p->serialNo);
		if (I_am_SPC_2_Reserved) {
			MHVTL_DBG(1, "SPC 2 reserved");
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		} else {
			dbuf_p->sz = get_unaligned_be32(&cdb[5]);
			nread = retrieve_CDB_data(cdev, dbuf_p);
			resp_spc_pro(cdb, dbuf_p);
		}
		break;

	case SECURITY_PROTOCOL_IN:
		MHVTL_DBG(1, "Security Protocol In (%ld) **",
						(long)dbuf_p->serialNo);
		dbuf_p->sz = resp_spin(cdb, dbuf_p->data, sam_stat);
		MHVTL_DBG(3, "Returning %d bytes", dbuf_p->sz);
		if (verbose > 2)
			hex_dump(dbuf_p->data, dbuf_p->sz);
		break;

	case SECURITY_PROTOCOL_OUT:
		MHVTL_DBG(1, "Security Protocol Out (%ld) **",
						(long)dbuf_p->serialNo);
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		dbuf_p->sz = get_unaligned_be32(&cdb[6]);
		/* Check for '512 increment' bit & multiply sz by 512 if set */
		dbuf_p->sz *= (cdb[4] & 0x80) ? 512 : 1;

		nread = retrieve_CDB_data(cdev, dbuf_p);
		dbuf_p->sz = resp_spout(cdb, dbuf_p);
		MHVTL_DBG(3, "Returning %d bytes", dbuf_p->sz);
		break;

	case A3_SA:
		resp_a3_service_action(cdb, dbuf_p);
		break;

	case A4_SA:
		resp_a4_service_action(cdb, dbuf_p);
		break;

	case ACCESS_CONTROL_IN:
		MHVTL_DBG(1, "ACCESS CONTROL IN (%ld) **",
						(long)dbuf_p->serialNo);
		break;

	case ACCESS_CONTROL_OUT:
		MHVTL_DBG(1, "ACCESS CONTROL OUT (%ld) **",
						(long)dbuf_p->serialNo);
		break;

	case EXTENDED_COPY:
		MHVTL_DBG(1, "EXTENDED COPY (%ld) **",
						(long)dbuf_p->serialNo);
		break;

	default:
		MHVTL_DBG(1, "Unknown OP code (%ld) **",
						(long)dbuf_p->serialNo);
		break;
	}

	last_cmd = cdb[0];
	return;
}

/*
 * Attempt to load PCL - i.e. Open datafile and read in BOT header & MAM
 *
 * Return 0 on failure, 1 on success
 */

static int load_tape(char *PCL, uint8_t *sam_stat)
{
	loff_t nread;
	uint64_t fg = 0;	// TapeAlert flags

	bytesWritten = 0;	// Global - Bytes written this load
	bytesRead = 0;		// Global - Bytes rearead this load

	sprintf(currentMedia, "%s/%s", MHVTL_HOME_PATH, PCL);
	MHVTL_DBG(2, "Opening file/media %s", currentMedia);
	if ((datafile = open(currentMedia, O_RDWR|O_LARGEFILE)) == -1) {
		MHVTL_DBG(1, "%s: open file/media failed, %m", currentMedia);
		return 0;	// Unsuccessful load
	}

	// Now read in header information from just opened datafile
	nread = read(datafile, &c_pos, sizeof(c_pos));
	if (nread < 0) {
		MHVTL_DBG(1, "%s: %m",
			 "Error reading header in datafile, load failed");
		close(datafile);
		return 0;	// Unsuccessful load
	} else if (nread < sizeof(c_pos)) {	// Did not read anything...
		MHVTL_DBG(1, "%s: %m",
				 "Error: Not a tape format, load failed");
		/* TapeAlert - Unsupported format */
		fg = 0x800;
		close(datafile);
		goto unsuccessful;
	}
	if (c_pos.blk_type != B_BOT) {
		MHVTL_DBG(1, "Header type: %d not valid, load failed",
							c_pos.blk_type);
		/* TapeAlert - Unsupported format */
		fg = 0x800;
		close(datafile);
		goto unsuccessful;
	}
	// FIXME: Need better validation checking here !!
	 if (c_pos.next_blk != (sizeof(struct blk_header) + sizeof(struct MAM))) {
		MHVTL_DBG(1, "MAM size incorrect, load failed"
			" - Expected size: %d, size found: %" PRId64,
			(int)(sizeof(struct blk_header) + sizeof(struct MAM)),
				c_pos.next_blk);
		close(datafile);
		return 0;	// Unsuccessful load
	}
	nread = read(datafile, &mam, sizeof(mam));
	if (nread < 0) {
		mediaSerialNo[0] = '\0';
		MHVTL_DBG(1, "Can not read MAM from mounted media");
		return 0;	// Unsuccessful load
	}
	// Set TapeAlert flg 32h =>
	//	Lost Statics
	if (mam.record_dirty != 0) {
		fg = 0x02000000000000ull;
		MHVTL_DBG(1, "Previous unload was not clean");
	}

	max_tape_capacity = (loff_t)c_pos.blk_size * (loff_t)1048576;
	MHVTL_DBG(1, "Tape capacity: %" PRId64, max_tape_capacity);

	blockDescriptorBlock[0] = mam.MediumDensityCode;
	mam.record_dirty = 1;
	// Increment load count
	updateMAM(&mam, sam_stat, 1);

	/* resp_rewind() will clean up for us..
	 * - It also set up media type & if we can write to media
	 */
	resp_rewind(sam_stat);

	strncpy((char *)mediaSerialNo, (char *)mam.MediumSerialNumber,
				sizeof(mam.MediumSerialNumber) - 1);
	switch (MediaType) {
	case MEDIA_TYPE_WORM:
		setWORM();
		MHVTL_DBG(1, "Write Once Read Many (WORM) media loaded");
		break;

	case MEDIA_TYPE_CLEAN:
		fg = 0x400;
		MHVTL_DBG(1, "Cleaning cart loaded");
		mkSenseBuf(UNIT_ATTENTION,E_CLEANING_CART_INSTALLED, sam_stat);
		break;
	default:
		mkSenseBuf(UNIT_ATTENTION,E_NOT_READY_TO_TRANSITION, sam_stat);
		break;
	}

	blockDescriptorBlock[0] = mam.MediumDensityCode;
	MHVTL_DBG(1, "Setting MediumDensityCode to 0x%02x",
			mam.MediumDensityCode);

	// Update TapeAlert flags
	setSeqAccessDevice(&seqAccessDevice, fg);
	setTapeAlert(&TapeAlert, fg);

	switch (mam.MediumDensityCode) {
	case medium_density_code_lto1:
		Media_Type = Media_LTO1;
		MHVTL_DBG(1, "LTO1 media");
		break;
	case medium_density_code_lto2:
		Media_Type = Media_LTO2;
		MHVTL_DBG(1, "LTO2 media");
		break;
	case medium_density_code_lto3:
		Media_Type = Media_LTO3;
		MHVTL_DBG(1, "LTO3 media");
		break;
	case medium_density_code_lto4:
		Media_Type = Media_LTO4;
		MHVTL_DBG(1, "LTO4 media");
		break;
	case medium_density_code_j1a:
		Media_Type = Media_3592_JA;
		MHVTL_DBG(1,"3592-JA media");
		break;
	case medium_density_code_e05:
		Media_Type = Media_3592_JA;
		MHVTL_DBG(1, "3592-JA media");
		break;
	case medium_density_code_e06:
		Media_Type = Media_3592_JA;
		MHVTL_DBG(1, "3592-JA media");
		break;
	case medium_density_code_ait4:
		Media_Type = Media_AIT4;
		MHVTL_DBG(1, "AIT4 media");
		break;
	case medium_density_code_10kA:
		Media_Type = Media_10K;
		MHVTL_DBG(1, "10K-A media");
		break;
	case medium_density_code_10kB:
		Media_Type = Media_10K;
		MHVTL_DBG(1, "10K-B media");
		break;
//	FIXME: denisty_code_600 is same as LTO1..
//	case medium_density_code_600:
//		Media_Type = Media_SDLT600;
//		break;
	default:
		Media_Type = Media_UNKNOWN;
		MHVTL_DBG(1, "unknown media");
		break;
	}

	return 1;	// Return successful load

unsuccessful:
	setSeqAccessDevice(&seqAccessDevice, fg);
	setTapeAlert(&TapeAlert, fg);
	return 2;
}


/* Strip (recover) the 'Physical Cartridge Label'
 *   Well at least the data filename which relates to the same thing
 */
static char * strip_PCL(char *p, int start) {
	char *q;

	/* p += 4 (skip over 'load' string)
	 * Then keep going until '*p' is a space or NULL
	 */
	for (p += start; *p == ' '; p++)
		if ('\0' == *p)
			break;
	q = p;	// Set end-of-word marker to start of word.
	for (q = p; *q != '\0'; q++)
		if (*q == ' ' || *q == '\t')
			break;
	*q = '\0';	// Set null terminated string
//	printf(":\nmedia ID:%s, p: %lx, q: %lx\n", p, &p, &q);

return p;
}

static int processMessageQ(char *mtext, uint8_t *sam_stat)
{
	char * pcl;
	char s[128];

	MHVTL_DBG(1, "Q msg : %s", mtext);

	/* Tape Load message from Library */
	if (!strncmp(mtext, "lload", 5)) {
		if ( ! inLibrary) {
			MHVTL_DBG(2, "lload & drive not in library");
			return (0);
		}

		if (tapeLoaded != TAPE_UNLOADED) {
			MHVTL_DBG(2, "Tape already mounted");
			send_msg("Load failed", LIBRARY_Q);
		} else {
			pcl = strip_PCL(mtext, 6); // 'lload ' => offset of 6
			tapeLoaded = load_tape(pcl, sam_stat);
			if (tapeLoaded == TAPE_LOADED)
				sprintf(s, "Loaded OK: %s\n", pcl);
			else
				sprintf(s, "Load failed: %s\n", pcl);
			send_msg(s, LIBRARY_Q);
		}
	}

	/* Tape Load message from User space */
	if (!strncmp(mtext, "load", 4)) {
		if (inLibrary)
			MHVTL_DBG(2, "Warn: Tape assigned to library");
		if (tapeLoaded == TAPE_LOADED) {
			MHVTL_DBG(2, "A tape is already mounted");
		} else {
			pcl = strip_PCL(mtext, 4);
			tapeLoaded = load_tape(pcl, sam_stat);
		}
	}

	if (!strncmp(mtext, "unload", 6)) {
		switch (tapeLoaded) {
		case TAPE_LOADED:
			mam.record_dirty = 0;
			// Don't update load count on unload -done at load time
			updateMAM(&mam, sam_stat, 0);
			/* Fall thru to case 2: */
		case TAPE_LOAD_BAD:
			tapeLoaded = TAPE_UNLOADED;
			OK_to_write = 0;
			clearWORM();
			MHVTL_DBG(1, "Library requested tape unload");
			close(datafile);
			break;
		default:
			MHVTL_DBG(2, "Tape not mounted");
			tapeLoaded = TAPE_UNLOADED;
			break;
		}
	}

	if (!strncmp(mtext, "exit", 4)) {
		MHVTL_DBG(1, "Notice to exit : %s", mtext);
		return 1;
	}

	if (!strncmp(mtext, "Register", 8)) {
		inLibrary = 1;
		MHVTL_DBG(1, "Notice from Library controller : %s", mtext);
	}

	if (!strncmp(mtext, "verbose", 7)) {
		if (verbose)
			verbose--;
		else
			verbose = 3;
		syslog(LOG_DAEMON|LOG_NOTICE, "Verbose: %s at level %d",
				 verbose ? "enabled" : "disabled", verbose);
	}

	if (!strncmp(mtext, "TapeAlert", 9)) {
		uint64_t flg = 0L;
		sscanf(mtext, "TapeAlert %" PRIx64, &flg);
		setTapeAlert(&TapeAlert, flg);
		setSeqAccessDevice(&seqAccessDevice, flg);
	}

	if (!strncmp(mtext, "debug", 5)) {
		if (debug) {
			debug--;
		} else {
			debug++;
			verbose = 2;
		}
	}
return 0;
}

int init_queue(void)
{
	int	queue_id;

	/* Attempt to create or open message queue */
	if ( (queue_id = msgget(QKEY, IPC_CREAT | QPERM)) == -1)
		syslog(LOG_DAEMON|LOG_ERR, "%s %m", "msgget failed");

return (queue_id);
}

/*
 * Initialise structure data for mode pages.
 * - Allocate memory for each mode page & init to 0
 * - Set up size of mode page
 * - Set initial values of mode pages
 *
 * Return void  - Nothing
 */
#define COMPRESSION_TYPE 0x10
static void init_mode_pages(struct mode *m)
{
	struct mode *mp;

	// RW Error Recovery: SSC-3 8.3.5
	if ((mp = alloc_mode_page(1, m, 12))) {
		// Init rest of page data..
	}

	// Disconnect-Reconnect: SPC-3 7.4.8
	if ((mp = alloc_mode_page(2, m, 16))) {
		mp->pcodePointer[2] = 50; // Buffer full ratio
		mp->pcodePointer[3] = 50; // Buffer enpty ratio
		mp->pcodePointer[10] = 4;
	}

	// Control: SPC-3 7.4.6
	if ((mp = alloc_mode_page(0x0a, m, 12))) {
		// Init rest of page data..
	}

	// Data compression: SSC-3 8.3.2
	if ((mp = alloc_mode_page(0x0f, m, 16))) {
		// Init rest of page data..
		mp->pcodePointer[2] = 0xc0; /* Set Data Compression Enable */
		mp->pcodePointer[3] = 0x80; /* Set Data Decompression Enable */
		/* Compression Algorithm */
		put_unaligned_be32(COMPRESSION_TYPE, &mp->pcodePointer[4]);
		/* Decompression Algorithm */
		put_unaligned_be32(COMPRESSION_TYPE, &mp->pcodePointer[8]);
	}

	// Device Configuration: SSC-3 8.3.3
	if ((mp = alloc_mode_page(0x10, m, 16))) {
		// Write delay time (100mSec intervals)
		mp->pcodePointer[7] = 0x64;
		// Block Identifiers Supported
		mp->pcodePointer[8] = 0x40;
		// Enable EOD & Sync at early warning
		mp->pcodePointer[10] = 0x18;
		// Select Data Compression
		mp->pcodePointer[14] = 0x01;
		// WTRE (WORM handling)
		mp->pcodePointer[15] = 0x80;
	}

	// Medium Partition: SSC-3 8.3.4
	if ((mp = alloc_mode_page(0x11, m, 16))) {
		// Init rest of page data..
	}

	// Extended: SPC-3 - Not used here.
	// Extended Device (Type Specific): SPC-3 - Not used here

	// Power condition: SPC-3 7.4.12
	mp = alloc_mode_page(0x1a, m, 12);
	if (mp) {
		// Init rest of page data..
	}

	// Informational Exception Control: SPC-3 7.4.11 (TapeAlert)
	if ((mp = alloc_mode_page(0x1c, m, 12))) {
		mp->pcodePointer[2] = 0x08;
		mp->pcodePointer[3] = 0x03;
	}

	// Medium configuration: SSC-3 8.3.7
	if ((mp = alloc_mode_page(0x1d, m, 32))) {
		// Init rest of page data..
	}
}

/* Set VPD data with device serial number */
static void update_vpd_80(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0x80)];

	memcpy(vpd_pg->data, p, strlen(p));
}

static void update_vpd_83(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0x83)];
	uint8_t *d;
	char *ptr;
	int num;
	int len, j;

	d = vpd_pg->data;

	d[0] = 2;
	d[1] = 1;
	d[2] = 0;
	num = VENDOR_ID_LEN + PRODUCT_ID_LEN + 10;
	d[3] = num;

	memcpy(&d[4], &lu->vendor_id, VENDOR_ID_LEN);
	memcpy(&d[12], &lu->product_id, PRODUCT_ID_LEN);
	memcpy(&d[28], &lu->lu_serial_no, 10);
	len = (int)strlen(lu->lu_serial_no);
	ptr = &lu->lu_serial_no[len];

	num += 4;
	/* NAA IEEE registered identifier (faked) */
	d[num] = 0x1;	/* Binary */
	d[num + 1] = 0x3;
	d[num + 2] = 0x0;
	d[num + 3] = 0x8;
	d[num + 4] = 0x51;
	d[num + 5] = 0x23;
	d[num + 6] = 0x45;
	d[num + 7] = 0x60;
	d[num + 8] = 0x3;
	d[num + 9] = 0x3;
	d[num + 10] = 0x3;
	d[num + 11] = 0x3;

	if (lu->naa) { /* If defined in config file */
		sscanf((const char *)lu->naa,
			"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			&d[num + 4], 
			&d[num + 5], 
			&d[num + 6], 
			&d[num + 7], 
			&d[num + 8], 
			&d[num + 9], 
			&d[num + 10], 
			&d[num + 11]);
	} else { /* Else munge the serial number */
		ptr--;
		for (j = 11; j > 3; ptr--, j--)
			d[num + j] = *ptr;
	}
}

/*
 * A place to setup any customisations (WORM / Security handling)
 */
static void config_lu(struct lu_phy_attr *lu)
{
	lu->drive_type = drive_UNKNOWN;

	/* Define lu->drive_type first */
	if (!strncasecmp(lu->product_id, "ULT", 3)) {
		char *dup_product_id;

		/* Ultrium drives */
		dup_product_id = strchr(lu->product_id, '-');
		if (!dup_product_id)
			return;

		MHVTL_DBG(2, "Ultrium drive: %s", dup_product_id);

		if (!strncasecmp(dup_product_id, "-TD1", 4)) {
			lu->drive_type = drive_LTO1;
			MHVTL_DBG(1, "LTO 1 drive");

		} else if (!strncasecmp(dup_product_id, "-TD2", 4)) {
			lu->drive_type = drive_LTO2;
			MHVTL_DBG(1, "LTO 2 drive");
		} else if (!strncasecmp(dup_product_id, "-TD3", 4)) {
			lu->drive_type = drive_LTO3;
			MHVTL_DBG(1, "LTO 3 drive");
		} else if (!strncasecmp(dup_product_id, "-TD4", 4)) {
			lu->drive_type = drive_LTO4;
			MHVTL_DBG(1, "LTO 4 drive");
		}
	} else if (!strncasecmp(lu->product_id, "SDLT600", 7)) {
		lu->drive_type = drive_SDLT600;
		MHVTL_DBG(1, "SDLT600 drive");
	} else if (!strncasecmp(lu->product_id, "SDX-900", 7)) {
		lu->drive_type = drive_AIT4;
		MHVTL_DBG(1, "AIT4 drive");
	} else if (!strncasecmp(lu->product_id, "03592J1A", 8)) {
		lu->drive_type = drive_3592_J1A;
		MHVTL_DBG(1, "3592_J1A drive");
	} else if (!strncasecmp(lu->product_id, "03592E05", 8)) {
		lu->drive_type = drive_3592_E05;
		MHVTL_DBG(1, "3952 E05 drive");
	} else if (!strncasecmp(lu->product_id, "03592E06", 8)) {
		lu->drive_type = drive_3592_E06;
		MHVTL_DBG(1, "3592 E06 drive");
	} else if (!strncasecmp(lu->product_id, "T10000B", 7)) {
		lu->drive_type = drive_10K_B;
		MHVTL_DBG(1, "T10000-B drive");
	} else if (!strncasecmp(lu->product_id, "T10000", 6)) {
		lu->drive_type = drive_10K_A;
		MHVTL_DBG(1, "T10000-A drive");
	}
}

static void update_vpd_b0(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xb0)];
	uint8_t *worm;

	worm = p;

	*vpd_pg->data = (*worm) ? 1 : 0;        /* Set WORM bit */
}

static void update_vpd_b1(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xb1)];

	memcpy(vpd_pg->data, p, vpd_pg->sz);
}

static void update_vpd_b2(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xb2)];

	memcpy(vpd_pg->data, p, vpd_pg->sz);
}

static void update_vpd_c0(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xc0)];

	memcpy(&vpd_pg->data[20], p, strlen(p));
}

static void update_vpd_c1(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xc1)];

	memcpy(vpd_pg->data, p, vpd_pg->sz);
}

#define VPD_83_SZ 52
#define VPD_B0_SZ 4
#define VPD_B1_SZ SCSI_SN_LEN
#define VPD_B2_SZ 8
#define VPD_C0_SZ 0x28

#define MALLOC_SZ 512
static int init_lu(struct lu_phy_attr *lu, int minor, struct vtl_ctl *ctl)
{

	struct vpd **lu_vpd = lu->lu_vpd;
	uint8_t worm = 1;	/* Supports WORM */
	int pg;
	uint8_t local_TapeAlert[8] =
			{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	char *config=MHVTL_CONFIG_PATH"/device.conf";
	FILE *conf;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	int indx, n = 0;
	struct vtl_ctl tmpctl;
	int found = 0;

	conf = fopen(config , "r");
	if (!conf) {
		syslog(LOG_DAEMON|LOG_ERR, "Can not open config file %s : %m",
								config);
		perror("Can not open config file");
		exit(1);
	}
	s = malloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = malloc(MALLOC_SZ);
	if (!b) {
		perror("Could not allocate memory");
		exit(1);
	}

	/* While read in a line */
	while (fgets(b, MALLOC_SZ, conf) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		if (strlen(b) == 1)	/* Reset drive number of blank line */
			indx = 0xff;
		if (sscanf(b, "Drive: %d CHANNEL: %d TARGET: %d LUN: %d",
					&indx, &tmpctl.channel,
					&tmpctl.id, &tmpctl.lun)) {
			MHVTL_DBG(2, "Found Drive %d, looking for %d",
							indx, minor);
			if (indx == minor) {
				found = 1;
				memcpy(ctl, &tmpctl, sizeof(tmpctl));
			}
		}
		if (indx == minor) {
			unsigned int c, d, e, f, g, h, j, k;
			int i;

			if (sscanf(b, " Unit serial number: %s", s))
				sprintf(lu->lu_serial_no, "%-10s", s);
			if (sscanf(b, " Product identification: %s", s))
				sprintf(lu->product_id, "%-16s", s);
			if (sscanf(b, " Product revision level: %s", s))
				sprintf(lu->product_rev, "%-4s", s);
			if (sscanf(b, " Vendor identification: %s", s))
				sprintf(lu->vendor_id, "%-8s", s);
			if (sscanf(b, " Density : %s", s)) {
				lu->supported_density[n] =
					(uint8_t)strtol(s, NULL, 16);
				MHVTL_DBG(2, "Supported density: 0x%x (%d)",
						lu->supported_density[n],
						lu->supported_density[n]);
				n++;
			}
			i = sscanf(b,
				" NAA: %x:%x:%x:%x:%x:%x:%x:%x",
					&c, &d, &e, &f, &g, &h, &j, &k);
			if (i == 8) {
				if (lu->naa)
					free(lu->naa);
				lu->naa = malloc(24);
				if (lu->naa)
					sprintf((char *)lu->naa,
				"%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
					c, d, e, f, g, h, j, k);
				MHVTL_DBG(2, "Setting NAA: to %s", lu->naa);
			} else if (i > 0) {
				MHVTL_DBG(1, "NAA: Incorrect num params %s", b);
			}
		}
	}
	fclose(conf);
	free(b);
	free(s);

	lu->ptype = TYPE_TAPE;
	lu->removable = 1;	/* Supports removable media */

	lu->version_desc[0] = 0x0300;	/* SPC-3 No version claimed */
	lu->version_desc[1] = 0x0960;	/* iSCSI */
	lu->version_desc[2] = 0x0200;	/* SSC */

	/* Unit Serial Number */
	pg = 0x80 & 0x7f;
	lu_vpd[pg] = alloc_vpd(strlen(lu->lu_serial_no));
	lu_vpd[pg]->vpd_update = update_vpd_80;
	lu_vpd[pg]->vpd_update(lu, lu->lu_serial_no);

	/* Device Identification */
	pg = 0x83 & 0x7f;
	lu_vpd[pg] = alloc_vpd(VPD_83_SZ);
	lu_vpd[pg]->vpd_update = update_vpd_83;
	lu_vpd[pg]->vpd_update(lu, NULL);

	/* Sequential Access device capabilities - Ref: 8.4.2 */
	pg = 0xb0 & 0x7f;
	lu_vpd[pg] = alloc_vpd(VPD_B0_SZ);
	lu_vpd[pg]->vpd_update = update_vpd_b0;
	lu_vpd[pg]->vpd_update(lu, &worm);

	/* Manufacture-assigned serial number - Ref: 8.4.3 */
	pg = 0xb1 & 0x7f;
	lu_vpd[pg] = alloc_vpd(VPD_B1_SZ);
	lu_vpd[pg]->vpd_update = update_vpd_b1;
	lu_vpd[pg]->vpd_update(lu, lu->lu_serial_no);

	/* TapeAlert supported flags - Ref: 8.4.4 */
	pg = 0xb2 & 0x7f;
	lu_vpd[pg] = alloc_vpd(VPD_B2_SZ);
	lu_vpd[pg]->vpd_update = update_vpd_b2;
	lu_vpd[pg]->vpd_update(lu, &local_TapeAlert);

	/* VPD page 0xC0 */
	pg = 0xc0 & 0x7f;
	lu_vpd[pg] = alloc_vpd(VPD_C0_SZ);
	lu_vpd[pg]->vpd_update = update_vpd_c0;
	lu_vpd[pg]->vpd_update(lu, "10-03-2008 19:38:00");

	/* VPD page 0xC1 */
	pg = 0xc1 & 0x7f;
	lu_vpd[pg] = alloc_vpd(strlen("Security"));
	lu_vpd[pg]->vpd_update = update_vpd_c1;
	lu_vpd[pg]->vpd_update(lu, "Security");

	return found;
}

static void process_cmd(int cdev, uint8_t *buf, struct vtl_header *vtl_cmd)
{
	struct vtl_ds dbuf;
	uint8_t *cdb;

	/* Get the SCSI cdb from vtl driver
	 * - Returns SCSI command S/No. */

	cdb = (uint8_t *)&vtl_cmd->cdb;

	/* Interpret the SCSI command & process
	-> Returns no. of bytes to send back to kernel
	 */
	memset(&dbuf, 0, sizeof(struct vtl_ds));
	dbuf.serialNo = vtl_cmd->serialNo;
	dbuf.data = buf;
	dbuf.sam_stat = sam_status;
	dbuf.sense_buf = &sense;

	processCommand(cdev, cdb, &dbuf);

	/* Complete SCSI cmd processing */
	completeSCSICommand(cdev, &dbuf);

	/* dbuf.sam_stat was zeroed in completeSCSICommand */
	sam_status = dbuf.sam_stat;
}

static void media_density_init(void)
{
	Drive_Native_Write_Density[drive_undefined] = 0x00;
	Drive_Native_Write_Density[drive_LTO1] = medium_density_code_lto1;
	Drive_Native_Write_Density[drive_LTO2] = medium_density_code_lto2;
	Drive_Native_Write_Density[drive_LTO3] = medium_density_code_lto3;
	Drive_Native_Write_Density[drive_LTO4] = medium_density_code_lto4;
	Drive_Native_Write_Density[drive_3592_J1A] = medium_density_code_j1a;
	Drive_Native_Write_Density[drive_3592_E05] = medium_density_code_e05;
	Drive_Native_Write_Density[drive_3592_E06] = medium_density_code_e06;
	Drive_Native_Write_Density[drive_AIT4] = medium_density_code_ait4;
	Drive_Native_Write_Density[drive_10K_A] = medium_density_code_10kA;
	Drive_Native_Write_Density[drive_10K_B] = medium_density_code_10kB;
	Drive_Native_Write_Density[drive_SDLT600] = medium_density_code_600;
	Drive_Native_Write_Density[drive_UNKNOWN] = 0x40;
}

int main(int argc, char *argv[])
{
	int cdev;
	int ret;
	int q_priority = 0;
	int exit_status = 0;
	long pollInterval = 50000L;
	uint8_t *buf;
	pid_t child_cleanup, pid, sid;

	char *progname = argv[0];

	char *dataFile = MHVTL_HOME_PATH;
	char *name = "mhvtl";
	int minor = 0;
	struct passwd *pw;

	struct vtl_header vtl_cmd;
	struct vtl_header *cmd;
	struct vtl_ctl ctl;

	/* Output file pointer (data file) */
	int ofp = -1;

	/* Message Q */
	int	mlen, r_qid;
	struct q_entry r_entry;

	if (sizeof(struct blk_header) != 512) {
		MHVTL_DBG(1, "Something wrong with blk_header data struct.\n"
			"Needs to be exactly 512 bytes in size. Currently: %d",
			(int)sizeof(struct blk_header));
		exit(1);
	}
		
	if (argc < 2) {
		usage(argv[0]);
		printf("  -- Not enough parameters --\n");
		exit(1);
	}

	while (argc > 0) {
		if (argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				debug++;
				verbose = 9;	// If debug, make verbose...
				break;
			case 'f':
				if (argc > 1) {
					printf("argv: -f %s\n", argv[1]);
					dataFile = argv[1];
				} else {
					usage(progname);
					puts("    More args needed for -f\n");
					exit(1);
				}
				break;
			case 'v':
				verbose++;
				break;
			case 'q':
				if (argc > 1)
					q_priority = atoi(argv[1]);
				break;
			default:
				usage(progname);
				printf("    Unknown option %c\n", argv[0][1]);
				exit(1);
				break;
			}
		}
		argv++;
		argc--;
	}

	if (q_priority <= 0 || q_priority > MAXPRIOR) {
		usage(progname);
		if (q_priority == 0)
			puts("    queue priority not specified\n");
		else
			printf("    queue prority out of range [1 - %d]\n",
						MAXPRIOR);
		exit(1);
	}
	minor = q_priority;	// Minor == Message Queue priority

	openlog(progname, LOG_PID, LOG_DAEMON|LOG_WARNING);
	syslog(LOG_DAEMON|LOG_INFO, "%s: version %s", progname, MHVTL_VERSION);
	if (verbose)
		printf("%s: version %s\n", progname, MHVTL_VERSION);

	/* Clear Sense arr */
	memset(sense, 0, sizeof(sense));

	/* Powered on / reset flag */
	reset = 1;

	init_mode_pages(sm);
	initTapeAlert(&TapeAlert);
	if (!init_lu(&lunit, minor, &ctl)) {
		printf("Can not find entry for '%d' in config file\n", minor);
		exit(1);
	}

	MHVTL_DBG(1, "starting...");

	/* Minor tweeks - setup WORM & SPIN/SPOUT customisations */
	config_lu(&lunit);

	/* Setup Media_Density */
	media_density_init();

	child_cleanup = add_lu(q_priority, &ctl);
	if (! child_cleanup) {
		MHVTL_DBG(1, "Could not create logical unit");
		exit(1);
	}

	pw = getpwnam(USR);	/* Find UID for user 'vtl' */
	if (!pw) {
		MHVTL_DBG(1, "Unable to find user: %s", USR);
		exit(1);
	}

	if (setgid(pw->pw_gid)) {
		perror("Unable to change gid");
		exit (1);
	}
	if (setuid(pw->pw_uid)) {
		perror("Unable to change uid");
		exit (1);
	}

	/* Initialise message queue as necessary */
	if ((r_qid = init_queue()) == -1) {
		printf("Could not initialise message queue\n");
		exit(1);
	}

	if (check_for_running_daemons(minor)) {
		syslog(LOG_DAEMON|LOG_INFO, "%s: version %s, found another running daemon... exiting\n", progname, MHVTL_VERSION);
		exit(2);
	}

	MHVTL_DBG(2, "Running as %s, uid: %d", pw->pw_name, getuid());

	if ((cdev = chrdev_open(name, minor)) == -1) {
		syslog(LOG_DAEMON|LOG_ERR,
				"Could not open /dev/%s%d: %m", name, minor);
		fflush(NULL);
		exit(1);
	}

	MHVTL_DBG(1, "Size of buffer is %d", bufsize);
	buf = (uint8_t *)malloc(bufsize);
	if (NULL == buf) {
		perror("Problems allocating memory");
		exit(1);
	}

	currentMedia = (char *)malloc(sizeof(dataFile) + 1024);
	if (NULL == currentMedia) {
		perror("Could not allocate memory -- exiting");
		exit(1);
	}

	strncpy(currentMedia, dataFile, sizeof(dataFile));

	/* If debug, don't fork/run in background */
	if (!debug) {
		switch(pid = fork()) {
		case 0:         /* Child */
			break;
		case -1:
			perror("Failed to fork daemon");
			exit (-1);
			break;
		default:
			MHVTL_DBG(1, "vtltape process PID is %d", (int)pid);
			exit (0);
			break;
		}

		umask(0);	/* Change the file mode mask */

		sid = setsid();
		if (sid < 0)
			exit(-1);

		if ((chdir(MHVTL_HOME_PATH)) < 0) {
			perror("Unable to change directory ");
			exit(-1);
		}

		close(STDIN_FILENO);
		close(STDERR_FILENO);
	}

	oom_adjust();

	for (;;) {
		/* Check for anything in the messages Q */
		mlen = msgrcv(r_qid, &r_entry, MAXOBN, q_priority, IPC_NOWAIT);
		if (mlen > 0) {
			r_entry.mtext[mlen] = '\0';
			exit_status =
				processMessageQ(r_entry.mtext, &sam_status);
		} else if (mlen < 0) {
			if ((r_qid = init_queue()) == -1) {
				syslog(LOG_DAEMON|LOG_ERR,
					"Can not open message queue: %m");
			}
		}
		if (exit_status)	/* Received a 'exit' message */
			goto exit;
		ret = ioctl(cdev, VTL_POLL_AND_GET_HEADER, &vtl_cmd);
		if (ret < 0) {
			MHVTL_DBG(2,
				"ioctl(VTL_POLL_AND_GET_HEADER: %d : %m", ret);
		} else {
			if (debug)
				printf("ioctl(VX_TAPE_POLL_STATUS) "
					"returned: %d, interval: %ld\n",
						ret, pollInterval);
			if (child_cleanup) {
				if (waitpid(child_cleanup, NULL, WNOHANG)) {
					MHVTL_DBG(1,
						"Cleaning up after child %d",
							child_cleanup);
					child_cleanup = 0;
				}
			}
			fflush(NULL);
			switch(ret) {
			case VTL_QUEUE_CMD:	/* A cdb to process */
				cmd = malloc(sizeof(struct vtl_header));
				if (!cmd) {
					syslog(LOG_DAEMON|LOG_ERR,
						"Out of memory");
					pollInterval = 1000000;
				} else {
					memcpy(cmd, &vtl_cmd, sizeof(vtl_cmd));
					process_cmd(cdev, buf, cmd);
					/* Something to do, reduce poll time */
					pollInterval = 10;
					free(cmd);
				}
				break;

			case VTL_IDLE:
				/* While nothing to do, increase
				 * time we sleep before polling again.
				 */
				if (pollInterval < 1000000)
					pollInterval += 1000;

				usleep(pollInterval);
				break;

			default:
				syslog(LOG_DAEMON|LOG_NOTICE,
					"ioctl(0x%x) returned %d\n",
						VTL_POLL_AND_GET_HEADER, ret);
				sleep(1);
				break;
			}
		}
	}

exit:
	ioctl(cdev, VTL_REMOVE_LU, &ctl);
	close(cdev);
	close(ofp);
	free(buf);
	exit(0);
}

