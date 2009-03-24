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
 * 	Since ability to define device serial number, increased ver from
 * 	0.12 to 0.14
 *
 */
static const char * Version = "$Id: vtltape.c 2008-11-26 19:35:01 markh Exp $";

#define _XOPEN_SOURCE 500

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <syslog.h>
#include <inttypes.h>
#include <pwd.h>
#include "vtl_common.h"
#include "scsi.h"
#include "q.h"
#include "vx.h"
#include "vtltape.h"
#include "vxshared.h"

/* Variables for simple, single initiator, SCSI Reservation system */
static int I_am_SPC_2_Reserved;
static unsigned int SPR_Reservation_Generation;
static unsigned int SPR_Reservation_Type;
static unsigned int SPR_Reservation_Key_LSW;
static unsigned int SPR_Reservation_Key_MSW;

#include <zlib.h>

/* Suppress Incorrect Length Indicator */
#define SILI  0x2
/* Fixed block format */
#define FIXED 0x1

/* Sense Data format bits & pieces */
/* Incorrect Length Indicator */
#define VALID 0x80
#define FILEMARK 0x80
#define EOM 0x40
#define ILI 0x20

/*
 * Define DEBUG to 0 and recompile to remove most debug messages.
 * or DEFINE TO 1 to make the -d (debug operation) mode more chatty
 */

#define DEBUG 0

#if DEBUG

#define DEB(a) a
#define DEBC(a) if (debug) { a ; }

#else

#define DEB(a)
#define DEBC(a)

#endif

#ifndef Solaris
  /* I'm sure there must be a header where lseek64() is defined */
  loff_t lseek64(int, loff_t, int);
//  int open64(char *, int);
  int ioctl(int, int, void *);
#endif

int send_msg(char *, int);
void logSCSICommand(uint8_t *);

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

static u64 bytesRead = 0;
static u64 bytesWritten = 0;
static unsigned char mediaSerialNo[34];	// Currently mounted media S/No.

uint8_t sense[SENSE_BUF_SIZE]; /* Request sense buffer */

struct lu_phy_attr lu;

static struct MAM mam;

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

DEB(
static void print_header(struct blk_header *h) {

	/* It should only be called in 'debug' mode */
	if (!debug)
		return;

	printf("Hdr:");
	switch(h->blk_type) {
	case B_UNCOMPRESS_DATA:
		printf(" Uncompressed data");
		break;
	case B_COMPRESSED_DATA:
		printf("   Compressed data");
		break;
	case B_FILEMARK:
		printf("          Filemark");
		break;
	case B_BOT:
		printf(" Beginning of Tape");
		break;
	case B_EOD:
		printf("       End of Data");
		break;
	case B_EOM_WARN:
		printf("End of Media - Early Warning");
		break;
	case B_EOM:
		printf("      End of Media");
		break;
	case B_NOOP:
		printf("      No Operation");
		break;
	default:
		printf("      Unknown type");
		break;
	}
	if (h->blk_type == B_BOT)
	     printf("(%d), Tape capacity %d, prev %" PRId64 ", curr %" PRId64 ", next %" PRId64 "\n",
			h->blk_type,
			h->blk_size,
			h->prev_blk,
			h->curr_blk,
			h->next_blk);
	else
	     printf("(%d), sz %d, prev %" PRId64 ", cur %" PRId64 ", next %" PRId64 "\n",
			h->blk_type,
			h->blk_size,
			h->prev_blk,
			h->curr_blk,
			h->next_blk);
}
// END of DEBC macro - compile this section out !
)

static void
mk_sense_short_block(u32 requested, u32 processed, uint8_t *sense_valid)
{
	int difference = (int)requested - (int)processed;

	/* No sense, ILI bit set */
	mkSenseBuf(ILI, NO_ADDITIONAL_SENSE, sense_valid);

	if (debug)
		printf("    Short block sense: Short %d bytes\n", difference);
	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO,
		"Short block read: Requested: %d, Read: %d, short by %d bytes",
					requested, processed, difference);

	/* Now fill in the datablock with number of bytes not read/written */
	sense[3] = difference >> 24;
	sense[4] = difference >> 16;
	sense[5] = difference >> 8;
	sense[6] = difference;
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
		if (verbose)
		    syslog(LOG_DAEMON|LOG_WARNING,
			"End of data detected while forward SPACEing!!");
		return -1;
	}

	if (c_pos.next_blk != lseek64(datafile, c_pos.next_blk, SEEK_SET)) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sam_stat);
		syslog(LOG_DAEMON|LOG_WARNING,
					"Unable to seek to next block header");
		return -1;
	}
	nread = read_header(&c_pos, sizeof(c_pos), sam_stat);
	if (nread == 0) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sam_stat);
		syslog(LOG_DAEMON|LOG_WARNING,
					"Unable to read next block header");
		return -1;
	}
	if (nread == -1) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sam_stat);
		syslog(LOG_DAEMON|LOG_WARNING,
					"Unable to read next block header: %m");
		return -1;
	}
	DEBC( print_header(&c_pos);) ;
	// Position to start of header (rewind over header)
	if (c_pos.curr_blk != position_to_curr_header(sam_stat)) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sam_stat);
		if (verbose)
			syslog(LOG_DAEMON|LOG_WARNING,
			"%s: Error positing in datafile, byte offset: %" PRId64,
				__func__, c_pos.curr_blk);
		return -1;
	}
	return 0;
}

static int skip_to_prev_header(uint8_t *sam_stat)
{
	loff_t nread;

	// Position to previous header
	if (debug) {
		printf("skip_to_prev_header()\n");
		printf("Positioning to c_pos.prev_blk: %" PRId64 "\n", c_pos.prev_blk);
	}
	if (c_pos.prev_blk != lseek64(datafile, c_pos.prev_blk, SEEK_SET)) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		if (verbose)
			syslog(LOG_DAEMON|LOG_WARNING,
					"%s: Error position in datafile !!",
					__func__);
		return -1;
	}
	// Read in header
	if (debug)
		printf("Reading in header: %d bytes\n", (int)sizeof(c_pos));

	nread = read_header(&c_pos, sizeof(c_pos), sam_stat);
	if (nread == 0) {
		if (verbose)
		    syslog(LOG_DAEMON|LOG_WARNING, "%s",
			"Error reading datafile while reverse SPACEing");
		return -1;
	}
	DEBC( print_header(&c_pos); ) ;
	if (c_pos.blk_type == B_BOT) {
		if (debug)
			printf("Found Beginning Of Tape, "
				"Skipping to next header..\n");
		skip_to_next_header(sam_stat);
		DEBC( print_header(&c_pos);) ;
		mkSenseBuf(MEDIUM_ERROR, E_BOM, sam_stat);
		syslog(LOG_DAEMON|LOG_WARNING, "Found BOT!!");
		return -1;
	}

	// Position to start of header (rewind over header)
	if (c_pos.curr_blk != position_to_curr_header(sam_stat)) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR,sam_stat);
		syslog(LOG_DAEMON|LOG_WARNING,
				"%s: Error position in datafile !!",
				__func__);
		return -1;
	}
	DEBC(
	 printf("Now rewinding over header just read in: curr_position: %" PRId64 "\n",
					c_pos.curr_blk);
		print_header(&c_pos);
	) ; // END debug macro
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

	h.blk_type = type;	// Header type
	h.blk_size = size;	// Size of uncompressed data
	h.disk_blk_size = comp_size; // For when I do compression..
	h.curr_blk = lseek64(datafile, 0, SEEK_CUR); // Update current position
	h.blk_number = c_pos.blk_number;

	// If we are writing a new EOD marker,
	//  - then set next pointer to itself
	// else
	//  - Set pointer to next header (header size + size of data)
	if (type == B_EOD)
		h.next_blk = h.curr_blk;
	else
		h.next_blk = h.curr_blk + comp_size + sizeof(h);

	if (h.curr_blk == c_pos.curr_blk) {
	// If current pos == last header read in we are about to overwrite the
	// current header block
		h.prev_blk = c_pos.prev_blk;
		h.blk_number = c_pos.blk_number;
	} else if (h.curr_blk == c_pos.next_blk) {
	// New header block at end of data file..
		h.prev_blk = c_pos.curr_blk;
		h.blk_number = c_pos.blk_number + 1;
	} else {
		DEBC(
	       printf("Position error trying to write header, curr_pos: %" PRId64 "\n",
								h.curr_blk);
			print_header(&c_pos);
		) ; // END debug macro
		syslog(LOG_DAEMON|LOG_ERR,
		"Fatal: Position error blk No: %" PRId64 ", Pos: %" PRId64
		", Exp: %" PRId64,
				h.blk_number, h.curr_blk, c_pos.curr_blk);
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sam_stat);
		return 0;
	}

	nwrite = write(datafile, &h, sizeof(h));

	/*
	 * If write was successful, update c_pos with this header block.
	 */
	if (nwrite <= 0) {
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
		if (debug) {
			if (nwrite < 0) perror("header write failed");
			printf("Error writing %d header, pos: %" PRId64 "\n",
							type, h.curr_blk);
		} else {
			syslog(LOG_DAEMON|LOG_ERR,
				"Write failure, pos: %" PRId64 ": %m", h.curr_blk);
		}
		return nwrite;
	}
	memcpy(&c_pos, &h, sizeof(h)); // Update where we think we are..

	return nwrite;
}

static int mkEODHeader(uint8_t *sam_stat)
{
	loff_t nwrite;

	nwrite = mkNewHeader(B_EOD, sizeof(c_pos), sizeof(c_pos), sam_stat);
	if (MediaType == MEDIA_TYPE_WORM)
		OK_to_write = 1;

	/* If we have just written a END OF DATA marker,
	 * rewind to just before it. */
	// Position to start of header (rewind over header)
	if (c_pos.curr_blk != position_to_curr_header(sam_stat)) {
		mkSenseBuf(MEDIUM_ERROR,E_SEQUENTIAL_POSITION_ERR,sam_stat);
		syslog(LOG_DAEMON|LOG_ERR, "Failed to write EOD header");
		if (verbose)
			syslog(LOG_DAEMON|LOG_WARNING,
				"%s: Error position in datafile!!",
				__func__);
		return -1;
	}

	return nwrite;
}

/*
 *
 */

static int skip_prev_filemark(uint8_t *sam_stat)
{

	DEBC(
		printf("  skip_prev_filemark()\n");
		print_header(&c_pos);
	) ; // END debug macro

	if (c_pos.blk_type == B_FILEMARK)
		c_pos.blk_type = B_NOOP;
	DEBC( print_header(&c_pos);) ;
	while(c_pos.blk_type != B_FILEMARK) {
		if (c_pos.blk_type == B_BOT) {
			mkSenseBuf(NO_SENSE, E_BOM, sam_stat);
			if (verbose)
				syslog(LOG_DAEMON|LOG_WARNING, "%s",
						"Found Beginning of tape");
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

	DEBC(
		printf("  skip_next_filemark()\n");
		print_header(&c_pos);
	) ;
	// While blk header is NOT a filemark, keep skipping to next header
	while(c_pos.blk_type != B_FILEMARK) {
		// END-OF-DATA -> Treat this as an error - return..
		if (c_pos.blk_type == B_EOD) {
			mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
			if (verbose)
				syslog(LOG_DAEMON|LOG_WARNING, "%s",
							"Found end of media");
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
setSeqAccessDevice(struct seqAccessDevice * seqAccessDevice, u64 flg) {

	seqAccessDevice->TapeAlert = htonll(flg);
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
		syslog(LOG_DAEMON|LOG_INFO, "Can not write - Cleaning cart");
		OK_to_write = 0;
		break;
	case MEDIA_TYPE_WORM:
		/* If we are not at end of data for a write
		 * and media is defined as WORM, fail...
		 */
		if (c_pos.blk_type == B_EOD)
			OK_to_write = 1;	// OK to append to end of 'tape'
		if (!OK_to_write) {
			syslog(LOG_DAEMON|LOG_ERR,
					"Attempt to overwrite WORM data");
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

	if (verbose > 2) {
		syslog(LOG_DAEMON|LOG_INFO, "checkRestrictions() returning %s",
				(OK_to_write) ? "Writable" : "Non-writable");
	}
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
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "WORM mode page set");
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
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "WORM mode page cleared");
	}
}


/*
 * Report density of media loaded.

FIXME:
 -  Need to grab info from MAM !!!
 -  Need to check 'media' bit buf[1] & 0x1 for currently loaded or drive support
 -  Need to return full list of media support.
    e.g. AIT-4 should return AIT1, AIT2 AIT3 & AIT4 data.
 */

#define REPORT_DENSITY_LEN 56
static int resp_report_density(uint8_t media, struct vtl_ds *dbuf_p)
{
	u16 *sp;
	u32 *lp;
	uint8_t *buf = dbuf_p->data;
	int len = dbuf_p->sz;

	// Zero out buf
	memset(buf, 0, len);

	sp = (u16 *)&buf[0];
	*sp = htons(REPORT_DENSITY_LEN - 4);

	buf[2] = 0;	// Reserved
	buf[3] = 0;	// Reserved

	buf[4] = 0x40;	// Primary Density Code
	buf[5] = 0x40;	// Secondary Density Code
	buf[6] = 0xa0;	// WRTOK = 1, DUP = 0, DEFLT = 1: 1010 0000b
	buf[7] = 0;


	// Bits per mm (only 24bits in len MS Byte should be 0).
	lp = (u32 *)&buf[8];
	*lp = ntohl(mam.media_info.bits_per_mm);

	// Media Width (tenths of mm)
	sp = (u16 *)&buf[12];
	*sp = htons(mam.MediumWidth);

	// Tracks
	sp = (u16 *)&buf[14];
	*sp = htons(mam.MediumLength);

	// Capacity
	lp = (u32 *)&buf[16];
	*lp = htonl(mam.max_capacity);

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
	 */
	if (!checkRestrictions(sam_stat))
		return 0;

	if (cdb[0] == MODE_SELECT) {
		block_descriptor_sz = buf[3];
		if (block_descriptor_sz)
			bdb = &buf[4];
	} else {
		block_descriptor_sz = (buf[6] << 8) +
					buf[7];
		long_lba = buf[4] & 1;
		if (block_descriptor_sz)
			bdb = &buf[8];
	}

	if (bdb) {
		if (!long_lba) {
			memcpy(blockDescriptorBlock, bdb, block_descriptor_sz);
		} else
			syslog(LOG_DAEMON|LOG_INFO, "%s: Warning can not "
				"handle long descriptor block (long_lba bit)",
					__func__);
	}

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
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"LOG SENSE: Sending supported pages");
		sp = (u16 *)&supported_pages[2];
		*sp = htons(sizeof(supported_pages) - 4);
		b = memcpy(b, supported_pages, sizeof(supported_pages));
		retval = sizeof(supported_pages);
		break;
	case WRITE_ERROR_COUNTER:	/* Write error page */
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
						"LOG SENSE: Write error page");
		pg_write_err_counter.pcode_head.len =
				htons((sizeof(pg_write_err_counter)) -
				sizeof(pg_write_err_counter.pcode_head));
		b = memcpy(b, &pg_write_err_counter,
					sizeof(pg_write_err_counter));
		retval += sizeof(pg_write_err_counter);
		break;
	case READ_ERROR_COUNTER:	/* Read error page */
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
						"LOG SENSE: Read error page");
		pg_read_err_counter.pcode_head.len =
				htons((sizeof(pg_read_err_counter)) -
				sizeof(pg_read_err_counter.pcode_head));
		b = memcpy(b, &pg_read_err_counter,
					sizeof(pg_read_err_counter));
		retval += sizeof(pg_read_err_counter);
		break;
	case SEQUENTIAL_ACCESS_DEVICE:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"LOG SENSE: Sequential Access Device Log page");
		seqAccessDevice.pcode_head.len = htons(sizeof(seqAccessDevice) -
					sizeof(seqAccessDevice.pcode_head));
		b = memcpy(b, &seqAccessDevice, sizeof(seqAccessDevice));
		retval += sizeof(seqAccessDevice);
		break;
	case TEMPERATURE_PAGE:	/* Temperature page */
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
						"LOG SENSE: Temperature page");
		Temperature_pg.pcode_head.len = htons(sizeof(Temperature_pg) -
					sizeof(Temperature_pg.pcode_head));
		Temperature_pg.temperature = htons(35);
		b = memcpy(b, &Temperature_pg, sizeof(Temperature_pg));
		retval += sizeof(Temperature_pg);
		break;
	case TAPE_ALERT:	/* TapeAlert page */
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,"LOG SENSE: TapeAlert page");
		if (verbose > 1)
			syslog(LOG_DAEMON|LOG_INFO,
				" Returning TapeAlert flags: 0x%" PRIx64,
					ntohll(seqAccessDevice.TapeAlert));

		TapeAlert.pcode_head.len = htons(sizeof(TapeAlert) -
					sizeof(TapeAlert.pcode_head));
		b = memcpy(b, &TapeAlert, sizeof(TapeAlert));
		retval += sizeof(TapeAlert);
		/* Clear flags after value read. */
		if (alloc_len > 4) {
			setTapeAlert(&TapeAlert, 0);
			setSeqAccessDevice(&seqAccessDevice, 0);
		} else
			syslog(LOG_DAEMON|LOG_INFO,
				"TapeAlert : Alloc len short -"
				" Not clearing TapeAlert flags.\n");
		break;
	case TAPE_USAGE:	/* Tape Usage Log */
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"LOG SENSE: Tape Usage page");
		TapeUsage.pcode_head.len = htons(sizeof(TapeUsage) -
					sizeof(TapeUsage.pcode_head));
		b = memcpy(b, &TapeUsage, sizeof(TapeUsage));
		retval += sizeof(TapeUsage);
		break;
	case TAPE_CAPACITY:	/* Tape Capacity page */
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"LOG SENSE: Tape Capacity page");
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
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"LOG SENSE: Data Compression page");
		DataCompression.pcode_head.len = htons(sizeof(DataCompression) -
					sizeof(DataCompression.pcode_head));
		b = memcpy(b, &DataCompression, sizeof(DataCompression));
		retval += sizeof(DataCompression);
		break;
	default:
		syslog(LOG_DAEMON|LOG_ERR,
			"LOG SENSE: Unknown code: 0x%x", cdb[2] & 0x3f);
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
	u16 *sp;
	u16 attribute;
	u32 *lp;
	u32 alloc_len;
	int ret_val = 0;
	int byte_index = 4;
	int index, found_attribute;

	sp = (u16 *)&cdb[8];
	attribute = ntohs(*sp);
	lp = (u32 *)&cdb[10];
	alloc_len = ntohl(*lp);
	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO,
			"Read Attribute: 0x%x, allocation len: %d",
							attribute, alloc_len);

	memset(buf, 0, alloc_len);	// Clear memory

	if (cdb[1] == 0) {
		/* Attribute Values */
		for (index = found_attribute = 0; MAM_Attributes[index].length; index++) {
			if (attribute == MAM_Attributes[index].attribute) {
				found_attribute = 1;
			}
			if (found_attribute) {
				/* calculate available data length */
				ret_val += MAM_Attributes[index].length + 5;
				if (ret_val < alloc_len) {
					/* add it to output */
					buf[byte_index++] = MAM_Attributes[index].attribute >> 8;
					buf[byte_index++] = MAM_Attributes[index].attribute;
					buf[byte_index++] = (MAM_Attributes[index].read_only << 7) |
					                    MAM_Attributes[index].format;
					buf[byte_index++] = MAM_Attributes[index].length >> 8;
					buf[byte_index++] = MAM_Attributes[index].length;
					memcpy(&buf[byte_index], MAM_Attributes[index].value,
					       MAM_Attributes[index].length);
					byte_index += MAM_Attributes[index].length;
				}
			}
		}
		if (!found_attribute) {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
			return 0;
		}
	} else {
		/* Attribute List */
		for (index = found_attribute = 0; MAM_Attributes[index].length; index++) {
			/* calculate available data length */
			ret_val += 2;
			if (ret_val < alloc_len) {
				/* add it to output */
				buf[byte_index++] = MAM_Attributes[index].attribute >> 8;
				buf[byte_index++] = MAM_Attributes[index].attribute;
			}
		}
	}

	buf[0] = ret_val >> 24;
	buf[1] = ret_val >> 16;
	buf[2] = ret_val >> 8;
	buf[3] = ret_val;

	if (ret_val > alloc_len)
		ret_val = alloc_len;

	return ret_val;
}

/*
 * Process PERSITENT RESERVE IN scsi command
 * Returns bytes to return if OK
 *         or -1 on failure.
 */
static int resp_pri(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	u16 *sp;
	u16 alloc_len;
	u32 *lp;
	u16 SA;
	uint8_t *buf = dbuf_p->data;
	uint8_t *sam_stat = &dbuf_p->sam_stat;

	SA = cdb[1] & 0x1f;

	sp = (u16 *)&cdb[7];
	alloc_len = ntohs(*sp);

	memset(buf, 0, alloc_len);	// Clear memory

	switch(SA) {
	case 0: /* READ KEYS */
		lp = (u32 *)&buf[0];
		*lp = htonl(SPR_Reservation_Generation);
		if (!SPR_Reservation_Key_MSW && !SPR_Reservation_Key_LSW)
			return(8);
		buf[7] = 8;
		lp = (u32 *)&buf[8];
		*lp = htonl(SPR_Reservation_Key_MSW);
		lp = (u32 *)&buf[12];
		*lp = htonl(SPR_Reservation_Key_LSW);
		return(16);
	case 1: /* READ RESERVATON */
		lp = (u32 *)&buf[0];
		*lp = htonl(SPR_Reservation_Generation);
		if (!SPR_Reservation_Type)
			return(8);
		buf[7] = 16;
		lp = (u32 *)&buf[8];
		*lp = htonl(SPR_Reservation_Key_MSW);
		lp = (u32 *)&buf[12];
		*lp = htonl(SPR_Reservation_Key_LSW);
		buf[21] = SPR_Reservation_Type;
		return(24);
	case 2: /* REPORT CAPABILITIES */
		buf[1] = 8;
		buf[2] = 0x10;
		buf[3] = 0x80;
		buf[4] = 0x08;
		return(8);
	case 3: /* READ FULL STATUS */
	default:
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		break;
	}
	return(0);
}

/*
 * Process PERSITENT RESERVE OUT scsi command
 * Returns 0 if OK
 *         or -1 on failure.
 */
static int resp_pro(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	u32 *lp;
	u32 RK_LSW, RK_MSW, SARK_LSW, SARK_MSW;
	u16 SA, TYPE;
	uint8_t *sam_stat = &dbuf_p->sam_stat;
	uint8_t *buf = dbuf_p->data;

	if (dbuf_p->sz != 24) {
		mkSenseBuf(ILLEGAL_REQUEST, E_PARAMETER_LIST_LENGTH_ERR, sam_stat);
		return(-1);
	}

	SA = cdb[1] & 0x1f;
	TYPE = cdb[2] & 0x0f;

	lp = (u32 *)&buf[0];
	RK_MSW = ntohl(*lp);
	lp = (u32 *)&buf[4];
	RK_LSW = ntohl(*lp);

	lp = (u32 *)&buf[8];
	SARK_MSW = ntohl(*lp);
	lp = (u32 *)&buf[12];
	SARK_LSW = ntohl(*lp);

	if (verbose)
		syslog(LOG_DAEMON|LOG_WARNING,
			"Key 0x%.8x %.8x SA Key 0x%.8x %.8x "
			"Service Action 0x%.2x Type 0x%.1x",
			RK_MSW, RK_LSW, SARK_MSW, SARK_LSW, SA, TYPE);

	switch(SA) {
	case 0: /* REGISTER */
		if (!SPR_Reservation_Key_MSW && !SPR_Reservation_Key_LSW) {
			if(!RK_LSW && !RK_MSW) {
				SPR_Reservation_Key_MSW = SARK_MSW;
				SPR_Reservation_Key_LSW = SARK_LSW;
				SPR_Reservation_Generation++;
			} else {
				*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
			}
		} else {
			if((RK_MSW == SPR_Reservation_Key_MSW) &&
					(RK_LSW == SPR_Reservation_Key_LSW)) {
				if (!SARK_MSW && !SARK_LSW) {
					SPR_Reservation_Key_MSW = 0;
					SPR_Reservation_Key_LSW = 0;
					SPR_Reservation_Type = 0;
				} else {
					SPR_Reservation_Key_MSW = SARK_MSW;
					SPR_Reservation_Key_LSW = SARK_LSW;
				}
				SPR_Reservation_Generation++;
			} else {
				*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
			}
		}
		break;
	case 1: /* RESERVE */
		if (!SPR_Reservation_Key_MSW && !SPR_Reservation_Key_LSW) {
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		} else {
			if((RK_MSW == SPR_Reservation_Key_MSW) && (RK_LSW == SPR_Reservation_Key_LSW)) {
				if(TYPE != 3) {
					*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
				} else {
					SPR_Reservation_Type = TYPE;
				}
			} else {
				*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
			}
		}
		break;
	case 2: /* RELEASE */
		if (!SPR_Reservation_Key_MSW && !SPR_Reservation_Key_LSW) {
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		} else {
			if((RK_MSW == SPR_Reservation_Key_MSW) && (RK_LSW == SPR_Reservation_Key_LSW)) {
				if(TYPE != 3) {
					*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
				} else {
					SPR_Reservation_Type = 0;
				}
			} else {
				*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
			}
		}
		break;
	case 3: /* CLEAR */
		if (!SPR_Reservation_Key_MSW && !SPR_Reservation_Key_LSW) {
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		} else {
			if((RK_MSW == SPR_Reservation_Key_MSW) && (RK_LSW == SPR_Reservation_Key_LSW)) {
				SPR_Reservation_Key_MSW = 0;
				SPR_Reservation_Key_LSW = 0;
				SPR_Reservation_Type = 0;
				SPR_Reservation_Generation++;
			} else {
				*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
			}
		}
		break;
	case 4: /* PREEMT */
	case 5: /* PREEMPT AND ABORT */
		/* this is pretty weird, in that we can only have a single key registered, so preempt is pretty simplified */
		if ((!SPR_Reservation_Key_MSW && !SPR_Reservation_Key_LSW) &&
		    (!RK_MSW && !RK_LSW) &&
		    (!SARK_MSW && !SARK_LSW)) {
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		} else {
			if (!SPR_Reservation_Type) {
				if((SARK_MSW == SPR_Reservation_Key_MSW) && (SARK_LSW == SPR_Reservation_Key_LSW)) {
					SPR_Reservation_Key_MSW = 0;
					SPR_Reservation_Key_LSW = 0;
					SPR_Reservation_Generation++;
				}
			} else {
				if((SARK_MSW == SPR_Reservation_Key_MSW) && (SARK_LSW == SPR_Reservation_Key_LSW)) {
					SPR_Reservation_Key_MSW = RK_MSW;
					SPR_Reservation_Key_LSW = RK_LSW;
					SPR_Reservation_Type = TYPE;
					SPR_Reservation_Generation++;
				}
			}
		}

		break;
	case 6: /* REGISTER AND IGNORE EXISTING KEY */
		if (!SPR_Reservation_Key_MSW && !SPR_Reservation_Key_LSW) {
			SPR_Reservation_Key_MSW = SARK_MSW;
			SPR_Reservation_Key_LSW = SARK_LSW;
		} else {
			if (!SARK_MSW && !SARK_LSW) {
				SPR_Reservation_Key_MSW = 0;
				SPR_Reservation_Key_LSW = 0;
				SPR_Reservation_Type = 0;
			} else {
				SPR_Reservation_Key_MSW = SARK_MSW;
				SPR_Reservation_Key_LSW = SARK_LSW;
			}
		}
		SPR_Reservation_Generation++;
		break;
	case 7: /* REGISTER AND MOVE */
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		break;
	default:
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		break;
	}
	return(0);
}

/*
 * Process WRITE ATTRIBUTE scsi command
 * Returns 0 if OK
 *         or 1 if MAM needs to be written.
 *         or -1 on failure.
 */
static int resp_write_attribute(uint8_t *cdb, struct vtl_ds *dbuf_p, struct MAM *mam)
{
	u32 *lp;
	u32 alloc_len;
	int byte_index;
	int index, attribute, attribute_length, found_attribute = 0;
	struct MAM mam_backup;
	uint8_t *buf = dbuf_p->data;
	uint8_t *sam_stat = &dbuf_p->sam_stat;

	lp = (u32 *)&cdb[10];
	alloc_len = ntohl(*lp);

	memcpy(&mam_backup, &mam, sizeof(struct MAM));
	for (byte_index = 4; byte_index < alloc_len; ) {
		attribute = ((u16)buf[byte_index++] << 8);
		attribute += buf[byte_index++];
		for (index = found_attribute = 0; MAM_Attributes[index].length; index++) {
			if (attribute == MAM_Attributes[index].attribute) {
				found_attribute = 1;
				byte_index += 1;
				attribute_length = ((u16)buf[byte_index++] << 8);
				attribute_length += buf[byte_index++];
				if ((attribute = 0x408) &&
					(attribute_length == 1) &&
						(buf[byte_index] == 0x80)) {
					/* set media to worm */
					syslog(LOG_DAEMON|LOG_WARNING,
						"Converting media to WORM");
					mam->MediumType = MEDIA_TYPE_WORM;
				} else {
					memcpy(MAM_Attributes[index].value,
						&buf[byte_index],
						MAM_Attributes[index].length);
				}
				byte_index += attribute_length;
				break;
			} else {
				found_attribute = 0;
			}
		}
		if (!found_attribute) {
			memcpy(&mam, &mam_backup, sizeof(mam));
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
static int readBlock(int cdev, uint8_t *buf, uint8_t *sam_stat, u32 request_sz)
{
	loff_t nread = 0;
	uint8_t	*comp_buf;
	uLongf uncompress_sz;
	uLongf comp_buf_sz;
	int z;
	u8 information[4];

	if (verbose > 1)
		syslog(LOG_DAEMON|LOG_WARNING, "Request to read: %d bytes",
							request_sz);

	DEBC( print_header(&c_pos);) ;

	/* check for a zero length read */
	if (request_sz == 0) {
		/* This is not an error, and doesn't change the tape position */
		return 0;
	}

	/* Read in block of data */
	switch(c_pos.blk_type) {
	case B_FILEMARK:
		if (verbose)
			syslog(LOG_DAEMON|LOG_ERR,
				"Expected to find hdr type: %d, found: %d",
					B_UNCOMPRESS_DATA, c_pos.blk_type);
		skip_to_next_header(sam_stat);
		mk_sense_short_block(request_sz, 0, sam_stat);
		information[0] = sense[3];
		information[1] = sense[4];
		information[2] = sense[5];
		information[3] = sense[6];
		mkSenseBuf(FILEMARK, E_MARK, sam_stat);
		sense[3] = information[0];
		sense[4] = information[1];
		sense[5] = information[2];
		sense[6] = information[3];
		return nread;
		break;
	case B_EOD:
		mk_sense_short_block(request_sz, 0, sam_stat);
		information[0] = sense[3];
		information[1] = sense[4];
		information[2] = sense[5];
		information[3] = sense[6];
		mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
		sense[3] = information[0];
		sense[4] = information[1];
		sense[5] = information[2];
		sense[6] = information[3];
		return nread;
		break;
	case B_BOT:
		skip_to_next_header(sam_stat);
		// Re-exec function.
		return readBlock(cdev, buf, sam_stat, request_sz);
		break;
	case B_COMPRESSED_DATA:
		// If we are positioned at beginning of header, read it in.
		if (c_pos.curr_blk == lseek64(datafile, 0, SEEK_CUR)) {
			nread = read_header(&c_pos, sizeof(c_pos), sam_stat);
			if (nread == 0) {	// Error
				syslog(LOG_DAEMON|LOG_ERR,
					"Unable to read header: %m");
				mkSenseBuf(MEDIUM_ERROR,E_UNRECOVERED_READ,
								sam_stat);
				return 0;
			}
		}
		comp_buf_sz = c_pos.disk_blk_size + 80;
		comp_buf = (uint8_t *)malloc(comp_buf_sz);
		uncompress_sz = bufsize;
		if (NULL == comp_buf) {
			syslog(LOG_DAEMON|LOG_WARNING,
				"Unable to alloc %ld bytes", comp_buf_sz);
			mkSenseBuf(MEDIUM_ERROR,E_UNRECOVERED_READ,sam_stat);
			return 0;
		}
		nread = read(datafile, comp_buf, c_pos.disk_blk_size);
		if (nread == 0) {	// End of data - no more to read
			if (verbose)
				syslog(LOG_DAEMON|LOG_WARNING, "%s",
				"End of data detected when reading from file");
			mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
			free(comp_buf);
			return 0;
		} else if (nread < 0) {	// Error
			syslog(LOG_DAEMON|LOG_ERR, "Read Error: %m");
			mkSenseBuf(MEDIUM_ERROR,E_UNRECOVERED_READ,sam_stat);
			free(comp_buf);
			return 0;
		}
		z = uncompress((uint8_t *)buf,&uncompress_sz, comp_buf, nread);
		if (z != Z_OK) {
			mkSenseBuf(MEDIUM_ERROR,E_DECOMPRESSION_CRC,sam_stat);
			if (z == Z_MEM_ERROR)
				syslog(LOG_DAEMON|LOG_ERR,
					"Not enough memory to decompress data");
			else if (z == Z_BUF_ERROR)
				syslog(LOG_DAEMON|LOG_ERR,
					"Not enough memory in destination buf"
					" to decompress data");
			else if (z == Z_DATA_ERROR)
				syslog(LOG_DAEMON|LOG_ERR,
					"Input data corrput or incomplete");
			free(comp_buf);
			return 0;
		}
		nread = uncompress_sz;
		// requested block and actual block size different
		if (uncompress_sz != request_sz) {
			syslog(LOG_DAEMON|LOG_WARNING,
			"Short block read %ld %d", uncompress_sz, request_sz);
			mk_sense_short_block(request_sz, uncompress_sz,
								sam_stat);
		}
		free(comp_buf);
		break;
	case B_UNCOMPRESS_DATA:
		// If we are positioned at beginning of header, read it in.
		if (c_pos.curr_blk == lseek64(datafile, 0, SEEK_CUR)) {
			nread = read_header(&c_pos, sizeof(c_pos), sam_stat);
			if (nread == 0) {	// Error
				syslog(LOG_DAEMON|LOG_ERR, "%m");
				mkSenseBuf(MEDIUM_ERROR,E_UNRECOVERED_READ,
								sam_stat);
				return 0;
			}
		}
		nread = read(datafile, buf, c_pos.disk_blk_size);
		if (nread == 0) {	// End of data - no more to read
			if (verbose)
				syslog(LOG_DAEMON|LOG_WARNING, "%s",
				"End of data detected when reading from file");
			mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
			return nread;
		} else if (nread < 0) {	// Error
			syslog(LOG_DAEMON|LOG_ERR, "%m");
			mkSenseBuf(MEDIUM_ERROR, E_UNRECOVERED_READ, sam_stat);
			return 0;
		} else if (nread != request_sz) {
			// requested block and actual block size different
			mk_sense_short_block(request_sz, nread, sam_stat);
		}
		break;
	default:
		if (verbose)
		  syslog(LOG_DAEMON|LOG_ERR,
			"Unknown blk header at offset %" PRId64 " - Abort read cmd",
							c_pos.curr_blk);
		mkSenseBuf(MEDIUM_ERROR, E_UNRECOVERED_READ, sam_stat);
		return 0;
		break;
	}
	// Now read in subsequent header
	skip_to_next_header(sam_stat);

	return nread;
}


/*
 * Return number of bytes written to 'file'
 */
static int writeBlock(uint8_t *src_buf, u32 src_sz,  uint8_t *sam_stat)
{
	loff_t	nwrite = 0;
	Bytef	* dest_buf = src_buf;
	uLong	dest_len = src_sz;
	uLong	src_len = src_sz;

	if (compressionFactor) {
/*
		src_len = compressBound(src_sz);

		dest_buf = malloc(src_len);
		if (NULL == dest_buf) {
			mkSenseBuf(MEDIUM_ERROR,E_COMPRESSION_CHECK, sam_stat);
			syslog(LOG_DAEMON|LOG_ERR, "malloc failed: %m");
			return 0;
		}
		compress2(dest_buf, &dest_len, src_buf, src_len,
							compressionFactor);
		syslog(LOG_DAEMON|LOG_INFO,
			"Compression: Orig %ld, after comp %ld",
						src_len, dest_len);

		// Create & write block header..
		nwrite =
		   mkNewHeader(B_COMPRESSED_DATA, src_len, dest_len, sam_stat);
*/
	} else {
		nwrite =
		   mkNewHeader(B_UNCOMPRESS_DATA, src_len, dest_len, sam_stat);

	}
	if (nwrite <= 0) {
		if (debug)
			printf("Failed to write header\n");
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
	} else {	// now write the block of data..
		nwrite = write(datafile, dest_buf, dest_len);
		if (nwrite <= 0) {
			syslog(LOG_DAEMON|LOG_ERR, "%s %m", "Write failed:");
			if (debug) {
				if (nwrite < 0)
					perror("writeBlk failed");
				printf("Failed to write %ld bytes\n", dest_len);
			}
			mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
		} else if (nwrite != dest_len) {
			DEBC(
			syslog(LOG_DAEMON|LOG_ERR, "Did not write all data");
			) ;
			mk_sense_short_block(src_len, nwrite, sam_stat);
		}
	}
	if (c_pos.curr_blk >= max_tape_capacity) {
		if (debug)
			syslog(LOG_DAEMON|LOG_INFO, "End of Medium - Setting EOM flag");
		mkSenseBuf(NO_SENSE|EOM_FLAG, NO_ADDITIONAL_SENSE, sam_stat);
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
		syslog(LOG_DAEMON|LOG_ERR,
			"%s: can't seek to beginning of file: %m\n", __func__);
		val = 1;
	}

	/*
	 * Read header..
	 * If this is not the BOT header we are in trouble
	 */
	retval = read(datafile, &c_pos, sizeof(c_pos));
	if (retval != sizeof(c_pos)) {
		syslog(LOG_DAEMON|LOG_ERR, "%s: can't read header: %m\n",
			__func__);
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
		DEBC( print_header(&c_pos);) ;
		return 2;
	}
	nread = read(datafile, &mam, sizeof(struct MAM));
	if (nread != sizeof(struct MAM)) {
		syslog(LOG_DAEMON|LOG_INFO, "read MAM short - corrupt");
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		memset(&mam, 0, sizeof(struct MAM));
		DEBC( print_header(&c_pos);) ;
		return 2;
	}

	if (mam.tape_fmt_version != TAPE_FMT_VERSION) {
		syslog(LOG_DAEMON|LOG_INFO, "Incorrect media format");
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sam_stat);
		DEBC( print_header(&c_pos);) ;
		return 2;
	}

	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO, "MAM: media S/No. %s",
							mam.MediumSerialNumber);

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

	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO,
				" media is %s",
				(OK_to_write) ? "writable" : "not writable");

	return 1;
}

/*
 * Space over (to) x filemarks. Setmarks not supported as yet.
 */
static void resp_space(u32 count, int code, uint8_t *sam_stat)
{

	switch(code) {
	// Space 'count' blocks
	case 0:
		if (verbose)
			syslog(LOG_DAEMON|LOG_NOTICE,
				"SCSI space 0x%02x blocks **", count);
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
		if (verbose)
			syslog(LOG_DAEMON|LOG_NOTICE,
				"SCSI space 0x%02x filemarks **\n", count);
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
		if (verbose)
			syslog(LOG_DAEMON|LOG_NOTICE, "%s",
				"SCSI space to end-of-data **");
		while(c_pos.blk_type != B_EOD)
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
	syslog(LOG_DAEMON|LOG_INFO, "%s: lookup %d\n", __func__, field);
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
static int resp_sp_in_page_0(uint8_t *buf, uint16_t sps, uint32_t alloc_len, uint8_t *sam_stat)
{
	int ret = 0;

	syslog(LOG_DAEMON|LOG_INFO, "%s: %s\n",
			 __func__, lookup_sp_specific(sps));

	switch(sps) {
	case SUPPORTED_SECURITY_PROTOCOL_LIST:
		memset(buf, 0, alloc_len);
		buf[6] = 0;	/* list length (MSB) */
		buf[7] = 2;	/* list length (LSB) */
		buf[8] = SUPPORTED_SECURITY_PROTOCOL_LIST;
		buf[9] = CERTIFICATE_DATA;
		ret = 10;
		break;

	case CERTIFICATE_DATA:
		memset(buf, 0, alloc_len);
		buf[2] = (sizeof(certificate) >> 8) & 0xff;
		buf[3] = sizeof(certificate) & 0xff;
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
static int resp_sp_in_page_20(uint8_t *buf, uint16_t sps, uint32_t alloc_len, uint8_t *sam_stat)
{
	int ret = 0;

	syslog(LOG_DAEMON|LOG_INFO, "%s: %s\n",
			 __func__, lookup_sp_specific(sps));

	memset(buf, 0, alloc_len);
	switch(sps) {
	case ENCR_IN_SUPPORT_PAGES:
		buf[0] = (ENCR_IN_SUPPORT_PAGES >> 8) & 0xff;
		buf[1] = ENCR_IN_SUPPORT_PAGES & 0xff;
		buf[2] = 0;	/* list length (MSB) */
		buf[3] = 14;	/* list length (LSB) */
		buf[4] = (ENCR_IN_SUPPORT_PAGES >> 8) & 0xff;
		buf[5] = ENCR_IN_SUPPORT_PAGES && 0xff;
		buf[6] = (ENCR_OUT_SUPPORT_PAGES >> 8) & 0xff;
		buf[7] = ENCR_OUT_SUPPORT_PAGES & 0xff;
		buf[8] = (ENCR_CAPABILITIES >> 8) & 0xff;
		buf[9] = ENCR_CAPABILITIES & 0xff;
		buf[10] = (ENCR_KEY_FORMATS >> 8) & 0xff;
		buf[11] = ENCR_KEY_FORMATS & 0xff;
		buf[12] = (ENCR_KEY_MGT_CAPABILITIES >> 8) & 0xff;
		buf[13] = ENCR_KEY_MGT_CAPABILITIES & 0xff;
		buf[14] = (ENCR_DATA_ENCR_STATUS >> 8) & 0xff;
		buf[15] = ENCR_DATA_ENCR_STATUS & 0xff;
		buf[16] = (ENCR_NEXT_BLK_ENCR_STATUS >> 8) & 0xff;
		buf[17] = ENCR_NEXT_BLK_ENCR_STATUS & 0xff;
		ret = 18;
		break;

	case ENCR_OUT_SUPPORT_PAGES:
		buf[0] = (ENCR_OUT_SUPPORT_PAGES >> 8) & 0XFF;
		buf[1] = ENCR_OUT_SUPPORT_PAGES & 0xff;
		buf[2] = 0;	/* List length (MSB) */
		buf[3] = 2;	/* List length (LSB) */
		buf[4] = (ENCR_SET_DATA_ENCRYPTION >> 8) & 0xff;
		buf[5] = ENCR_SET_DATA_ENCRYPTION & 0xff;
		ret = 6;
		break;

	case ENCR_CAPABILITIES:
		buf[0] = (ENCR_CAPABILITIES >> 8) & 0xff;
		buf[1] = ENCR_CAPABILITIES & 0xff;
		buf[2] = 0;	/* List length (MSB) */
		buf[3] = 42;	/* List length (LSB) */

		buf[20] = 1;	/* Algorithm index */
		buf[21] = 0;	/* Reserved */
		buf[22] = 0;	/* Descriptor length (0x14) */
		buf[23] = 0x14;	/* Descriptor length (0x14) */
		buf[24] = 0x3a;	/* See table 202 of IBM Ultrium doco */
		buf[25] = 0x30;	/* See table 202 of IBM Ultrium doco */
		buf[26] = 0;	/* Max unauthenticated key data */
		buf[27] = 0x20;	/* Max unauthenticated key data */
		buf[28] = 0;	/* Max authenticated key data */
		buf[29] = 0x0c;	/* Max authenticated key data */
		buf[30] = 0;	/* Key size */
		buf[31] = 0x20;	/* Key size */
		/* buf 12 - 19 reserved */
		buf[40] = 0;	/* Encryption Algorithm Id */
		buf[41] = 0;	/* Encryption Algorithm Id */
		buf[42] = 0;	/* Encryption Algorithm Id */
		buf[43] = 0x14;	/* Encryption Algorithm Id */
		ret = 48;
		break;

	case ENCR_KEY_FORMATS:
		buf[0] = (ENCR_KEY_FORMATS >> 8) & 0xff;
		buf[1] = ENCR_KEY_FORMATS & 0xff;
		buf[2] = 0;	/* List length (MSB) */
		buf[3] = 2;	/* List length (MSB) */
		buf[4] = 0;	/* Plain text */
		buf[5] = 0;	/* Plain text */
		ret = 6;
		break;

	case ENCR_KEY_MGT_CAPABILITIES:
		buf[0] = (ENCR_KEY_MGT_CAPABILITIES >> 8) & 0xff;
		buf[1] = ENCR_KEY_MGT_CAPABILITIES & 0xff;
		buf[2] = 0;	/* List length (MSB) */
		buf[3] = 0x0c;	/* List length (MSB) */
		buf[4] = 1;	/* LOCK_C */
		buf[5] = 7;	/* CKOD_C, DKOPR_C, CKORL_C */
		buf[6] = 0;	/* Reserved */
		buf[7] = 7;	/* AITN_C, LOCAL_C, PUBLIC_C */
		/* buf 8 - 15 reserved */
		ret = 16;
		break;

	case ENCR_DATA_ENCR_STATUS:
		buf[0] = (ENCR_DATA_ENCR_STATUS >> 8) & 0xff;
		buf[1] = ENCR_DATA_ENCR_STATUS & 0xff;
		buf[2] = 0;	/* List length (MSB) */
		buf[3] = 2;	/* List length (MSB) */
		ret = 6;
		break;
	case ENCR_NEXT_BLK_ENCR_STATUS:
		buf[0] = (ENCR_NEXT_BLK_ENCR_STATUS >> 8) & 0xff;
		buf[1] = ENCR_NEXT_BLK_ENCR_STATUS & 0xff;
		buf[2] = 0;	/* List length (MSB) */
		buf[3] = 2;	/* List length (MSB) */
		ret = 6;
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
	uint16_t sps = ((cdb[2] & 0xff) << 8) + (cdb[3] & 0xff);
	uint32_t alloc_len = ((cdb[6] & 0xff) << 24) +
				((cdb[7] & 0xff) << 16) +
				((cdb[8] & 0xff) << 8) +
				(cdb[9] & 0xff);
	uint8_t inc_512 = (cdb[4] & 0x80) ? 1 : 0;

	if (inc_512)
		alloc_len = alloc_len * 512;

	switch(cdb[1]) {
	case SECURITY_PROTOCOL_INFORMATION:
		return resp_sp_in_page_0(buf, sps, alloc_len, sam_stat);
		break;

	case TAPE_DATA_ENCRYPTION:
		return resp_sp_in_page_20(buf, sps, alloc_len, sam_stat);
		break;
	}

	syslog(LOG_DAEMON|LOG_INFO,
			"%s: security protocol 0x%04x unknown\n",
				__func__, cdb[1]);

	mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
	return 0;
}

static int resp_spout(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	uint16_t sps = ((cdb[2] | 0xff) << 8) + (cdb[3] | 0xff);
	uint8_t inc_512 = (cdb[4] & 0x80) ? 1 : 0;
	uint8_t *sam_stat = &dbuf_p->sam_stat;

	if (cdb[1] != TAPE_DATA_ENCRYPTION) {
		syslog(LOG_DAEMON|LOG_INFO,
			"%s: security protocol 0x%02x unknown\n",
				__func__, cdb[1]);
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return 0;
	}
	syslog(LOG_DAEMON|LOG_INFO,
			"%s: Tape Data Encryption, %s, "
			" alloc len: 0x%02x, inc_512: %s\n",
				__func__, lookup_sp_specific(sps),
				dbuf_p->sz, (inc_512) ? "Set" : "Unset");
	return 0;
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
 * Update MAM contents with current counters
 */
static void updateMAM(struct MAM *mamp, uint8_t *sam_stat, int loadCount)
{
	u64 bw;		// Bytes Written
	u64 br;		// Bytes Read
	u64 load;	// load count

	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO, "updateMAM(%d)", loadCount);

	// Update bytes written this load.
	mamp->WrittenInLastLoad = ntohll(bytesWritten);
	mamp->ReadInLastLoad = ntohll(bytesRead);

	// Update total bytes read/written
	bw = htonll(mamp->WrittenInMediumLife);
	bw += bytesWritten;
	mamp->WrittenInMediumLife = ntohll(bw);

	br = htonll(mamp->ReadInMediumLife);
	br += bytesRead;
	mamp->ReadInMediumLife = ntohll(br);

	// Update load count
	if (loadCount) {
		load = htonll(mamp->LoadCount);
		load++;
		mamp->LoadCount = ntohll(load);
	}

	rewriteMAM(mamp, sam_stat);
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
 * Return:
 *	total number of bytes to send back to vtl device
 */
static int processCommand(int cdev, uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	u32 block_size = 0;
	u32 count;
	u32 ret = 0;
	u32 retval = 0;
	u32 *lp;
	u16 *sp;
	int k;
	int code;
	int service_action;
	struct mode *smp = sm;
	uint8_t *sam_stat = &dbuf_p->sam_stat;
	loff_t nread;
	char str[256];


	/* Limited subset of commands don't need to check for power-on reset */
	switch (cdb[0]) {
	case REPORT_LUN:
	case REQUEST_SENSE:
	case MODE_SELECT:
	case INQUIRY:
		break;
	default:
		if (check_reset(sam_stat))
			return ret;
	}

	// Now process SCSI command.
	switch (cdb[0]) {
	case ALLOW_MEDIUM_REMOVAL:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Allow removal (%ld) **",
						(long)dbuf_p->serialNo);
		resp_allow_prevent_removal(cdb, sam_stat);
		break;

	case INQUIRY:
		ret += spc_inquiry(cdb, dbuf_p, &lu);
		break;

	case FORMAT_UNIT:	// That's FORMAT_MEDIUM for an SSC device...
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Format Medium (%ld) **",
						(long)dbuf_p->serialNo);

		if (!checkRestrictions(sam_stat))
			break;

		if (c_pos.blk_number != 0) {
			syslog(LOG_DAEMON|LOG_INFO, "Not at beginning **");
			mkSenseBuf(ILLEGAL_REQUEST,E_POSITION_PAST_BOM,
								 sam_stat);
			break;
		}
		mkEODHeader(sam_stat);
		break;

	case SEEK_10:	// Thats LOCATE_BLOCK for SSC devices...
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"Fast Block Locate (%ld) **",
						(long)dbuf_p->serialNo);
		lp = (u32 *)&cdb[3];
		count = ntohl(*lp);

		/* If we want to seek closer to beginning of file than
		 * we currently are, rewind and seek from there
		 */
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"Current blk: %" PRId64 ", seek: %d",
					c_pos.blk_number, count);
		if (((u32)(count - c_pos.blk_number) > count) &&
						(count < c_pos.blk_number)) {
			resp_rewind(sam_stat);
		}
		if (MediaType == MEDIA_TYPE_WORM)
			OK_to_write = 0;
		while(c_pos.blk_number != count) {
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
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "LOG SELECT (%ld) **",
						(long)dbuf_p->serialNo);
		resp_log_select(cdb, sam_stat);
		break;

	case LOG_SENSE:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "LOG SENSE (%ld) **",
						(long)dbuf_p->serialNo);
		dbuf_p->sz = (cdb[7] << 8) | cdb[8];
		k = resp_log_sense(cdb, dbuf_p);
		ret += (k < dbuf_p->sz) ? k : dbuf_p->sz;
		break;

	case MODE_SELECT:
	case MODE_SELECT_10:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "MODE SELECT (%ld) **",
						(long)dbuf_p->serialNo);
		dbuf_p->sz = (MODE_SELECT == cdb[0]) ? cdb[4] :
						((cdb[7] << 8) | cdb[8]);
		if (cdb[1] & 0x10) { /* Page Format: 1 - SPC, 0 - vendor uniq */
		}
		ret += resp_mode_select(cdev, cdb, dbuf_p);
		break;

	case MODE_SENSE:
	case MODE_SENSE_10:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "MODE SENSE (%ld) **",
						(long)dbuf_p->serialNo);
		ret += resp_mode_sense(cdb, dbuf_p->data, smp, MediaWriteProtect, sam_stat);
		break;

	case READ_10:
		syslog(LOG_DAEMON|LOG_ERR,
				"READ_10 op code not currently supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case READ_12:
		syslog(LOG_DAEMON|LOG_ERR,
				"READ_12 op code not currently supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case READ_16:
		syslog(LOG_DAEMON|LOG_ERR,
				"READ_16 op code not currently supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case READ_6:
		block_size =	(cdb[2] << 16) +
				(cdb[3] << 8) +
				 cdb[4];
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"Read_6 (%ld) : %d bytes **",
						(long)dbuf_p->serialNo,
						block_size);
		/* If both FIXED & SILI bits set, invalid combo.. */
		if ((cdb[1] & (SILI | FIXED)) == (SILI | FIXED)) {
			syslog(LOG_DAEMON|LOG_WARNING,
					"Fixed block read not supported");
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sam_stat);
			reset = 0;
			break;
		}
		/* This driver does not support fixed blocks at the moment */
		if (cdb[1] & FIXED) {
			syslog(LOG_DAEMON|LOG_WARNING,
					"\"Fixed block read\" not supported");
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sam_stat);
			reset = 0;
			break;
		}
		if (tapeLoaded == TAPE_LOADED) {
			if (MediaType == MEDIA_TYPE_CLEAN) {
				mkSenseBuf(NOT_READY, E_CLEANING_CART_INSTALLED,
								sam_stat);
				break;
			}
			retval = readBlock(cdev, dbuf_p->data, sam_stat, block_size);
			/* adjust for a read that asks for fewer bytes than available */
			if (retval > block_size)
				retval = block_size;
			bytesRead += retval;
			pg_read_err_counter.bytesProcessed = bytesRead;
		} else if (tapeLoaded == TAPE_UNLOADED) {
			mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		} else
			mkSenseBuf(NOT_READY, E_MEDIUM_FORMAT_CORRUPT,sam_stat);

		ret += retval;
		break;

	case READ_ATTRIBUTE:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Read Attribute (%ld) **",
						(long)dbuf_p->serialNo);
		if (tapeLoaded == TAPE_UNLOADED) {
			if (verbose)
				syslog(LOG_DAEMON|LOG_INFO,
					"Failed due to \"no media loaded\"");
			mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
			break;
		} else if (tapeLoaded > TAPE_LOADED) {
			if (verbose)
				syslog(LOG_DAEMON|LOG_INFO,
					"Failed due to \"media corrupt\"");
			mkSenseBuf(NOT_READY,E_MEDIUM_FORMAT_CORRUPT,sam_stat);
			break;
		}
		/* Only support Service Action - Attribute Values */
		if (cdb[1] < 2)
			ret += resp_read_attribute(cdb, dbuf_p->data, sam_stat);
		else {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sam_stat);
			break;
		}
		if (verbose > 1) {
			uint8_t *p = dbuf_p->data;
			syslog(LOG_DAEMON|LOG_INFO,
				" dump return data, length: %d", ret);
			for (k = 0; k < ret; k += 8) {
				syslog(LOG_DAEMON|LOG_INFO,
					" 0x%02x 0x%02x 0x%02x 0x%02x"
					" 0x%02x 0x%02x 0x%02x 0x%02x",
					p[k+0], p[k+1], p[k+2], p[k+3],
					p[k+4], p[k+5], p[k+6], p[k+7]);
			}
		}
		break;

	case READ_BLOCK_LIMITS:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"Read block limits (%ld) **",
						(long)dbuf_p->serialNo);
		if (tapeLoaded == TAPE_LOADED)
			ret += resp_read_block_limits(dbuf_p, bufsize);
		else if (tapeLoaded == TAPE_UNLOADED)
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sam_stat);
		else
			mkSenseBuf(NOT_READY,E_MEDIUM_FORMAT_CORRUPT,sam_stat);
		break;

	case READ_MEDIA_SERIAL_NUMBER:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"Read Medium Serial No. (%ld) **",
						(long)dbuf_p->serialNo);
		if (tapeLoaded == TAPE_UNLOADED)
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sam_stat);
		else if (tapeLoaded == TAPE_LOADED) {
			ret += resp_read_media_serial(mediaSerialNo, dbuf_p->data,
								sam_stat);
			if (verbose)
				syslog(LOG_DAEMON|LOG_INFO, "   %d", ret);
		} else
			mkSenseBuf(NOT_READY,E_MEDIUM_FORMAT_CORRUPT,sam_stat);
		break;

	case READ_POSITION:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Read Position (%ld) **",
						(long)dbuf_p->serialNo);
		service_action = cdb[1] & 0x1f;
/* service_action == 0 or 1 -> Returns 20 bytes of data (short) */
		if ((service_action == 0) || (service_action == 1)) {
			ret += resp_read_position(c_pos.blk_number,
							dbuf_p->data, sam_stat);
		} else {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sam_stat);
		}
		break;

	case RELEASE:
	case RELEASE_10:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Release (%ld) **",
						(long)dbuf_p->serialNo);
		if (!SPR_Reservation_Type &&
			(SPR_Reservation_Key_MSW || SPR_Reservation_Key_LSW))
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		I_am_SPC_2_Reserved = 0;
		break;

	case REPORT_DENSITY:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Report Density (%ld) **",
						(long)dbuf_p->serialNo);
		dbuf_p->sz = (cdb[7] << 8) | cdb[8];
		ret += resp_report_density((cdb[1] & 0x01), dbuf_p);
		break;

	case REPORT_LUN:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Report LUNs (%ld) **",
						(long)dbuf_p->serialNo);
		lp = (u32 *)&cdb[6];
		if (*lp < 16) {	// Minimum allocation length is 16 bytes.
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sam_stat);
			break;
		}
		report_luns.size = htonl(sizeof(report_luns) - 8);
		resp_report_lun(&report_luns, dbuf_p->data, sam_stat);
		break;

	case REQUEST_SENSE:
		if (verbose) {
			syslog(LOG_DAEMON|LOG_INFO,
			"Request Sense (%ld) : key/ASC/ASCQ "
				"[0x%02x 0x%02x 0x%02x]"
				" Filemark: %s, EOM: %s, ILI: %s",
					(long)dbuf_p->serialNo,
					sense[2] & 0x0f, sense[12], sense[13],
					(sense[2] & FILEMARK) ? "yes" : "no",
					(sense[2] & EOM) ? "yes" : "no",
					(sense[2] & ILI) ? "yes" : "no");
		}
		block_size =
			(cdb[4] < sizeof(sense)) ? cdb[4] : sizeof(sense);
		memcpy(dbuf_p->data, sense, block_size);
		/* Clear out the request sense flag */
		*sam_stat = 0;
		memset(sense, 0, sizeof(sense));
		ret += block_size;
		break;

	case RESERVE:
	case RESERVE_10:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Reserve (%ld) **",
						(long)dbuf_p->serialNo);
		if (!SPR_Reservation_Type && !SPR_Reservation_Key_MSW &&
						!SPR_Reservation_Key_LSW)
			I_am_SPC_2_Reserved = 1;
		if (!SPR_Reservation_Type &
			(SPR_Reservation_Key_MSW || SPR_Reservation_Key_LSW))
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		break;

	case REZERO_UNIT:	/* Rewind */
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Rewinding (%ld) **",
						(long)dbuf_p->serialNo);

		resp_rewind(sam_stat);
		sleep(1);
		break;

	case ERASE_6:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Erasing (%ld) **",
						(long)dbuf_p->serialNo);

		if (!checkRestrictions(sam_stat))
			break;

		/* Rewind and postition just after the first header. */
		resp_rewind(sam_stat);

		if (ftruncate(datafile, c_pos.curr_blk))
			syslog(LOG_DAEMON|LOG_ERR,
					"Failed to truncate datafile");

		/* Position to just before first header. */
		position_to_curr_header(sam_stat);

		/* Write EOD header */
		mkEODHeader(sam_stat);
		sleep(2);
		break;

	case SPACE:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"SPACE (%ld) **",
						(long)dbuf_p->serialNo);
		count = (cdb[2] << 16) +
			(cdb[3] << 8) +
			 cdb[4];

		code = cdb[1] & 0x07;

		/* Can return a '2s complement' to seek backwards */
		if (cdb[2] & 0x80)
			count += (0xff << 24);

		resp_space(count, code, sam_stat);
		break;

	case START_STOP:	/* Load/Unload cmd */
		if (cdb[4] && 0x1) {
			if (verbose)
				syslog(LOG_DAEMON|LOG_INFO,
					"Loading Tape (%ld) **",
						(long)dbuf_p->serialNo);
			tapeLoaded = resp_rewind(sam_stat);
		} else {
			mam.record_dirty = 0;
			// Don't update load count on unload -done at load time
			updateMAM(&mam, sam_stat, 0);
			close(datafile);
			tapeLoaded = TAPE_UNLOADED;
			OK_to_write = 0;
			clearWORM();
			if (verbose)
				syslog(LOG_DAEMON|LOG_INFO,
					"Unloading Tape (%ld)  **",
						(long)dbuf_p->serialNo);
			close(datafile);
		}
		break;

	case TEST_UNIT_READY:	// Return OK by default
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"Test Unit Ready (%ld) : %s",
				(long)dbuf_p->serialNo,
				(tapeLoaded == TAPE_UNLOADED) ? "No" : "Yes");
		if (tapeLoaded == TAPE_UNLOADED)
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sam_stat);
		if (tapeLoaded > TAPE_LOADED)
			mkSenseBuf(NOT_READY,E_MEDIUM_FORMAT_CORRUPT,sam_stat);

		break;

	case WRITE_10:
		syslog(LOG_DAEMON|LOG_ERR,
				"WRITE_10 op code not currently supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case WRITE_12:
		syslog(LOG_DAEMON|LOG_ERR,
				"WRITE_12 op code not currently supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case WRITE_16:
		syslog(LOG_DAEMON|LOG_ERR,
				"WRITE_16 op code not currently supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;

	case WRITE_6:
		block_size =	(cdb[2] << 16) +
				(cdb[3] << 8) +
				 cdb[4];
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"WRITE_6: %d bytes (%ld) **",
						block_size,
						(long)dbuf_p->serialNo);

		/* FIXME: should handle this test in a nicer way... */
		if (block_size > bufsize)
			syslog(LOG_DAEMON|LOG_ERR,
			"Fatal: bufsize %d, requested write of %d bytes",
							bufsize, block_size);

		/* FIXME: Add 'fixed' block write support here */
		if (cdb[1] & 0x1) { /* Fixed block write */
			syslog(LOG_DAEMON|LOG_ERR,
				"Fixed block write not currently supported");
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
					sam_stat);
			break;
		}

		// Attempt to read complete buffer size of data
		// from vx char device into buffer..
		dbuf_p->sz = block_size;
		nread = retrieve_CDB_data(cdev, dbuf_p);

		// NOTE: This needs to be performed AFTER we read
		//	 data block from kernel char driver.
		if (!checkRestrictions(sam_stat))
			break;

		if (OK_to_write) {
			retval = writeBlock(dbuf_p->data, block_size, sam_stat);
			bytesWritten += retval;
			pg_write_err_counter.bytesProcessed = bytesWritten;
		}

		break;

	case WRITE_ATTRIBUTE:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Write Attributes (%ld) **",
						(long)dbuf_p->serialNo);

		if (tapeLoaded == TAPE_UNLOADED) {
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sam_stat);
			break;
		} else if (tapeLoaded > TAPE_LOADED) {
			mkSenseBuf(NOT_READY,E_MEDIUM_FORMAT_CORRUPT, sam_stat);
			break;
		}

		lp = (u32 *)&cdb[10];
		// Read '*lp' bytes from char device...
		dbuf_p->sz = ntohl(*lp);
		block_size = retrieve_CDB_data(cdev, dbuf_p);
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"  --> Expected to read %d bytes"
				", read %d", dbuf_p->sz, block_size);
		if (resp_write_attribute(cdb, dbuf_p, &mam))
			rewriteMAM(&mam, sam_stat);
		break;

	case WRITE_FILEMARKS:
		block_size =	(cdb[2] << 16) +
				(cdb[3] << 8) +
				 cdb[4];
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"Write %d filemarks (%ld) **",
						block_size,
						(long)dbuf_p->serialNo);
		if (!checkRestrictions(sam_stat))
			break;

		while(block_size > 0) {
			block_size--;
			mkNewHeader(B_FILEMARK, 0, 0, sam_stat);
			mkEODHeader(sam_stat);
		}
		break;

	case RECEIVE_DIAGNOSTIC:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"Receive Diagnostic (%ld) **",
						(long)dbuf_p->serialNo);
		ret += ProcessReceiveDiagnostic(cdb, dbuf_p->data, sam_stat);
		break;

	case SEND_DIAGNOSTIC:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Send Diagnostic (%ld) **",
						(long)dbuf_p->serialNo);
		sp = (u16 *)&cdb[3];
		count = ntohs(*sp);
		if (count) {
			dbuf_p->sz = count;
			block_size = retrieve_CDB_data(cdev, dbuf_p);
			ProcessSendDiagnostic(cdb, 16, dbuf_p->data, block_size, sam_stat);
		}
		break;

	case PERSISTENT_RESERVE_IN:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"PERSISTENT RESERVE IN (%ld) **",
						(long)dbuf_p->serialNo);
		if (I_am_SPC_2_Reserved)
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		else {
			retval = resp_pri(cdb, dbuf_p);
			ret += retval;
		}
		break;

	case PERSISTENT_RESERVE_OUT:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"PERSISTENT RESERVE OUT (%ld) **",
						(long)dbuf_p->serialNo);
		if (I_am_SPC_2_Reserved)
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		else {
			dbuf_p->sz = cdb[5] << 24 |
					cdb[6] << 16 |
					cdb[7] << 8 |
					cdb[8];
			nread = retrieve_CDB_data(cdev, dbuf_p);
			resp_pro(cdb, dbuf_p);
		}
		break;

	case SECURITY_PROTOCOL_IN:
		syslog(LOG_DAEMON|LOG_INFO,
			"Security Protocol In - Under Development (%ld) **",
						(long)dbuf_p->serialNo);
		logSCSICommand(cdb);
		ret += resp_spin(cdb, dbuf_p->data, sam_stat);
		syslog(LOG_DAEMON|LOG_INFO,
				"Returning %d bytes\n", ret);
		if (verbose > 2)
			hex_dump(dbuf_p->data, ret);
		break;

	case SECURITY_PROTOCOL_OUT:
		syslog(LOG_DAEMON|LOG_INFO,
			"Security Protocol Out - Under Development (%ld) **",
						(long)dbuf_p->serialNo);
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		logSCSICommand(cdb);
		dbuf_p->sz = (cdb[6] << 24) | (cdb[7] << 16) |
				 (cdb[8] << 8) | cdb[9];
		/* Check for '512 increment' bit & multiply sz by 512 if set */
		dbuf_p->sz *= (cdb[4] & 0x80) ? 512 : 1;

		nread = retrieve_CDB_data(cdev, dbuf_p);
		ret += resp_spout(cdb, dbuf_p);
		syslog(LOG_DAEMON|LOG_INFO,
				"Returning %d bytes\n", ret);
		break;

	case A3_SA:
		resp_a3_service_action(cdb, sam_stat);
		break;

	case A4_SA:
		resp_a4_service_action(cdb, sam_stat);
		break;

	case ACCESS_CONTROL_IN:
		sprintf(str, "ACCESS CONTROL IN (%ld) **",
						(long)dbuf_p->serialNo);
		break;

	case ACCESS_CONTROL_OUT:
		sprintf(str, "ACCESS CONTROL OUT (%ld) **",
						(long)dbuf_p->serialNo);
		break;

	case EXTENDED_COPY:
		sprintf(str, "EXTENDED COPY (%ld) **",
						(long)dbuf_p->serialNo);
		break;

	default:
		sprintf(str, "Unknown OP code (%ld) **",
						(long)dbuf_p->serialNo);
		break;
	}
	dbuf_p->sz = ret;
	return 0;
}

/*
 * Attempt to load PCL - i.e. Open datafile and read in BOT header & MAM
 *
 * Return 0 on failure, 1 on success
 */

static int load_tape(char *PCL, uint8_t *sam_stat)
{
	loff_t nread;
	u64 fg = 0;	// TapeAlert flags

	bytesWritten = 0;	// Global - Bytes written this load
	bytesRead = 0;		// Global - Bytes rearead this load

	sprintf(currentMedia ,"%s/%s", HOME_PATH, PCL);
	if (debug)
		syslog(LOG_DAEMON|LOG_INFO, "Opening file/media %s", currentMedia);
	if ((datafile = open(currentMedia, O_RDWR|O_LARGEFILE)) == -1) {
		syslog(LOG_DAEMON|LOG_ERR, "%s: open file/media failed, %m", currentMedia);
		return 0;	// Unsuccessful load
	}

	// Now read in header information from just opened datafile
	nread = read(datafile, &c_pos, sizeof(c_pos));
	if (nread < 0) {
		syslog(LOG_DAEMON|LOG_ERR, "%s: %m",
			 "Error reading header in datafile, load failed");
		close(datafile);
		return 0;	// Unsuccessful load
	} else if (nread < sizeof(c_pos)) {	// Did not read anything...
		syslog(LOG_DAEMON|LOG_ERR, "%s: %m",
				 "Error: Not a tape format, load failed");
		/* TapeAlert - Unsupported format */
		fg = 0x800;
		close(datafile);
		goto unsuccessful;
	}
	if (c_pos.blk_type != B_BOT) {
		syslog(LOG_DAEMON|LOG_ERR,
			"Header type: %d not valid, load failed",
							c_pos.blk_type);
		/* TapeAlert - Unsupported format */
		fg = 0x800;
		close(datafile);
		goto unsuccessful;
	}
	// FIXME: Need better validation checking here !!
	 if (c_pos.next_blk != (sizeof(struct blk_header) + sizeof(struct MAM))) {
		syslog(LOG_DAEMON|LOG_ERR,
			"MAM size incorrect, load failed"
			" - Expected size: %d, size found: %" PRId64,
			(int)(sizeof(struct blk_header) + sizeof(struct MAM)),
				c_pos.next_blk);
		close(datafile);
		return 0;	// Unsuccessful load
	}
	nread = read(datafile, &mam, sizeof(mam));
	if (nread < 0) {
		mediaSerialNo[0] = '\0';
		syslog(LOG_DAEMON|LOG_ERR,
					"Can not read MAM from mounted media");
		return 0;	// Unsuccessful load
	}
	// Set TapeAlert flg 32h =>
	//	Lost Statics
	if (mam.record_dirty != 0) {
		fg = 0x02000000000000ull;
		syslog(LOG_DAEMON|LOG_WARNING, "Previous unload was not clean");
	}

	max_tape_capacity = (loff_t)c_pos.blk_size * (loff_t)1048576;
	if (debug)
		syslog(LOG_DAEMON|LOG_INFO, "Tape capacity: %" PRId64, max_tape_capacity);

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
		syslog(LOG_DAEMON|LOG_INFO,
				"Write Once Read Many (WORM) media loaded");
		break;

	case MEDIA_TYPE_CLEAN:
		fg = 0x400;
		syslog(LOG_DAEMON|LOG_WARNING, "Cleaning cart loaded");
		mkSenseBuf(UNIT_ATTENTION,E_CLEANING_CART_INSTALLED, sam_stat);
		break;
	default:
		mkSenseBuf(UNIT_ATTENTION,E_NOT_READY_TO_TRANSITION, sam_stat);
		break;
	}

	blockDescriptorBlock[0] = mam.MediumDensityCode;
	syslog(LOG_DAEMON|LOG_INFO, "Setting MediumDensityCode to 0x%02x\n",
			mam.MediumDensityCode);

	// Update TapeAlert flags
	setSeqAccessDevice(&seqAccessDevice, fg);
	setTapeAlert(&TapeAlert, fg);

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

	if (verbose)
		syslog(LOG_DAEMON|LOG_NOTICE, "Q msg : %s", mtext);

	/* Tape Load message from Library */
	if (!strncmp(mtext, "lload", 5)) {
		if ( ! inLibrary) {
			syslog(LOG_DAEMON|LOG_NOTICE,
						"lload & drive not in library");
			return (0);
		}

		if (tapeLoaded != TAPE_UNLOADED) {
			syslog(LOG_DAEMON|LOG_NOTICE, "Tape already mounted");
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
			syslog(LOG_DAEMON|LOG_WARNING,
					"Warn: Tape assigned to library");
		if (tapeLoaded == TAPE_LOADED) {
			syslog(LOG_DAEMON|LOG_NOTICE, "Tape already mounted");
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
			if (debug)
				syslog(LOG_DAEMON|LOG_INFO,
					"Library requested tape unload");
			close(datafile);
			break;
		default:
			if (debug)
				syslog(LOG_DAEMON|LOG_NOTICE, "Tape not mounted");
			tapeLoaded = TAPE_UNLOADED;
			break;
		}
	}

	if (!strncmp(mtext, "exit", 4)) {
		syslog(LOG_DAEMON|LOG_NOTICE, "Notice to exit : %s", mtext);
		return 1;
	}

	if (!strncmp(mtext, "Register", 8)) {
		inLibrary = 1;
		syslog(LOG_DAEMON|LOG_NOTICE,
				"Notice from Library controller : %s", mtext);
	}

	if (!strncmp(mtext, "verbose", 7)) {
		if (verbose)
			verbose--;
		else
			verbose = 3;
		syslog(LOG_DAEMON|LOG_NOTICE, "Verbose: %s (%d)",
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

int init_queue(void) {
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
static void init_mode_pages(struct mode *m) {
	struct mode *mp;
	u32	*lp;

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
		mp->pcodePointer[2] = 0xc0;	// Set Data Compression Enable
		mp->pcodePointer[3] = 0x80;	// Set Data Decompression Enable
		lp = (u32 *)&mp->pcodePointer[4];
		*lp = htonl(COMPRESSION_TYPE);	// Compression Algorithm
		lp++;
		*lp = htonl(COMPRESSION_TYPE);	// Decompression algorithm
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
	int num;

	d = vpd_pg->data;

	d[0] = 2;
	d[1] = 1;
	d[2] = 0;
	num = VENDOR_ID_LEN + PRODUCT_ID_LEN + 10;
	d[3] = num;

	memcpy(&d[4], &lu->vendor_id, VENDOR_ID_LEN);
	memcpy(&d[12], &lu->product_id, PRODUCT_ID_LEN);
	memcpy(&d[28], &lu->lu_serial_no, 10);

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
	uint8_t TapeAlert[8] =
			{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	char *config="/etc/vtl/device.conf";
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
	while( fgets(b, MALLOC_SZ, conf) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		if (strlen(b) == 1)	/* Reset drive number of blank line */
			indx = 0xff;
		if (sscanf(b, "Drive: %d CHANNEL: %d TARGET: %d LUN: %d",
					&indx, &tmpctl.channel,
					&tmpctl.id, &tmpctl.lun)) {
			if (verbose)
				syslog(LOG_DAEMON|LOG_INFO,
					"Found Drive %d, looking for %d\n",
							indx, minor);
			if (indx == minor) {
				found = 1;
				memcpy(ctl, &tmpctl, sizeof(tmpctl));
			}
		}
		if (indx == minor) {
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
				if (verbose)
					syslog(LOG_DAEMON|LOG_INFO,
					"Supported density: 0x%x (%d)\n",
						lu->supported_density[n],
						lu->supported_density[n]);
				if (debug)
					printf("Supported density: 0x%x (%d)\n",
						lu->supported_density[n],
						lu->supported_density[n]);
				n++;
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
	lu_vpd[pg]->vpd_update(lu, &TapeAlert);

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

	char *dataFile = HOME_PATH;
	char *name = "vtl";
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

	if (argc < 2) {
		usage(argv[0]);
		printf("  -- Not enough parameters --\n");
		exit(1);
	}

	while(argc > 0) {
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
	syslog(LOG_DAEMON|LOG_INFO, "%s: version %s", progname, Version);
	if (verbose)
		printf("%s: version %s\n", progname, Version);

	/* Clear Sense arr */
	memset(sense, 0, sizeof(sense));

	/* Powered on / reset flag */
	reset = 1;

	init_mode_pages(sm);
	initTapeAlert(&TapeAlert);
	if (!init_lu(&lu, minor, &ctl)) {
		printf("Can not find entry for '%d' in config file\n", minor);
		exit(1);
	}

	pw = getpwnam("vtl");	/* Find UID for user 'vtl' */
	if (!pw) {
		printf("Unable to find user: vtl\n");
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

	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO, "Running as %s, uid: %d\n",
					pw->pw_name, getuid());

	if ((cdev = chrdev_open(name, minor)) == -1) {
		syslog(LOG_DAEMON|LOG_ERR,
				"Could not open /dev/%s%d: %m", name, minor);
		fflush(NULL);
		exit(1);
	}

	syslog(LOG_DAEMON|LOG_INFO, "Size of buffer is %d", bufsize);
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
			if (verbose)
				printf("vtltape process PID is %d\n", (int)pid);
			exit (0);
			break;
		}

		umask(0);	/* Change the file mode mask */

		sid = setsid();
		if (sid < 0)
			exit(-1);

		if ((chdir("/opt/vtl")) < 0) {
			perror("Unable to change directory to /opt/vtl ");
			exit(-1);
		}

		close(STDIN_FILENO);
		close(STDERR_FILENO);
	}

	oom_adjust();

	child_cleanup = add_lu(q_priority, &ctl);
	if (! child_cleanup) {
		printf("Could not create logical unit\n");
		exit(1);
	}

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
			syslog(LOG_DAEMON|LOG_WARNING,
				"ioctl(VTL_POLL_AND_GET_HEADER: %d : %m", ret);
		} else {
			if (debug)
				printf("ioctl(VX_TAPE_POLL_STATUS) "
					"returned: %d, interval: %ld\n",
						ret, pollInterval);
			if (child_cleanup) {
				if (waitpid(child_cleanup, NULL, WNOHANG)) {
					if (verbose)
						syslog(LOG_DAEMON|LOG_INFO,
						"Cleaning up after child %d\n",
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

