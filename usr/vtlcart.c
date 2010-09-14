
#define _FILE_OFFSET_BITS 64

#define _XOPEN_SOURCE 600	// for unistd.h pread/pwrite and fcntl.h posix_fadvise

#include <sys/syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

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

static int filemark_alloc = 0;
static int filemark_delta = 500;
static uint32_t *filemarks = NULL;

/* Globally visible variables. */

struct MAM mam;
struct blk_header *c_pos = &raw_pos.hdr;
int OK_to_write = 0;

static char * mhvtl_block_type_desc(int blk_type)
{
	int i;

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

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

static int
mkEODHeader(uint32_t blk_number, uint64_t data_offset)
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

static int
read_header(uint32_t blk_number, uint8_t *sam_stat)
{
	loff_t nread;

	if (blk_number > eod_blk_number) {
		MHVTL_DBG(1, "Attempt to seek [%d] beyond EOD [%d]",
				blk_number, eod_blk_number);
	} else if (blk_number == eod_blk_number) {
		mkEODHeader(eod_blk_number, eod_data_offset);
	} else {
		nread = pread(indxfile, &raw_pos, sizeof(raw_pos),
			blk_number * sizeof(raw_pos));
		if (nread < 0) {
			MHVTL_DBG(1, "Medium format corrupt");
			mkSenseBuf(MEDIUM_ERROR,E_MEDIUM_FMT_CORRUPT, sam_stat);
			return -1;
		} else if (nread != sizeof(raw_pos)) {
			MHVTL_DBG(1, "Failed to read next header");
			mkSenseBuf(MEDIUM_ERROR, E_END_OF_DATA, sam_stat);
			return -1;
		}
	}

	MHVTL_DBG(3, "Reading header %d at offset %ld, type: %s",
			raw_pos.hdr.blk_number,
			(unsigned long)raw_pos.data_offset,
			mhvtl_block_type_desc(raw_pos.hdr.blk_type));
	return 0;
}

static int
tape_loaded(uint8_t *sam_stat)
{
	if (datafile != -1) {
		return 1;
	}
	mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
	return 0;
}

static int
rewrite_meta_file(void)
{
	ssize_t io_size, nwrite;
	size_t io_offset;

	io_size = sizeof(meta);
	io_offset = sizeof(struct MAM);
	if ((nwrite = pwrite(metafile, &meta, io_size, io_offset)) < 0) {
		MHVTL_DBG(1, "Error writing meta_header to metafile: %s",
			strerror(errno));
		return -1;
	} else if (nwrite != io_size) {
		MHVTL_DBG(1, "Error writing meta_header map to metafile");
		return -1;
	}

	io_size = meta.filemark_count * sizeof(*filemarks);
	io_offset = sizeof(struct MAM) + sizeof(meta);

	if (io_size == 0) {
		/* do nothing */
	} else if ((nwrite = pwrite(metafile, filemarks, io_size, io_offset)) < 0) {
		MHVTL_DBG(1, "Error writing filemark map to metafile: %s",
			strerror(errno));
		return -1;
	} else if (nwrite != io_size) {
		MHVTL_DBG(1, "Error writing filemark map to metafile");
		return -1;
	}

	/* If filemarks were overwritten, the meta file may need to be shorter
	   than before.
	*/

	if (ftruncate(metafile, io_offset + io_size) < 0) {
		MHVTL_DBG(1, "Error truncating metafile: %s", strerror(errno));
		return -1;
	}

	return 0;
}

static int
check_for_overwrite(uint8_t *sam_stat)
{
	uint32_t blk_number;
	uint64_t data_offset;
	int i;

	if (raw_pos.hdr.blk_type == B_EOD) {
		return 0;
	}

	/* We aren't at EOD so we are performing a rewrite.  Truncate
	   the data and index files back to the current length.
	*/

	blk_number = raw_pos.hdr.blk_number;
	data_offset = raw_pos.data_offset;

	if (ftruncate(indxfile, blk_number * sizeof(raw_pos))) {
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
		MHVTL_DBG(1, "Index file ftruncate failure, pos: "
			"%" PRId64 ": %s", blk_number * sizeof(raw_pos),
			strerror(errno));
		return -1;
	}
	if (ftruncate(datafile, data_offset)) {
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
		MHVTL_DBG(1, "Data file ftruncate failure, pos: "
			"%" PRId64 ": %s", data_offset,
			strerror(errno));
		return -1;
	}

	/* Update the filemark map removing any filemarks which will be
	   overwritten.  Rewrite the filemark map so that the on-disk image
	   of the map is consistent with the new sizes of the other two files.
	*/

	for (i = 0; i < meta.filemark_count; i++) {
		if (filemarks[i] > blk_number) {
			meta.filemark_count = i;
			return rewrite_meta_file();
		}
	}

	return 0;
}

static int
check_filemarks_alloc(uint32_t count)
{
	uint32_t new_size;

	/* See if we have enough space allocated to hold 'count' filemarks.
	   If not, realloc now.
	*/

	if (count > filemark_alloc) {
		new_size = ((count + filemark_delta - 1) / filemark_delta) *
			filemark_delta;

		filemarks = realloc(filemarks, new_size * sizeof(*filemarks));
		if (filemarks == NULL) {
			MHVTL_DBG(1, "filemark map realloc failed, %s",
				strerror(errno));
			return -1;
		}
		filemark_alloc = new_size;
	}
	return 0;
}

static int
add_filemark(uint32_t blk_number)
{
	/* See if we have enough space remaining to add the new filemark.  If
	   not, realloc now.
	*/

	if (check_filemarks_alloc(meta.filemark_count + 1)) {
			return -1;
	}

	filemarks[meta.filemark_count++] = blk_number;

	/* Now rewrite the meta_header structure and the filemark map. */

	return rewrite_meta_file();
}

/*
 * Return 0 -> Not loaded.
 *        1 -> Load OK
 *        2 -> format corrupt.
 */

int
rewind_tape(uint8_t *sam_stat)
{
	if (!tape_loaded(sam_stat)) {
		return -1;
	}

	if (read_header(0, sam_stat)) {
		return -1;
	}

	switch(mam.MediumType) {
	case MEDIA_TYPE_CLEAN:
		OK_to_write = 0;
		break;
	case MEDIA_TYPE_WORM:
		// Check if this header is a filemark and the next header
		//  is End of Data. If it is, we are OK to write

		if (raw_pos.hdr.blk_type == B_EOD ||
		    (raw_pos.hdr.blk_type == B_FILEMARK && eod_blk_number == 1))
		{
			OK_to_write = 1;
		} else {
			OK_to_write = 0;
		}
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
 * Returns:
 * == 0, success
 * != 0, failure
*/

int
position_to_eod(uint8_t *sam_stat)
{
	if (!tape_loaded(sam_stat)) {
		return -1;
	}

	if (read_header(eod_blk_number, sam_stat)) {
		return -1;
	}

	if (mam.MediumType == MEDIA_TYPE_WORM) {
		OK_to_write = 1;
	}

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
		mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
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

int
position_blocks_forw(uint32_t count, uint8_t *sam_stat)
{
	uint32_t residual;
	uint32_t blk_target;
	int i;

	if (!tape_loaded(sam_stat)) {
		return -1;
	}

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	blk_target = raw_pos.hdr.blk_number + count;

	/* Find the first filemark forward from our current position, if any. */

	for (i = 0; i < meta.filemark_count; i++) {
		MHVTL_DBG(3, "filemark at %ld", (unsigned long)filemarks[i]);
		if (filemarks[i] >= raw_pos.hdr.blk_number) {
			break;
		}
	}

	/* If there is one, see if it is between our current position and our
	   desired destination.
	*/

	if (i < meta.filemark_count) {
		if (filemarks[i] >= blk_target) {
			return position_to_block(blk_target, sam_stat);
		}

		residual = blk_target - raw_pos.hdr.blk_number + 1;
		if (read_header(filemarks[i] + 1, sam_stat)) {
			return -1;
		}
		MHVTL_DBG(2, "Filemark encountered: block %d", filemarks[i]);
		mkSenseBuf(NO_SENSE | SD_FILEMARK, E_MARK, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}

	if (blk_target > eod_blk_number) {
		residual = blk_target - eod_blk_number;
		if (read_header(eod_blk_number, sam_stat)) {
			return -1;
		}
		MHVTL_DBG(2, "EOD encountered");
		mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
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

int
position_blocks_back(uint32_t count, uint8_t *sam_stat)
{
	uint32_t residual;
	uint32_t blk_target;
	int i;

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

	for (i = meta.filemark_count - 1; i <= meta.filemark_count; i--) {
		MHVTL_DBG(3, "filemark at %ld", (unsigned long)filemarks[i]);
		if (filemarks[i] < raw_pos.hdr.blk_number)
			break;
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
		mkSenseBuf(NO_SENSE | SD_FILEMARK, E_MARK, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}

	if (count > raw_pos.hdr.blk_number) {
		residual = count - raw_pos.hdr.blk_number;
		if (read_header(0, sam_stat))
			return -1;

		MHVTL_DBG(2, "BOM encountered");
		mkSenseBuf(NO_SENSE | SD_EOM, E_BOM, sam_stat);
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

int
position_filemarks_forw(uint32_t count, uint8_t *sam_stat)
{
	uint32_t residual;
	int i;

	if (!tape_loaded(sam_stat)) {
		return -1;
	}

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	/* Find the block number of the first filemark greater than our
	   current position.
	*/

	for (i = 0; i < meta.filemark_count; i++) {
		if (filemarks[i] >= raw_pos.hdr.blk_number) {
			break;
		}
	}

	if (i + count - 1 < meta.filemark_count) {
		return position_to_block(filemarks[i + count - 1] + 1, sam_stat);
	} else {
		residual = i + count - meta.filemark_count;
		if (read_header(eod_blk_number, sam_stat)) {
			return -1;
		}
		mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}
}

/*
 * Returns:
 * == 0, success
 * != 0, failure
*/

int
position_filemarks_back(uint32_t count, uint8_t *sam_stat)
{
	uint32_t residual;
	int i;

	if (!tape_loaded(sam_stat)) {
		return -1;
	}

	if (mam.MediumType == MEDIA_TYPE_WORM)
		OK_to_write = 0;

	/* Find the block number of the first filemark less than our
	   current position.
	*/

	for (i = meta.filemark_count - 1; i >= 0; i--) {
		if (filemarks[i] < raw_pos.hdr.blk_number) {
			break;
		}
	}

	if (i + 1 >= count) {
		return position_to_block(filemarks[i - count + 1], sam_stat);
	} else {
		residual = count - i - 1;
		if (read_header(0, sam_stat)) {
			return -1;
		}
		mkSenseBuf(NO_SENSE | SD_EOM, E_BOM, sam_stat);
		put_unaligned_be32(residual, &sense[3]);
		return -1;
	}
}

/*
 * Writes data in struct MAM back to beginning of metafile..
 * Returns 0 if nothing written or -1 on error
 */

int
rewriteMAM(uint8_t *sam_stat)
{
	loff_t nwrite = 0;

	if (!tape_loaded(sam_stat)) {
		return -1;
	}

	// Rewrite MAM data

	nwrite = pwrite(metafile, &mam, sizeof(mam), 0);
	if (nwrite != sizeof(mam)) {
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FMT_CORRUPT, sam_stat);
		return -1;
	}

	return 0;
}

/*
 * Returns:
 * == 0, the new PCL was successfully created.
 * == 2, the PCL (probably) already existed.
 * == 1, an error occurred.
*/

int
create_tape(const char *pcl, const struct MAM *mamp, uint8_t *sam_stat)
{
	char newMedia[1024];
	char newMedia_data[1024];
	char newMedia_indx[1024];
	char newMedia_meta[1024];
	int rc = 0;

	/* Attempt to create the new PCL.  This will fail if the PCL's directory
	   or any of the PCL's three files already exist, leaving any existing
	   files as they were.
	*/

	sprintf(newMedia, "%s/%s", MHVTL_HOME_PATH, pcl);
	sprintf(newMedia_data, "%s/data", newMedia);
	sprintf(newMedia_indx, "%s/indx", newMedia);
	sprintf(newMedia_meta, "%s/meta", newMedia);

	if (mkdir(newMedia, S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP) < 0)
	{
		MHVTL_DBG(1, "Failed to create directory %s: %s", newMedia,
			strerror(errno));
		return 2;
	}

	datafile = creat(newMedia_data, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	if (datafile == -1) {
		MHVTL_DBG(1, "Failed to create file %s: %s", newMedia_data,
			strerror(errno));
		return 2;
	}
	indxfile = creat(newMedia_indx, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	if (indxfile == -1) {
		MHVTL_DBG(1, "Failed to create file %s: %s", newMedia_indx,
			strerror(errno));
		unlink(newMedia_data);
		rc = 2;
		goto cleanup;
	}
	metafile = creat(newMedia_meta, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	if (metafile == -1) {
		MHVTL_DBG(1, "Failed to create file %s: %s", newMedia_meta,
			strerror(errno));
		unlink(newMedia_data);
		unlink(newMedia_indx);
		rc = 2;
		goto cleanup;
	}

	syslog(LOG_DAEMON|LOG_INFO, "%s files created", newMedia);

	/* Write the meta file consisting of the MAM and the meta_header
	   structure with the filemark count initialized to zero.
	*/

	mam = *mamp;

	memset(&meta, 0, sizeof(meta));
	meta.filemark_count = 0;

	if (write(metafile, &mam, sizeof(mam)) != sizeof(mam) ||
	    write(metafile, &meta, sizeof(meta)) != sizeof(meta))
	{
		MHVTL_DBG(1, "Failed to initialize file %s: %s", newMedia_meta,
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

int
load_tape(const char *pcl, uint8_t *sam_stat)
{
	char pcl_data[1024], pcl_indx[1024], pcl_meta[1024];
	struct stat data_stat, indx_stat, meta_stat;
	uint64_t exp_size;
	size_t	io_size;
	loff_t nread;
	int rc = 0;

/* KFRDEBUG - sam_stat needs updates in lots of places here. */

	/* If some other PCL is already open, return. */

	if (datafile >= 0)
		return 1;

	/* Open all three files and stat them to get their current sizes. */

	sprintf(currentPCL,"%s/%s", MHVTL_HOME_PATH, pcl);
	MHVTL_DBG(2, "Opening file/media %s", currentPCL);

	sprintf(pcl_data,"%s/data", currentPCL);
	sprintf(pcl_indx,"%s/indx", currentPCL);
	sprintf(pcl_meta,"%s/meta", currentPCL);

	if ((datafile = open(pcl_data, O_RDWR|O_LARGEFILE)) == -1) {
		MHVTL_DBG(1, "open of pcl %s file %s failed, %s", pcl,
			pcl_data, strerror(errno));
		rc = 3;
		goto failed;
	}
	if ((indxfile = open(pcl_indx, O_RDWR|O_LARGEFILE)) == -1) {
		MHVTL_DBG(1, "open of pcl %s file %s failed, %s", pcl,
			pcl_indx, strerror(errno));
		rc = 3;
		goto failed;
	}
	if ((metafile = open(pcl_meta, O_RDWR|O_LARGEFILE)) == -1) {
		MHVTL_DBG(1, "open of pcl %s file %s failed, %s", pcl,
			pcl_meta, strerror(errno));
		rc = 3;
		goto failed;
	}

	if (fstat(datafile, &data_stat) < 0) {
		MHVTL_DBG(1, "stat of pcl %s file %s failed: %s", pcl,
			pcl_data, strerror(errno));
		rc = 3;
		goto failed;
	}

	if (fstat(indxfile, &indx_stat) < 0) {
		MHVTL_DBG(1, "stat of pcl %s file %s failed: %s", pcl,
			pcl_indx, strerror(errno));
		rc = 3;
		goto failed;
	}

	if (fstat(metafile, &meta_stat) < 0) {
		MHVTL_DBG(1, "stat of pcl %s file %s failed: %s", pcl,
			pcl_meta, strerror(errno));
		rc = 3;
		goto failed;
	}

	/* Verify that the metafile size is at least reasonable. */

	exp_size = sizeof(mam) + sizeof(meta);
	if (meta_stat.st_size < exp_size) {
		MHVTL_DBG(1, "pcl %s file %s is not the correct length, "
			"expected at least %" PRId64 ", actual %" PRId64,
			pcl, pcl_meta, exp_size, meta_stat.st_size);
		rc = 2;
		goto failed;
	}

	/* Read in the MAM and sanity-check it. */

	if ((nread = read(metafile, &mam, sizeof(mam))) < 0) {
		MHVTL_DBG(1, "Error reading pcl %s MAM from metafile: %s",
			pcl, strerror(errno));
		rc = 2;
		goto failed;
	} else if (nread != sizeof(mam)) {
		MHVTL_DBG(1, "Error reading pcl %s MAM from metafile: "
			"unexpected read length", pcl);
		rc = 2;
		goto failed;
	}

	if (mam.tape_fmt_version != TAPE_FMT_VERSION) {
		MHVTL_DBG(1, "pcl %s MAM contains incorrect media format", pcl);
		mkSenseBuf(MEDIUM_ERROR, E_MEDIUM_FMT_CORRUPT, sam_stat);
		rc = 2;
		goto failed;
	}

	/* Read in the meta_header structure and sanity-check it. */

	if ((nread = read(metafile, &meta, sizeof(meta))) < 0) {
		MHVTL_DBG(1, "Error reading pcl %s meta_header from "
			"metafile: %s", pcl, strerror(errno));
		rc = 2;
		goto failed;
	} else if (nread != sizeof(meta)) {
		MHVTL_DBG(1, "Error reading pcl %s meta header from "
			"metafile: unexpected read length", pcl);
		rc = 2;
		goto failed;
	}

	/* Now recompute the correct size of the meta file. */

	exp_size = sizeof(mam) + sizeof(meta) +
		(meta.filemark_count * sizeof(*filemarks));

	if (meta_stat.st_size != exp_size) {
		MHVTL_DBG(1, "pcl %s file %s is not the correct length, "
			"expected %" PRId64 ", actual %" PRId64, pcl,
			pcl_meta, exp_size, meta_stat.st_size);
		rc = 2;
		goto failed;
	}

	/* See if we have allocated enough space for the actual number of
	   filemarks on the tape.  If not, realloc now.
	*/

	if (check_filemarks_alloc(meta.filemark_count)) {
		rc = 3;
		goto failed;
	}

	/* Now read in the filemark map. */

	io_size = meta.filemark_count * sizeof(*filemarks);
	if (io_size == 0) {
		/* do nothing */
	} else if ((nread = read(metafile, filemarks, io_size)) < 0) {
		MHVTL_DBG(1, "Error reading pcl %s filemark map from "
			"metafile: %s", pcl, strerror(errno));
		rc = 2;
		goto failed;
	} else if (nread != io_size) {
		MHVTL_DBG(1, "Error reading pcl %s filemark map from "
			"metafile: unexpected read length", pcl);
		rc = 2;
		goto failed;
	}

	/* Use the size of the indx file to work out where the virtual
	   B_EOD block resides.
	*/

	if ((indx_stat.st_size % sizeof(struct raw_header)) != 0) {
		MHVTL_DBG(1, "pcl %s indx file has improper length, indicating "
			"possible file corruption", pcl);
		rc = 2;
		goto failed;
	}
	eod_blk_number = indx_stat.st_size / sizeof(struct raw_header);

	/* Make sure that the filemark map is consistent with the size of the
	   indx file.
	*/

	if (meta.filemark_count > 0 &&
		filemarks[meta.filemark_count - 1] >= eod_blk_number)
	{
		MHVTL_DBG(1, "pcl %s indx file has improper length as compared "
			"to the meta file, indicating possible file corruption",
			pcl);
		rc = 2;
		goto failed;
	}

	/* Read in the last raw_header struct from the indx file and use that
	   to validate the correct size of the data file.
	*/

	if (eod_blk_number == 0) {
		eod_data_offset = 0;
	} else {
		if (read_header(eod_blk_number - 1, sam_stat)) {
			rc = 3;
			goto failed;
		}
		eod_data_offset = raw_pos.data_offset +
			raw_pos.hdr.disk_blk_size;
	}

	if (data_stat.st_size != eod_data_offset) {
		MHVTL_DBG(1, "pcl %s file %s is not the correct length, "
			"expected %" PRId64 ", actual %" PRId64, pcl,
			pcl_data, eod_data_offset, data_stat.st_size);
		rc = 2;
		goto failed;
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

int
write_filemarks(uint32_t count, uint8_t *sam_stat)
{
	uint32_t blk_number;
	uint64_t data_offset;
	ssize_t nwrite;

	if (!tape_loaded(sam_stat)) {
		return -1;
	}

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

	if (check_for_overwrite(sam_stat)) {
		return -1;
	}

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

		nwrite = pwrite(indxfile, &raw_pos, sizeof(raw_pos),
			blk_number * sizeof(raw_pos));
		if (nwrite != sizeof(raw_pos)) {
			mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
			MHVTL_DBG(1, "Index file write failure, pos: %" PRId64 ": %s",
				blk_number * sizeof(raw_pos), strerror(errno));
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

int
write_tape_block(const uint8_t *buffer, uint32_t blk_size, uint32_t comp_size,
	const struct encryption *encryptp, uint8_t *sam_stat)
{
	uint32_t blk_number, disk_blk_size;
	uint64_t data_offset;
	ssize_t nwrite;

	if (!tape_loaded(sam_stat)) {
		return -1;
	}

	if (check_for_overwrite(sam_stat)) {
		return -1;
	}

	/* Preserve existing raw_pos data we need, then clear out raw_pos and
	   fill it in with new data.
	*/

	blk_number = raw_pos.hdr.blk_number;
	data_offset = raw_pos.data_offset;

	memset(&raw_pos, 0, sizeof(raw_pos));

	raw_pos.data_offset = data_offset;

	raw_pos.hdr.blk_type = B_DATA;	/* Header type */
	raw_pos.hdr.blk_flags = 0;
	raw_pos.hdr.blk_number = blk_number;
	raw_pos.hdr.blk_size = blk_size; /* Size of uncompressed data */

	if (comp_size) {
		raw_pos.hdr.blk_flags |= BLKHDR_FLG_COMPRESSED;
		raw_pos.hdr.disk_blk_size = disk_blk_size = comp_size;
	} else {
		raw_pos.hdr.disk_blk_size = disk_blk_size = blk_size;
	}

	if (encryptp != NULL) {
		int i;

		raw_pos.hdr.blk_flags |= BLKHDR_FLG_ENCRYPTED;
		raw_pos.hdr.encryption.ukad_length = encryptp->ukad_length;
		for (i = 0; i < encryptp->ukad_length; ++i) {
			raw_pos.hdr.encryption.ukad[i] = encryptp->ukad[i];
		}

		raw_pos.hdr.encryption.akad_length = encryptp->akad_length;
		for (i = 0; i < encryptp->akad_length; ++i) {
			raw_pos.hdr.encryption.akad[i] = encryptp->akad[i];
		}

		raw_pos.hdr.encryption.key_length = encryptp->key_length;
		for (i = 0; i < encryptp->key_length; ++i) {
			raw_pos.hdr.encryption.key[i] = encryptp->key[i];
		}
	}

	/* Now write out both the header and the data. */

	nwrite = pwrite(indxfile, &raw_pos, sizeof(raw_pos),
		blk_number * sizeof(raw_pos));
	if (nwrite != sizeof(raw_pos)) {
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
		MHVTL_DBG(1, "Index file write failure, pos: %" PRId64 ": %s",
			blk_number * sizeof(raw_pos), strerror(errno));
		return -1;
	}

	nwrite = pwrite(datafile, buffer, disk_blk_size, data_offset);
	if (nwrite != disk_blk_size) {
		mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
		MHVTL_DBG(1, "Data file write failure, pos: %" PRId64 ": %s",
			data_offset, strerror(errno));
		return -1;
	}

	return mkEODHeader(blk_number + 1, data_offset + disk_blk_size);
}

void
unload_tape(uint8_t *sam_stat)
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
}

uint32_t
read_tape_block(uint8_t *buf, uint32_t buf_size, uint8_t *sam_stat)
{
	loff_t nread;
	uint32_t iosize;

	if (!tape_loaded(sam_stat))
		return -1;

	MHVTL_DBG(3, "Reading blk %ld", (unsigned long)raw_pos.hdr.blk_number);

	/* The caller should have already verified that this is a
	   B_DATA block before issuing this read, so we shouldn't have to
	   worry about B_EOD or B_FILEMARK here.
	*/

	if (raw_pos.hdr.blk_type == B_EOD) {
		mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
		MHVTL_DBG(1, "End of data detected while reading");
		return -1;
	}

	iosize = raw_pos.hdr.disk_blk_size;
	if (iosize > buf_size)
		iosize = buf_size;

	nread = pread(datafile, buf, iosize, raw_pos.data_offset);
	if (nread != iosize) {
		MHVTL_DBG(1, "Failed to read %d bytes", iosize);
		return -1;
	}

	// Now position to the following block.

	if (read_header(raw_pos.hdr.blk_number + 1, sam_stat)) {
		MHVTL_DBG(1, "Failed to read block header %d",
				raw_pos.hdr.blk_number + 1);
		return -1;
	}

	return nread;
}

uint64_t
current_tape_offset(void)
{
	if (datafile != -1) {
		return raw_pos.data_offset;
	}
	return 0;
}

void
print_raw_header(void)
{
	printf("Hdr:");
	switch(raw_pos.hdr.blk_type) {
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

		printf("(%02x), sz %6d/%-6d, Blk No.: %u, data %" PRId64 "\n",
			raw_pos.hdr.blk_type,
			raw_pos.hdr.disk_blk_size,
			raw_pos.hdr.blk_size,
			raw_pos.hdr.blk_number,
			raw_pos.data_offset);
		if (raw_pos.hdr.blk_flags & BLKHDR_FLG_ENCRYPTED)
			printf("   => Encr key length %d, ukad length %d, "
				"akad length %d\n",
				raw_pos.hdr.encryption.key_length,
				raw_pos.hdr.encryption.ukad_length,
				raw_pos.hdr.encryption.akad_length);
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
