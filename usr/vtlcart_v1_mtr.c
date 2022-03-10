/*
 * Original tape format.
 *
 * Basically a double-linked-list headers followed by the block of data.
 * Each header describes the header type (data, filemark, EOD etc
 * along with pointer to previous and next block
 *
 * Copyright (C) 2005 - 2010 Mark Harvey       markh794@gmail.com
 *                                          mark.harvey at nutanix.com
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

/* for unistd.h pread() and pwrite() prototypes */
#define	_XOPEN_SOURCE	500

#include <sys/syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "mhvtl_scsi.h"
#include "mhvtl_list.h"
#include "vtltape.h"
#include "be_byteshift.h"

#define B_BOT           14      /* Beginning of Tape TAPE_FMT_VERSION 2 */

#ifndef MHVTL_DEBUG
#error DEBUG use only !
#endif

struct raw_header {
	loff_t		prev_blk;
	loff_t		curr_blk;
	loff_t		next_blk;
	struct blk_header hdr;
	char		pad[512 - (3 * sizeof(loff_t)) -
					sizeof(struct blk_header)];
};

static struct raw_header raw_pos;
static int datafile = -1;
static uint8_t MediumType;
static char currentMedia[1024];

/* GLobally visible variables. */

struct MAM mam;
struct blk_header *c_pos = &raw_pos.hdr;

int OK_to_write;


/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

static int read_header(struct raw_header *h, uint8_t *sam_stat)
{
	MHVTL_DBG(1, "read_header");

	loff_t nread;

	nread = read(datafile, h, sizeof(*h));
	if (nread < 0) {
		sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return -1;
	} else if (nread != sizeof(*h)) {
		sam_medium_error(E_END_OF_DATA, sam_stat);
		return -1;
	}
	return 0;
}

static loff_t position_to_curr_header(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "position_to_curr_header");
	return (lseek64(datafile, raw_pos.curr_blk, SEEK_SET));
}

static int skip_to_next_header(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "skip_to_next_header");
	if (raw_pos.hdr.blk_type == B_EOD) {
		sam_blank_check(E_END_OF_DATA, sam_stat);
		MHVTL_DBG(1, "End of data detected while forward SPACEing!!");
		return -1;
	}

	if (raw_pos.next_blk != lseek64(datafile, raw_pos.next_blk, SEEK_SET)) {
		sam_medium_error(E_SEQUENTIAL_POSITION_ERR, sam_stat);
		MHVTL_DBG(1, "Unable to seek to next block header");
		return -1;
	}
	if (read_header(&raw_pos, sam_stat)) {
		sam_medium_error(E_SEQUENTIAL_POSITION_ERR, sam_stat);
		MHVTL_DBG(1, "Unable to read next block header");
		return -1;
	}
	/* Position to start of header (rewind over header) */
	if (raw_pos.curr_blk != position_to_curr_header(sam_stat)) {
		sam_medium_error(E_SEQUENTIAL_POSITION_ERR, sam_stat);
		MHVTL_DBG(1, "Error position in datafile. Offset: %" PRId64,
				raw_pos.curr_blk);
		return -1;
	}
	return 0;
}

static int skip_to_prev_header(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "skip_to_prev_header");
	/* Position to previous header */
	MHVTL_DBG(3, "Positioning to raw_pos.prev_blk: %" PRId64,
				raw_pos.prev_blk);
	if (raw_pos.prev_blk != lseek64(datafile, raw_pos.prev_blk, SEEK_SET)) {
		sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
		MHVTL_DBG(1, "Error position in datafile !!");
		return -1;
	}
	/* Read in header */
	MHVTL_DBG(3, "Reading in header: %d bytes", (int)sizeof(raw_pos));

	if (read_header(&raw_pos, sam_stat)) {
		MHVTL_DBG(1, "Error reading datafile while reverse SPACEing");
		return -1;
	}
	if (raw_pos.hdr.blk_type == B_BOT) {
		MHVTL_DBG(3, "Found Beginning Of Tape, "
				"Skipping to next header..");
		skip_to_next_header(sam_stat);
		sam_medium_error(E_BOM, sam_stat);
		MHVTL_DBG(3, "Found BOT!!");
		return -1;
	}

	/* Position to start of header (rewind over header) */
	if (raw_pos.curr_blk != position_to_curr_header(sam_stat)) {
		sam_medium_error(E_SEQUENTIAL_POSITION_ERR, sam_stat);
		MHVTL_DBG(1, "Error position in datafile !!");
		return -1;
	}
	MHVTL_DBG(3, "Rewinding over header just read in: "
			"curr_position: %" PRId64, raw_pos.curr_blk);
	return 0;
}

/*
 * Create & write a new block header
 *
 * Returns:
 * == 0, success
 * != 0, failure
*/

static int mkNewHeader(uint32_t type, int blk_size, int comp_size,
	const struct encryption *cp, uint8_t *sam_stat)
{
	struct raw_header h;
	MHVTL_DBG(1, "mkNewHeader");

	memset(&h, 0, sizeof(h));

	h.hdr.blk_type = type;	/* Header type */
	h.hdr.blk_flags = 0;
	h.hdr.blk_number = raw_pos.hdr.blk_number;

	if (type != B_DATA) {
		h.hdr.blk_size = 0;
		h.hdr.disk_blk_size = 0;
	} else {
		h.hdr.blk_size = blk_size;	/* Size of uncompressed data */

		if (comp_size) {
			h.hdr.blk_flags |= BLKHDR_FLG_COMPRESSED;
			h.hdr.disk_blk_size = comp_size;
		} else {
			h.hdr.disk_blk_size = blk_size;
		}

		if (cp != NULL) {
			int i;

			h.hdr.blk_flags |= BLKHDR_FLG_ENCRYPTED;
			h.hdr.encryption.ukad_length = cp->ukad_length;
			for (i = 0; i < cp->ukad_length; ++i)
				h.hdr.encryption.ukad[i] = cp->ukad[i];

			h.hdr.encryption.akad_length = cp->akad_length;
			for (i = 0; i < cp->akad_length; ++i)
				h.hdr.encryption.akad[i] = cp->akad[i];

			h.hdr.encryption.key_length = cp->key_length;
			for (i = 0; i < cp->key_length; ++i)
				h.hdr.encryption.key[i] = cp->key[i];
		}
	}

	/* Update current position */
	h.curr_blk = lseek64(datafile, 0, SEEK_CUR);

	/* If we are writing a new EOD marker,
	 *  - then set next pointer to itself
	 * else
	 *  - Set pointer to next header (header size + size of data)
	 */
	if (type == B_EOD)
		h.next_blk = h.curr_blk;
	else
		h.next_blk = h.curr_blk + h.hdr.disk_blk_size + sizeof(h);

	if (h.curr_blk == raw_pos.curr_blk) {
	/* If current pos == last header read in we are about to overwrite the
	 * current header block
	 */
		h.prev_blk = raw_pos.prev_blk;
		h.hdr.blk_number = raw_pos.hdr.blk_number;
	} else if (h.curr_blk == raw_pos.next_blk) {
	/* New header block at end of data file.. */
		h.prev_blk = raw_pos.curr_blk;
		h.hdr.blk_number = raw_pos.hdr.blk_number + 1;
	} else {
		MHVTL_DBG(1, "Position error blk No: %d, Pos: %" PRId64
			", Exp: %" PRId64,
				h.hdr.blk_number, h.curr_blk, raw_pos.curr_blk);
		sam_medium_error(E_SEQUENTIAL_POSITION_ERR, sam_stat);
		return -1;
	}

	if (write(datafile, &h, sizeof(h)) != sizeof(h)) {
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		MHVTL_DBG(1, "Write failure, pos: %" PRId64 ": %s",
						h.curr_blk, strerror(errno));
		return -1;
	}

	/*
	 * Write was successful, update raw_pos with this header block.
	 */

	memcpy(&raw_pos, &h, sizeof(h)); /* Update where we think we are.. */

	return 0;
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

static int mkEODHeader(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "mkEODHeader");
	if (mkNewHeader(B_EOD, 0, 0, NULL, sam_stat))
		return -1;

	if (MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 1;

	/* If we have just written a END OF DATA marker,
	 * rewind to just before it. */
	/* Position to start of header (rewind over header) */
	if (raw_pos.curr_blk != position_to_curr_header(sam_stat)) {
		sam_medium_error(E_SEQUENTIAL_POSITION_ERR, sam_stat);
		MHVTL_DBG(1, "Failed to write EOD header");
		return -1;
	}
	return 0;
}

/*
 *
 */

static int skip_prev_filemark(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "skip_prev_filemark");

	if (raw_pos.hdr.blk_type == B_FILEMARK)
		raw_pos.hdr.blk_type = B_NOOP;
	while (raw_pos.hdr.blk_type != B_FILEMARK) {
		if (raw_pos.hdr.blk_type == B_BOT) {
			sam_no_sense(NO_SENSE, E_BOM, sam_stat);
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
	MHVTL_DBG(1, "skip_next_filemark");
	/* While blk header is NOT a filemark, keep skipping to next header */
	while (raw_pos.hdr.blk_type != B_FILEMARK) {
		/* END-OF-DATA -> Treat this as an error - return.. */
		if (raw_pos.hdr.blk_type == B_EOD) {
			sam_blank_check(E_END_OF_DATA, sam_stat);
			MHVTL_DBG(2, "%s", "Found end of media");
			if (MediumType == MEDIA_TYPE_WORM)
				OK_to_write = 1;
			return -1;
		}
		if (skip_to_next_header(sam_stat))
			return -1;	/* On error */
	}
	/* Position to header AFTER the FILEMARK.. */
	if (skip_to_next_header(sam_stat))
		return -1;

	return 0;
}

static int tape_loaded(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "tape_loaded");
	switch (datafile != -1) {
		return 1;
	}
	sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
	return 0;
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int position_to_eod(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "position_to_eod");
	if (!tape_loaded(sam_stat))
		return -1;

	while (raw_pos.hdr.blk_type != B_EOD) {
		if (skip_to_next_header(sam_stat))
			return -1;
	}

	if (MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 1;
	return 0;
}

/*
 * Rewind 'tape'.
 */
static int rawRewind(uint8_t *sam_stat)
{
	off64_t retval;

	MHVTL_DBG(1, "rawRewind");
	/* Start at beginning of datafile.. */
	retval = lseek64(datafile, 0L, SEEK_SET);
	if (retval < 0) {
		MHVTL_DBG(1, "Can't seek to beginning of file: %s",
			strerror(errno));
		return -1;
	}

	/*
	 * Read header..
	 * If this is not the BOT header we are in trouble
	 */
	if (read_header(&raw_pos, sam_stat))
		return -1;

	return 0;
}

/*
 * Rewind to beginning of data file and the position to first data header.
 *
 * Return 0 -> Not loaded.
 *        1 -> Load OK
 *        2 -> format corrupt.
 */
int rewind_tape(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "rewind_tape");
	if (rawRewind(sam_stat)) {
		sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return 2;
	}

	if (raw_pos.hdr.blk_type != B_BOT) {
		sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return 2;
	}

	if (skip_to_next_header(sam_stat))
		return 2;

	switch (MediumType) {
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
		if (raw_pos.hdr.blk_type != B_EOD)
			OK_to_write = 0;

		/* Check that this header is a filemark and the next header
		 *  is End of Data. If it is, we are OK to write
		 */
		if (raw_pos.hdr.blk_type == B_FILEMARK) {
			skip_to_next_header(sam_stat);
			if (raw_pos.hdr.blk_type == B_EOD)
				OK_to_write = 1;
		}
		/* Now we have to go thru thru the rewind again.. */
		if (rawRewind(sam_stat)) {
			sam_medium_error(E_MEDIUM_FMT_CORRUPT,
								sam_stat);
			return 2;
		}

		/* No need to do all previous error checking... */
		skip_to_next_header(sam_stat);
		break;
	case MEDIA_TYPE_DATA:
		OK_to_write = 1;	/* Reset flag to OK. */
		break;
	}

	MHVTL_DBG(1, "Media is %s",
				(OK_to_write) ? "writable" : "not writable");

	return 1;
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int position_to_block(uint32_t blk_no, uint8_t *sam_stat)
{
	MHVTL_DBG(1, "position_to_block");
	if (!tape_loaded(sam_stat))
		return -1;

	if (MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	if (blk_no < raw_pos.hdr.blk_number &&
				raw_pos.hdr.blk_number - blk_no > blk_no) {
		if (rewind_tape(sam_stat))
			return -1;
	}
	while (raw_pos.hdr.blk_number != blk_no) {
		if (raw_pos.hdr.blk_number > blk_no) {
			if (skip_to_prev_header(sam_stat) == -1)
				return -1;
		} else {
			if (skip_to_next_header(sam_stat) == -1)
				return -1;
		}
	}
	return 0;
}

/*
 * 'count' is in the range 0xff000001 to 0x00ffffff
 *
 * Returns:
 * == 0, success
 * != 0, failure
*/

int position_blocks(int32_t count, uint8_t *sam_stat)
{
	MHVTL_DBG(1, "position_blocks");
	if (!tape_loaded(sam_stat))
		return -1;

	if (MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	if (count < 0) {
		for (; count < 0; count++) {
			if (skip_to_prev_header(sam_stat))
				return -1;
			if (raw_pos.hdr.blk_type == B_FILEMARK) {
				sam_no_sense(SD_FILEMARK, E_MARK, sam_stat);
				return -1;
			}
		}
	} else {
		for (; count > 0; count--) {
			if (skip_to_next_header(sam_stat))
				return -1;
			if (raw_pos.hdr.blk_type == B_FILEMARK) {
				sam_no_sense(SD_FILEMARK, E_MARK, sam_stat);
				return -1;
			}
		}
	}
	return 0;
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int position_filemarks(int32_t count, uint8_t *sam_stat)
{
	MHVTL_DBG(1, "position_filemarks");
	if (!tape_loaded(sam_stat))
		return -1;

	if (MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	if (count < 0) {
		for (; count < 0; count++)
			if (skip_prev_filemark(sam_stat))
				return -1;
	} else {
		for (; count > 0; count--)
			if (skip_next_filemark(sam_stat))
				return -1;
	}
	return 0;
}

int position_blocks_forw(uint32_t count, uint8_t *sam_stat)
{
	MHVTL_DBG(1, "position_blks_forw");
	return position_blocks(count, sam_stat);
}

int position_blocks_back(uint32_t count, uint8_t *sam_stat)
{
	MHVTL_DBG(1, "position_blks_back");
	return position_blocks(count > 0 ? -count : count, sam_stat);
}

int position_filemarks_forw(uint32_t count, uint8_t *sam_stat)
{
	MHVTL_DBG(1, "position_filemarks_forw");
	return position_filemarks(count, sam_stat);
}


int position_filemarks_back(uint32_t count, uint8_t *sam_stat)
{
	MHVTL_DBG(1, "position_filemarks_back");
	return position_filemarks(count > 0 ? -count : count, sam_stat);
}



/*
 * Writes data in struct MAM back to beginning of datafile..
 * Returns 0 if nothing written or -1 on error
 */
int rewriteMAM(uint8_t *sam_stat)
{
	loff_t nwrite = 0;

	MHVTL_DBG(1, "rewriteMAM");
	/* Rewrite MAM data */
	nwrite = pwrite(datafile, &mam, sizeof(mam), sizeof(struct blk_header));
	if (nwrite != sizeof(mam)) {
		sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return -1;
	}
	MediumType = mam.MediumType;

	return 0;
}

/*
 * Returns:
 * == 0, the new PCL was successfully created.
 * == 2, the PCL (probably) already existed.
 * == 1, an error occurred.
*/

int create_tape(const char *pcl, const struct MAM *mamp, uint8_t *sam_stat)
{
	char newMedia[1024];
	struct raw_header h;

	MHVTL_DBG(1, "create_tape");

	/* Attempt to create the new PCL.
	 * It will fail if the PCL already exists.
	 */
	sprintf((char *)newMedia, "%s/%s", MHVTL_HOME_PATH, pcl);
	syslog(LOG_DAEMON|LOG_INFO, "%s being created", newMedia);
	datafile = open((char *)newMedia, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	if (datafile == -1) {
		perror("Failed creating file");
		return 2;
	}

	/* Write a B_BOT record consisting of the B_BOT header plus the MAM. */
	memset(&h, 0, sizeof(h));
	h.next_blk = sizeof(*mamp) + sizeof(h);
	h.hdr.blk_type = B_BOT;
	h.hdr.blk_size = ntohl(mamp->max_capacity) / 1048576;

	if (write(datafile, &h, sizeof(h)) != sizeof(h)) {
		perror("Unable to write header");
		unlink(newMedia);
		return 1;
	}

	if (write(datafile, mamp, sizeof(*mamp)) != sizeof(*mamp)) {
		perror("Unable to write MAM");
		unlink(newMedia);
		return 1;
	}

	/* Write a B_EOD record. */

	memset(&h, 0, sizeof(h));
	h.curr_blk = lseek64(datafile, 0, SEEK_CUR);
	h.next_blk = h.curr_blk;
	h.hdr.blk_type = B_EOD;

	if (write(datafile, &h, sizeof(h)) != sizeof(h)) {
		perror("Unable to write header");
		unlink(newMedia);
		return 1;
	}

	close(datafile);
	datafile = -1;
	return 0;
}

/*
 * Attempt to load PCL - i.e. Open datafile and read in BOT header & MAM
 *
 * Returns:
 * == 0 -> Load OK
 * == 1 -> Tape already loaded.
 * == 2 -> format corrupt.
 * == 3 -> cartridge does not exist or cannot be opened.
 */

int load_tape(const char *pcl, uint8_t *sam_stat)
{
	loff_t nread;
	uint32_t version = 0;

	MHVTL_DBG(1, "load_tape");
/* KFRDEBUG - sam_stat needs updates in lots of places here. */
#if NOTDEF
	if (datafile == -1)
		/* return 1; */	/* don't return 1 here */
		return 0;
#endif

	sprintf(currentMedia, "%s/%s", MHVTL_HOME_PATH, pcl);
	/* MHVTL_DBG(2, "Opening file/media %s", currentMedia); */
	MHVTL_LOG("Opening file/media %s", currentMedia);
	datafile = open(currentMedia, O_RDWR|O_LARGEFILE);
	if (datafile == -1) {
		MHVTL_DBG(1, "%s: open file/media failed, %s", currentMedia,
			strerror(errno));
		return 3;	/* Unsuccessful load */
	}

	/* Now read in header information from just opened datafile */
	nread = read(datafile, &raw_pos, sizeof(raw_pos));
	if (nread < 0) {
		MHVTL_LOG("%s: %s",
			 "Error reading header in datafile, load failed",
			strerror(errno));
		close(datafile);
		datafile = -1;
		return 4;	/* Unsuccessful load */
	} else if (nread < sizeof(raw_pos)) {	/* Did not read anything... */
		MHVTL_LOG("%s: %s",
				 "Error: Not a tape format, load failed",
				strerror(errno));
		close(datafile);
		datafile = -1;
		return 5;
	}
	if (raw_pos.hdr.blk_type != B_BOT) {
		MHVTL_LOG("Header type: %d not valid, load failed",
							raw_pos.hdr.blk_type);
		close(datafile);
		datafile = -1;
		return 6;
	}
	/* FIXME: Need better validation checking here !! */
	 if (raw_pos.next_blk != (sizeof(raw_pos) + sizeof(mam))) {
		MHVTL_LOG("MAM size incorrect, load failed"
			" - Expected size: %d, size found: %" PRId64,
			(int)(sizeof(raw_pos) + sizeof(mam)),
				raw_pos.next_blk);
		close(datafile);
		datafile = -1;
		return 7;	/* Unsuccessful load */
	}
	nread = read(datafile, &mam, sizeof(mam));
	if (nread < 0) {
		MHVTL_LOG("Can not read MAM from mounted media, %s",
							strerror(errno));
		close(datafile);
		datafile = -1;
		return 8;	/* Unsuccessful load */
	}
	if (nread != sizeof(mam)) {
		MHVTL_LOG("Can not read MAM from mounted media, "
				"insufficient data");
		close(datafile);
		datafile = -1;
		return 9;	/* Unsuccessful load */
	}

	version = mam.tape_fmt_version;
	if (version != TAPE_FMT_VERSION) {
		MHVTL_LOG("Incorrect media format %lu", version);
		sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
		close(datafile);
		datafile = -1;
		return 10;
	}

	MediumType = mam.MediumType;
	c_pos = &raw_pos.hdr;
	return 0;
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int write_filemarks(uint32_t count, uint8_t *sam_stat)
{
	MHVTL_DBG(1, "write_filemarks");
	if (!tape_loaded(sam_stat))
		return -1;

	if (count > 0) {
		while (count > 0) {
			count--;
			if (mkNewHeader(B_FILEMARK, 0, 0, NULL, sam_stat))
				return -1;
		}
		mkEODHeader(sam_stat);
	}
	return 0;
}

int write_tape_block(const uint8_t *buf, uint32_t blk_size,
	uint32_t comp_size, const struct encryption *cp, uint8_t *sam_stat)
{
	loff_t	nwrite;
	uint32_t iosize;

	MHVTL_DBG(1, "write_tape_block");
	if (!tape_loaded(sam_stat))
		return -1;

	/* If comp_size is non-zero then the data is compressed, so use
	   comp_size for the I/O size.  If comp_size is zero, the data is
	   non-compressed, so use the blk_size as the I/O size.
	*/

	iosize = comp_size ? comp_size : blk_size;

	if (mkNewHeader(B_DATA, blk_size, comp_size, cp, sam_stat)) {
		MHVTL_DBG(1, "Failed to write header");
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		return -1;
	}

	/* now write the block of data.. */
	nwrite = write(datafile, buf, iosize);
	if (nwrite <= 0) {
		MHVTL_DBG(1, "failed to write %d bytes, %s", iosize,
			strerror(errno));
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		return -1;
	} else if (nwrite != iosize) {
		MHVTL_DBG(1, "Did not write all data");
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		return -1;
	}

	/* Write END-OF-DATA marker */
	if (mkEODHeader(sam_stat)) {
		MHVTL_DBG(1, "Did not write EOD");
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		return -1;
	}
	return 0;
}

void unload_tape(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "unload_tape");
	if (datafile >= 0) {
		close(datafile);
		datafile = -1;
	}
}

uint32_t read_tape_block(uint8_t *buf, uint32_t buf_size, uint8_t *sam_stat)
{
	loff_t nread;
	uint32_t size;

	MHVTL_DBG(1, "read_tape_block");
	if (!tape_loaded(sam_stat))
		return -1;

	size = raw_pos.hdr.disk_blk_size;
	if (size > buf_size)
		size = buf_size;

	nread = read(datafile, buf, size);

	/* Now read in subsequent header */
	skip_to_next_header(sam_stat);

	return nread;
}

uint64_t current_tape_offset(void)
{
	if (datafile != -1)
		return raw_pos.curr_blk;

	return 0;
}

void print_raw_header(void)
{
	printf("Hdr:");
	switch (raw_pos.hdr.blk_type) {
	case B_DATA:
		if ((raw_pos.hdr.blk_flags &&
			(BLKHDR_FLG_COMPRESSED | BLKHDR_FLG_ENCRYPTED)) ==
				(BLKHDR_FLG_COMPRESSED | BLKHDR_FLG_ENCRYPTED))
			printf(" Encrypt/Comp data");
		else if (raw_pos.hdr.blk_flags & BLKHDR_FLG_ENCRYPTED)
			printf("    Encrypted data");
		else if (raw_pos.hdr.blk_flags & BLKHDR_FLG_COMPRESSED)
			printf("   Compressed data");
			else
		printf("             data");

		printf("(%02x), sz %6d/%-6d, Blk No.: %u, prev %" PRId64
			", cur %" PRId64 ", next %" PRId64 "\n",
			raw_pos.hdr.blk_type,
			raw_pos.hdr.disk_blk_size,
			raw_pos.hdr.blk_size,
			raw_pos.hdr.blk_number,
			raw_pos.prev_blk,
			raw_pos.curr_blk,
			raw_pos.next_blk);
		if (raw_pos.hdr.blk_flags & BLKHDR_FLG_ENCRYPTED)
			printf("   => Encr key length %d, ukad length %d, "
				"akad length %d\n",
				raw_pos.hdr.encryption.key_length,
				raw_pos.hdr.encryption.ukad_length,
				raw_pos.hdr.encryption.akad_length);
		break;
	case B_FILEMARK:
		printf("          Filemark");
		printf("(%02x), sz %13d, Blk No.: %u, prev %" PRId64
			", cur %" PRId64 ", next %" PRId64 "\n",
			raw_pos.hdr.blk_type,
			raw_pos.hdr.blk_size,
			raw_pos.hdr.blk_number,
			raw_pos.prev_blk,
			raw_pos.curr_blk,
			raw_pos.next_blk);
		break;
	case B_BOT:
		printf(" Beginning of Tape");
		printf("(%02x), Capacity %6d Mbytes"
			", prev %" PRId64
			", cur %" PRId64
			", next %" PRId64 "\n",
			raw_pos.hdr.blk_type,
			raw_pos.hdr.blk_size,
			raw_pos.prev_blk,
			raw_pos.curr_blk,
			raw_pos.next_blk);
		return;
		break;
	case B_EOD:
		printf("       End of Data");
		printf("(%02x), sz %13d, Blk No.: %u, prev %" PRId64
			", cur %" PRId64 ", next %" PRId64 "\n",
			raw_pos.hdr.blk_type,
			raw_pos.hdr.blk_size,
			raw_pos.hdr.blk_number,
			raw_pos.prev_blk,
			raw_pos.curr_blk,
			raw_pos.next_blk);
		break;
	case B_NOOP:
		printf("      No Operation");
		break;
	default:
		printf("      Unknown type");
		printf("(%02x), %6d/%-6d, Blk No.: %u, prev %" PRId64
			", cur %" PRId64 ", next %" PRId64 "\n",
			raw_pos.hdr.blk_type,
			raw_pos.hdr.disk_blk_size,
			raw_pos.hdr.blk_size,
			raw_pos.hdr.blk_number,
			raw_pos.prev_blk,
			raw_pos.curr_blk,
			raw_pos.next_blk);
		break;
	}
}

static int check_for_overwrite(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "check_for_overwrite");
#if NOTDEF
	uint32_t blk_number;
	uint64_t data_offset;
	int i;

	if (raw_pos.hdr.blk_type == B_EOD)
		return 0;

	MHVTL_DBG(2, "At block %ld", (unsigned long)raw_pos.hdr.blk_number);

	/* We aren't at EOD so we are performing a rewrite.  Truncate
	 * the data and index files back to the current length.
	 */

	blk_number = raw_pos.hdr.blk_number;
	data_offset = raw_pos.data_offset;

	if (ftruncate(indxfile, blk_number * sizeof(raw_pos))) {
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		MHVTL_LOG("Index file ftruncate failure, pos: "
				"%" PRId64 ": %s",
				(uint64_t)blk_number * sizeof(raw_pos),
		strerror(errno));
		return -1;
	}
	if (ftruncate(datafile, data_offset)) {
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		MHVTL_LOG("Data file ftruncate failure, pos: "
				"%" PRId64 ": %s", data_offset,
				strerror(errno));
		return -1;
	}

	/* Update the filemark map removing any filemarks which will be
	 * overwritten.  Rewrite the filemark map so that the on-disk image
	 * of the map is consistent with the new sizes of the other two files.
	 */

	for (i = 0; i < meta.filemark_count; i++) {
		MHVTL_DBG(2, "filemarks[%d] %d", i, filemarks[i]);
		if (filemarks[i] >= blk_number) {
			MHVTL_DBG(2, "Setting filemark_count from %d to %d",
					meta.filemark_count, i);
			meta.filemark_count = i;
			return rewrite_meta_file();
		}
	}
#endif
	return 0;
}

void zero_filemark_count(void)
{
	MHVTL_DBG(1, "zero_filemark_count");
/*
	free(filemarks);
	filemark_alloc = 0;
	filemarks = NULL;

	meta.filemark_count = 0;
	rewrite_meta_file();
*/
}

int format_tape(uint8_t *sam_stat)
{
	MHVTL_DBG(1, "format_tape");
	if (!tape_loaded(sam_stat))
		return -1;

	if (check_for_overwrite(sam_stat))
		return -1;

	zero_filemark_count();

	return mkEODHeader(raw_pos.hdr.blk_number);
}

void print_filemark_count(void)
{
/*	printf("Total num of filemarks: %d\n", meta.filemark_count); */
}

void print_metadata(void)
{
/*
	int a;

	for (a = 0; a < meta.filemark_count; a++)
		printf("Filemark: %d\n", filemarks[a]);
*/
}

