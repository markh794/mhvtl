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

#define __STDC_FORMAT_MACROS /* for PRId64 */

/* for unistd.h pread/pwrite and fcntl.h posix_fadvise */
#define _XOPEN_SOURCE 600

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "logging.h"
#include "mhvtl_scsi.h"
#include "vtlcart.h"
#include "vtllib.h"
#include "mhvtl_update.h"
#include "be_byteshift.h"

/* The .indx file consists of an array of one raw_header structure per
   written tape block or filemark.  There is no separate raw_header
   structure required for BOT or EOM.  The raw_header structure is padded
   out to 512 bytes to allow for the addition of fields in the future without
   breaking backwards compatibility with existing PCL indx files.
*/

struct raw_header {
	loff_t			  data_offset;
	struct blk_header hdr;
	char			  pad[512 - sizeof(loff_t) - sizeof(struct blk_header)];
};

/* The .meta file consists of a meta_header
   structure, followed by a variable-length array of filemark block numbers.
   Both the MAM and meta_header structures also contain padding to allow
   for future expansion with backwards compatibility.
*/

struct meta_header {
	uint32_t filemark_count;
	char	 pad[512 - sizeof(uint32_t)];
};

static char *currentPCL = NULL;

static int datafile[MAX_PARTITIONS] = {[0 ... MAX_PARTITIONS - 1] = -1};
static int indxfile[MAX_PARTITIONS] = {[0 ... MAX_PARTITIONS - 1] = -1};
static int metafile[MAX_PARTITIONS] = {[0 ... MAX_PARTITIONS - 1] = -1};
static int mamfile					= -1;
static int mhvtlfile				= -1;

static struct raw_header  raw_pos;
static struct meta_header meta[MAX_PARTITIONS];
static uint64_t			  eod_data_offset[MAX_PARTITIONS];
static uint32_t			  eod_blk_number[MAX_PARTITIONS];

#define FM_DELTA 500
static int		 filemark_alloc[MAX_PARTITIONS] = {[0 ... MAX_PARTITIONS - 1] = 0};
static uint32_t *filemarks[MAX_PARTITIONS]		= {[0 ... MAX_PARTITIONS - 1] = NULL};

/* Initialisation of current position (global blk_header) */
struct blk_header *c_pos = &raw_pos.hdr;

#ifdef MHVTL_DEBUG
static char *mhvtl_block_type_desc(int blk_type) {
	unsigned int i;

	static const struct {
		int	  blk_type;
		char *desc;
	} block_type_desc[] = {
		{B_FILEMARK, "FILEMARK"},
		{B_EOD, "END OF DATA"},
		{B_NOOP, "NO OP"},
		{B_DATA, "DATA"},
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

static int mkEODHeader(uint32_t blk_number, uint64_t data_offset) {
	uint32_t partition_id = c_pos->partition_id;
	memset(&raw_pos, 0, sizeof(raw_pos));

	raw_pos.data_offset = data_offset;

	c_pos->blk_type		= B_EOD;
	c_pos->blk_number	= blk_number;
	c_pos->partition_id = partition_id;

	eod_blk_number[c_pos->partition_id]	 = blk_number;
	eod_data_offset[c_pos->partition_id] = data_offset;

	OK_to_write = 1;

	return 0;
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
 */

static int read_header(uint32_t blk_number, uint8_t *sam_stat) {
	loff_t nread;

	MHVTL_DBG(3, "Reading header partition/block %d/%d", c_pos->partition_id, blk_number);

	if (blk_number > eod_blk_number[c_pos->partition_id]) {
		MHVTL_ERR("Attempt to seek [%d] beyond EOD [%d]",
				  blk_number, eod_blk_number[c_pos->partition_id]);
	} else if (blk_number == eod_blk_number[c_pos->partition_id])
		mkEODHeader(eod_blk_number[c_pos->partition_id], eod_data_offset[c_pos->partition_id]);
	else {
		nread = pread(indxfile[c_pos->partition_id], &raw_pos, sizeof(raw_pos),
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

	MHVTL_DBG(3, "Reading header partition/block %u/%u at disk offset %ld, type: %s, size: %d",
			  c_pos->partition_id,
			  raw_pos.hdr.blk_number,
			  (unsigned long)raw_pos.data_offset,
			  mhvtl_block_type_desc(raw_pos.hdr.blk_type),
			  raw_pos.hdr.blk_size);
	return 0;
}

static int tape_loaded(uint8_t *sam_stat) {
	if (datafile[c_pos->partition_id] != -1)
		return 1;

	sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
	return 0;
}

static int rewrite_meta_file(void) {
	ssize_t io_size, nwrite;
	size_t	io_offset;

	io_size = sizeof(struct meta_header);
	nwrite	= pwrite(metafile[c_pos->partition_id], &meta[c_pos->partition_id], io_size, 0);
	if (nwrite < 0) {
		MHVTL_ERR("Error writing meta_header to metafile: %s",
				  strerror(errno));
		return -1;
	}
	if (nwrite != io_size) {
		MHVTL_ERR("Error writing meta_header map to metafile."
				  " Expected to write %d bytes",
				  (int)io_size);
		return -1;
	}

	io_size	  = meta[c_pos->partition_id].filemark_count * sizeof(*filemarks[c_pos->partition_id]);
	io_offset = sizeof(struct meta_header);

	if (io_size) {
		nwrite = pwrite(metafile[c_pos->partition_id], filemarks[c_pos->partition_id], io_size, io_offset);
		if (nwrite < 0) {
			MHVTL_ERR("Error writing filemark map to metafile: %s",
					  strerror(errno));
			return -1;
		}
		if (nwrite != io_size) {
			MHVTL_ERR("Error writing filemark map to metafile."
					  " Expected to write %d bytes",
					  (int)io_size);
			return -1;
		}
	}

	/* If filemarks were overwritten, the meta file may need to be shorter
	   than before.
	*/

	if (ftruncate(metafile[c_pos->partition_id], io_offset + io_size) < 0) {
		MHVTL_ERR("Error truncating metafile: %s", strerror(errno));
		return -1;
	}

	return 0;
}

static int check_for_overwrite(uint8_t *sam_stat) {
	uint32_t	 blk_number;
	uint64_t	 data_offset;
	unsigned int i;

	if (c_pos->blk_type == B_EOD)
		return 0;

	MHVTL_DBG(2, "At block %ld", (unsigned long)c_pos->blk_number);

	/* We aren't at EOD so we are performing a rewrite.  Truncate
	   the data and index files back to the current length.
	*/

	blk_number	= c_pos->blk_number;
	data_offset = raw_pos.data_offset;

	if (ftruncate(indxfile[c_pos->partition_id], blk_number * sizeof(raw_pos))) {
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		MHVTL_ERR("Index file ftruncate failure, pos: "
				  "%" PRId64 ": %s",
				  (uint64_t)blk_number * sizeof(raw_pos),
				  strerror(errno));
		return -1;
	}
	if (ftruncate(datafile[c_pos->partition_id], data_offset)) {
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		MHVTL_ERR("Data file ftruncate failure, pos: "
				  "%" PRId64 ": %s",
				  data_offset,
				  strerror(errno));
		return -1;
	}

	/* Update the filemark map removing any filemarks which will be
	   overwritten.  Rewrite the filemark map so that the on-disk image
	   of the map is consistent with the new sizes of the other two files.
	*/

	for (i = 0; i < meta[c_pos->partition_id].filemark_count; i++) {
		MHVTL_DBG(2, "filemarks[c_pos->partition_id][%d] %d", i, filemarks[c_pos->partition_id][i]);
		if (filemarks[c_pos->partition_id][i] >= blk_number) {
			MHVTL_DBG(2, "Setting filemark_count from %d to %d",
					  meta[c_pos->partition_id].filemark_count, i);
			meta[c_pos->partition_id].filemark_count = i;
			return rewrite_meta_file();
		}
	}

	return 0;
}

static int check_filemarks_alloc(uint32_t count) {
	uint32_t new_size;

	/* See if we have enough space allocated to hold 'count' filemarks.
	   If not, realloc now.
	*/

	if (count > (uint32_t)filemark_alloc[c_pos->partition_id]) {
		new_size = ((count + FM_DELTA - 1) / FM_DELTA) * FM_DELTA;

		filemarks[c_pos->partition_id] = (uint32_t *)realloc(filemarks[c_pos->partition_id],
															 new_size * sizeof(*filemarks[c_pos->partition_id]));
		if (filemarks[c_pos->partition_id] == NULL) {
			MHVTL_ERR("filemark map realloc failed, %s",
					  strerror(errno));
			return -1;
		}
		filemark_alloc[c_pos->partition_id] = new_size;
	}
	return 0;
}

static int add_filemark(uint32_t blk_number) {
	/* See if we have enough space remaining to add the new filemark.  If
	   not, realloc now.
	*/

	if (check_filemarks_alloc(meta[c_pos->partition_id].filemark_count + 1))
		return -1;

	filemarks[c_pos->partition_id][meta[c_pos->partition_id].filemark_count++] = blk_number;

	/* Now rewrite the meta_header structure and the filemark map. */

	return rewrite_meta_file();
}

/*
 * Return 0 -> Not loaded.
 *        1 -> Load OK
 *        2 -> format corrupt.
 */

int rewind_tape(uint8_t *sam_stat) {
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

		if (c_pos->blk_type == B_EOD ||
			(c_pos->blk_type == B_FILEMARK && eod_blk_number[c_pos->partition_id] == 1))
			OK_to_write = 1;
		else
			OK_to_write = 0;
		break;
	case MEDIA_TYPE_DATA:
		OK_to_write = 1; /* Reset flag to OK. */
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

int position_to_eod(uint8_t *sam_stat) {
	if (!tape_loaded(sam_stat))
		return -1;

	if (read_header(eod_blk_number[c_pos->partition_id], sam_stat))
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

int position_to_block(uint32_t blk_number, uint8_t *sam_stat) {
	if (!tape_loaded(sam_stat))
		return -1;

	MHVTL_DBG(2, "Position to partition/block %u/%u", c_pos->partition_id, blk_number);

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	if (blk_number > eod_blk_number[c_pos->partition_id]) {
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

int position_blocks_forw(uint64_t count, uint8_t *sam_stat) {
	uint32_t	 residual;
	uint32_t	 blk_target;
	unsigned int i;

	if (!tape_loaded(sam_stat))
		return -1;

	MHVTL_DBG(3, "Moving %lu blocks forward from block %u/%u", count, c_pos->partition_id, c_pos->blk_number);

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	blk_target = c_pos->blk_number + count;

	/* Find the first filemark forward from our current position, if any. */

	for (i = 0; i < meta[c_pos->partition_id].filemark_count; i++) {
		MHVTL_DBG(3, "filemark at %ld", (unsigned long)filemarks[c_pos->partition_id][i]);
		if (filemarks[c_pos->partition_id][i] >= c_pos->blk_number)
			break;
	}

	/* If there is one, see if it is between our current position and our
	   desired destination.
	*/

	if (i < meta[c_pos->partition_id].filemark_count) {
		if (filemarks[c_pos->partition_id][i] >= blk_target)
			return position_to_block(blk_target, sam_stat);

		residual = blk_target - c_pos->blk_number + 1;
		if (read_header(filemarks[c_pos->partition_id][i] + 1, sam_stat))
			return -1;

		MHVTL_DBG(1, "Filemark encountered: block %d", filemarks[c_pos->partition_id][i]);
		sam_no_sense(SD_FILEMARK, E_MARK, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}

	if (blk_target > eod_blk_number[c_pos->partition_id]) {
		residual = blk_target - eod_blk_number[c_pos->partition_id];
		if (read_header(eod_blk_number[c_pos->partition_id], sam_stat))
			return -1;

		MHVTL_DBG(1, "Moving to EOD encountered at block %u", eod_blk_number[c_pos->partition_id]);
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

int position_blocks_back(uint64_t count, uint8_t *sam_stat) {
	uint32_t residual;
	uint32_t blk_target;
	int		 i;

	if (!tape_loaded(sam_stat))
		return -1;

	MHVTL_DBG(2, "Moving %lu blocks backward from block %u/%u", count, c_pos->partition_id, c_pos->blk_number);

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	blk_target = c_pos->blk_number - count;

	/* Find the first filemark prior to our current position, if any. */

	for (i = meta[c_pos->partition_id].filemark_count - 1; i >= 0; i--) {
		if (filemarks[c_pos->partition_id][i] < c_pos->blk_number)
			break;
	}

	/* If there is one, see if it is between our current position and our
	   desired destination.
	*/
	if (i >= 0) {
		if (filemarks[c_pos->partition_id][i] < blk_target)
			return position_to_block(blk_target, sam_stat);

		residual = c_pos->blk_number - blk_target;
		if (read_header(filemarks[c_pos->partition_id][i], sam_stat))
			return -1;

		MHVTL_DBG(2, "Filemark encountered: block %d", filemarks[c_pos->partition_id][i]);
		sam_no_sense(SD_FILEMARK, E_MARK, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}

	if (blk_target < 0) {
		residual = count - c_pos->blk_number;
		if (read_header(0, sam_stat))
			return -1;

		MHVTL_DBG(1, "Moving to BOP encountered at block 0");
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

int position_filemarks_forw(uint64_t count, uint8_t *sam_stat) {
	uint32_t	 residual;
	unsigned int i;

	if (!tape_loaded(sam_stat))
		return -1;

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	/* Find the block number of the first filemark greater than our
	   current position.
	*/

	for (i = 0; i < meta[c_pos->partition_id].filemark_count; i++)
		if (filemarks[c_pos->partition_id][i] >= c_pos->blk_number)
			break;

	if (i + count - 1 < meta[c_pos->partition_id].filemark_count)
		return position_to_block(filemarks[c_pos->partition_id][i + count - 1] + 1, sam_stat);
	else {
		residual = i + count - meta[c_pos->partition_id].filemark_count;
		if (read_header(eod_blk_number[c_pos->partition_id], sam_stat))
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

int position_filemarks_back(uint64_t count, uint8_t *sam_stat) {
	uint32_t residual;
	int		 i;

	if (!tape_loaded(sam_stat))
		return -1;

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	/* Find the block number of the first filemark less than our
	   current position.
	*/

	for (i = meta[c_pos->partition_id].filemark_count - 1; i >= 0; i--)
		if (filemarks[c_pos->partition_id][i] < c_pos->blk_number)
			break;

	if (i + 1 >= count)
		return position_to_block(filemarks[c_pos->partition_id][i - count + 1], sam_stat);
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
 * Reads data in mam file to the given mam pointer
 * if a fd is closed, the file is not read and no error is raised
 * Returns 0 if successful or -1 on error
 */
int read_mam(int mam_fd, int mhvtl_fd, struct MAM *mamp) {

	if (mam_fd >= 0) {
		struct MAM_attr attr;

		if (lseek(mam_fd, 0, SEEK_SET) != 0) {
			perror("fseek");
			return -1;
		}

		/* versions */
		if (read(mam_fd, &mamp->tape_fmt_version, sizeof(mamp->tape_fmt_version)) != sizeof(mamp->tape_fmt_version))
			return -1;

		if (read(mam_fd, &mamp->mam_fmt_version, sizeof(mamp->mam_fmt_version)) != sizeof(mamp->tape_fmt_version))
			return -1;

		/* mam attributes */
		while (read(mam_fd, &attr.attribute_id, sizeof(uint16_t)) == sizeof(uint16_t)) {
			int idx = -1;

			if (read(mam_fd, &attr.length, sizeof(uint16_t)) != sizeof(uint16_t)) {
				MHVTL_ERR("Error reading attribute %04x length : %s",
						  attr.attribute_id, strerror(errno));
				return -1;
			}

			for (int i = 0; i <= MAM_ATTRIBUTE_END; i++) {
				if (mamp->attributes[i].attribute_id == attr.attribute_id) {
					idx = i;
					break;
				}
			}

			/* attribute not known: skip value */
			if (idx < 0) {
				if (attr.length) lseek(mam_fd, attr.length, SEEK_CUR);
				continue;
			}

			if (read(mam_fd, mamp->attributes[idx].value, attr.length) != attr.length) {
				MHVTL_ERR("Error reading mam attribute %04x value : %s",
						  attr.attribute_id, strerror(errno));
				return -1;
			}
		}
	}

	/* mhvtl attributes */
	if (mhvtl_fd >= 0) {
		struct MHVTL_attr attr;

		if (lseek(mhvtl_fd, 0, SEEK_SET) != 0) {
			perror("fseek");
			return -1;
		}

		while (read(mhvtl_fd, &attr.attribute_id, sizeof(uint16_t)) == sizeof(uint16_t)) {
			int idx = -1;

			if (read(mhvtl_fd, &attr.length, sizeof(uint16_t)) != sizeof(uint16_t)) {
				MHVTL_ERR("Error reading mhvtl attribute %04x length : %s",
						  attr.attribute_id, strerror(errno));
				return -1;
			}

			for (int i = 0; i <= MAM_MHVTL_ATTRIBUTE_END; i++) {
				if (mamp->mhvtl_attr[i].attribute_id == attr.attribute_id) {
					idx = i;
					break;
				}
			}

			/* attribute not known: skip value */
			if (idx < 0) {
				if (attr.length) lseek(mhvtl_fd, attr.length, SEEK_CUR);
				continue;
			}

			if (read(mhvtl_fd, mamp->mhvtl_attr[idx].value, attr.length) != attr.length) {
				MHVTL_ERR("Error reading mhvtl attribute %04x value : %s",
						  attr.attribute_id, strerror(errno));
				return -1;
			}
		}
	}

	return 0;
}

/*
 * Writes data in global struct MAM in mam/mhvtl_data files
 * Using a Type-Length-Value format to keep mam auto-descriptive
 * Returns 0 if nothing written or -1 on error
 */
int write_mam(int mam_fd, int mhvtl_fd) {

	if ((lseek(mam_fd, 0, SEEK_SET) != 0) || (lseek(mhvtl_fd, 0, SEEK_SET) != 0)) {
		perror("fseek");
		return -1;
	}

	/* versions */
	if (write(mam_fd, &mam.tape_fmt_version, sizeof(mam.tape_fmt_version)) != sizeof(mam.tape_fmt_version))
		return -1;

	if (write(mam_fd, &mam.mam_fmt_version, sizeof(mam.mam_fmt_version)) != sizeof(mam.tape_fmt_version))
		return -1;

	/* mam attributes */
	for (int i = 0; i < MAM_ATTRIBUTE_END; i++) {
		const struct MAM_attr *attr = &mam.attributes[i];

		if (write(mam_fd, &attr->attribute_id, sizeof(uint16_t)) != sizeof(uint16_t))
			return -1;

		if (write(mam_fd, &attr->length, sizeof(uint16_t)) != sizeof(uint16_t))
			return -1;

		if (write(mam_fd, attr->value, attr->length) != attr->length)
			return -1;
	}

	/* mhvtl attributes */
	for (int i = 0; i < MAM_MHVTL_ATTRIBUTE_END; i++) {
		const struct MHVTL_attr *attr = &mam.mhvtl_attr[i];

		if (write(mhvtl_fd, &attr->attribute_id, sizeof(uint16_t)) != sizeof(uint16_t))
			return -1;

		if (write(mhvtl_fd, &attr->length, sizeof(uint16_t)) != sizeof(uint16_t))
			return -1;

		if (attr->length == 0)
			continue;

		if (write(mhvtl_fd, attr->value, attr->length) != attr->length)
			return -1;
	}

	return 0;
}

/*
 * Writes data in struct MAM in mam and mhvtl_data files if tape is loaded
 * Returns 0 if nothing written or -1 on error
 */
int rewriteMAM(uint8_t *sam_stat) {
	char mam_path[1024];
	char mhvtl_data_path[1024];

	if (!tape_loaded(sam_stat))
		return -1;

	/* Rewrite MAM data */
	snprintf(mam_path, sizeof(mam_path), "%s/mam", currentPCL);
	snprintf(mhvtl_data_path, sizeof(mhvtl_data_path), "%s/mhvtl_data", currentPCL);
	if (write_mam(mamfile, mhvtlfile) < 0) {
		sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return -1;
	};

	return 0;
}

static void unlink_partition(int partition_number) {
	char path[1024];

	if (datafile >= 0) {
		snprintf(path, sizeof(path), "%s/data.%d", currentPCL, partition_number);
		unlink(path);
	}
	if (indxfile >= 0) {
		snprintf(path, sizeof(path), "%s/indx.%d", currentPCL, partition_number);
		unlink(path);
	}
	if (metafile >= 0) {
		snprintf(path, sizeof(path), "%s/meta.%d", currentPCL, partition_number);
		unlink(path);
	}
}

static int open_partition(uint8_t partition_number) {
	int			 rc = 0;
	char		 pcl_data[1024], pcl_indx[1024], pcl_meta[1024];
	const char	*pcl_files[3] = {pcl_data, pcl_indx, pcl_meta};
	struct stat	 data_stat, indx_stat, meta_stat;
	struct stat *stats[3]	= {&data_stat, &indx_stat, &meta_stat};
	int			*fd_open[3] = {&datafile[partition_number],
							   &indxfile[partition_number],
							   &metafile[partition_number]};

	snprintf(pcl_data, ARRAY_SIZE(pcl_data), "%s/data.%d", currentPCL, partition_number);
	snprintf(pcl_indx, ARRAY_SIZE(pcl_indx), "%s/indx.%d", currentPCL, partition_number);
	snprintf(pcl_meta, ARRAY_SIZE(pcl_meta), "%s/meta.%d", currentPCL, partition_number);

	for (int i = 0; i < 3; i++) {
		*fd_open[i] = open(pcl_files[i], O_RDWR | O_LARGEFILE);
		if (*fd_open[i] == -1) {
			MHVTL_ERR("open of file %s failed: %s", pcl_files[i], strerror(errno));
			rc = 3;
		}
		if (fstat(*fd_open[i], stats[i]) < 0) {
			MHVTL_ERR("stat of pcl %s file %s failed: %s", currentPCL, pcl_files[i], strerror(errno));
			rc = 3;
		}
	}

	return rc;
}

static void close_partition(uint8_t partition_number) {
	int *fd_close[3] = {&datafile[partition_number],
						&indxfile[partition_number],
						&metafile[partition_number]};
	for (int i = 0; i < 3; i++) {
		if (*fd_close[i] >= 0) {
			close(*fd_close[i]);
			*fd_close[i] = -1;
		}
	}
}

int change_partition(uint8_t partition_number) {
	uint8_t *sam_stat = SAM_STAT_GOOD;
	int		 rc		  = 0;

	close_partition(c_pos->partition_id);
	c_pos->partition_id = partition_number;
	rc					= open_partition(partition_number);
	read_header(0, sam_stat);
	return rc;
}

static void erase_partition(uint8_t *sam_stat) {

	/* Erasing all data instead of (ideally) putting Data Set Separator (DSS) from EOD */
	c_pos->blk_number	= 0;
	raw_pos.data_offset = 0;
	format_partition(sam_stat);
	close_partition(c_pos->partition_id);
	unlink_partition(c_pos->partition_id);
}

/*
 * Returns:
 * == 0, the new partition was successfully created.
 * == 2, could not create some file(s)
 * == 1, an error occurred.
 */
static int create_partition(int partition_number) {
	char		path[1024];
	int		   *fd[3]		= {&datafile[partition_number],
							   &indxfile[partition_number],
							   &metafile[partition_number]};
	const char *file_name[] = {"data", "indx", "meta"};

	for (int k = 0; k < 3; k++) {
		snprintf(path, ARRAY_SIZE(path), "%s/%s.%d", currentPCL, file_name[k], partition_number);
		if (verbose)
			printf("Creating new media %s file: %s\n", file_name[k], path);
		*fd[k] = open(path, O_CREAT | O_TRUNC | O_WRONLY,
					  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
		if (*fd[k] == -1) {
			MHVTL_ERR("Failed to create file %s: %s", path, strerror(errno));
			close_partition(partition_number);
			unlink_partition(partition_number);
			return 2;
		}
	}

	MHVTL_LOG("%s files created", currentPCL);

	/* Write the meta file consisting of the meta_header
	   structure with the filemark count initialized to zero.
	*/
	memset(&meta[partition_number], 0, sizeof(struct meta_header));
	meta[partition_number].filemark_count = 0;
	if (write(metafile[partition_number], &meta[partition_number],
			  sizeof(struct meta_header)) != sizeof(struct meta_header)) {
		snprintf(path, ARRAY_SIZE(path), "%s/meta.%d", currentPCL, partition_number);
		MHVTL_ERR("Failed to initialize file %s: %s", path,
				  strerror(errno));
		close_partition(partition_number);
		unlink_partition(partition_number);
		return 1;
	}

	close_partition(partition_number);

	return 0;
}

/*
 * Returns:
 * == 0, the new PCL was successfully created.
 * == 2, could not create the directory or some file(s)
 * == 1, an error occurred.
 */
int create_tape(const char *pcl, uint8_t *sam_stat) {
	struct stat data_stat;
	char		path[1024];
	char		mhvtl_data_path[1024];

	/* Attempt to create the new PCL.  This will fail if the PCL's directory
	   or any of the PCL's three files already exist, leaving any existing
	   files as they were.
	*/

	if (asprintf(&currentPCL, "%s/%s", home_directory, pcl) < 0) {
		perror("Could not allocate memory");
		exit(1);
	}

	/* Check if data file already exists, nothing to create */
	snprintf(path, ARRAY_SIZE(path), "%s/data.0", currentPCL);
	if (stat(path, &data_stat) != -1) {
		if (verbose)
			printf("error: Data file already exists for new media\n");
		return 0;
	}

	if (verbose) printf("Creating new media directory: %s\n", currentPCL);

	if (mkdir(currentPCL, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_ISGID)) {
		/* No need to fail just because the parent dir exists */
		if (errno != EEXIST) {
			MHVTL_ERR("Failed to create directory %s: %s",
					  currentPCL, strerror(errno));
			free(currentPCL);
			return 2;
		}
	}

	/* create mam/mhvtl_data files and fill them */
	snprintf(path, ARRAY_SIZE(path), "%s/mam", currentPCL);
	snprintf(mhvtl_data_path, ARRAY_SIZE(mhvtl_data_path), "%s/mhvtl_data", currentPCL);
	if (verbose) printf("Creating new media mam files: %s and %s\n", path, mhvtl_data_path);
	mamfile = open(path, O_CREAT | O_TRUNC | O_WRONLY,
				   S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (mamfile == -1) {
		MHVTL_ERR("Failed to create file %s: %s", path, strerror(errno));
		return 2;
	}
	mhvtlfile = open(mhvtl_data_path, O_CREAT | O_TRUNC | O_WRONLY,
					 S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (mhvtlfile == -1) {
		MHVTL_ERR("Failed to create file %s: %s", mhvtl_data_path, strerror(errno));
		return 2;
	}

	if (write_mam(mamfile, mhvtlfile) < 0) {
		MHVTL_ERR("Failed to initialize mam/mhvtl_data files");
	};

	close(mamfile);
	mamfile = -1;
	close(mhvtlfile);
	mhvtlfile = -1;

	return create_partition(0);
}

int load_partition(const char *pcl, uint8_t *sam_stat, uint8_t error_check, uint8_t partition_number) {
	int			 rc = 0;
	char		 pcl_data[1024], pcl_meta[1024];
	struct stat	 data_stat, indx_stat, meta_stat;
	struct stat *stats[3] = {&data_stat, &indx_stat, &meta_stat};
	int			*fd[3]	  = {&datafile[partition_number],
							 &indxfile[partition_number],
							 &metafile[partition_number]};

	uint64_t exp_size;
	size_t	 io_size;
	loff_t	 nread;

	/* Open all three files and stat them to get their current sizes. */

	snprintf(pcl_data, ARRAY_SIZE(pcl_data), "%s/data.%d", currentPCL, partition_number);
	if (stat(pcl_data, &data_stat) == -1) {
		MHVTL_DBG(2, "Couldn't find %s, trying previous default: %s/%s",
				  pcl_data, MHVTL_HOME_PATH, pcl);
		free(currentPCL);
		if (asprintf(&currentPCL, "%s/%s", MHVTL_HOME_PATH, pcl) < 0) {
			perror("Could not allocate memory");
			exit(1);
		}
	}

	/* Opening files */

	if (open_partition(partition_number) == 3)
		goto cleanup;
	for (int i = 0; i < 3; i++) { fstat(*fd[i], stats[i]); }

	/* Verify that the metafile size is at least reasonable. */
	exp_size = sizeof(struct meta_header);
	if ((uint32_t)meta_stat.st_size < exp_size) {
		MHVTL_ERR("sizeof(struct meta_header) - "
				  "pcl %s file %s is not the correct length, "
				  "expected at least %" PRId64 ", actual %" PRId64,
				  pcl, pcl_meta, exp_size, meta_stat.st_size);
		if (error_check) {
			rc = 2;
			goto cleanup;
		}
	}

	/* Read in the meta_header structure and sanity-check it. */
	nread = read(metafile[partition_number], &meta[partition_number], sizeof(struct meta_header));
	if (nread < 0) {
		MHVTL_ERR("Error reading pcl %s meta_header from "
				  "metafile: %s",
				  pcl, strerror(errno));
		rc = 2;
		goto cleanup;
	} else if (nread != sizeof(struct meta_header)) {
		MHVTL_ERR("Error reading pcl %s meta header from "
				  "metafile: unexpected read length",
				  pcl);
		rc = 2;
		goto cleanup;
	}

	/* Now recompute the correct size of the meta file. */
	exp_size += meta[partition_number].filemark_count * sizeof(*filemarks[partition_number]);
	if ((uint32_t)meta_stat.st_size != exp_size) {
		MHVTL_ERR("sizeof(struct MAM) + sizeof(struct_meta_header) + sizeof(*filemarks[c_pos->partition_id]) - "
				  "pcl %s file %s is not the correct length, "
				  "expected %" PRId64 ", actual %" PRId64,
				  pcl,
				  pcl_meta, exp_size, meta_stat.st_size);
		if (error_check) {
			rc = 2;
			goto cleanup;
		}
	}

	/* See if we have allocated enough space for the actual number of
	   filemarks on the tape.  If not, realloc now.
	*/
	if (check_filemarks_alloc(meta[partition_number].filemark_count)) {
		if (error_check) {
			rc = 3;
			goto cleanup;
		}
	}

	/* Now read in the filemark map. */
	io_size = meta[partition_number].filemark_count * sizeof(*filemarks[partition_number]);
	if (io_size) {
		nread = read(metafile[partition_number], filemarks[partition_number], io_size);
		if (nread < 0) {
			MHVTL_ERR("Error reading pcl %s filemark map from "
					  "metafile: %s",
					  pcl, strerror(errno));
			rc = 2;
			goto cleanup;
		} else if ((size_t)nread != io_size) {
			MHVTL_ERR("Error reading pcl %s filemark map from "
					  "metafile: unexpected read length",
					  pcl);
			if (error_check) {
				rc = 2;
				goto cleanup;
			}
		}
	}

	/* Use the size of the indx file to work out where the virtual
	   B_EOD block resides.
	*/

	if ((indx_stat.st_size % sizeof(struct raw_header)) != 0) {
		MHVTL_ERR("pcl %s indx file has improper length, indicating "
				  "possible file corruption",
				  pcl);
		rc = 2;
		goto cleanup;
	}
	eod_blk_number[partition_number] = indx_stat.st_size / sizeof(struct raw_header);

	/* Make sure that the filemark map is consistent with the size of the
	   indx file.
	*/
	if (meta[partition_number].filemark_count && eod_blk_number[partition_number] &&
		filemarks[partition_number][meta[partition_number].filemark_count - 1] >= eod_blk_number[partition_number]) {
		MHVTL_ERR("pcl %s indx file has improper length as compared "
				  "to the meta file, indicating possible file corruption",
				  pcl);
		MHVTL_ERR("Filemark count: %u eod_blk_number: %u",
				  meta[partition_number].filemark_count, eod_blk_number[partition_number]);
		rc = 2;
		goto cleanup;
	}

	/* Read in the last raw_header struct from the indx file and use that
	   to validate the correct size of the data file.
	*/

	if (eod_blk_number[partition_number] == 0)
		eod_data_offset[partition_number] = 0;
	else {
		MHVTL_DBG(3, "Media format sanity check - Reading block before EOD: %u",
				  eod_blk_number[partition_number] - 1);
		if (read_header(eod_blk_number[partition_number] - 1, sam_stat)) {
			rc = 3;
			goto cleanup;
		}
		eod_data_offset[partition_number] = raw_pos.data_offset +
											raw_pos.hdr.disk_blk_size;
	}

	if (mam.MediumType == MEDIA_TYPE_NULL) {
		MHVTL_LOG("Loaded NULL media type"); /* Skip check */
	} else if ((uint64_t)data_stat.st_size != eod_data_offset[partition_number]) {
		MHVTL_ERR("st_size != eod_data_offset - "
				  "pcl %s file %s is not the correct length, "
				  "expected %" PRId64 ", actual %" PRId64,
				  pcl,
				  pcl_data, eod_data_offset[partition_number], data_stat.st_size);
		if (error_check) {
			rc = 2;
			goto cleanup;
		}
	}

	/* Give a hint to the kernel that data, once written, tends not to be
	   accessed again immediately.
	*/

	posix_fadvise(indxfile[partition_number], 0, 0, POSIX_FADV_DONTNEED);
	posix_fadvise(datafile[partition_number], 0, 0, POSIX_FADV_DONTNEED);

cleanup:
	close_partition(partition_number);
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

int load_tape(const char *pcl, uint8_t *sam_stat) {
	char		path[1024];
	char		touch_file[128];
	uint8_t		error_check;
	struct stat data_stat;
	loff_t		nread;

	snprintf(touch_file, 127, "%s/bypass_error_check", MHVTL_HOME_PATH);
	error_check = (stat(touch_file, &data_stat) == -1) ? FALSE : TRUE;

	if (error_check) {
		MHVTL_LOG("WARNING - touch file %s found - bypassing sanity checks on open", touch_file);
	}

	/* KFRDEBUG - sam_stat needs updates in lots of places here. */

	/* If some other PCL is already open, return. */
	if (datafile[c_pos->partition_id] >= 0) {
		MHVTL_DBG(1, "Drive already full, cannot open %s", pcl);
		return 1;
	}

	MHVTL_DBG(1, "Opening media: %s", pcl);

	/* Determining pcl filepaths */
	if (!((strlen(home_directory) && asprintf(&currentPCL, "%s/%s", home_directory, pcl) >= 0) || asprintf(&currentPCL, "%s/%s", MHVTL_HOME_PATH, pcl) >= 0)) {
		perror("Could not allocate memory");
		exit(1);
	}

	/* initialize global mam */
	init_mam(&mam);

	/* load MAM and sanity-check it. */
	snprintf(path, ARRAY_SIZE(path), "%s/mam", currentPCL);
	printf("mam from %s\n", path);
	mamfile = open(path, O_RDWR | O_LARGEFILE);

	if (mamfile == -1) { /* Check for MAM location update */
		MHVTL_ERR("open of file %s failed: %s", path, strerror(errno));
		MHVTL_LOG("Trying to find mam in the meta file and update tape format...");

		if (try_extract_mam(currentPCL)) {
			MHVTL_ERR("Could not find or extract mam file : medium corrupted");
			return 3;
		};
		mamfile = open(path, O_RDWR | O_LARGEFILE);
	}

	/* Reading tape/mam versions by reading tape_fmt_version and mam_fmt_version  */
	nread = read(mamfile, &mam.tape_fmt_version, sizeof(uint32_t));
	if (nread < 0) {
		MHVTL_ERR("Error reading pcl %s Tape Format Version from mam file: %s",
				  pcl, strerror(errno));
		if (error_check) return 2;
	}
	nread = read(mamfile, &mam.mam_fmt_version, sizeof(uint32_t));
	if (nread < 0) {
		MHVTL_ERR("Error reading pcl %s MAM Format Version from mam file: %s",
				  pcl, strerror(errno));
		if (error_check) return 2;
	}

	if (mam.mam_fmt_version != MAM_VERSION) { /* Check for MAM Format Update */
		MHVTL_ERR("pcl %s MAM contains incorrect media format", pcl);
		MHVTL_LOG("Trying to update mam format...");
		if (try_update_mam(currentPCL)) {
			MHVTL_ERR("Error : MAM update failed");
			sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
			if (error_check) return 2;
		}
	}

	snprintf(path, ARRAY_SIZE(path), "%s/mhvtl_data", currentPCL);
	mhvtlfile = open(path, O_RDWR | O_LARGEFILE);
	read_mam(mamfile, mhvtlfile, &mam);

	if (mam.tape_fmt_version != TAPE_FMT_VERSION) { /* Check for Tape Format Update */
		MHVTL_ERR("pcl %s contains incorrect media format", pcl);
		MHVTL_LOG("Trying to update tape format...");
		if (try_update_tape(currentPCL)) {
			MHVTL_ERR("Error : Tape update failed");
			sam_medium_error(E_MEDIUM_FMT_CORRUPT, sam_stat);
			if (error_check) return 2;
		}
	}

	/* load all partitions */
	mam.num_partitions = 0;
	snprintf(path, ARRAY_SIZE(path), "%s/data.%d", currentPCL, mam.num_partitions);
	while (access(path, F_OK) == 0) {
		c_pos->partition_id = mam.num_partitions;
		load_partition(pcl, sam_stat, error_check, mam.num_partitions);
		snprintf(path, ARRAY_SIZE(path), "%s/data.%d", currentPCL, ++mam.num_partitions);
	}

	change_partition(0);

	/* Initialise SAM STATUS */
	*sam_stat = SAM_STAT_GOOD;

	/* Now initialize raw_pos by reading in the first header, if any. */
	if (read_header(0, sam_stat)) {
		close_partition(c_pos->partition_id);
		return 3;
	}

	return 0;
}

void zero_filemark_count(void) {
	free(filemarks[c_pos->partition_id]);
	filemark_alloc[c_pos->partition_id] = 0;
	filemarks[c_pos->partition_id]		= NULL;

	meta[c_pos->partition_id].filemark_count = 0;
	rewrite_meta_file();
}

int format_partition(uint8_t *sam_stat) {
	if (!tape_loaded(sam_stat))
		return -1;

	if (check_for_overwrite(sam_stat))
		return -1;

	zero_filemark_count();

	return mkEODHeader(c_pos->blk_number, raw_pos.data_offset);
}

int format_tape(uint8_t *sam_stat) {
	char path[1024];
	int	 partition_number = 0;

	/* Erase all partitions */
	snprintf(path, ARRAY_SIZE(path), "%s/data.%d", currentPCL, partition_number);
	while (access(path, F_OK) == 0) {
		MHVTL_DBG(1, "Erasing partition %d", partition_number);
		change_partition(partition_number);
		erase_partition(sam_stat);
		snprintf(path, ARRAY_SIZE(path), "%s/data.%d", currentPCL, ++partition_number);
	}

	/* Create <mam.num_partitions> partitions */
	for (int j = 0; j < mam.num_partitions; ++j) {
		create_partition(j);
	}

	change_partition(0);

	return 0;
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
 */

int write_filemarks(uint32_t count, uint8_t *sam_stat) {
	uint32_t blk_number;
	uint32_t partition_id;
	uint64_t data_offset;
	ssize_t	 nwrite;

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
		fsync(datafile[c_pos->partition_id]);
		fsync(indxfile[c_pos->partition_id]);
		fsync(metafile[c_pos->partition_id]);

		return 0;
	}

	if (check_for_overwrite(sam_stat))
		return -1;

	/* Preserve existing raw_pos data we need, then clear raw_pos and
	   fill it in with new data.
	*/

	blk_number	 = c_pos->blk_number;
	partition_id = c_pos->partition_id;
	data_offset	 = raw_pos.data_offset;

	memset(&raw_pos, 0, sizeof(raw_pos));

	raw_pos.data_offset = data_offset;

	c_pos->blk_type		 = B_FILEMARK; /* Header type */
	c_pos->blk_flags	 = 0;
	c_pos->blk_number	 = blk_number;
	c_pos->blk_size		 = 0;
	c_pos->disk_blk_size = 0;
	c_pos->partition_id	 = partition_id;

	/* Now write out one header per filemark. */

	for (; count > 0; count--, blk_number++) {
		c_pos->blk_number = blk_number;

		MHVTL_DBG(3, "Writing filemark: partition/block %u/%u", partition_id, blk_number);

		nwrite = pwrite(indxfile[c_pos->partition_id], &raw_pos, sizeof(raw_pos),
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

	fsync(datafile[c_pos->partition_id]);
	fsync(indxfile[c_pos->partition_id]);
	fsync(metafile[c_pos->partition_id]);

	return mkEODHeader(blk_number, data_offset);
}

int write_tape_block(const uint8_t *buffer, uint32_t blk_size,
					 uint32_t comp_size, const struct encryption *encryptp,
					 uint8_t comp_type, uint8_t null_media_type, uint32_t crc, uint8_t *sam_stat) {
	uint32_t blk_number, disk_blk_size, partition_id;
	uint32_t max_blk_number;
	uint64_t data_offset;
	ssize_t	 nwrite;

	/* Medium format limits to unsigned 32bit blks */
	max_blk_number = 0xfffffff0;

	if (!tape_loaded(sam_stat))
		return -1;

	if (check_for_overwrite(sam_stat))
		return -1;

	/* Preserve existing raw_pos data we need, then clear out raw_pos and
	   fill it in with new data.
	*/

	blk_number	 = c_pos->blk_number;
	partition_id = c_pos->partition_id;
	data_offset	 = raw_pos.data_offset;

	if (blk_number > max_blk_number) {
		MHVTL_ERR("Too many tape blocks - 32bit overflow");
		return -1;
	}

	memset(&raw_pos, 0, sizeof(raw_pos));

	raw_pos.data_offset = data_offset;

	c_pos->blk_type		= B_DATA; /* Header type */
	c_pos->blk_flags	= 0;
	c_pos->blk_number	= blk_number;
	c_pos->partition_id = partition_id;
	c_pos->blk_size		= blk_size; /* Size of uncompressed data */

	c_pos->uncomp_crc = crc;
	c_pos->blk_flags |= BLKHDR_FLG_CRC; /* Logical Block Protection */

	MHVTL_DBG(2, "CRC is 0x%08x", crc);

	if (comp_size) {
		if (comp_type == LZO)
			c_pos->blk_flags |= BLKHDR_FLG_LZO_COMPRESSED;
		else
			c_pos->blk_flags |= BLKHDR_FLG_ZLIB_COMPRESSED;
		c_pos->disk_blk_size = disk_blk_size = comp_size;
	} else
		c_pos->disk_blk_size = disk_blk_size = blk_size;

	if (encryptp != NULL) {
		unsigned int i;

		c_pos->blk_flags |= BLKHDR_FLG_ENCRYPTED;
		c_pos->blk_encryption_info.ukad_length = encryptp->ukad_length;
		for (i = 0; i < encryptp->ukad_length; ++i)
			c_pos->blk_encryption_info.ukad[i] = encryptp->ukad[i];

		c_pos->blk_encryption_info.akad_length = encryptp->akad_length;
		for (i = 0; i < encryptp->akad_length; ++i)
			c_pos->blk_encryption_info.akad[i] = encryptp->akad[i];

		c_pos->blk_encryption_info.key_length = encryptp->key_length;
		for (i = 0; i < encryptp->key_length; ++i)
			c_pos->blk_encryption_info.key[i] = encryptp->key[i];
	}

	/* Now write out both the data and the header. */
	if (null_media_type) {
		nwrite = disk_blk_size;
	} else
		nwrite = pwrite(datafile[c_pos->partition_id], buffer, disk_blk_size, data_offset);
	if (nwrite != disk_blk_size) {
		sam_medium_error(E_WRITE_ERROR, sam_stat);

		MHVTL_ERR("Data file write failure, pos: %" PRId64 ": %s",
				  data_offset, strerror(errno));

		/* Truncate last partital write */
		MHVTL_DBG(1, "Truncating data file size: %" PRId64, data_offset);
		if (ftruncate(datafile[c_pos->partition_id], data_offset) < 0) {
			MHVTL_ERR("Error truncating data: %s", strerror(errno));
		}

		mkEODHeader(blk_number, data_offset);
		return -1;
	}

	MHVTL_DBG(3, "writing partition/header %d/%u at offset %lu, type: %s, size: %u",
			  c_pos->partition_id,
			  c_pos->blk_number,
			  (unsigned long)raw_pos.data_offset,
			  mhvtl_block_type_desc(c_pos->blk_type),
			  c_pos->blk_size);

	nwrite = pwrite(indxfile[c_pos->partition_id], &raw_pos, sizeof(raw_pos),
					blk_number * sizeof(raw_pos));
	if (nwrite != sizeof(raw_pos)) {
		long indxsz = (blk_number - 1) * sizeof(raw_pos);

		sam_medium_error(E_WRITE_ERROR, sam_stat);

		MHVTL_ERR("Index file write failure, pos: %" PRId64 ": %s",
				  (uint64_t)blk_number * sizeof(raw_pos),
				  strerror(errno));

		MHVTL_DBG(1, "Truncating index file size to: %ld", indxsz);
		if (ftruncate(indxfile[c_pos->partition_id], indxsz) < 0) {
			MHVTL_ERR("Error truncating indx: %s", strerror(errno));
		}

		if (!null_media_type) {
			MHVTL_DBG(1, "Truncating data file size: %" PRId64,
					  data_offset);
			if (ftruncate(datafile[c_pos->partition_id], data_offset) < 0) {
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

void unload_tape(uint8_t *sam_stat) {
	for (int j = 0; j < mam.num_partitions; ++j) {
		MHVTL_DBG(3, "Unloading tape : partition %d", j);
		change_partition(j);
		rewrite_meta_file();
		close_partition(j);
		if (filemarks[j]) {
			free(filemarks[j]);
			filemarks[j] = NULL;
		}
	}

	memset(filemark_alloc, 0, sizeof(filemark_alloc));
	memset(eod_blk_number, 0, sizeof(eod_blk_number));
	memset(eod_data_offset, 0, sizeof(eod_data_offset));

	if (mamfile >= 0) {
		close(mamfile);
		mamfile = -1;
	}

	if (mhvtlfile >= 0) {
		close(mhvtlfile);
		mhvtlfile = -1;
	}

	free(currentPCL);
}

uint32_t read_tape_block(uint8_t *buf, uint32_t buf_size, uint8_t *sam_stat) {
	loff_t	 nread;
	uint32_t iosize;

	if (!tape_loaded(sam_stat))
		return -1;

	MHVTL_DBG(3, "Reading data partition/block %u/%u, size: %d",
			  c_pos->partition_id, c_pos->blk_number, buf_size);

	/* The caller should have already verified that this is a
	   B_DATA block before issuing this read, so we shouldn't have to
	   worry about B_EOD or B_FILEMARK here.
	*/

	if (c_pos->blk_type == B_EOD) {
		sam_blank_check(E_END_OF_DATA, sam_stat);
		MHVTL_ERR("End of data detected while reading");
		return -1;
	}

	iosize = c_pos->disk_blk_size;
	if (iosize > buf_size)
		iosize = buf_size;

	nread = pread(datafile[c_pos->partition_id], buf, iosize, raw_pos.data_offset);
	if (nread != iosize) {
		MHVTL_ERR("Failed to read %d bytes", iosize);
		return -1;
	}

	/* Now position to the following block. */
	MHVTL_DBG(3, "Reading data succeeded, now positioning to next header");
	if (read_header(c_pos->blk_number + 1, sam_stat)) {
		MHVTL_ERR("Failed to read next partition/block header %u/%u",
				  c_pos->partition_id, c_pos->blk_number + 1);
		return -1;
	}

	return nread;
}

uint64_t current_tape_offset(void) {
	if (datafile[c_pos->partition_id] != -1)
		return raw_pos.data_offset;

	return 0;
}

uint64_t current_tape_block(void) {
	if (datafile[c_pos->partition_id] != -1)
		return (uint64_t)c_pos->blk_number;
	return 0;
}

uint64_t last_block(uint8_t partition_number) {
	return eod_blk_number[c_pos->partition_id];
}

uint64_t block_from_filemark(uint8_t partition_number, uint32_t filemark) {
	return filemarks[partition_number][filemark];
}

/* Return number of filemarks up to 'block' : -1 for all */
uint64_t count_filemarks(int64_t block) {
	uint64_t count;

	MHVTL_DBG(3, "counting filemarks till partition/block %d/%ld (total = %d)",
			  c_pos->partition_id, (unsigned long)block, meta[c_pos->partition_id].filemark_count);

	if (block == -1)
		return (uint64_t)meta[c_pos->partition_id].filemark_count;

	for (count = 0; count < meta[c_pos->partition_id].filemark_count; count++) {
		if (filemarks[c_pos->partition_id][count] >= block)
			return count;
	}
	return (uint64_t)meta[c_pos->partition_id].filemark_count;
}

static void enc_key_to_string(char *dst, uint8_t *key, int len) {
	char b[16];
	int	 i;

	dst[0] = '\0';

	for (i = 0; i < len; i++) {
		sprintf(b, "%02x", key[i]);
		strncat(dst, b, 16);
	}
	key[i] = '\0';
}

void print_raw_header(void) {
	char *f	  = NULL;
	char *enc = NULL;

	enc = malloc(256);
	if (!enc) {
		printf("Unable to malloc 256 bytes of memory to produce dump_tape report");
		MHVTL_ERR("Unable to malloc 256 bytes of memory to produce dump_tape report");
		return;
	}
	f = malloc(256);
	if (!f) {
		printf("Unable to malloc 256 bytes of memory to produce dump_tape report");
		MHVTL_ERR("Unable to malloc 256 bytes of memory to produce dump_tape report");
		free(enc);
		return;
	}

	sprintf(f, "%s", "Hdr:");

	switch (c_pos->blk_type) {
	case B_DATA:
		if (c_pos->blk_flags & BLKHDR_FLG_ENCRYPTED) {
			strncat(f, "Encrypt/", 9);
		}
		if (c_pos->blk_flags & BLKHDR_FLG_ZLIB_COMPRESSED) {
			strncat(f, "zlibCompressed", 15);
		} else if (c_pos->blk_flags & BLKHDR_FLG_LZO_COMPRESSED) {
			strncat(f, "lzoCompressed", 14);
		} else {
			strncat(f, "non-compressed", 15);
		}

		if (c_pos->blk_flags & BLKHDR_FLG_CRC) {
			strncat(f, " with crc", 10);
		} else {
			strncat(f, " no crc", 10);
		}
		break;
	case B_FILEMARK:
		strncat(f, "Filemark", 9);
		break;
	case B_EOD:
		strncat(f, "End of Data", 12);
		break;
	case B_NOOP:
		strncat(f, "No Operation", 13);
		break;
	default:
		strncat(f, "Unknown type", 13);
		break;
	}
	printf("%-35s (0x%02x/0x%02x), sz: %6d/%-6d, Blk No.: %7u, data_offset: %10" PRId64 ", CRC: %08x\n",
		   f,
		   c_pos->blk_type,
		   c_pos->blk_flags,
		   c_pos->disk_blk_size,
		   c_pos->blk_size,
		   c_pos->blk_number,
		   raw_pos.data_offset,
		   c_pos->uncomp_crc);
	if ((c_pos->blk_type == B_DATA) && (c_pos->blk_flags & BLKHDR_FLG_ENCRYPTED)) {

		printf("   => Encr key length %d, ukad length %d, "
			   "akad length %d\n",
			   c_pos->blk_encryption_info.key_length,
			   c_pos->blk_encryption_info.ukad_length,
			   c_pos->blk_encryption_info.akad_length);

		if (c_pos->blk_encryption_info.key_length > 0) {
			enc_key_to_string(enc, c_pos->blk_encryption_info.key, c_pos->blk_encryption_info.key_length);
			printf("%12s : %32s\n", "Key", enc);
		}
		if (c_pos->blk_encryption_info.ukad_length > 0) {
			enc_key_to_string(enc, c_pos->blk_encryption_info.ukad, c_pos->blk_encryption_info.ukad_length);
			printf("%12s : %32s\n", "Ukad", enc);
		}
		if (c_pos->blk_encryption_info.akad_length > 0) {
			enc_key_to_string(enc, c_pos->blk_encryption_info.akad, c_pos->blk_encryption_info.akad_length);
			printf("%12s : %32s\n", "Akad", enc);
		}
	}

	free(enc);
	free(f);
}

void print_filemark_count(void) {
	printf("Total num of filemarks: %d\n", meta[c_pos->partition_id].filemark_count);
}

void print_metadata(void) {
	unsigned int a;

	for (a = 0; a < meta[c_pos->partition_id].filemark_count; a++)
		printf("Filemark: %d\n", filemarks[c_pos->partition_id][a]);
}

/*
 * Cleanup entry point
 */
void cart_deinit(void) {
	for (int j = 0; j < mam.num_partitions; j++) {
		free(filemarks[j]);
		filemarks[j] = NULL;
	}
}
