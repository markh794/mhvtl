/*
 * Version 2 of tape format.
 *
 * Each media contains 3 files.
 *  - The .data file contains each block of data written to the media
 *  - The .indx file consists of an array of one raw_header structure per
 *    written tape block or filemark.
 *  - The .meta file consists of a MAM structure followed by a meta_header
 *    structure, followed by a variable-length array of filemark block numbers.
 *
 * Copyright (C) 2009 - 2010 Kevan Rehm

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

#define _FILE_OFFSET_BITS 64

#define __STDC_FORMAT_MACROS	/* for PRId64 */

/* for unistd.h pread/pwrite and fcntl.h posix_fadvise */
#define _XOPEN_SOURCE 600

#include <sys/syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#include "logging.h"
#include "scsi.h"
#include "list.h"
#include "vtltape.h"
#include "be_byteshift.h"

/* The .indx file consists of an array of one raw_header structure per
   written tape block or filemark.  There is no separate raw_header
   structure required for BOT or EOM.  The raw_header structure is padded
   out to 512 bytes to allow for the addition of fields in the future without
   breaking backwards compatibility with existing PCL indx files.
*/

struct raw_header {
	loff_t	data_offset;
	struct blk_header hdr;
	char pad[512 - sizeof(loff_t) - sizeof(struct blk_header)];
};

/* The .meta file consists of a MAM structure followed by a meta_header
   structure, followed by a variable-length array of filemark block numbers.
   Both the MAM and meta_header structures also contain padding to allow
   for future expansion with backwards compatibility.
*/

struct	meta_header {
	uint32_t filemark_count;
	char pad[512 - sizeof(uint32_t)];
};

static char currentPCL[1024];
static int datafile = -1;
static int indxfile = -1;
static int metafile = -1;

static struct raw_header raw_pos;
static struct meta_header meta;
static uint64_t eod_data_offset;
static uint32_t eod_blk_number;

#define FM_DELTA 500
static int filemark_alloc = 0;
static uint32_t *filemarks = NULL;

/* Globally visible variables. */

struct MAM mam;
struct blk_header *c_pos = &raw_pos.hdr;
int OK_to_write = 0;
char home_directory[HOME_DIR_PATH_SZ + 1];

#ifdef MHVTL_DEBUG
static char *mhvtl_block_type_desc(int blk_type)
{
	unsigned int i;

	static const struct {
		int blk_type;
		char *desc;
		} block_type_desc[] = {
			{ B_FILEMARK, "FILEMARK" },
			{ B_EOD, "END OF DATA" },
			{ B_NOOP, "NO OP"},
			{ B_DATA, "DATA" },
		};
	for (i = 0; i < ARRAY_SIZE(block_type_desc); i++)
		if (block_type_desc[i].blk_type == blk_type)
			return block_type_desc[i].desc;
	return NULL;
}
#endif

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

static int mkEODHeader(uint32_t blk_number, uint64_t data_offset)
{
	memset(&raw_pos, 0, sizeof(raw_pos));

	raw_pos.data_offset = data_offset;

	raw_pos.hdr.blk_type = B_EOD;
	raw_pos.hdr.blk_number = blk_number;

	eod_blk_number = blk_number;
	eod_data_offset = data_offset;

	OK_to_write = 1;

	return 0;
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

static int read_header(uint32_t blk_number, uint8_t *sam_stat)
{
	loff_t nread;

	if (blk_number > eod_blk_number) {
		MHVTL_ERR("Attempt to seek [%d] beyond EOD [%d]",
				blk_number, eod_blk_number);
	} else if (blk_number == eod_blk_number)
		mkEODHeader(eod_blk_number, eod_data_offset);
	else {
		nread = pread(indxfile, &raw_pos, sizeof(raw_pos),
			blk_number * sizeof(raw_pos));
		if (nread < 0) {
			MHVTL_ERR("Medium format corrupt");
			sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
			return -1;
		} else if (nread != sizeof(raw_pos)) {
			MHVTL_ERR("Failed to read next header");
			sam_medium_error(E_END_OF_DATA, sam_stat);
			return -1;
		}
	}

	MHVTL_DBG(3, "Reading header %d at offset %ld, type: %s, size: %d",
			raw_pos.hdr.blk_number,
			(unsigned long)raw_pos.data_offset,
			mhvtl_block_type_desc(raw_pos.hdr.blk_type),
			raw_pos.hdr.blk_size);
	return 0;
}

static int tape_loaded(uint8_t *sam_stat)
{
	if (datafile != -1)
		return 1;

	sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
	return 0;
}

static int rewrite_meta_file(void)
{
	ssize_t io_size, nwrite;
	size_t io_offset;

	io_size = sizeof(meta);
	io_offset = sizeof(struct MAM);
	nwrite = pwrite(metafile, &meta, io_size, io_offset);
	if (nwrite < 0) {
		MHVTL_ERR("Error writing meta_header to metafile: %s",
					strerror(errno));
		return -1;
	}
	if (nwrite != io_size) {
		MHVTL_ERR("Error writing meta_header map to metafile."
				" Expected to write %d bytes", (int)io_size);
		return -1;
	}

	io_size = meta.filemark_count * sizeof(*filemarks);
	io_offset = sizeof(struct MAM) + sizeof(meta);

	if (io_size) {
		nwrite = pwrite(metafile, filemarks, io_size, io_offset);
		if (nwrite < 0) {
			MHVTL_ERR("Error writing filemark map to metafile: %s",
					strerror(errno));
			return -1;
		}
		if (nwrite != io_size) {
			MHVTL_ERR("Error writing filemark map to metafile."
				" Expected to write %d bytes", (int)io_size);
			return -1;
		}
	}

	/* If filemarks were overwritten, the meta file may need to be shorter
	   than before.
	*/

	if (ftruncate(metafile, io_offset + io_size) < 0) {
		MHVTL_ERR("Error truncating metafile: %s", strerror(errno));
		return -1;
	}

	return 0;
}

static int check_for_overwrite(uint8_t *sam_stat)
{
	uint32_t blk_number;
	uint64_t data_offset;
	unsigned int i;

	if (raw_pos.hdr.blk_type == B_EOD)
		return 0;

	MHVTL_DBG(2, "At block %ld", (unsigned long)raw_pos.hdr.blk_number);

	/* We aren't at EOD so we are performing a rewrite.  Truncate
	   the data and index files back to the current length.
	*/

	blk_number = raw_pos.hdr.blk_number;
	data_offset = raw_pos.data_offset;

	if (ftruncate(indxfile, blk_number * sizeof(raw_pos))) {
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		MHVTL_ERR("Index file ftruncate failure, pos: "
			"%" PRId64 ": %s",
			(uint64_t)blk_number * sizeof(raw_pos),
			strerror(errno));
		return -1;
	}
	if (ftruncate(datafile, data_offset)) {
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		MHVTL_ERR("Data file ftruncate failure, pos: "
			"%" PRId64 ": %s", data_offset,
			strerror(errno));
		return -1;
	}

	/* Update the filemark map removing any filemarks which will be
	   overwritten.  Rewrite the filemark map so that the on-disk image
	   of the map is consistent with the new sizes of the other two files.
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

	return 0;
}

static int check_filemarks_alloc(uint32_t count)
{
	uint32_t new_size;

	/* See if we have enough space allocated to hold 'count' filemarks.
	   If not, realloc now.
	*/

	if (count > (uint32_t)filemark_alloc) {
		new_size = ((count + FM_DELTA - 1) / FM_DELTA) * FM_DELTA;

		filemarks = (uint32_t *)realloc(filemarks,
						new_size * sizeof(*filemarks));
		if (filemarks == NULL) {
			MHVTL_ERR("filemark map realloc failed, %s",
				strerror(errno));
			return -1;
		}
		filemark_alloc = new_size;
	}
	return 0;
}

static int add_filemark(uint32_t blk_number)
{
	/* See if we have enough space remaining to add the new filemark.  If
	   not, realloc now.
	*/

	if (check_filemarks_alloc(meta.filemark_count + 1))
			return -1;

	filemarks[meta.filemark_count++] = blk_number;

	/* Now rewrite the meta_header structure and the filemark map. */

	return rewrite_meta_file();
}

/*
 * Return 0 -> Not loaded.
 *        1 -> Load OK
 *        2 -> format corrupt.
 */

int rewind_tape(uint8_t *sam_stat)
{
	if (!tape_loaded(sam_stat))
		return -1;

	if (read_header(0, sam_stat))
		return -1;

	switch (mam.MediumType) {
	case MEDIA_TYPE_CLEAN:
		OK_to_write = 0;
		break;
	case MEDIA_TYPE_WORM:
		/* Check if this header is a filemark and the next header
		 *  is End of Data. If it is, we are OK to write
		 */

		if (raw_pos.hdr.blk_type == B_EOD ||
		    (raw_pos.hdr.blk_type == B_FILEMARK && eod_blk_number == 1))
			OK_to_write = 1;
		else
			OK_to_write = 0;
		break;
	case MEDIA_TYPE_DATA:
		OK_to_write = 1;	/* Reset flag to OK. */
		break;
	}

	MHVTL_DBG(1, "Media is%s writable", (OK_to_write) ? "" : " not");

	return 1;
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int position_to_eod(uint8_t *sam_stat)
{
	if (!tape_loaded(sam_stat))
		return -1;

	if (read_header(eod_blk_number, sam_stat))
		return -1;

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 1;

	return 0;
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int position_to_block(uint32_t blk_number, uint8_t *sam_stat)
{
	if (!tape_loaded(sam_stat))
		return -1;

	MHVTL_DBG(2, "Position to block %d", blk_number);

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	if (blk_number > eod_blk_number) {
		sam_blank_check(E_END_OF_DATA, sam_stat);
		MHVTL_DBG(1, "End of data detected while positioning");
		return position_to_eod(sam_stat);
	}

	/* Treat a position to block zero specially, as it has different
	   semantics than other blocks when the tape is WORM.
	*/

	if (blk_number == 0)
		return rewind_tape(sam_stat);
	else
		return read_header(blk_number, sam_stat);
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int position_blocks_forw(uint64_t count, uint8_t *sam_stat)
{
	uint32_t residual;
	uint32_t blk_target;
	unsigned int i;

	if (!tape_loaded(sam_stat))
		return -1;

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	blk_target = raw_pos.hdr.blk_number + count;

	/* Find the first filemark forward from our current position, if any. */

	for (i = 0; i < meta.filemark_count; i++) {
		MHVTL_DBG(3, "filemark at %ld", (unsigned long)filemarks[i]);
		if (filemarks[i] >= raw_pos.hdr.blk_number)
			break;
	}

	/* If there is one, see if it is between our current position and our
	   desired destination.
	*/

	if (i < meta.filemark_count) {
		if (filemarks[i] >= blk_target)
			return position_to_block(blk_target, sam_stat);

		residual = blk_target - raw_pos.hdr.blk_number + 1;
		if (read_header(filemarks[i] + 1, sam_stat))
			return -1;

		MHVTL_DBG(1, "Filemark encountered: block %d", filemarks[i]);
		sam_no_sense(SD_FILEMARK, E_MARK, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}

	if (blk_target > eod_blk_number) {
		residual = blk_target - eod_blk_number;
		if (read_header(eod_blk_number, sam_stat))
			return -1;

		MHVTL_DBG(1, "EOD encountered");
		sam_blank_check(E_END_OF_DATA, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}

	return position_to_block(blk_target, sam_stat);
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int position_blocks_back(uint64_t count, uint8_t *sam_stat)
{
	uint32_t residual;
	uint32_t blk_target;
	int i = -1;
	unsigned int num_filemarks = meta.filemark_count;

	if (!tape_loaded(sam_stat))
		return -1;

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	MHVTL_DBG(2, "Position before movement: %d", raw_pos.hdr.blk_number);

	if (count < raw_pos.hdr.blk_number)
		blk_target = raw_pos.hdr.blk_number - count;
	else
		blk_target = 0;

	/* Find the first filemark prior to our current position, if any. */

	if (num_filemarks > 0) {
		for (i = num_filemarks - 1; i >= 0; i--) {
			MHVTL_DBG(3, "filemark at %ld",
						(unsigned long)filemarks[i]);
			if (filemarks[i] < raw_pos.hdr.blk_number)
				break;
		}
	}

	/* If there is one, see if it is between our current position and our
	   desired destination.
	*/
	if (i >= 0) {
		if (filemarks[i] < blk_target)
			return position_to_block(blk_target, sam_stat);

		residual = raw_pos.hdr.blk_number - blk_target;
		if (read_header(filemarks[i], sam_stat))
			return -1;

		MHVTL_DBG(2, "Filemark encountered: block %d", filemarks[i]);
		sam_no_sense(SD_FILEMARK, E_MARK, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}

	if (count > raw_pos.hdr.blk_number) {
		residual = count - raw_pos.hdr.blk_number;
		if (read_header(0, sam_stat))
			return -1;

		MHVTL_DBG(1, "BOM encountered");
		sam_no_sense(SD_EOM, E_BOM, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}

	return position_to_block(blk_target, sam_stat);
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int position_filemarks_forw(uint64_t count, uint8_t *sam_stat)
{
	uint32_t residual;
	unsigned int i;

	if (!tape_loaded(sam_stat))
		return -1;

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	/* Find the block number of the first filemark greater than our
	   current position.
	*/

	for (i = 0; i < meta.filemark_count; i++)
		if (filemarks[i] >= raw_pos.hdr.blk_number)
			break;

	if (i + count - 1 < meta.filemark_count)
		return position_to_block(filemarks[i + count - 1] + 1, sam_stat);
	else {
		residual = i + count - meta.filemark_count;
		if (read_header(eod_blk_number, sam_stat))
			return -1;

		sam_blank_check(E_END_OF_DATA, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int position_filemarks_back(uint64_t count, uint8_t *sam_stat)
{
	uint32_t residual;
	int i;

	if (!tape_loaded(sam_stat))
		return -1;

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	/* Find the block number of the first filemark less than our
	   current position.
	*/

	for (i = meta.filemark_count - 1; i >= 0; i--)
		if (filemarks[i] < raw_pos.hdr.blk_number)
			break;

	if (i + 1 >= count)
		return position_to_block(filemarks[i - count + 1], sam_stat);
	else {
		residual = count - i - 1;
		if (read_header(0, sam_stat))
			return -1;

		sam_no_sense(SD_EOM, E_BOM, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}
}

/*
 * Writes data in struct MAM back to beginning of metafile..
 * Returns 0 if nothing written or -1 on error
 */

int rewriteMAM(uint8_t *sam_stat)
{
	loff_t nwrite = 0;

	if (!tape_loaded(sam_stat))
		return -1;

	/* Rewrite MAM data */

	nwrite = pwrite(metafile, &mam, sizeof(mam), 0);
	if (nwrite != sizeof(mam)) {
		sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return -1;
	}

	return nwrite;
}

/*
 * Returns:
 * == 0, the new PCL was successfully created.
 * == 2, the PCL (probably) already existed.
 * == 1, an error occurred.
*/

int create_tape(const char *pcl, const struct MAM *mamp, uint8_t *sam_stat)
{
	struct stat data_stat;
	char newMedia[1024];
	char newMedia_data[1024];
	char newMedia_indx[1024];
	char newMedia_meta[1024];
	struct passwd *pw;
	int rc = 0;

	/* Attempt to create the new PCL.  This will fail if the PCL's directory
	   or any of the PCL's three files already exist, leaving any existing
	   files as they were.
	*/

	pw = getpwnam(USR);	/* Find UID for user 'vtl' */
	if (!pw) {
		MHVTL_ERR("Failed to get UID for user '%s': %s", USR,
			strerror(errno));
		return 1;
	}

	snprintf(newMedia, ARRAY_SIZE(newMedia), "%s/%s", home_directory, pcl);

	snprintf(newMedia_data, ARRAY_SIZE(newMedia_data), "%s/data", newMedia);
	snprintf(newMedia_indx, ARRAY_SIZE(newMedia_indx), "%s/indx", newMedia);
	snprintf(newMedia_meta, ARRAY_SIZE(newMedia_meta), "%s/meta", newMedia);

	/* Check if data file already exists, nothing to create */
	if (stat(newMedia_data, &data_stat) != -1)
		return 0;

	umask(0007);
	rc = mkdir(newMedia, S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP|S_ISGID);
	if (rc) {
		/* No need to fail just because the parent dir exists */
		if (errno != EEXIST) {
			MHVTL_ERR("Failed to create directory %s: %s",
					newMedia, strerror(errno));
			return 2;
		}
		rc = 0;
	}

	/* Don't really care if chown() fails or not..
	 * But lets try anyway
	 */
	chown(newMedia, pw->pw_uid, pw->pw_gid);

	datafile = creat(newMedia_data, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	if (datafile == -1) {
		MHVTL_ERR("Failed to create file %s: %s", newMedia_data,
			strerror(errno));
		return 2;
	}
	indxfile = creat(newMedia_indx, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	if (indxfile == -1) {
		MHVTL_ERR("Failed to create file %s: %s", newMedia_indx,
			strerror(errno));
		unlink(newMedia_data);
		rc = 2;
		goto cleanup;
	}
	metafile = creat(newMedia_meta, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	if (metafile == -1) {
		MHVTL_ERR("Failed to create file %s: %s", newMedia_meta,
			strerror(errno));
		unlink(newMedia_data);
		unlink(newMedia_indx);
		rc = 2;
		goto cleanup;
	}
	chown(newMedia_data, pw->pw_uid, pw->pw_gid);
	chown(newMedia_indx, pw->pw_uid, pw->pw_gid);
	chown(newMedia_meta, pw->pw_uid, pw->pw_gid);

	MHVTL_LOG("%s files created", newMedia);

	/* Write the meta file consisting of the MAM and the meta_header
	   structure with the filemark count initialized to zero.
	*/
	mam = *mamp;

	memset(&meta, 0, sizeof(meta));
	meta.filemark_count = 0;

	if (write(metafile, &mam, sizeof(mam)) != sizeof(mam) ||
		    write(metafile, &meta, sizeof(meta)) != sizeof(meta)) {
		MHVTL_ERR("Failed to initialize file %s: %s", newMedia_meta,
			strerror(errno));
		unlink(newMedia_data);
		unlink(newMedia_indx);
		unlink(newMedia_meta);
		rc = 1;
	}

cleanup:
	if (datafile >= 0) {
		close(datafile);
		datafile = -1;
	}
	if (indxfile >= 0) {
		close(indxfile);
		indxfile = -1;
	}
	if (metafile >= 0) {
		close(metafile);
		metafile = -1;
	}

	return rc;
}

/*
 * Attempt to load PCL - i.e. Open datafile and read in BOT header & MAM
 *
 * Returns:
 * == 0 -> Load OK
 * == 1 -> Another tape already loaded.
 * == 2 -> format corrupt.
 * == 3 -> cartridge does not exist or cannot be opened.
 */

int load_tape(const char *pcl, uint8_t *sam_stat)
{
	char pcl_data[1024], pcl_indx[1024], pcl_meta[1024];
	struct stat data_stat, indx_stat, meta_stat;
	uint64_t exp_size;
	size_t	io_size;
	loff_t nread;
	int rc = 0;
	int null_media_type;
	char touch_file[128];

	uint8_t error_check;

	snprintf(touch_file, 127, "%s/bypass_error_check", MHVTL_HOME_PATH);
	error_check = (stat(touch_file, &data_stat) == -1) ? FALSE : TRUE;

	if (error_check) {
		MHVTL_LOG("WARNING - touch file %s found - bypassing sanity checks on open", touch_file);
	}

/* KFRDEBUG - sam_stat needs updates in lots of places here. */

	/* If some other PCL is already open, return. */
	if (datafile >= 0)
		return 1;

	/* Open all three files and stat them to get their current sizes. */

	if (strlen(home_directory))
		snprintf(currentPCL, ARRAY_SIZE(currentPCL), "%s/%s",
						home_directory, pcl);
	else
		snprintf(currentPCL, ARRAY_SIZE(currentPCL), "%s/%s",
						MHVTL_HOME_PATH, pcl);

	snprintf(pcl_data, ARRAY_SIZE(pcl_data), "%s/data", currentPCL);
	snprintf(pcl_indx, ARRAY_SIZE(pcl_indx), "%s/indx", currentPCL);
	snprintf(pcl_meta, ARRAY_SIZE(pcl_meta), "%s/meta", currentPCL);

	MHVTL_DBG(2, "Opening media: %s", pcl);

	if (stat(pcl_data, &data_stat) == -1) {
		MHVTL_DBG(2, "Couldn't find %s, trying previous default: %s/%s",
				pcl_data, MHVTL_HOME_PATH, pcl);
		snprintf(currentPCL, ARRAY_SIZE(currentPCL), "%s/%s",
						MHVTL_HOME_PATH, pcl);
		snprintf(pcl_data, ARRAY_SIZE(pcl_data), "%s/data", currentPCL);
		snprintf(pcl_indx, ARRAY_SIZE(pcl_indx), "%s/indx", currentPCL);
		snprintf(pcl_meta, ARRAY_SIZE(pcl_meta), "%s/meta", currentPCL);
	}

	datafile = open(pcl_data, O_RDWR|O_LARGEFILE);
	if (datafile == -1) {
		MHVTL_ERR("open of pcl %s file %s failed, %s", pcl,
			pcl_data, strerror(errno));
		rc = 3;
		goto failed;
	}
	indxfile = open(pcl_indx, O_RDWR|O_LARGEFILE);
	if (indxfile == -1) {
		MHVTL_ERR("open of pcl %s file %s failed, %s", pcl,
			pcl_indx, strerror(errno));
		rc = 3;
		goto failed;
	}
	metafile = open(pcl_meta, O_RDWR|O_LARGEFILE);
	if (metafile == -1) {
		MHVTL_ERR("open of pcl %s file %s failed, %s", pcl,
			pcl_meta, strerror(errno));
		rc = 3;
		goto failed;
	}

	if (fstat(datafile, &data_stat) < 0) {
		MHVTL_ERR("stat of pcl %s file %s failed: %s", pcl,
			pcl_data, strerror(errno));
		rc = 3;
		goto failed;
	}

	if (fstat(indxfile, &indx_stat) < 0) {
		MHVTL_ERR("stat of pcl %s file %s failed: %s", pcl,
			pcl_indx, strerror(errno));
		rc = 3;
		goto failed;
	}

	if (fstat(metafile, &meta_stat) < 0) {
		MHVTL_ERR("stat of pcl %s file %s failed: %s", pcl,
			pcl_meta, strerror(errno));
		rc = 3;
		goto failed;
	}

	/* Verify that the metafile size is at least reasonable. */

	exp_size = sizeof(mam) + sizeof(meta);
	if ((uint32_t)meta_stat.st_size < exp_size) {
		MHVTL_ERR("sizeof(mam) + sizeof(meta) - "
			"pcl %s file %s is not the correct length, "
			"expected at least %" PRId64 ", actual %" PRId64,
			pcl, pcl_meta, exp_size, meta_stat.st_size);
		if (error_check) {
			rc = 2;
			goto failed;
		}
	}

	/* Read in the MAM and sanity-check it. */
	nread = read(metafile, &mam, sizeof(mam));
	if (nread < 0) {
		MHVTL_ERR("Error reading pcl %s MAM from metafile: %s",
			pcl, strerror(errno));
		if (error_check) {
			rc = 2;
			goto failed;
		}
	} else if (nread != sizeof(mam)) {
		MHVTL_ERR("Error reading pcl %s MAM from metafile: "
			"unexpected read length", pcl);
		if (error_check) {
			rc = 2;
			goto failed;
		}
	}

	null_media_type = mam.MediumType == MEDIA_TYPE_NULL ? 1 : 0;

	if (mam.tape_fmt_version != TAPE_FMT_VERSION) {
		MHVTL_ERR("pcl %s MAM contains incorrect media format", pcl);
		sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
		if (error_check) {
			rc = 2;
			goto failed;
		}
	}

	/* Read in the meta_header structure and sanity-check it. */

	nread = read(metafile, &meta, sizeof(meta));
	if (nread < 0) {
		MHVTL_ERR("Error reading pcl %s meta_header from "
			"metafile: %s", pcl, strerror(errno));
		rc = 2;
		goto failed;
	} else if (nread != sizeof(meta)) {
		MHVTL_ERR("Error reading pcl %s meta header from "
			"metafile: unexpected read length", pcl);
		rc = 2;
		goto failed;
	}

	/* Now recompute the correct size of the meta file. */

	exp_size = sizeof(mam) + sizeof(meta) +
		(meta.filemark_count * sizeof(*filemarks));

	if ((uint32_t)meta_stat.st_size != exp_size) {
		MHVTL_ERR("sizeof(mam) + sizeof(meta) + sizeof(*filemarks) - "
			"pcl %s file %s is not the correct length, "
			"expected %" PRId64 ", actual %" PRId64, pcl,
			pcl_meta, exp_size, meta_stat.st_size);
		if (error_check) {
			rc = 2;
			goto failed;
		}
	}

	/* See if we have allocated enough space for the actual number of
	   filemarks on the tape.  If not, realloc now.
	*/

	if (check_filemarks_alloc(meta.filemark_count)) {
		if (error_check) {
			rc = 3;
			goto failed;
		}
	}

	/* Now read in the filemark map. */

	io_size = meta.filemark_count * sizeof(*filemarks);
	if (io_size) {
		nread = read(metafile, filemarks, io_size);
		if (nread < 0) {
			MHVTL_ERR("Error reading pcl %s filemark map from "
				"metafile: %s", pcl, strerror(errno));
			rc = 2;
			goto failed;
		} else if ((size_t)nread != io_size) {
			MHVTL_ERR("Error reading pcl %s filemark map from "
				"metafile: unexpected read length", pcl);
			if (error_check) {
				rc = 2;
				goto failed;
			}
		}
	}

	/* Use the size of the indx file to work out where the virtual
	   B_EOD block resides.
	*/

	if ((indx_stat.st_size % sizeof(struct raw_header)) != 0) {
		MHVTL_ERR("pcl %s indx file has improper length, indicating "
			"possible file corruption", pcl);
		rc = 2;
		goto failed;
	}
	eod_blk_number = indx_stat.st_size / sizeof(struct raw_header);

	/* Make sure that the filemark map is consistent with the size of the
	   indx file.
	*/
	if (meta.filemark_count && eod_blk_number &&
		filemarks[meta.filemark_count - 1] >= eod_blk_number) {
		MHVTL_ERR("pcl %s indx file has improper length as compared "
			"to the meta file, indicating possible file corruption",
			pcl);
		MHVTL_ERR("Filemark count: %d eod_blk_number: %d",
				meta.filemark_count, eod_blk_number);
		rc = 2;
		goto failed;
	}

	/* Read in the last raw_header struct from the indx file and use that
	   to validate the correct size of the data file.
	*/

	if (eod_blk_number == 0)
		eod_data_offset = 0;
	else {
		if (read_header(eod_blk_number - 1, sam_stat)) {
			rc = 3;
			goto failed;
		}
		eod_data_offset = raw_pos.data_offset +
			raw_pos.hdr.disk_blk_size;
	}

	if (null_media_type) {
		MHVTL_LOG("Loaded NULL media type");	/* Skip check */
	} else if ((uint64_t)data_stat.st_size != eod_data_offset) {
		MHVTL_ERR("st_size != eod_data_offset - "
			"pcl %s file %s is not the correct length, "
			"expected %" PRId64 ", actual %" PRId64, pcl,
			pcl_data, eod_data_offset, data_stat.st_size);
		if (error_check) {
			rc = 2;
			goto failed;
		}
	}

	/* Give a hint to the kernel that data, once written, tends not to be
	   accessed again immediately.
	*/

	posix_fadvise(indxfile, 0, 0, POSIX_FADV_DONTNEED);
	posix_fadvise(datafile, 0, 0, POSIX_FADV_DONTNEED);

	/* Now initialize raw_pos by reading in the first header, if any. */

	if (read_header(0, sam_stat)) {
		rc = 3;
		goto failed;
	}

	return 0;

failed:
	if (datafile >= 0) {
		close(datafile);
		datafile = -1;
	}
	if (indxfile >= 0) {
		close(indxfile);
		indxfile = -1;
	}
	if (metafile >= 0) {
		close(metafile);
		metafile = -1;
	}
	return rc;
}

void zero_filemark_count(void)
{
	free(filemarks);
	filemark_alloc = 0;
	filemarks = NULL;

	meta.filemark_count = 0;
	rewrite_meta_file();
}

int format_tape(uint8_t *sam_stat)
{
	if (!tape_loaded(sam_stat))
		return -1;

	if (check_for_overwrite(sam_stat))
		return -1;

	zero_filemark_count();

	return mkEODHeader(raw_pos.hdr.blk_number, raw_pos.data_offset);
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int write_filemarks(uint32_t count, uint8_t *sam_stat)
{
	uint32_t blk_number;
	uint64_t data_offset;
	ssize_t nwrite;

	if (!tape_loaded(sam_stat))
		return -1;

	/* Applications assume that writing a filemark (even writing zero
	   filemarks) will force-flush any data buffered in the drive to media
	   so that after the write-filemarks call returns there is no
	   possibility that any data previously written could be lost due
	   to a power hit.  Provide a similar guarantee here.
	*/

	if (count == 0) {
		MHVTL_DBG(2, "Flushing data - 0 filemarks written");
		fsync(datafile);
		fsync(indxfile);
		fsync(metafile);

		return 0;
	}

	if (check_for_overwrite(sam_stat))
		return -1;

	/* Preserve existing raw_pos data we need, then clear raw_pos and
	   fill it in with new data.
	*/

	blk_number = raw_pos.hdr.blk_number;
	data_offset = raw_pos.data_offset;

	memset(&raw_pos, 0, sizeof(raw_pos));

	raw_pos.data_offset = data_offset;

	raw_pos.hdr.blk_type = B_FILEMARK;	/* Header type */
	raw_pos.hdr.blk_flags = 0;
	raw_pos.hdr.blk_number = blk_number;
	raw_pos.hdr.blk_size = 0;
	raw_pos.hdr.disk_blk_size = 0;

	/* Now write out one header per filemark. */

	for ( ; count > 0; count--, blk_number++) {
		raw_pos.hdr.blk_number = blk_number;

		MHVTL_DBG(3, "Writing filemark: block %d", blk_number);

		nwrite = pwrite(indxfile, &raw_pos, sizeof(raw_pos),
			blk_number * sizeof(raw_pos));
		if (nwrite != sizeof(raw_pos)) {
			sam_medium_error(E_WRITE_ERROR, sam_stat);
			MHVTL_ERR("Index file write failure,"
					" pos: %" PRId64 ": %s",
				(uint64_t)blk_number * sizeof(raw_pos),
				strerror(errno));
			return -1;
		}
		add_filemark(blk_number);
	}

	/* Provide the force-flush guarantee. */

	fsync(datafile);
	fsync(indxfile);
	fsync(metafile);

	return mkEODHeader(blk_number, data_offset);
}

int write_tape_block(const uint8_t *buffer, uint32_t blk_size,
		uint32_t comp_size, const struct encryption *encryptp,
		uint8_t comp_type, uint8_t null_media_type, uint8_t *sam_stat)
{
	uint32_t blk_number, disk_blk_size;
	uint32_t max_blk_number;
	uint64_t data_offset;
	ssize_t nwrite;

	/* Medium format limits to unsigned 32bit blks */
	max_blk_number = 0xfffffff0;

	if (!tape_loaded(sam_stat))
		return -1;

	if (check_for_overwrite(sam_stat))
		return -1;

	/* Preserve existing raw_pos data we need, then clear out raw_pos and
	   fill it in with new data.
	*/

	blk_number = raw_pos.hdr.blk_number;
	data_offset = raw_pos.data_offset;

	if (blk_number > max_blk_number) {
		MHVTL_ERR("Too many tape blocks - 32byte overflow");
		return -1;
	}

	memset(&raw_pos, 0, sizeof(raw_pos));

	raw_pos.data_offset = data_offset;

	raw_pos.hdr.blk_type = B_DATA;	/* Header type */
	raw_pos.hdr.blk_flags = 0;
	raw_pos.hdr.blk_number = blk_number;
	raw_pos.hdr.blk_size = blk_size; /* Size of uncompressed data */

	if (comp_size) {
		if (comp_type == LZO)
			raw_pos.hdr.blk_flags |= BLKHDR_FLG_LZO_COMPRESSED;
		else
			raw_pos.hdr.blk_flags |= BLKHDR_FLG_ZLIB_COMPRESSED;
		raw_pos.hdr.disk_blk_size = disk_blk_size = comp_size;
	} else
		raw_pos.hdr.disk_blk_size = disk_blk_size = blk_size;

	if (encryptp != NULL) {
		unsigned int i;

		raw_pos.hdr.blk_flags |= BLKHDR_FLG_ENCRYPTED;
		raw_pos.hdr.encryption.ukad_length = encryptp->ukad_length;
		for (i = 0; i < encryptp->ukad_length; ++i)
			raw_pos.hdr.encryption.ukad[i] = encryptp->ukad[i];

		raw_pos.hdr.encryption.akad_length = encryptp->akad_length;
		for (i = 0; i < encryptp->akad_length; ++i)
			raw_pos.hdr.encryption.akad[i] = encryptp->akad[i];

		raw_pos.hdr.encryption.key_length = encryptp->key_length;
		for (i = 0; i < encryptp->key_length; ++i)
			raw_pos.hdr.encryption.key[i] = encryptp->key[i];
	}

	/* Now write out both the data and the header. */
	if (null_media_type) {
		nwrite = disk_blk_size;
	} else
		nwrite = pwrite(datafile, buffer, disk_blk_size, data_offset);
	if (nwrite != disk_blk_size) {
		sam_medium_error(E_WRITE_ERROR, sam_stat);

		MHVTL_ERR("Data file write failure, pos: %" PRId64 ": %s",
			data_offset, strerror(errno));

		/* Truncate last partital write */
		MHVTL_DBG(1, "Truncating data file size: %"PRId64, data_offset);
		if (ftruncate(datafile, data_offset) < 0) {
			MHVTL_ERR("Error truncating data: %s", strerror(errno));
		}

		mkEODHeader(blk_number, data_offset);
		return -1;
	}

	nwrite = pwrite(indxfile, &raw_pos, sizeof(raw_pos),
						blk_number * sizeof(raw_pos));
	if (nwrite != sizeof(raw_pos)) {
		long indxsz = (blk_number - 1) * sizeof(raw_pos);

		sam_medium_error(E_WRITE_ERROR, sam_stat);

		MHVTL_ERR("Index file write failure, pos: %" PRId64 ": %s",
			(uint64_t)blk_number * sizeof(raw_pos),
			strerror(errno));

		MHVTL_DBG(1, "Truncating index file size to: %ld", indxsz);
		if (ftruncate(indxfile, indxsz) < 0) {
			MHVTL_ERR("Error truncating indx: %s", strerror(errno));
		}

		if (!null_media_type) {
			MHVTL_DBG(1, "Truncating data file size: %"PRId64,
							data_offset);
			if (ftruncate(datafile, data_offset) < 0) {
				MHVTL_ERR("Error truncating data: %s",
							strerror(errno));
			}
		}

		mkEODHeader(blk_number, data_offset);
		return -1;
	}

	MHVTL_DBG(3, "Successfully wrote block: %u", blk_number);

	return mkEODHeader(blk_number + 1, data_offset + disk_blk_size);
}

void unload_tape(uint8_t *sam_stat)
{
	if (datafile >= 0) {
		close(datafile);
		datafile = -1;
	}
	if (indxfile >= 0) {
		close(indxfile);
		indxfile = -1;
	}
	if (metafile >= 0) {
		rewrite_meta_file();
		close(metafile);
		metafile = -1;
	}
	free(filemarks);
	filemarks = NULL;
	filemark_alloc = 0;
}

uint32_t read_tape_block(uint8_t *buf, uint32_t buf_size, uint8_t *sam_stat)
{
	loff_t nread;
	uint32_t iosize;

	if (!tape_loaded(sam_stat))
		return -1;

	MHVTL_DBG(3, "Reading blk %ld, size: %d",
			(unsigned long)raw_pos.hdr.blk_number, buf_size);

	/* The caller should have already verified that this is a
	   B_DATA block before issuing this read, so we shouldn't have to
	   worry about B_EOD or B_FILEMARK here.
	*/

	if (raw_pos.hdr.blk_type == B_EOD) {
		sam_blank_check(E_END_OF_DATA, sam_stat);
		MHVTL_ERR("End of data detected while reading");
		return -1;
	}

	iosize = raw_pos.hdr.disk_blk_size;
	if (iosize > buf_size)
		iosize = buf_size;

	nread = pread(datafile, buf, iosize, raw_pos.data_offset);
	if (nread != iosize) {
		MHVTL_ERR("Failed to read %d bytes", iosize);
		return -1;
	}

	/* Now position to the following block. */

	if (read_header(raw_pos.hdr.blk_number + 1, sam_stat)) {
		MHVTL_ERR("Failed to read block header %d",
				raw_pos.hdr.blk_number + 1);
		return -1;
	}

	return nread;
}

uint64_t current_tape_offset(void)
{
	if (datafile != -1)
		return raw_pos.data_offset;

	return 0;
}

uint64_t current_tape_block(void)
{
	if (datafile != -1)
		return (uint64_t)c_pos->blk_number;
	return 0;
}

void print_raw_header(void)
{
	int i;
	printf("Hdr:");
	switch (raw_pos.hdr.blk_type) {
	case B_DATA:
		if ((raw_pos.hdr.blk_flags &
			(BLKHDR_FLG_LZO_COMPRESSED | BLKHDR_FLG_ENCRYPTED)) ==
			(BLKHDR_FLG_LZO_COMPRESSED | BLKHDR_FLG_ENCRYPTED))
			printf("  Encrypt/Comp data");
		else if ((raw_pos.hdr.blk_flags &
			(BLKHDR_FLG_ZLIB_COMPRESSED | BLKHDR_FLG_ENCRYPTED)) ==
			(BLKHDR_FLG_ZLIB_COMPRESSED | BLKHDR_FLG_ENCRYPTED))
			printf("  Encrypt/Comp data");
		else if (raw_pos.hdr.blk_flags & BLKHDR_FLG_ENCRYPTED)
			printf("     Encrypted data");
		else if (raw_pos.hdr.blk_flags & BLKHDR_FLG_ZLIB_COMPRESSED)
			printf("zlibCompressed data");
		else if (raw_pos.hdr.blk_flags & BLKHDR_FLG_LZO_COMPRESSED)
			printf(" lzoCompressed data");
			else
		printf("              data");

		printf("(%02x), sz %6d/%-6d, Blk No.: %u, data %" PRId64 "\n",
			raw_pos.hdr.blk_type,
			raw_pos.hdr.disk_blk_size,
			raw_pos.hdr.blk_size,
			raw_pos.hdr.blk_number,
			raw_pos.data_offset);
		if (raw_pos.hdr.blk_flags & BLKHDR_FLG_ENCRYPTED) {
			printf("   => Encr key length %d, ukad length %d, "
				"akad length %d\n",
				raw_pos.hdr.encryption.key_length,
				raw_pos.hdr.encryption.ukad_length,
				raw_pos.hdr.encryption.akad_length);
			printf("       Key  : ");
			for (i = 0; i < raw_pos.hdr.encryption.key_length; i++)
				printf("%02x", raw_pos.hdr.encryption.key[i]);
			if (raw_pos.hdr.encryption.ukad_length > 0) {
				printf("\n       Ukad : ");
				for (i = 0; i < raw_pos.hdr.encryption.ukad_length; i++)
					printf("%02x", raw_pos.hdr.encryption.ukad[i]);
			}
			if (raw_pos.hdr.encryption.akad_length > 0) {
				printf("\n       Akad : ");
				for (i = 0; i < raw_pos.hdr.encryption.akad_length; i++)
					printf("%02x", raw_pos.hdr.encryption.akad[i]);
			}
			puts("");
		}
		break;
	case B_FILEMARK:
		printf("         Filemark");
		printf("(%02x), sz %13d, Blk No.: %u, data %" PRId64 "\n",
			raw_pos.hdr.blk_type,
			raw_pos.hdr.blk_size,
			raw_pos.hdr.blk_number,
			raw_pos.data_offset);
		break;
	case B_EOD:
		printf("      End of Data");
		printf("(%02x), sz %13d, Blk No.: %u, data %" PRId64 "\n",
			raw_pos.hdr.blk_type,
			raw_pos.hdr.blk_size,
			raw_pos.hdr.blk_number,
			raw_pos.data_offset);
		break;
	case B_NOOP:
		printf("      No Operation");
		break;
	default:
		printf("      Unknown type");
		printf("(%02x), %6d/%-6d, Blk No.: %u, data %" PRId64 "\n",
			raw_pos.hdr.blk_type,
			raw_pos.hdr.disk_blk_size,
			raw_pos.hdr.blk_size,
			raw_pos.hdr.blk_number,
			raw_pos.data_offset);
		break;
	}
}

void print_filemark_count(void)
{
	printf("Total num of filemarks: %d\n", meta.filemark_count);
}

void print_metadata(void)
{
	unsigned int a;

	for (a = 0; a < meta.filemark_count; a++)
		printf("Filemark: %d\n", filemarks[a]);
}

/*
 * Cleanup entry point
 */
void cart_deinit(void)
{
	free(filemarks);
}
