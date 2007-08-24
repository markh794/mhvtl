/*
 * This daemon is the SCSI SSC target (Sequential device - tape drive)
 * portion of the vtl package.
 *
 * The vtl package consists of:
 *   a kernel module (vlt.ko) - Currently on 2.6.x Linux kernel support.
 *   SCSI target daemons for both SMC and SSC devices.
 *
 *
 * $Id: vtltape.c,v 1.10.2.5 2006-08-30 06:35:01 markh Exp $
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
 */
static const char * Version = "$Id: vtltape.c 1.14 2007-08-24 06:35:01 markh Exp $";

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <syslog.h>
#include <inttypes.h>
#include "scsi.h"
#include "q.h"
#include "vx.h"
#include "vtltape.h"
#include "vxshared.h"

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

static int bufsize = 0;
static loff_t max_tape_capacity;	/* Max capacity of media */
static int tapeLoaded = 0;	/* Default to Off-line */
static int inLibrary = 0;	/* Default to stand-alone drive */
static int datafile;		/* Global file handle - This goes against the
			grain, however the handle is passed to every function
			anyway. */
static char *currentMedia;	/* filename of 'datafile' */
static uint8_t request_sense = 0;	/* Non-zero if Sense-data is valid */
static uint8_t MediaType = 0;	/* 0 = Data, 1 WORM, 6 = Cleaning. */
static int OK_to_write = 1;	// True if in correct position to start writing
static int compressionFactor = 0;

static uint8_t *rw_buf;	// Data buffer (malloc'ed memory)

static u64 bytesRead = 0;
static u64 bytesWritten = 0;
static unsigned char mediaSerialNo[34];	// Currently mounted media S/No.

uint8_t sense[SENSE_BUF_SIZE]; /* Request sense buffer */

static struct MAM mam;

/* Log pages */
static struct	Temperature_page Temperature_pg = {
	{ TEMPERATURE_PAGE, 0x00, 0x06, },
	{ 0x00, 0x00, 0x60, 0x02, }, 0x00, 	// Temperature
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

static struct TapeAlert_page	TapeAlert = {
	{ TAPE_ALERT, 0x00, 100, },
	{ 0x00, 1, 0xc0, 1, }, 0x00,	// Read warning
	{ 0x00, 2, 0xc0, 1, }, 0x00,	// Write warning
	{ 0x00, 3, 0xc0, 1, }, 0x00,	// Hard error
	{ 0x00, 4, 0xc0, 1, }, 0x00,	// Media
	{ 0x00, 5, 0xc0, 1, }, 0x00,	// Read failure
	{ 0x00, 6, 0xc0, 1, }, 0x00,	// Write failure
	{ 0x00, 7, 0xc0, 1, }, 0x00,	// Tape has reached EOL
	{ 0x00, 8, 0xc0, 1, }, 0x00,	// Not data grade
	{ 0x00, 9, 0xc0, 1, }, 0x00,	// Write protect
	{ 0x00, 0xa, 0xc0, 1, }, 0x00,	// No removal
	{ 0x00, 0xb, 0xc0, 1, }, 0x00,	// Cleaning media
	{ 0x00, 0xc, 0xc0, 1, }, 0x00,	// Unsupported format
	{ 0x00, 0xd, 0xc0, 1, }, 0x00,	// Recoverable mechanical cart failure
	{ 0x00, 0xe, 0xc0, 1, }, 0x00,	// Unrecoverable mechanical cart failure
	{ 0x00, 0xf, 0xc0, 1, }, 0x00,	// Memory chip in cart failure
	{ 0x00, 0x10, 0xc0, 1, }, 0x00,	// Forced eject
	{ 0x00, 0x11, 0xc0, 1, }, 0x00,	// Read only format
	{ 0x00, 0x12, 0xc0, 1, }, 0x00,	// Tape directory corrupted on load
	{ 0x00, 0x13, 0xc0, 1, }, 0x00,	// Nearing EOL
	{ 0x00, 0x14, 0xc0, 1, }, 0x00,	// Clean now
	{ 0x00, 0x15, 0xc0, 1, }, 0x00,	// Clean periodic
	{ 0x00, 0x16, 0xc0, 1, }, 0x00,	// Expired cleaning media
	{ 0x00, 0x17, 0xc0, 1, }, 0x00,	// Invalid cleaning tpae
	{ 0x00, 0x18, 0xc0, 1, }, 0x00,	// Retention requested
	{ 0x00, 0x19, 0xc0, 1, }, 0x00,	// Dual port interface failure
	{ 0x00, 0x1a, 0xc0, 1, }, 0x00,	// Cooling fan failure
	{ 0x00, 0x1b, 0xc0, 1, }, 0x00,	// PSU failure
	{ 0x00, 0x1c, 0xc0, 1, }, 0x00,	// Power consumption
	{ 0x00, 0x1d, 0xc0, 1, }, 0x00,	// Drive maintenance - PM required
	{ 0x00, 0x1e, 0xc0, 1, }, 0x00,	// Hardware A
	{ 0x00, 0x1f, 0xc0, 1, }, 0x00,	// Hardware B
	{ 0x00, 0x20, 0xc0, 1, }, 0x00,	// Interface problems.
	{ 0x00, 0x21, 0xc0, 1, }, 0x00,	// Eject media (failed)
	{ 0x00, 0x22, 0xc0, 1, }, 0x00,	// Download failed
	{ 0x00, 0x23, 0xc0, 1, }, 0x00,	// Drive humidity
	{ 0x00, 0x24, 0xc0, 1, }, 0x00,	// Drive temperature
	{ 0x00, 0x25, 0xc0, 1, }, 0x00,	// Drive voltage
	{ 0x00, 0x26, 0xc0, 1, }, 0x00,	// Predictive failure
	{ 0x00, 0x27, 0xc0, 1, }, 0x00,	// Diagnostics required
	{ 0x00, 0x28, 0xc0, 1, }, 0x00,	// Obsolete
	{ 0x00, 0x29, 0xc0, 1, }, 0x00,	// Obsolete
	{ 0x00, 0x2a, 0xc0, 1, }, 0x00,	// Obsolete
	{ 0x00, 0x2b, 0xc0, 1, }, 0x00,	// Obsolete
	{ 0x00, 0x2c, 0xc0, 1, }, 0x00,	// Obsolete
	{ 0x00, 0x2d, 0xc0, 1, }, 0x00,	// Obsolete
	{ 0x00, 0x2e, 0xc0, 1, }, 0x00,	// Obsolete
	{ 0x00, 0x2f, 0xc0, 1, }, 0x00,	// reserved
	{ 0x00, 0x30, 0xc0, 1, }, 0x00,	// reserved
	{ 0x00, 0x31, 0xc0, 1, }, 0x00,	// reserved
	{ 0x00, 0x32, 0xc0, 1, }, 0x00,	// Lost statistics
	{ 0x00, 0x33, 0xc0, 1, }, 0x00,	// Tape directory invalid at unload
	{ 0x00, 0x34, 0xc0, 1, }, 0x00,	// Tape system area write failure
	{ 0x00, 0x35, 0xc0, 1, }, 0x00,	// Tape system area read failure
	{ 0x00, 0x36, 0xc0, 1, }, 0x00,	// No start of data
	{ 0x00, 0x37, 0xc0, 1, }, 0x00,	// Loading failure
	{ 0x00, 0x38, 0xc0, 1, }, 0x00,	// Unrecoverable unload failure
	{ 0x00, 0x39, 0xc0, 1, }, 0x00,	// Automation interface failure
	{ 0x00, 0x3a, 0xc0, 1, }, 0x00,	// firmware failure
	{ 0x00, 0x3b, 0xc0, 1, }, 0x00,	// WORM Medium integerity check failed
	{ 0x00, 0x3c, 0xc0, 1, }, 0x00,	// WORM overwrite attempted
	{ 0x00, 0x3d, 0xc0, 1, }, 0x00,	// reserved
	{ 0x00, 0x3e, 0xc0, 1, }, 0x00,	// reserved
	{ 0x00, 0x3f, 0xc0, 1, }, 0x00,	// reserved
	{ 0x00, 0x40, 0xc0, 1, }, 0x00,	// reserved
	};

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
	0x00, 0x00, 0x00, 			// 16 bytes in length..
	};


/*
 * Mode Pages defined for SSC-3 devices..
 */

// Used by Mode Sense - if set, return block descriptor
uint8_t blockDescriptorBlock[8] = {0, 0, 0, 0, 0, 0, 0, 0, };

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
	if( ! debug)
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
	if(h->blk_type == B_BOT)
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
mk_sense_short_block(u32 requested, u32 processed, uint8_t *sense_valid) {
	u32 difference = requested - processed;
	u32 * lp;

	/* No sense, ILI bit set */
	mkSenseBuf(ILI, NO_ADDITIONAL_SENSE, sense_valid);

	if(debug)
		printf("    Short block sense: Short %d bytes\n", difference);
	if(verbose)
		syslog(LOG_DAEMON|LOG_INFO,
		"Short block read: Requested: %d, Read: %d, short by %d bytes",
					requested, processed, difference);

	/* Now fill in the datablock with number of bytes not read/written */
	sense[0] |= VALID;	/* Set Valid bit only */

	lp = (u32 *)&sense[3];
	*lp = htonl(difference);
}

static loff_t read_header(struct blk_header *h, int size, uint8_t *sense_flg) {
	loff_t nread;

	nread = read(datafile, h, size);
	if(nread < 0) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sense_flg);
		nread = 0;
	} else if (nread == 0) {
		mkSenseBuf(MEDIUM_ERROR, E_END_OF_DATA, sense_flg);
		nread = 0;
	}
	return nread;
}

static loff_t position_to_curr_header(uint8_t * sense_flg) {
	return (lseek64(datafile, c_pos.curr_blk, SEEK_SET));
}

static int skipToNextHeader(uint8_t * sense_flg) {
	loff_t nread;

	if(c_pos.blk_type == B_EOD) {
		mkSenseBuf(MEDIUM_ERROR, E_END_OF_DATA, sense_flg);
		if(verbose)
		    syslog(LOG_DAEMON|LOG_WARNING,
			"End of data detected while forward SPACEing!!");
		return -1;
	}

	if(c_pos.next_blk != lseek64(datafile, c_pos.next_blk, SEEK_SET)) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sense_flg);
		syslog(LOG_DAEMON|LOG_WARNING,
					"Unable to seek to next block header");
		return -1;
	}
	nread = read_header(&c_pos, sizeof(c_pos), sense_flg);
	if(nread == 0) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sense_flg);
		syslog(LOG_DAEMON|LOG_WARNING,
					"Unable to read next block header");
		return -1;
	}
	if(nread == -1) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sense_flg);
		syslog(LOG_DAEMON|LOG_WARNING,
					"Unable to read next block header: %m");
		return -1;
	}
	DEBC( print_header(&c_pos);) ;
	// Position to start of header (rewind over header)
	if(c_pos.curr_blk != position_to_curr_header(sense_flg)) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR, sense_flg);
		if(verbose)
			syslog(LOG_DAEMON|LOG_WARNING,
			"%s: Error positing in datafile, byte offset: %" PRId64,
				__FUNCTION__, c_pos.curr_blk);
		return -1;
	}
	return 0;
}

static int skip_to_prev_header(uint8_t * sense_flg) {
	loff_t nread;

	// Position to previous header
	if(debug) {
		printf("skip_to_prev_header()\n");
		printf("Positioning to c_pos.prev_blk: %" PRId64 "\n", c_pos.prev_blk);
	}
	if(c_pos.prev_blk != lseek64(datafile, c_pos.prev_blk, SEEK_SET)) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sense_flg);
		if(verbose)
			syslog(LOG_DAEMON|LOG_WARNING,
					"%s: Error position in datafile !!",
					__FUNCTION__);
		return -1;
	}
	// Read in header
	if(debug)
		printf("Reading in header: %d bytes\n", sizeof(c_pos));

	nread = read_header(&c_pos, sizeof(c_pos), sense_flg);
	if(nread == 0) {
		if(verbose)
		    syslog(LOG_DAEMON|LOG_WARNING, "%s",
			"Error reading datafile while reverse SPACEing");
		return -1;
	}
	DEBC( print_header(&c_pos); ) ;
	if(c_pos.blk_type == B_BOT) {
		if(debug)
			printf("Found Beginning Of Tape, "
				"Skipping to next header..\n");
		skipToNextHeader(sense_flg);
		DEBC( print_header(&c_pos);) ;
		mkSenseBuf(MEDIUM_ERROR, E_BOM, sense_flg);
		syslog(LOG_DAEMON|LOG_WARNING, "Found BOT!!");
		return -1;
	}

	// Position to start of header (rewind over header)
	if(c_pos.curr_blk != position_to_curr_header(sense_flg)) {
		mkSenseBuf(MEDIUM_ERROR, E_SEQUENTIAL_POSITION_ERR,sense_flg);
		syslog(LOG_DAEMON|LOG_WARNING,
				"%s: Error position in datafile !!",
				__FUNCTION__);
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
static int mkNewHeader(char type, int size, int comp_size, uint8_t * sense_flg) {
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
	if(type == B_EOD)
		h.next_blk = h.curr_blk;
	else
		h.next_blk = h.curr_blk + comp_size + sizeof(h);

	if(h.curr_blk == c_pos.curr_blk) {
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
		mkSenseBuf(MEDIUM_ERROR,E_SEQUENTIAL_POSITION_ERR,sense_flg);
		return 0;
	}

	nwrite = write(datafile, &h, sizeof(h));

	/*
	 * If write was successful, update c_pos with this header block.
	 */
	if(nwrite <= 0) {
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sense_flg);
		if(debug) {
			if(nwrite < 0) perror("header write failed");
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

static int mkEODHeader(uint8_t *sense_flg) {
	loff_t nwrite;

	nwrite = mkNewHeader(B_EOD, sizeof(c_pos), sizeof(c_pos), sense_flg);
	if(MediaType == MEDIA_TYPE_WORM)
		OK_to_write = 1;

	/* If we have just written a END OF DATA marker,
	 * rewind to just before it. */
	// Position to start of header (rewind over header)
	if(c_pos.curr_blk != position_to_curr_header(sense_flg)) {
		mkSenseBuf(MEDIUM_ERROR,E_SEQUENTIAL_POSITION_ERR,sense_flg);
		syslog(LOG_DAEMON|LOG_ERR, "Failed to write EOD header");
		if(verbose)
			syslog(LOG_DAEMON|LOG_WARNING,
				"%s: Error position in datafile!!",
				__FUNCTION__);
		return -1;
	}

	return nwrite;
}

/*
 * Simple function to read 'count' bytes from the chardev into 'buf'.
 */
static int retrieve_CDB_data(int cdev, uint8_t *buf, int count) {

	if(verbose > 2)
		syslog(LOG_DAEMON|LOG_INFO,
			"retrieving %d bytes from char dev, bufsize: %d",
					count, bufsize);

	return (read(cdev, buf, bufsize));
}

/*
 *
 */

static int skip_prev_filemark(uint8_t *sense_flg) {

	DEBC(
		printf("  skip_prev_filemark()\n");
		print_header(&c_pos);
	) ; // END debug macro

	if(c_pos.blk_type == B_FILEMARK)
		c_pos.blk_type = B_NOOP;
	DEBC( print_header(&c_pos);) ;
	while(c_pos.blk_type != B_FILEMARK) {
		if(c_pos.blk_type == B_BOT) {
			mkSenseBuf(NO_ADDITIONAL_SENSE, E_BOM, sense_flg);
			if(verbose)
				syslog(LOG_DAEMON|LOG_WARNING, "%s",
						"Found Beginning of tape");
			return -1;
		}
		if(skip_to_prev_header(sense_flg))
			return -1;
	}
	return 0;
}

/*
 *
 */
static int skip_next_filemark(uint8_t *sense_flg) {

	DEBC(
		printf("  skip_next_filemark()\n");
		print_header(&c_pos);
	) ;
	// While blk header is NOT a filemark, keep skipping to next header
	while(c_pos.blk_type != B_FILEMARK) {
		// END-OF-DATA -> Treat this as an error - return..
		if(c_pos.blk_type == B_EOD) {
			mkSenseBuf(NO_ADDITIONAL_SENSE, E_END_OF_DATA,
								 sense_flg);
			if(verbose)
				syslog(LOG_DAEMON|LOG_WARNING, "%s",
							"Found end of media");
			if(MediaType == MEDIA_TYPE_WORM)
				OK_to_write = 1;
			return -1;
		}
		if(skipToNextHeader(sense_flg))
			return -1;	// On error
	}
	// Position to header AFTER the FILEMARK..
	if(skipToNextHeader(sense_flg))
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
static int checkRestrictions(uint8_t *sense_flg) {

	// Check that there is a piece of media loaded..
	if(! tapeLoaded) {
		mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sense_flg);
		return 1;
	}

	switch(MediaType) {
	case MEDIA_TYPE_CLEAN:
		mkSenseBuf(NOT_READY, E_CLEANING_CART_INSTALLED, sense_flg);
		syslog(LOG_DAEMON|LOG_INFO, "Can not write - Cleaning cart");
		OK_to_write = 0;
		break;
	case MEDIA_TYPE_WORM:
		/* If we are not at end of data for a write
	 	* and media is defined as WORM, fail...
	 	*/
		if(c_pos.blk_type == B_EOD)
			OK_to_write = 1;	// OK to append to end of 'tape'
		if(! OK_to_write) {
			syslog(LOG_DAEMON|LOG_ERR,
					"Attempt to overwrite WORM data");
			mkSenseBuf(DATA_PROTECT,
				E_MEDIUM_OVERWRITE_ATTEMPTED, sense_flg);
		}
		break;
	case MEDIA_TYPE_DATA:
		OK_to_write = 1;
		break;
	}
	if(verbose > 2) {
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
	if(m) {
		p = m->pcodePointer;
		p[2] = 1;	/* Set WORMM bit */
		if(verbose)
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
	if(m) {
		p = m->pcodePointer;
		p[2] = 0;
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "WORM mode page cleared");
	}
}


/*
 * Report density of media loaded.

 */
// FIXME: Need to grab info from MAM !!!

#define REPORT_DENSITY_LEN 56
static int resp_report_density(uint8_t media, int len, uint8_t *buf, uint8_t *sense_flg) {
	u16	*sp;
	u32	*lp;
	u32	t;

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
	*lp = ntohl(4880);

	// Media Width (tenths of mm)
	t = htonl(mam.MediumWidth);
	sp = (u16 *)&buf[12];
	*sp = htons(t);

	// Tracks
	t = htonl(mam.MediumLength);
	sp = (u16 *)&buf[14];
	*sp = htons(t);

	// Capacity
	lp = (u32 *)&buf[16];
	*lp = htonl(95367);

	// Assigning Oranization (8 chars long)
	snprintf((char *)&buf[20], 8, "%-8s", "LTO-CVE");
	// Density Name (8 chars long)
	snprintf((char *)&buf[28], 8, "%-8s", "U-18");
	// Description (18 chars long)
	snprintf((char *)&buf[36], 18, "%-18s", "Ultrium 1/8T");

return(REPORT_DENSITY_LEN);
}


/*
 * Process the MODE_SELECT command
 */
static int resp_mode_select(int cdev, uint8_t *cmd, uint8_t *buf, uint8_t *sense_flg) {
	int alloc_len;
	int k;

	alloc_len = (MODE_SELECT == cmd[0]) ? cmd[4] : ((cmd[7] << 8) | cmd[8]);

	retrieve_CDB_data(cdev, buf, alloc_len);

	/* This should be more selective..
	 * Only needed for cmds that alter the partitioning or format..
	 */
	if(! checkRestrictions(sense_flg))
		return 0; 

	if(debug) {
		for(k = 0; k < alloc_len; k++)
			printf("%02x ", (u32)buf[k]);
		printf("\n");
	}

	return 0;
}


static int resp_log_sense(uint8_t *SCpnt, uint8_t *buf) {
	uint8_t	*b = buf;
	int	retval = 0;
	u16	*sp;

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

	switch (SCpnt[2] & 0x3f) {
	case 0:	/* Send supported pages */
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"LOG SENSE: Sending supported pages");
		sp = (u16 *)&supported_pages[2];
		*sp = htons(sizeof(supported_pages) - 4);
		b = memcpy(b, supported_pages, sizeof(supported_pages));
		retval = sizeof(supported_pages);
		break;
	case WRITE_ERROR_COUNTER:	/* Write error page */
		if(verbose)
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
		if(verbose)
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
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"LOG SENSE: Sequential Access Device Log page");
		seqAccessDevice.pcode_head.len = htons(sizeof(seqAccessDevice) -
					sizeof(seqAccessDevice.pcode_head));
		b = memcpy(b, &seqAccessDevice, sizeof(seqAccessDevice));
		retval += sizeof(seqAccessDevice);
		break;
	case TEMPERATURE_PAGE:	/* Temperature page */
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO,
						"LOG SENSE: Temperature page");
		Temperature_pg.pcode_head.len = htons(sizeof(Temperature_pg) -
					sizeof(Temperature_pg.pcode_head));
		Temperature_pg.temperature = htons(35);
		b = memcpy(b, &Temperature_pg, sizeof(Temperature_pg));
		retval += sizeof(Temperature_pg);
		break;
	case TAPE_ALERT:	/* TapeAlert page */
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO,"LOG SENSE: TapeAlert page");
		if(verbose > 1)
			syslog(LOG_DAEMON|LOG_INFO,
				" Returning TapeAlert flags: 0x%" PRIx64,
					ntohll(seqAccessDevice.TapeAlert));
			
		TapeAlert.pcode_head.len = htons(sizeof(TapeAlert) -
					sizeof(TapeAlert.pcode_head));
		b = memcpy(b, &TapeAlert, sizeof(TapeAlert));
		retval += sizeof(TapeAlert);
		setTapeAlert(&TapeAlert, 0);	// Clear flags after value read.
		setSeqAccessDevice(&seqAccessDevice, 0);
		break;
	case TAPE_USAGE:	/* Tape Usage Log */
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"LOG SENSE: Tape Usage page");
		TapeUsage.pcode_head.len = htons(sizeof(TapeUsage) -
					sizeof(TapeUsage.pcode_head));
		b = memcpy(b, &TapeUsage, sizeof(TapeUsage));
		retval += sizeof(TapeUsage);
		break;
	case TAPE_CAPACITY:	/* Tape Capacity page */
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"LOG SENSE: Tape Capacity page");
		TapeCapacity.pcode_head.len = htons(sizeof(TapeCapacity) -
					sizeof(TapeCapacity.pcode_head));
		if(tapeLoaded) {
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
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO,
					"LOG SENSE: Data Compression page");
		DataCompression.pcode_head.len = htons(sizeof(DataCompression) -
					sizeof(DataCompression.pcode_head));
		b = memcpy(b, &DataCompression, sizeof(DataCompression));
		retval += sizeof(DataCompression);
		break;
	default:
		syslog(LOG_DAEMON|LOG_ERR,
			"LOG SENSE: Unknown code: 0x%x", SCpnt[2] & 0x3f);
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
static int resp_read_attribute(uint8_t * SCpnt, uint8_t * buf, uint8_t * sense_flg) {
	u16	*sp;
	u16	attribute;
	u32	*lp;
	u32	alloc_len;
	int	ret_val = 0;

	sp = (u16 *)&SCpnt[8];
	attribute = ntohs(*sp);
	lp = (u32 *)&SCpnt[10];
	alloc_len = ntohl(*lp);
	if(verbose)
		syslog(LOG_DAEMON|LOG_INFO,
			"Read Attribute: 0x%x, allocation len: %d",
			 				attribute, alloc_len);

	memset(buf, 0, alloc_len);	// Clear memory

	sp = (u16 *)&buf[0];
	*sp = htons(attribute);

	switch(attribute) {
	case 0x0400:	/* Medium Manufacturer */
		buf[2] = 0x81;		// Read-only + ASCII format..
		buf[4] = 0x8;	// Attribute length
		memcpy(&buf[5], &mam.MediumManufacturer, 8);
		ret_val = 14;
		break;
	case 0x0408:	/* Medium Type  - Hack for SDLT and WORM */
		buf[2] = 0x80;		// Read-only + binary format..
		buf[4] = 0x8;	// Attribute length
		buf[5] = mam.MediumType;
		buf[9] = (MediaType == MEDIA_TYPE_WORM) ? 0x80 : 0;
		ret_val = 14;
		break;
	case 0x0800:	/* Application Vendor */
		buf[4] = 0x8;	// Attribute length
		memcpy(&buf[5], &mam.ApplicationVendor, 8);
		ret_val = 14;
		break;
	default:
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sense_flg);
		return 0;
		break;
	}
return ret_val;
}

/*
 *
 * Return number of bytes read.
 *        0 on error with sense[] filled in...
 */
static int readBlock(int cdev, uint8_t * buf, uint8_t * sense_flg, u32 request_sz) {
	loff_t	nread = 0;
	uint8_t	*comp_buf;
	uLongf	uncompress_sz;
	uLongf	comp_buf_sz;
	int	z;

	if(verbose > 1)
		syslog(LOG_DAEMON|LOG_WARNING, "Request to read: %d bytes",
							request_sz);

	DEBC( print_header(&c_pos);) ;

	/* Read in block of data */
	switch(c_pos.blk_type) {
	case B_FILEMARK:
		if(verbose)
			syslog(LOG_DAEMON|LOG_ERR,
				"Expected to find hdr type: %d, found: %d",
				 	B_UNCOMPRESS_DATA, c_pos.blk_type);
		mkSenseBuf(FILEMARK, E_MARK, sense_flg);
		return nread;
		break;
	case B_EOD:
		mkSenseBuf(NO_ADDITIONAL_SENSE, E_END_OF_DATA, sense_flg);
		return nread;
		break;
	case B_BOT:
		skipToNextHeader(sense_flg);
		// Re-exec function.
		return readBlock(cdev, buf, sense_flg, request_sz);
		break;
	case B_COMPRESSED_DATA:
		// If we are positioned at beginning of header, read it in.
		if(c_pos.curr_blk == lseek64(datafile, 0, SEEK_CUR)) {
			nread = read_header(&c_pos, sizeof(c_pos), sense_flg);
			if(nread == 0) {	// Error
				syslog(LOG_DAEMON|LOG_ERR,
					"Unable to read header: %m");
				mkSenseBuf(MEDIUM_ERROR,E_UNRECOVERED_READ,
								sense_flg);
				return 0;
			}
		}
		comp_buf_sz = c_pos.disk_blk_size + 80;
		comp_buf = (uint8_t *)malloc(comp_buf_sz);
		uncompress_sz = bufsize;
		if(NULL == comp_buf) {
			syslog(LOG_DAEMON|LOG_WARNING,
				"Unable to alloc %ld bytes", comp_buf_sz);
			mkSenseBuf(MEDIUM_ERROR,E_UNRECOVERED_READ,sense_flg);
			return 0;
		}
		nread = read(datafile, comp_buf, c_pos.disk_blk_size);
		if(nread == 0) {	// End of data - no more to read
			if(verbose)
				syslog(LOG_DAEMON|LOG_WARNING, "%s",
				"End of data detected when reading from file");
			mkSenseBuf(NO_ADDITIONAL_SENSE, E_END_OF_DATA,
								sense_flg);
			free(comp_buf);
			return 0;
		} else if(nread < 0) {	// Error
			syslog(LOG_DAEMON|LOG_ERR, "Read Error: %m");
			mkSenseBuf(MEDIUM_ERROR,E_UNRECOVERED_READ,sense_flg);
			free(comp_buf);
			return 0;
		}
		z = uncompress((uint8_t *)buf,&uncompress_sz, comp_buf, nread);
		if(z != Z_OK) {
			mkSenseBuf(MEDIUM_ERROR,E_DECOMPRESSION_CRC,sense_flg);
			if(z == Z_MEM_ERROR)
				syslog(LOG_DAEMON|LOG_ERR,
					"Not enough memory to decompress data");
			else if(z == Z_BUF_ERROR)
				syslog(LOG_DAEMON|LOG_ERR,
					"Not enough memory in destination buf"
					" to decompress data");
			else if(z == Z_DATA_ERROR)
				syslog(LOG_DAEMON|LOG_ERR,
					"Input data corrput or incomplete");
			free(comp_buf);
			return 0;
		}
		// requested block and actual block size different
		if(uncompress_sz != request_sz) {
			syslog(LOG_DAEMON|LOG_WARNING,
			"Short block read %ld %d", uncompress_sz, request_sz);
			mk_sense_short_block(request_sz, uncompress_sz,
								sense_flg);
		}
		free(comp_buf);
		break;
	case B_UNCOMPRESS_DATA:
		// If we are positioned at beginning of header, read it in.
		if(c_pos.curr_blk == lseek64(datafile, 0, SEEK_CUR)) {
			nread = read_header(&c_pos, sizeof(c_pos), sense_flg);
			if(nread == 0) {	// Error
				syslog(LOG_DAEMON|LOG_ERR, "%m");
				mkSenseBuf(MEDIUM_ERROR,E_UNRECOVERED_READ,
								sense_flg);
				return 0;
			}
		}
		nread = read(datafile, buf, c_pos.disk_blk_size);
		if(nread == 0) {	// End of data - no more to read
			if(verbose)
				syslog(LOG_DAEMON|LOG_WARNING, "%s",
				"End of data detected when reading from file");
			mkSenseBuf(NO_ADDITIONAL_SENSE, E_END_OF_DATA,
								sense_flg);
			return nread;
		} else if(nread < 0) {	// Error
			syslog(LOG_DAEMON|LOG_ERR, "%m");
			mkSenseBuf(MEDIUM_ERROR,E_UNRECOVERED_READ,sense_flg);
			return 0;
		} else if(nread != request_sz) {
			// requested block and actual block size different
			mk_sense_short_block(request_sz, nread, sense_flg);
		}
		break;
	default:
		if(verbose)
		  syslog(LOG_DAEMON|LOG_ERR,
			"Unknown blk header at offset %" PRId64 " - Abort read cmd",
							c_pos.curr_blk);
		mkSenseBuf(MEDIUM_ERROR, E_UNRECOVERED_READ, sense_flg);
		return 0;
		break;
	}
	// Now read in subsequent header
	skipToNextHeader(sense_flg);

	return nread;
}


/*
 * Return number of bytes written to 'file'
 */
static int writeBlock(uint8_t * src_buf, u32 src_sz,  uint8_t * sense_flg) {
	loff_t	nwrite = 0;
	Bytef	* dest_buf = src_buf;
	uLong	dest_len = src_sz;
	uLong	src_len = src_sz;

	if(compressionFactor) {
/*
		src_len = compressBound(src_sz);

		dest_buf = malloc(src_len);
		if(NULL == dest_buf) {
			mkSenseBuf(MEDIUM_ERROR,E_COMPRESSION_CHECK, sense_flg);
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
		   mkNewHeader(B_COMPRESSED_DATA, src_len, dest_len, sense_flg);
*/
	} else {
		nwrite =
		   mkNewHeader(B_UNCOMPRESS_DATA, src_len, dest_len, sense_flg);

	}
	if(nwrite <= 0) {
		if(debug)
			printf("Failed to write header\n");
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sense_flg);
	} else {	// now write the block of data..
		nwrite = write(datafile, dest_buf, dest_len);
		if(nwrite <= 0) {
			syslog(LOG_DAEMON|LOG_ERR, "%s %m", "Write failed:");
			if(debug) {
				if(nwrite < 0)
					perror("writeBlk failed");
				printf("Failed to write %ld bytes\n", dest_len);
			}
			mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sense_flg);
		} else if (nwrite != dest_len) {
			DEBC(
			syslog(LOG_DAEMON|LOG_ERR, "Did not write all data");
			) ;
			mk_sense_short_block(src_len, nwrite, sense_flg);
		}
	}
	if(c_pos.curr_blk >= max_tape_capacity) {
		syslog(LOG_DAEMON|LOG_INFO, "End of Medium - Setting EOM flag");
		mkSenseBuf(NO_ADDITIONAL_SENSE|EOM_FLAG, NO_ADDITIONAL_SENSE,
								sense_flg);
	}

	if(compressionFactor)
		free(dest_buf);

	/* Write END-OF-DATA marker */
	nwrite = mkEODHeader(sense_flg);
	if(nwrite <= 0)
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sense_flg);

	return src_len;
}


/*
 * Rewind 'tape'.
 */
static void rawRewind(uint8_t *sense_flg) {

	// Start at beginning of datafile..
	lseek64(datafile, 0L, SEEK_SET);

	/*
	 * Read header..
	 * If this is not the BOT header we are in trouble
	 */
	read(datafile, &c_pos, sizeof(c_pos));
}

/*
 * Rewind to beginning of data file and the position to first data header.
 */
static void respRewind(uint8_t * sense_flg) {

	rawRewind(sense_flg);

	if(c_pos.blk_type != B_BOT) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sense_flg);
		DEBC( print_header(&c_pos);) ;
	}
	read(datafile, &mam, c_pos.blk_size);
	if(verbose)
		syslog(LOG_DAEMON|LOG_INFO, "MAM: media S/No. %s",
							mam.MediumSerialNumber);

	skipToNextHeader(sense_flg);

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
		if(c_pos.blk_type != B_EOD)
			OK_to_write = 0;

		// Check that this header is a filemark and the next header
		//  is End of Data. If it is, we are OK to write
		if(c_pos.blk_type == B_FILEMARK) {
			skipToNextHeader(sense_flg);
			if(c_pos.blk_type == B_EOD)
				OK_to_write = 1;
		}
		// Now we have to go thru thru the rewind again..
		rawRewind(sense_flg);
		// No need to do all previous error checking...
		skipToNextHeader(sense_flg);
		break;
	case MEDIA_TYPE_DATA:
		OK_to_write = 1;	// Reset flag to OK.
		break;
	}

	if(verbose)
		syslog(LOG_DAEMON|LOG_INFO,
				" media is %s",
				(OK_to_write) ? "writable" : "not writable");
}

/*
 * Space over (to) x filemarks. Setmarks not supported as yet.
 */
static void resp_space(u32 count, int code, uint8_t *sense_flg) {

	switch(code) {
	// Space 'count' blocks
	case 0:
		if(verbose)
			syslog(LOG_DAEMON|LOG_NOTICE,
				"SCSI space 0x%02x blocks **", count);
		if(count > 0xff000000) {
	 		// Moved backwards. Disable writing..
			if(MediaType == MEDIA_TYPE_WORM)
				OK_to_write = 0;
			for(;count > 0; count++)
				if(skip_to_prev_header(sense_flg))
					return;
		} else {
			for(;count > 0; count--)
				if(skipToNextHeader(sense_flg))
					return;
		}
		break;
	// Space 'count' filemarks
	case 1:
		if(verbose)
			syslog(LOG_DAEMON|LOG_NOTICE,
				"SCSI space 0x%02x filemarks **\n", count);
		if(count > 0xff000000) {	// skip backwards..
			// Moved backwards. Disable writing..
			if(MediaType == MEDIA_TYPE_WORM)
				OK_to_write = 0;
			for(;count > 0; count++)
				if(skip_prev_filemark(sense_flg))
					return;
		} else {
			for(;count > 0; count--)
				if(skip_next_filemark(sense_flg))
					return;
		}
		break;
	// Space to end-of-data - Ignore 'count'
	case 3:
		if(verbose)
			syslog(LOG_DAEMON|LOG_NOTICE, "%s",
				"SCSI space to end-of-data **");
		while(c_pos.blk_type != B_EOD)
			if(skipToNextHeader(sense_flg)) {
				if(MediaType == MEDIA_TYPE_WORM)
					OK_to_write = 1;
				return;
			}
		break;

	default:
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sense_flg);
		break;
	}
}

/*
 * Writes data in struct mam back to beginning of datafile..
 * Returns 0 if nothing written or -1 on error
 */
static int rewriteMAM(struct MAM *mamp, uint8_t *sense_flg) {
	loff_t nwrite = 0;

	// Start at beginning of datafile..
	lseek64(datafile, 0L, SEEK_SET);

	/* Update the c_pos data struct.
	 * If this is not the BOT header we are in trouble
	 * Just using this to position to MAM
	 */
	nwrite = read(datafile, &c_pos, sizeof(c_pos));
	if(nwrite != sizeof(c_pos)) {
		mkSenseBuf(MEDIUM_ERROR, E_UNKNOWN_FORMAT, sense_flg);
		return -1;
	}

	// Rewrite MAM data
	nwrite = write(datafile, mamp, sizeof(mam));
	if(nwrite != sizeof(mam)) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FORMAT_CORRUPT, sense_flg);
		return -1;
	}

	return 0;
}

/*
 * Update MAM contents with current counters
 */
static void updateMAM(struct MAM *mamp, uint8_t *sense_flg, int loadCount) {
	u64 bw;		// Bytes Written
	u64 br;		// Bytes Read
	u64 load;	// load count

	if(verbose)
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
	if(loadCount) {
		load = htonll(mamp->LoadCount);
		load++;
		mamp->LoadCount = ntohll(load);
	}

	rewriteMAM(mamp, sense_flg);
}

/*
 *
 * Process the SCSI command
 *
 * Called with:
 * 	cdev     -> Char dev file handle,
 * 	SCpnt    -> SCSI Command buffer pointer,
 *	buf      -> General purpose data buffer pointer
 * 	sense_flg    -> int buffer -> 1 = request_sense, 0 = no sense
 *
 * Return:
 *	total number of bytes to send back to vtl device
 */
static u32 processCommand(int cdev, uint8_t *SCpnt, uint8_t *buf, uint8_t *sense_flg)
{
	u32	block_size = 0L;
	u32	count;
	u32	ret = 5L;	// At least 4 bytes for Sense & 4 for S/No.
	u32	retval=0L;
	u32	*lp;
	u16	*sp;
	int	k;
	int	code;
	int	service_action;
	struct	mode *smp = sm;
	loff_t	nread;

	DEB( logSCSICommand(SCpnt); ) ;

	// Limited subset of commands don't need to check for power-on reset
	switch(SCpnt[0]) {
	case REPORT_LUN:
	case REQUEST_SENSE:
	case MODE_SELECT:
		break;
	default:
		if(check_reset(sense_flg))
			return ret;
	}

	// Now process SCSI command.
	switch(SCpnt[0]) {
	case ALLOW_MEDIUM_REMOVAL:
		resp_allow_prevent_removal(SCpnt, sense_flg);
		break;

	case FORMAT_UNIT:	// That's FORMAT_MEDIUM for an SSC device...
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Format Medium **");
		if(! tapeLoaded) {
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sense_flg);
			break;
		}

		if(! checkRestrictions(sense_flg))
			break;

		if(c_pos.blk_number != 0) {
			syslog(LOG_DAEMON|LOG_INFO, "Not at beginning **");
			mkSenseBuf(ILLEGAL_REQUEST,E_POSITION_PAST_BOM,
								 sense_flg);
			break;
		}
		mkEODHeader(sense_flg);
		break;

	case SEEK_10:	// Thats LOCATE_BLOCK for SSC devices...
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Fast Block Locate **");
		lp = (u32 *)&SCpnt[3];
		count = ntohl(*lp);

		/* If we want to seek closer to beginning of file than
		 * we currently are, rewind and seek from there
		 */
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO,
			"Current blk: %" PRId64 ", seek: %d",c_pos.blk_number,count);
		if(((u32)(count - c_pos.blk_number) > count) &&
						(count < c_pos.blk_number)) {
			respRewind(sense_flg);
		}
		if(MediaType == MEDIA_TYPE_WORM)
			OK_to_write = 0;
		while(c_pos.blk_number != count) {
			if(c_pos.blk_number > count) {
				if(skip_to_prev_header(sense_flg) == -1)
					break;
			} else {
				if(skipToNextHeader(sense_flg) == -1)
					break;
			}
		}
		break;

	case LOG_SELECT:	// Set or reset LOG stats.
		resp_log_select(SCpnt, sense_flg);
		break;

	case LOG_SENSE:
		k = resp_log_sense(SCpnt, buf);
		sp = (u16 *)&SCpnt[7];
		count = ntohs(*sp);
		ret += (k < count) ? k : count;
		break;

	case MODE_SELECT:
	case MODE_SELECT_10:
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "%s", "MODE SELECT **");
		ret += resp_mode_select(cdev, SCpnt, buf, sense_flg);
		break;

	case MODE_SENSE:
	case MODE_SENSE_10:
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "%s", "MODE SENSE **");
		ret += resp_mode_sense(SCpnt, buf, smp, sense_flg);
		break;

	case READ_12:
	case READ_10:
	case READ_6:
		block_size = 	(SCpnt[2] << 16) +
				(SCpnt[3] << 8) +
				 SCpnt[4];
		if(verbose) 
			syslog(LOG_DAEMON|LOG_INFO, "Read: %d bytes **",
								block_size);
		/* If both FIXED & SILI bits set, invalid combo.. */
		if((SCpnt[1] & (SILI | FIXED)) == (SILI | FIXED)) {
			syslog(LOG_DAEMON|LOG_WARNING,
					"Fixed block read not supported");
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sense_flg);
			reset = 0;
			break;
		}
		/* This driver does not support fixed blocks at the moment */
		if((SCpnt[1] & FIXED) == FIXED) {
			syslog(LOG_DAEMON|LOG_WARNING,
					"Fixed block read not supported");
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sense_flg);
			reset = 0;
			break;
		}
		if(tapeLoaded) {
			if(MediaType == MEDIA_TYPE_CLEAN) {
				mkSenseBuf(NOT_READY, E_CLEANING_CART_INSTALLED,
								sense_flg);
			} else {
			  retval = readBlock(cdev, buf, sense_flg, block_size);
			  bytesRead += retval;
			  pg_read_err_counter.bytesProcessed = bytesRead;
			}
		} else
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sense_flg);

		ret += retval;
		break;

	case READ_ATTRIBUTE:
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Read Attribute**");
		if(! tapeLoaded) {
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sense_flg);
			break;
		}
		if(SCpnt[1]) { // Only support Service Action - Attribute Values
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sense_flg);
			break;
		}
		ret += resp_read_attribute(SCpnt, buf, sense_flg);
		if(verbose > 1) {
			syslog(LOG_DAEMON|LOG_INFO,
				" dump return data, length: %d", ret);
			for(k = 0; k < ret; k += 8) {
				syslog(LOG_DAEMON|LOG_INFO,
					" 0x%02x 0x%02x 0x%02x 0x%02x"
					" 0x%02x 0x%02x 0x%02x 0x%02x",
					buf[k+0], buf[k+1], buf[k+2], buf[k+3],
					buf[k+4], buf[k+5], buf[k+6], buf[k+7]);
			}
		}
		break;

	case READ_MEDIA_SERIAL_NUMBER:
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Read Medium Serial No.**");
		if(tapeLoaded)
			ret += resp_read_media_serial(mediaSerialNo, buf,
								sense_flg);
			if(verbose)
				syslog(LOG_DAEMON|LOG_INFO, "   %d", ret);
		else
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sense_flg);
		break;

	case READ_POSITION:
		service_action = SCpnt[1] & 0x1f;
/* service_action == 0 or 1 -> Returns 20 bytes of data (short) */
		if((service_action == 0) || (service_action == 1)) {
			ret += resp_read_position(c_pos.blk_number, buf,
								sense_flg);
		} else {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sense_flg);
		}
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Read Position => %d **",
								ret);
		break;

	case RELEASE:
	case RELEASE_10:
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Release **");
		break;

	case REPORT_DENSITY:
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Report Density **");
		sp = (u16 *)&SCpnt[7];
		ret += resp_report_density((SCpnt[1] & 0x01), 	// media flg
					ntohs(*sp),		// alloc len
					buf,			// fifo buf
					sense_flg );
		break;

	case REPORT_LUN:
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Report LUNs **");
		lp = (u32 *)&SCpnt[6];
		if(*lp < 16) {	// Minimum allocation length is 16 bytes.
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
								sense_flg);
			break;
		}
		report_luns.size = htonl(sizeof(report_luns) - 8);
		resp_report_lun(&report_luns, buf, sense_flg);
		break;

	case REQUEST_SENSE:
		if(verbose) {
			syslog(LOG_DAEMON|LOG_INFO,
			"Request Sense: key/ASC/ASCQ [0x%02x 0x%02x 0x%02x]"
				" Filemark: %s, EOM: %s, ILI: %s",
					sense[2] & 0x0f, sense[12], sense[13],
					(sense[2] & FILEMARK) ? "yes" : "no",
					(sense[2] & EOM) ? "yes" : "no",
					(sense[2] & ILI) ? "yes" : "no");
		}
		block_size =
			(SCpnt[4] < sizeof(sense)) ? SCpnt[4] : sizeof(sense);
		memcpy(buf, sense, block_size);
		/* Clear out the request sense flag */
		*sense_flg = 0;
		memset(sense, 0, sizeof(sense));
		ret += block_size;
		break;

	case RESERVE:
	case RESERVE_10:
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Reserve **");
		break;

	case REZERO_UNIT:	/* Rewind */
		if(verbose) 
			syslog(LOG_DAEMON|LOG_INFO, "%s", "Rewinding **");

		respRewind(sense_flg);
		sleep(1);
		break;

	case ERASE_6:
		if(verbose) 
			syslog(LOG_DAEMON|LOG_INFO, "%s", "Erasing **");

		if(! checkRestrictions(sense_flg))
			break;

		// Rewind and postition just after the first header.
		respRewind(sense_flg);

		ftruncate(datafile, c_pos.curr_blk);

		// Position to just before first header.
		position_to_curr_header(sense_flg);

		// Write EOD header
		mkEODHeader(sense_flg);
		sleep(2);
		break;

	case SPACE:
		count = (SCpnt[2] << 16) +
			(SCpnt[3] << 8) +
			 SCpnt[4];

		code = SCpnt[1] & 0x07;

		/* Can return a '2s complement' to seek backwards */
		if(SCpnt[2] & 0x80)
			count += (0xff << 24);

		resp_space(count, code, sense_flg);
		break;

	case START_STOP:	// Load/Unload cmd
		if(SCpnt[4] && 0x1) {
			tapeLoaded = 1;
			if(verbose) 
				syslog(LOG_DAEMON|LOG_INFO, "Loading Tape **");
			respRewind(sense_flg);
		} else {
			mam.record_dirty = 0;
			// Don't update load count on unload -done at load time
			updateMAM(&mam, sense_flg, 0);
			close(datafile);
			tapeLoaded = 0;
			OK_to_write = 0;
			clearWORM();
			if(verbose)
				syslog(LOG_DAEMON|LOG_INFO,"Unloading Tape **");
			close(datafile);
		}
		break;

	case TEST_UNIT_READY:	// Return OK by default
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Test Unit Ready : %s",
					(tapeLoaded == 0) ? "No" : "Yes");
		if( ! tapeLoaded)
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sense_flg);

		break;

	case WRITE_12:
	case WRITE_10:
	case WRITE_6:
		block_size = 	(SCpnt[2] << 16) +
				(SCpnt[3] << 8) +
				 SCpnt[4];
		if(verbose) 
			syslog(LOG_DAEMON|LOG_INFO, "Write: %d bytes **",
								block_size);
		if(! checkRestrictions(sense_flg))
			break;

		// FIXME: should handle this test in a nicer way...
		if(block_size > bufsize)
			syslog(LOG_DAEMON|LOG_ERR,
			"Fatal: bufsize %d, requested write of %d bytes",
							bufsize, block_size);

		// Attempt to read complete buffer size of data
		// from vx char device into buffer..
		nread = retrieve_CDB_data(cdev, buf, bufsize);

		// NOTE: This needs to be performed AFTER we read
		//	 data block from kernel char driver.
		if(! checkRestrictions(sense_flg))
			break;

		if(tapeLoaded) {
			retval = writeBlock(buf, block_size, sense_flg);
			bytesWritten += retval;
			pg_write_err_counter.bytesProcessed = bytesWritten;
		} else
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sense_flg);
		break;

	case WRITE_ATTRIBUTE:
		if(verbose) 
			syslog(LOG_DAEMON|LOG_INFO, "%s", "Write Attributes**");
		if(! tapeLoaded) {
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sense_flg);
			break;
		}
		lp = (u32 *)&SCpnt[10];
		count = htonl(*lp);
		// Read '*lp' bytes from char device...
		block_size = retrieve_CDB_data(cdev, buf, count);
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"  --> Expected to read %d bytes"
				", read %d", count, block_size);
		if(resp_write_attribute(buf, count, &mam, sense_flg) == 1) {
			rewriteMAM(&mam, sense_flg);
			// respRewind() will clean up for us..
			respRewind(sense_flg);
		}

		break;

	case WRITE_FILEMARKS:
		if(! tapeLoaded) {
			mkSenseBuf(NOT_READY,E_MEDIUM_NOT_PRESENT, sense_flg);
			break;
		}

		if(! checkRestrictions(sense_flg))
			break;

		block_size = 	(SCpnt[2] << 16) +
				(SCpnt[3] << 8) +
				 SCpnt[4];
		if(verbose)
			syslog(LOG_DAEMON|LOG_INFO, "Write %d filemarks **",
								block_size);
		while(block_size > 0) {
			block_size--;
			mkNewHeader(B_FILEMARK, 0, 0, sense_flg);
			mkEODHeader(sense_flg);
		}
		break;

	default:
		syslog(LOG_DAEMON|LOG_ERR, "*** Unsupported command %d ***",
				SCpnt[0]);
		logSCSICommand(SCpnt);
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sense_flg);
		break;
	}
	return ret;
}

/*
 * Attempt to load PCL - i.e. Open datafile and read in BOT header & MAM
 *
 * Return 0 on failure, 1 on success
 */

static int load_tape(char *PCL, uint8_t *sense_flg) {
	loff_t nread;
	u64 fg = 0;	// TapeAlert flags

	bytesWritten = 0;	// Global - Bytes written this load
	bytesRead = 0;		// Global - Bytes rearead this load

	sprintf(currentMedia ,"%s/%s", HOME_PATH, PCL);
	syslog(LOG_DAEMON|LOG_INFO, "%s", currentMedia);
	if((datafile = open(currentMedia, O_RDWR|O_LARGEFILE)) == -1) {
		syslog(LOG_DAEMON|LOG_ERR, "%s: open failed, %m", currentMedia);
		return 0; 	// Unsuccessful load
	}

	// Now read in header information from just opened datafile
	nread = read(datafile, &c_pos, sizeof(c_pos));
	if(nread < 0) {
		syslog(LOG_DAEMON|LOG_ERR, "%s: %m",
			 "Error reading header in datafile, load failed");
		close(datafile);
		return 0;	// Unsuccessful load
	} else if (nread < sizeof(c_pos)) {	// Did not read anything...
		syslog(LOG_DAEMON|LOG_ERR, "%s: %m",
				 "Error: Not a tape format, load failed");
		close(datafile);
		return 0;	// Unsuccessful load
	}
	if(c_pos.blk_type != B_BOT) {
		syslog(LOG_DAEMON|LOG_ERR,
			"Header type: %d not valid, load failed",
							c_pos.blk_type);
		close(datafile);
		return 0;	// Unsuccessful load
	}
	// FIXME: Need better validation checking here !!
	if(c_pos.next_blk != (sizeof(struct blk_header) + sizeof(struct MAM))) {
		syslog(LOG_DAEMON|LOG_ERR,
			"MAM size incorrect, load failed"
			" - Expected size: %d, size found: %" PRId64,
				sizeof(struct blk_header) + sizeof(struct MAM),
				c_pos.next_blk);
		close(datafile);
		return 0;	// Unsuccessful load
	}
	nread = read(datafile, &mam, sizeof(mam));
	if(nread < 0) {
		mediaSerialNo[0] = '\0';
		syslog(LOG_DAEMON|LOG_ERR,
					"Can not read MAM from mounted media");
		return 0;	// Unsuccessful load
	}
	// Set TapeAlert flg 32h & 35h =>
	//	Lost Statics
	//	Tape system read failure.
	if(mam.record_dirty != 0) {
		fg = 0xa000000000000ull;
		syslog(LOG_DAEMON|LOG_WARNING, "Previous unload was not clean");
	}

	max_tape_capacity = (loff_t)c_pos.blk_size * (loff_t)1048576;
	syslog(LOG_DAEMON|LOG_INFO, "Tape capacity: %" PRId64, max_tape_capacity);

	mam.record_dirty = 1;
	// Increment load count
	updateMAM(&mam, sense_flg, 1);

	/* respRewind() will clean up for us..
	 * - It also set up media type & if we can write to media
	 */
	respRewind(sense_flg);

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
		mkSenseBuf(UNIT_ATTENTION,E_CLEANING_CART_INSTALLED,sense_flg);
		break;
	default:
		mkSenseBuf(UNIT_ATTENTION,E_NOT_READY_TO_TRANSITION,sense_flg);
		break;
	}

	// Update TapeAlert flags
	setSeqAccessDevice(&seqAccessDevice,fg);
	setTapeAlert(&TapeAlert, fg);

return 1;	// Return successful load
}


/* Strip (recover) the 'Physical Cartridge Label'
 *   Well at least the data filename which relates to the same thing
 */
static char * strip_PCL(char *p, int start) {
	char *q;

	/* p += 4 (skip over 'load' string)
	 * Then keep going until '*p' is a space or NULL
	 */
	for(p += start; *p == ' '; p++)
		if('\0' == *p)
			break;
	q = p;	// Set end-of-word marker to start of word.
	for(q = p; *q != '\0'; q++)
		if(*q == ' ' || *q == '\t')
			break;
	*q = '\0';	// Set null terminated string
//	printf(":\nmedia ID:%s, p: %lx, q: %lx\n", p, &p, &q);

return p;
}

static int processMessageQ(char *mtext, uint8_t *sense_flg) {
	char * pcl;
	char s[128];

	if(verbose)
		syslog(LOG_DAEMON|LOG_NOTICE, "Q msg : %s", mtext);

	/* Tape Load message from Library */
	if(! strncmp(mtext, "lload", 5)) {
		if( ! inLibrary) {
			syslog(LOG_DAEMON|LOG_NOTICE,
						"lload & drive not in library");
			return (0);
		}

		if(tapeLoaded) {
			syslog(LOG_DAEMON|LOG_NOTICE, "Tape already mounted");
			send_msg("Load failed", LIBRARY_Q);
		} else {
			pcl = strip_PCL(mtext, 6); // 'lload ' => offset of 6
			tapeLoaded = load_tape(pcl, sense_flg);
			if(tapeLoaded)
				sprintf(s, "Loaded OK: %s\n", pcl);
			else
				sprintf(s, "Load failed: %s\n", pcl);
			send_msg(s, LIBRARY_Q);
		}
	}

	/* Tape Load message from User space */
	if(! strncmp(mtext, "load", 4)) {
		if(inLibrary)
			syslog(LOG_DAEMON|LOG_WARNING,
					"Warn: Tape assigned to library");
		if(tapeLoaded) {
			syslog(LOG_DAEMON|LOG_NOTICE, "Tape already mounted");
		} else {
			pcl = strip_PCL(mtext, 4);
			tapeLoaded = load_tape(pcl, sense_flg);
		}
	}

	if(! strncmp(mtext, "unload", 6)) {
		if(tapeLoaded) {
			mam.record_dirty = 0;
			// Don't update load count on unload -done at load time
			updateMAM(&mam, sense_flg, 0);
			close(datafile);
			tapeLoaded = 0;
			OK_to_write = 0;
			clearWORM();
			syslog(LOG_DAEMON|LOG_INFO,
					"Library requested tape unload");
			close(datafile);
		} else
			syslog(LOG_DAEMON|LOG_NOTICE, "Tape not mounted");

		tapeLoaded = 0;
	}

	if(! strncmp(mtext, "exit", 4)) {
		syslog(LOG_DAEMON|LOG_NOTICE, "Notice to exit : %s", mtext);
		return 1;
	}

	if(! strncmp(mtext, "Register", 8)) {
		inLibrary = 1;
		syslog(LOG_DAEMON|LOG_NOTICE,
				"Notice from Library controller : %s", mtext);
	}

	if(! strncmp(mtext, "verbose", 7)) {
		if(verbose)
			verbose--;
		else
			verbose = 3;
		syslog(LOG_DAEMON|LOG_NOTICE, "Verbose: %s (%d)",
				 verbose ? "enabled" : "disabled", verbose);
	}

	if(! strncmp(mtext, "TapeAlert", 9)) {
		u64 flg = 0L;
		sscanf(mtext, "TapeAlert %" PRIx64, &flg);
		setTapeAlert(&TapeAlert, flg);
		setSeqAccessDevice(&seqAccessDevice, flg);
	}

	if(! strncmp(mtext, "debug", 5)) {
		if(debug) {
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
	if( (queue_id = msgget(QKEY, IPC_CREAT | QPERM)) == -1)
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
	if((mp = alloc_mode_page(1, m, 12))) {
		// Init rest of page data..
	}

	// Disconnect-Reconnect: SPC-3 7.4.8
	if((mp = alloc_mode_page(2, m, 16))) {
		mp->pcodePointer[2] = 50; // Buffer full ratio
		mp->pcodePointer[3] = 50; // Buffer enpty ratio
		mp->pcodePointer[10] = 4;
	}

	// Control: SPC-3 7.4.6
	if((mp = alloc_mode_page(0x0a, m, 12))) {
		// Init rest of page data..
	}

	// Data compression: SSC-3 8.3.2
	if((mp = alloc_mode_page(0x0f, m, 16))) {
		// Init rest of page data..
		mp->pcodePointer[2] = 0xc0;	// Set Data Compression Enable
		mp->pcodePointer[3] = 0x80;	// Set Data Decompression Enable
		lp = (u32 *)&mp->pcodePointer[4];
		*lp = htonl(COMPRESSION_TYPE);	// Compression Algorithm
		lp++;
		*lp = htonl(COMPRESSION_TYPE);	// Decompression algorithm
	}

	// Device Configuration: SSC-3 8.3.3
	if((mp = alloc_mode_page(0x10, m, 16))) {
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
	if((mp = alloc_mode_page(0x11, m, 16))) {
		// Init rest of page data..
	}

	// Extended: SPC-3 - Not used here.
	// Extended Device (Type Specific): SPC-3 - Not used here

	// Power condition: SPC-3 7.4.12
	if((mp = alloc_mode_page(0x1a, m, 12))) {
		// Init rest of page data..
	}

	// Informational Exception Control: SPC-3 7.4.11 (TapeAlert)
	if((mp = alloc_mode_page(0x1c, m, 12))) {
		mp->pcodePointer[2] = 0x08;
		mp->pcodePointer[3] = 0x03;
	}

	// Medium configuration: SSC-3 8.3.7
	if((mp = alloc_mode_page(0x1d, m, 32))) {
		// Init rest of page data..
	}
}

int
main(int argc, char *argv[])
{
	int cdev, k;
	int ret;
	int vx_status;
	int q_priority = 0;
	int exit_status = 0;
	u32 pollInterval = 50000;
	u32 serialNo = 0L;
	u32  byteCount;
	uint8_t * buf;
	uint8_t * SCpnt;
	struct vtl_header vtl_head;

	pid_t pid;

	char * progname = argv[0];

	char * dataFile = HOME_PATH;
	char * name = "vtl";
	int	minor = 0;

	/* Output file pointer (data file) */
	int ofp = -1;

	/* Message Q */
	int	mlen, r_qid;
	struct q_entry r_entry;

	if(argc < 2) {
		usage(argv[0]);
		printf("  -- Not enough parameters --\n");
		exit(1);
	}

	while(argc > 0) {
		if(argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				debug++;
				verbose = 9;	// If debug, make verbose...
				break;
			case 'f':
				if(argc > 1) {
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
				if(argc > 1)
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

	if(q_priority <= 0 || q_priority > MAXPRIOR) {
		usage(progname);
		if(q_priority == 0)
			puts("    queue priority not specified\n");
		else
			printf("    queue prority out of range [1 - %d]\n",
						MAXPRIOR);
		exit(1);
	}
	minor = q_priority;	// Minor == Message Queue priority

	openlog(progname, LOG_PID, LOG_DAEMON|LOG_WARNING);
	syslog(LOG_DAEMON|LOG_INFO, "%s: version %s", progname, Version);
	if(verbose)
		printf("%s: version %s\n", progname, Version);

	if((cdev = chrdev_open(name, minor)) == -1) {
		syslog(LOG_DAEMON|LOG_ERR,
				"Could not open /dev/%s%d: %m", name, minor);
		fflush(NULL);
		exit(1);
	}

	rw_buf = (uint8_t *)malloc(1024 * 1024 * 1);	// 1M
	if(NULL == rw_buf) {
		perror("Could not alloc memory -- exiting");
		exit(1);
	}

	k = TYPE_TAPE;
	if (ioctl(cdev, VX_TAPE_ONLINE, &k) < 0) {
		syslog(LOG_DAEMON|LOG_ERR, "Failed to connect to /dev/%s%d: %m",
								name, minor);
		perror("Failed bringing unit online");
		exit(1);
	}

	if (ioctl(cdev, VX_TAPE_FIFO_SIZE, &bufsize) < 0) {
		perror("Failed quering FIFO size");
		exit(1);
	} else {
		syslog(LOG_DAEMON|LOG_INFO, "Size of kfifo is %d", bufsize);
		buf = (uint8_t *)malloc(bufsize);
		if(NULL == buf) {
			perror("Problems allocating memory");
			exit(1);
		}
	}

	currentMedia = (char *)malloc(sizeof(dataFile) + 1024);
	if(NULL == currentMedia) {
		perror("Could not allocate memory -- exiting");
		exit(1);
	}

	strncpy(currentMedia, dataFile, sizeof(dataFile));

	/* Clear Sense arr */
	memset(sense, 0, sizeof(sense));
	reset = 1;

	init_mode_pages(sm);

	/* Initialise message queue as necessary */
	if((r_qid = init_queue()) == -1) {
		printf("Could not initialise message queue\n");
		exit(1);
	}

	/* If debug, don't fork/run in background */
	if( ! debug) {
		switch(pid = fork()) {
		case 0:         /* Child */
                	break;
        	case -1:
                	printf("Failed to fork daemon\n");
                	break;
        	default:
			if(verbose)
                		printf("vtltape process PID is %d\n", (int)pid);
                	break;
        	}
 
		/* Time for the parent to terminate */
		if(pid != 0)
        		exit(pid != -1 ? 0 : 1);

	}

	oom_adjust();
	
	for(;;) {
		/* Check for anything in the messages Q */
		mlen = msgrcv(r_qid, &r_entry, MAXOBN, q_priority, IPC_NOWAIT);
		if(mlen > 0) {
			r_entry.mtext[mlen] = '\0';
			exit_status =
				processMessageQ(r_entry.mtext, &request_sense);
		} else if (mlen < 0) {
			if((r_qid = init_queue()) == -1) {
				syslog(LOG_DAEMON|LOG_ERR,
					"Can not open message queue: %m");
			}
		}
		if(exit_status)	/* Received a 'exit' message */
			goto exit;
		if ((ret = ioctl(cdev, VX_TAPE_POLL_STATUS, &vx_status)) < 0) {
			syslog(LOG_DAEMON|LOG_WARNING, "ret: %d : %m", ret);
		} else {
			fflush(NULL);	/* So I can pipe debug o/p thru tee */
			switch(ret) {
			case STATUS_QUEUE_CMD:	// The new & improved method
				/* Get the SCSI cdb from vtl driver
				 * - Returns SCSI command S/No. */
				getCommand(cdev, &vtl_head, vx_status);

				SCpnt = (uint8_t *)&vtl_head.cdb;
				serialNo = htonl(vtl_head.serialNo);

				/* Interpret the SCSI command & process
				-> Returns no. of bytes to send back to kernel
				 */
				byteCount = processCommand(cdev, SCpnt, buf,
								&request_sense);

				/* Complete SCSI cmd processing */
				completeSCSICommand(cdev,
						serialNo,	// SCSI S/No.
						buf,		// Data buf
						sense,		// Sense buf
						&request_sense,	// sense valid?
						byteCount);	// total xfer

				/* Something to do, reduce poll time */
				pollInterval = 1000;
				break;

			case STATUS_OK:
				/* While nothing to do, increase
				 * time we sleep before polling again.
				 */
				if(pollInterval < 1000000)
					pollInterval += 1000;

				usleep(pollInterval);
				break;

			default:
				syslog(LOG_DAEMON|LOG_NOTICE,
					"ioctl(0x%x) returned %d: argv %d",
					VX_TAPE_POLL_STATUS, ret, vx_status);
				sleep(1);
				break;
			}	// End switch(vx_status)
		}
	}

exit:
	close(cdev);
	close(ofp);
	free(buf);
	free(rw_buf);
	exit(0);
}

