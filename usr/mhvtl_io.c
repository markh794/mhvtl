/*
 * mhvtl_io.c
 *
 * deduplicate code - Move Read/Write routines
 *
 * dump_tape / vtltape both use same functions
 * which are currently duplicated
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <strings.h>
#include <syslog.h>
#include <inttypes.h>
#include <pwd.h>
#include <signal.h>
#include <ctype.h>
#include "mhvtl_list.h"
#include "be_byteshift.h"
#include "vtl_common.h"
#include "mhvtl_scsi.h"
#include "q.h"
#include "logging.h"
#include "vtllib.h"
#include "vtltape.h"
#include "spc.h"
#include "ssc.h"
#include "mhvtl_log.h"
#include "mode.h"
#include "ccan/crc32c/crc32c.h"
#include <zlib.h>
#include "minilzo.h"


extern int verbose;
extern int debug;
extern long my_id;
extern struct priv_lu_ssc lu_ssc;
extern struct lu_phy_attr lunit;

static void
mk_sense_short_block(uint32_t requested, uint32_t processed, uint8_t *sense_valid)
{
	int difference = (int)requested - (int)processed;

	/* No sense, ILI bit set */
	sam_no_sense(SD_ILI, NO_ADDITIONAL_SENSE, sense_valid);

	MHVTL_DBG(2, "Short block read: Requested: %d, Read: %d,"
			" short by %d bytes",
					requested, processed, difference);

	/* Now fill in the datablock with number of bytes not read/written */
	put_unaligned_be32(difference, &sense[3]);
}

static int uncompress_lzo_block(uint8_t *buf, uint32_t tgtsize, uint8_t *sam_stat)
{
	uint8_t *cbuf, *c2buf;
	uint32_t disk_blk_size, blk_size;
	int rc, z;
	loff_t nread = 0;
	lzo_uint uncompress_sz;

	/* The tape block is compressed.
	   Save field values we will need after the read which
	   causes the tape block to advance.
	*/
	blk_size = c_pos->blk_size;
	disk_blk_size = c_pos->disk_blk_size;

	/* Malloc a buffer to hold the compressed data, and read the
	   data into it.
	*/
	cbuf = (uint8_t *)malloc(disk_blk_size);
	if (!cbuf) {
		MHVTL_ERR("Out of memory: %d", __LINE__);
		sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
		return 0;
	}

	/* Can't reference c_pos after this point
	 * read_tape_block increments c_pos to next block header
	 */
	nread = read_tape_block(cbuf, disk_blk_size, sam_stat);
	if (nread != disk_blk_size) {
		MHVTL_ERR("read failed, %s", strerror(errno));
		sam_medium_error(E_UNRECOVERED_READ, sam_stat);
		free(cbuf);
		return 0;
	}

	rc = tgtsize;
	uncompress_sz = blk_size;

	/* If the scsi read buffer is at least as big as the size of
	   the uncompressed data then we can uncompress directly into
	   the read buffer.  If not, then we need an extra buffer to
	   uncompress into, then memcpy the subrange we need to the
	   read buffer.
	*/

	if (tgtsize >= blk_size) {
		/* block sizes match, uncompress directly into buf */
		z = lzo1x_decompress_safe(cbuf, disk_blk_size, buf, &uncompress_sz, NULL);
	} else {
		/* Initiator hasn't requested same size as data block */
		c2buf = (uint8_t *)malloc(uncompress_sz);
		if (c2buf == NULL) {
			MHVTL_ERR("Out of memory: %d", __LINE__);
			sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
			free(cbuf);
			return 0;
		}
		z = lzo1x_decompress_safe(cbuf, disk_blk_size, c2buf, &uncompress_sz, NULL);
		/* Now copy 'requested size' of data into buffer */
		memcpy(buf, c2buf, tgtsize);
		free(c2buf);
	}

	switch (z) {
	case LZO_E_OK:
		MHVTL_DBG(2, "Read %u bytes of lzo compressed"
				" data, have %u bytes for result",
				(uint32_t)nread, blk_size);
		goto complete;
		break;
	case LZO_E_INPUT_NOT_CONSUMED:
		MHVTL_DBG(1, "The end of compressed block has been detected before all %d bytes",
				blk_size);
		break;
	case LZO_E_INPUT_OVERRUN:
		MHVTL_ERR("The decompressor requested more bytes from the compressed block");
		break;
	case LZO_E_OUTPUT_OVERRUN:
		MHVTL_ERR("The decompressor requested to write more bytes than the uncompressed block can hold");
		break;
	case LZO_E_LOOKBEHIND_OVERRUN:
		MHVTL_ERR("Look behind overrun - data is corrupted");
		break;
	case LZO_E_EOF_NOT_FOUND:
		MHVTL_ERR("No EOF code was found in the compressed block");
		break;
	case LZO_E_ERROR:
		MHVTL_ERR("Data is corrupt - generic lzo error received");
		break;
	}

	sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
	rc = 0;

complete:
	free(cbuf);

	return rc;
}

static int uncompress_zlib_block(uint8_t *buf, uint32_t tgtsize, uint8_t *sam_stat)
{
	uint8_t	*cbuf, *c2buf;
	loff_t nread = 0;
	uLongf uncompress_sz;
	uint32_t disk_blk_size, blk_size;
	int rc, z;

	/* The tape block is compressed.
	   Save field values we will need after the read which
	   causes the tape block to advance.
	*/
	blk_size = c_pos->blk_size;
	disk_blk_size = c_pos->disk_blk_size;

	/* Malloc a buffer to hold the compressed data, and read the
	   data into it.
	*/
	cbuf = (uint8_t *)malloc(disk_blk_size);
	if (!cbuf) {
		MHVTL_ERR("Out of memory: %d", __LINE__);
		sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
		return 0;
	}

	nread = read_tape_block(cbuf, disk_blk_size, sam_stat);
	if (nread != disk_blk_size) {
		MHVTL_ERR("read failed, %s", strerror(errno));
		sam_medium_error(E_UNRECOVERED_READ, sam_stat);
		free(cbuf);
		return 0;
	}

	rc = tgtsize;
	uncompress_sz = blk_size;

	/* If the scsi read buffer is at least as big as the size of
	   the uncompressed data then we can uncompress directly into
	   the read buffer.  If not, then we need an extra buffer to
	   uncompress into, then memcpy the subrange we need to the
	   read buffer.
	*/

	if (tgtsize >= blk_size) {
		/* block sizes match, uncompress directly into buf */
		z = uncompress(buf, &uncompress_sz, cbuf, disk_blk_size);
	} else {
		/* Initiator hasn't requested same size as data block */
		c2buf = (uint8_t *)malloc(uncompress_sz);
		if (c2buf == NULL) {
			MHVTL_ERR("Out of memory: %d", __LINE__);
			sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
			free(cbuf);
			return 0;
		}
		z = uncompress(c2buf, &uncompress_sz, cbuf, disk_blk_size);
		/* Now copy 'requested size' of data into buffer */
		memcpy(buf, c2buf, tgtsize);
		free(c2buf);
	}

	switch (z) {
	case Z_OK:
		MHVTL_DBG(2, "Read %u bytes of zlib compressed"
			" data, have %u bytes for result",
			(uint32_t)nread, blk_size);
		break;
	case Z_MEM_ERROR:
		MHVTL_ERR("Not enough memory to decompress");
		sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
		rc = 0;
		break;
	case Z_DATA_ERROR:
		MHVTL_ERR("Block corrupt or incomplete");
		sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
		rc = 0;
		break;
	case Z_BUF_ERROR:
		MHVTL_ERR("Not enough memory in destination buf");
		sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
		rc = 0;
		break;
	}

	free(cbuf);

	return rc;
}

/*
 * Return number of bytes read.
 *        0 on error with sense[] filled in...
 */
int readBlock(uint8_t *buf, uint32_t request_sz, int sili, uint8_t *sam_stat)
{
	uint32_t disk_blk_size, blk_size;
	uint32_t rc;
	uint32_t save_sense;
	uint32_t pre_crc;
	uint32_t post_crc;
	uint8_t *bounce_buffer;

	MHVTL_DBG(3, "Request to read: %d bytes, SILI: %d", request_sz, sili);

	/* check for a zero length read
	 * This is not an error, and shouldn't change the tape position */
	if (request_sz == 0)
		return 0;

	switch (c_pos->blk_type) {
	case B_DATA:
		break;
	case B_FILEMARK:
		MHVTL_DBG(1, "Expected to find DATA header, found: FILEMARK");
		position_blocks_forw(1, sam_stat);
		mk_sense_short_block(request_sz, 0, sam_stat);
		save_sense = get_unaligned_be32(&sense[3]);
		sam_no_sense(SD_FILEMARK, E_MARK, sam_stat);
		put_unaligned_be32(save_sense, &sense[3]);
		return 0;
		break;
	case B_EOD:
		MHVTL_DBG(1, "Expected to find DATA header, found: EOD");
		mk_sense_short_block(request_sz, 0, sam_stat);
		save_sense = get_unaligned_be32(&sense[3]);
		sam_blank_check(E_END_OF_DATA, sam_stat);
		put_unaligned_be32(save_sense, &sense[3]);
		return 0;
		break;
	default:
		MHVTL_ERR("Unknown blk header at offset %u"
				" - Abort read cmd", c_pos->blk_number);
		sam_medium_error(E_UNRECOVERED_READ, sam_stat);
		return 0;
		break;
	}

	/* The tape block is compressed.  Save field values we will need after
	   the read causes the tape block to advance.
	*/
	blk_size = c_pos->blk_size;
	disk_blk_size = c_pos->disk_blk_size;
	pre_crc = c_pos->uncomp_crc;

	if (blk_size > request_sz) {
		bounce_buffer = malloc(blk_size);
		if (!bounce_buffer) {
			MHVTL_ERR("Unable to allocate %d bytes for bounce buffer",
					blk_size);
			sam_medium_error(E_UNRECOVERED_READ, sam_stat);
			return 0;
		}
	} else {
		bounce_buffer = buf;
	}

	if (c_pos->blk_flags & BLKHDR_FLG_LZO_COMPRESSED)
		rc = uncompress_lzo_block(bounce_buffer, blk_size, sam_stat);
	else if (c_pos->blk_flags & BLKHDR_FLG_ZLIB_COMPRESSED)
		rc = uncompress_zlib_block(bounce_buffer, blk_size, sam_stat);
	else {
	/* If the tape block is uncompressed, we can read the number of bytes
	   we need directly into the scsi read buffer and we are done.
	*/
		if (read_tape_block(bounce_buffer, blk_size, sam_stat) != blk_size) {
			MHVTL_ERR("read failed, %s", strerror(errno));
			sam_medium_error(E_UNRECOVERED_READ, sam_stat);
			goto free_bounce_buf;
		}
		rc = blk_size;
	}

	if (c_pos->blk_flags & BLKHDR_FLG_UNCOMPRESSED_CRC) {
		post_crc = crc32c(0, bounce_buffer, blk_size);
		if (pre_crc != post_crc) {
			MHVTL_ERR("BLK CRC: 0x%08x, Calculated CRC after read: 0x%08x", pre_crc, post_crc);
			sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
			goto free_bounce_buf;
		}
		MHVTL_DBG(3, "BLK_CRC: 0x%08x, calculated CRC: 0x%08x", pre_crc, post_crc);
	}

	/* If bounce_buffer != buf then we're reading a large block and need to copy
	 * data back into buf
	*/
	if (bounce_buffer != buf) {
		memcpy(buf, bounce_buffer, request_sz);
		free(bounce_buffer);
		rc = request_sz;
	}

	lu_ssc.bytesRead_I += blk_size;
	lu_ssc.bytesRead_M += disk_blk_size;

	/*
	 * What SSC4-r01e says about incorrect length reads

	If the SILI bit is zero and an incorrect-length logical block is read,
	CHECK CONDITION status shall be returned.

	The ILI and VALID bits shall be set to one in the sense data and the
	additional sense code shall be set to NO ADDITIONAL SENSE INFORMATION.

	Upon termination, the logical position shall be after the
	incorrect-length logical block (i.e., end-of-partition side).

	If the FIXED bit is one, the INFORMATION field shall be set to the
	requested transfer length minus the actual number of logical blocks
	read, not including the incorrect-length logical block.
	If the FIXED bit is zero, the INFORMATION field shall be set to the
	requested transfer length minus the actual logical block length.
	Logical units that do not support negative values shall set the
	INFORMATION field to zero if the overlength condition exists.

		NOTE 35 - In the above case with the FIXED bit of one, only
		the position of the incorrect-length logical block may be
		determined from the sense data. The actual length of the
		incorrect logical block is not reported. Other means may
		be used to determine its actual length (e.g., read it again
		with the fixed bit set to zero).
	*/
	if (rc != request_sz)
		mk_sense_short_block(request_sz, rc, sam_stat);
	else if (!sili) {
		if (request_sz < blk_size)
			mk_sense_short_block(request_sz, blk_size, sam_stat);
	}

	return rc;

free_bounce_buf:
	if (bounce_buffer != buf)
		free(bounce_buffer);

	return 0;
}
