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
extern struct encryption encryption;

uint32_t GenerateRSCRC(uint32_t crc, uint32_t size, const void *buf);

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

/* Reed-Solomon CRC */
static uint32_t mhvtl_rscrc(unsigned char const *buf, size_t size)
{
/*	return ~GenerateRSCRC(0xffffffff, size, buf); */
	return GenerateRSCRC(0, size, buf);
}

/* CRC32C */
static uint32_t mhvtl_crc32c(unsigned char const *buf, size_t size)
{
	return crc32c(0, buf, size);
}

/*
 * Return number of bytes read.
 *        0 on error with sense[] filled in...
 */
int readBlock(uint8_t *buf, uint32_t request_sz, int sili, int lbp_method, uint8_t *sam_stat)
{
	uint32_t disk_blk_size, blk_size;
	uint32_t rc;
	uint32_t save_sense;
	uint32_t pre_crc;
	uint32_t blk_flags;
	uint32_t post_crc;
	uint32_t lbp_crc;
	uint8_t *bounce_buffer;
	int lbp_sz;

	MHVTL_DBG(3, "Request to read: %d bytes, SILI: %d, LBP_method: %s",
			request_sz, sili, (lbp_method == 0) ? "None" : (lbp_method == 1) ? "RS-CRC" : "CRC32c");

	/* check for a zero length read
	 * This is not an error, and shouldn't change the tape position */
	if (request_sz == 0)
		return 0;

	/** Note: lbp_method will only be set if LBP_R is also set.
	 ** Logical Block Protection - account for 4 byte CRC
	 **/
	lbp_sz = (lbp_method) ? request_sz - 4 : request_sz;

	switch (c_pos->blk_type) {
	case B_DATA:
		break;
	case B_FILEMARK:
		MHVTL_DBG(1, "Expected to find DATA header, found: FILEMARK");
		position_blocks_forw(1, sam_stat);
		mk_sense_short_block(lbp_sz, 0, sam_stat);
		save_sense = get_unaligned_be32(&sense[3]);
		sam_no_sense(SD_FILEMARK, E_MARK, sam_stat);
		put_unaligned_be32(save_sense, &sense[3]);
		return 0;
		break;
	case B_EOD:
		MHVTL_DBG(1, "Expected to find DATA header, found: EOD");
		mk_sense_short_block(lbp_sz, 0, sam_stat);
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
	blk_flags = c_pos->blk_flags;

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

	if (blk_flags & BLKHDR_FLG_LZO_COMPRESSED)
		rc = uncompress_lzo_block(bounce_buffer, blk_size, sam_stat);
	else if (blk_flags & BLKHDR_FLG_ZLIB_COMPRESSED)
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

	if (blk_flags & BLKHDR_FLG_CRC) {
		post_crc = mhvtl_crc32c(bounce_buffer, blk_size);

		if (pre_crc != post_crc) {
			MHVTL_ERR("Recorded CRC: 0x%08x, Calculated CRC: 0x%08x", pre_crc, post_crc);
			sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
			goto free_bounce_buf;
		}
		MHVTL_DBG(3, "Recorded CRC: 0x%08x, calculated CRC: 0x%08x", pre_crc, post_crc);
	}

	/* Update Logical Block Protection CRC */
	switch (lbp_method) {
	case 1:
		lbp_crc = mhvtl_rscrc(bounce_buffer, blk_size);
		put_unaligned_be32(lbp_crc, &bounce_buffer[rc]);
		MHVTL_DBG(2, "Logical Block Protection - Reed-Solomon CRC, rc: %d, request_sz: %d, lbp_size: %d, RS-CRC: 0x%08x",
							rc, request_sz, lbp_sz, lbp_crc);
		rc += 4;	/* Account for LBP checksum */
		break;
	case 2:
		MHVTL_DBG(2, "rc: %d, request_sz: %d bounce buffer before LBP: 0x%08x %08x", rc, request_sz, get_unaligned_be32(&bounce_buffer[rc - 4]), get_unaligned_be32(&bounce_buffer[rc]));
		/* If we don't have a LBP CRC32C format, re-calculate now */
		lbp_crc = (blk_flags & BLKHDR_FLG_CRC) ? pre_crc : mhvtl_crc32c(bounce_buffer, blk_size);
		put_unaligned_be32(lbp_crc, &bounce_buffer[rc]);
		MHVTL_DBG(2, "Logical Block Protection - CRC32C, rc: %d, request_sz: %d, lbp_size: %d, CRC32C: 0x%8x",
							rc, request_sz, lbp_sz, lbp_crc);
		MHVTL_DBG(2, "rc: %d, request_sz: %d bounce buffer after LBP: 0x%08x %08x", rc, request_sz, get_unaligned_be32(&bounce_buffer[rc - 4]), get_unaligned_be32(&bounce_buffer[rc]));
		rc += 4;	/* Account for LBP checksum */
		break;
	case 3:
		/* This should never occur - MODE 0a/f0 should not accept this value */
		MHVTL_ERR("LBP method 3 not supported : Returning E_LOGICAL_BLOCK_GUARD_FAILED");
		sam_hardware_error(E_LOGICAL_BLOCK_GUARD_FAILED, sam_stat);
		rc = 0;
		goto free_bounce_buf;
		break;
	default:
		break;
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
		mk_sense_short_block(lbp_sz, rc, sam_stat);
	else if (!sili) {
		if (lbp_sz < blk_size)
			mk_sense_short_block(lbp_sz, blk_size, sam_stat);
	}

	return rc;

free_bounce_buf:
	if (bounce_buffer != buf)
		free(bounce_buffer);

	return 0;
}

static lzo_uint mhvtl_compressBound(lzo_uint src_sz)
{
	return src_sz + src_sz / 16 + 67;
}

/* Determine whether or not to store the crypto info in the tape
 * blk_header.
 * We may adjust this decision for the 3592. (See ibm_3592_xx.pm)
 */
static void setup_crypto(struct scsi_cmd *cmd, struct priv_lu_ssc *lu_priv)
{
	lu_priv->cryptop = lu_priv->ENCRYPT_MODE == 2 ? &encryption : NULL;

	if (lu_priv->pm->valid_encryption_media)
		lu_priv->pm->valid_encryption_media(cmd);
}

/** Call with
 ** LBP method, bufer and size, and CRC32C CRC which was already calculated
 **
 ** Return -1 on error or 0 on success (LBP CRC match)
 */
static uint32_t get_lbp_crc(int lbp_method, unsigned char const *buf, size_t src_sz, uint32_t crc32c)
{
	uint32_t lbp_crc = 0;

	switch (lbp_method) {
		case 0:	/* No method defined - skip check */
		break;
	case 1:
		MHVTL_DBG(1, "Reed-Solomon CRC check");
		lbp_crc = mhvtl_rscrc(buf, src_sz);
		if (lbp_crc != get_unaligned_be32(&buf[src_sz])) {
			MHVTL_ERR("Reed-Solomon CRC mismatch - LBP: 0x%08x, calculated: 0x%08x",
					get_unaligned_be32(&buf[src_sz]), lbp_crc);
			return -1;	/* CRC mismatch */
		}
		break;
	case 2:
		MHVTL_DBG(1, "CRC32C check");
		if (crc32c != get_unaligned_be32(&buf[src_sz])) {
			MHVTL_ERR("CRC32C mismatch - LBP: 0x%08x, calculated: 0x%08x",
					get_unaligned_be32(&buf[src_sz]), crc32c);
			return -1;	/* CRC mismatch */
		}
		break;
	default:
		MHVTL_ERR("Undefined LBP_method - failing check");
		return -1;
		break;
	}

	return 0;
}

/*
 * Return number of bytes written to 'file'
 *
 * Zero on error with sense buffer already filled in
 */
static int writeBlock_nocomp(struct scsi_cmd *cmd, uint32_t src_sz, uint8_t null_wr, int lbp_method)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t *src_buf = (uint8_t *)cmd->dbuf_p->data;
	struct priv_lu_ssc *lu_priv;
	uint32_t crc;
	int rc;

	lu_priv = (struct priv_lu_ssc *)cmd->lu->lu_private;

	crc = mhvtl_crc32c((unsigned char const *)src_buf, (size_t)src_sz);
	setup_crypto(cmd, lu_priv);

	rc = write_tape_block(src_buf, src_sz, 0, lu_priv->cryptop, 0,
							null_wr, crc, sam_stat);

	if (lu_priv->pm->drive_supports_LBP && lbp_method) {
		MHVTL_DBG(1, "Drive supports Logical Block Protection and LBP method: %d", lbp_method);
		if (get_lbp_crc(lbp_method, src_buf, src_sz, crc)) {
			MHVTL_ERR("LBP mis-compare on write : Returning E_LOGICAL_BLOCK_GUARD_FAILED");
			sam_hardware_error(E_LOGICAL_BLOCK_GUARD_FAILED, sam_stat);
			return 0;
		}
	}

	lu_priv->bytesWritten_M += src_sz;
	lu_priv->bytesWritten_I += src_sz;

	if (rc < 0)
		return 0;

	return src_sz;
}

/*
 * Return number of bytes written to 'file'
 *
 * Zero on error with sense buffer already filled in
 */
static int writeBlock_lzo(struct scsi_cmd *cmd, uint32_t src_sz, uint8_t null_wr, int lbp_method)
{
	lzo_uint dest_len;
	lzo_uint src_len = src_sz;
	lzo_bytep dest_buf;
	lzo_bytep wrkmem = NULL;
	uint32_t crc;

	lzo_bytep src_buf = (lzo_bytep)cmd->dbuf_p->data;

	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	struct priv_lu_ssc *lu_priv;
	int rc;
	int z;

	lu_priv = (struct priv_lu_ssc *)cmd->lu->lu_private;

	crc = mhvtl_crc32c((unsigned char const *)src_buf, (size_t)src_sz);
	setup_crypto(cmd, lu_priv);

	dest_len = mhvtl_compressBound(src_sz);
	dest_buf = (lzo_bytep)malloc(dest_len);
	wrkmem = (lzo_bytep)malloc(LZO1X_1_MEM_COMPRESS);

	if (unlikely(!dest_buf)) {
		MHVTL_ERR("dest_buf malloc(%d) failed", (int)dest_len);
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		free(wrkmem);
		return 0;
	}

	if (unlikely(!wrkmem)) {
		MHVTL_ERR("wrkmem malloc(%d) failed", LZO1X_1_MEM_COMPRESS);
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		free(dest_buf);
		return 0;
	}

	z = lzo1x_1_compress(src_buf, src_sz, dest_buf, &dest_len, wrkmem);
	if (unlikely(z != LZO_E_OK)) {
		MHVTL_ERR("LZO compression error");
		sam_hardware_error(E_COMPRESSION_CHECK, sam_stat);
		return 0;
	}

	MHVTL_DBG(2, "Compression: Orig %d, after comp: %ld",
					src_sz, (unsigned long)dest_len);

	rc = write_tape_block(dest_buf, src_len, dest_len, lu_priv->cryptop,
						LZO, null_wr, crc, sam_stat);

	free(dest_buf);
	free(wrkmem);
	lu_priv->bytesWritten_M += dest_len;
	lu_priv->bytesWritten_I += src_len;

	if (lu_priv->pm->drive_supports_LBP && lbp_method) {
		MHVTL_DBG(1, "Drive supports Logical Block Protection and LBP method: %d", lbp_method);
		if (get_lbp_crc(lbp_method, src_buf, src_sz, crc)) {
			MHVTL_ERR("LBP mis-compare on write : Returning E_LOGICAL_BLOCK_GUARD_FAILED");
			sam_hardware_error(E_LOGICAL_BLOCK_GUARD_FAILED, sam_stat);
			return 0;
		}
	}

	if (rc < 0)
		return 0;

	return src_len;
}

/*
 * Return number of bytes written to 'file'
 *
 * Zero on error with sense buffer already filled in
 */
static int writeBlock_zlib(struct scsi_cmd *cmd, uint32_t src_sz, uint8_t null_wr, int lbp_method)
{
	Bytef *dest_buf;
	uLong dest_len;
	uLong src_len = src_sz;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t *src_buf = (uint8_t *)cmd->dbuf_p->data;
	struct priv_lu_ssc *lu_priv;
	uint32_t crc;
	int rc;
	int z;

	lu_priv = (struct priv_lu_ssc *)cmd->lu->lu_private;

	crc = mhvtl_crc32c((unsigned char const *)src_buf, (size_t)src_sz);
	setup_crypto(cmd, lu_priv);

	dest_len = compressBound(src_sz);
	dest_buf = (Bytef *)malloc(dest_len);
	if (!dest_buf) {
		MHVTL_ERR("malloc(%d) failed", (int)dest_len);
		sam_medium_error(E_WRITE_ERROR, sam_stat);
		return 0;
	}

	z = compress2(dest_buf, &dest_len, src_buf, src_sz,
						*lu_priv->compressionFactor);
	if (z != Z_OK) {
		switch (z) {
		case Z_MEM_ERROR:
			MHVTL_ERR("Not enough memory to compress data");
			break;
		case Z_BUF_ERROR:
			MHVTL_ERR("Not enough memory in destination "
					"buf to compress data");
			break;
		case Z_DATA_ERROR:
			MHVTL_ERR("Input data corrupt / incomplete");
			break;
		}
		sam_hardware_error(E_COMPRESSION_CHECK, sam_stat);
		return 0;
	}
	MHVTL_DBG(2, "Compression: Orig %d, after comp: %ld"
				", Compression factor: %d",
					src_sz, (unsigned long)dest_len,
					*lu_priv->compressionFactor);

	rc = write_tape_block(dest_buf, src_len, dest_len, lu_priv->cryptop,
						ZLIB, null_wr, crc, sam_stat);

	free(dest_buf);
	lu_priv->bytesWritten_M += dest_len;
	lu_priv->bytesWritten_I += src_len;

	if (lu_priv->pm->drive_supports_LBP && lbp_method) {
		MHVTL_DBG(1, "Drive supports Logical Block Protection and LBP method: %d", lbp_method);
		if (get_lbp_crc(lbp_method, src_buf, src_sz, crc)) {
			MHVTL_ERR("LBP mis-compare on write : Returning E_LOGICAL_BLOCK_GUARD_FAILED");
			sam_hardware_error(E_LOGICAL_BLOCK_GUARD_FAILED, sam_stat);
			return 0;
		}
	}

	if (rc < 0)
		return 0;

	return src_len;
}

int writeBlock(struct scsi_cmd *cmd, uint32_t src_sz)
{
	struct priv_lu_ssc *lu_priv;
	int src_len;
	uint64_t current_position;
	int64_t remaining_capacity;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint32_t lbp_sz = src_sz;
	int lbp_method = 0;

	lu_priv = (struct priv_lu_ssc *)cmd->lu->lu_private;
	src_len = 0;

	if (lu_priv->pm->drive_supports_LBP) {
		if (lu_priv->LBP_W) {
			MHVTL_DBG(1, "LBP on write - CRC type is %s",
					(lu_priv->LBP_method == 0) ? "Off" :
					(lu_priv->LBP_method == 1) ? "RS-CRC" :
					(lu_priv->LBP_method == 2) ? "CRC32C" : "Invalid");
			switch (lu_priv->LBP_method) {
			case 1:
				lbp_method = 1;
				lbp_sz = src_sz - 4;
				break;
			case 2:
				lbp_method = 2;
				lbp_sz = src_sz - 4;
				break;
			default:
				break;
			}
		}
	}

	/* Check if we hit EOT and fail before attempting to write */
	current_position = current_tape_offset();
	if (current_position >= lu_priv->max_capacity) {
		mam.remaining_capacity = 0L;
		MHVTL_DBG(1, "End of Medium - VOLUME_OVERFLOW/EOM");
		sam_no_sense(VOLUME_OVERFLOW | SD_EOM, E_EOM, sam_stat);
		return src_len;
	}

	if (lu_priv->mamp->MediumType == MEDIA_TYPE_NULL) {
		/* Don't compress if null tape media */
		src_len = writeBlock_nocomp(cmd, lbp_sz, TRUE, 0);
	} else if (*lu_priv->compressionFactor == MHVTL_NO_COMPRESSION) {
		/* No compression - use the no-compression function */
		return writeBlock_nocomp(cmd, lbp_sz, FALSE, lbp_method);
	} else {
		switch (lu_priv->compressionType) {
		case LZO:
			src_len = writeBlock_lzo(cmd, lbp_sz, FALSE, lbp_method);
			break;
		case ZLIB:
			src_len = writeBlock_zlib(cmd, lbp_sz, FALSE, lbp_method);
			break;
		default:
			src_len = writeBlock_nocomp(cmd, lbp_sz, FALSE, lbp_method);
			break;
		}
	}

	if (!src_len) {
		/* Set 'Read/Write error' TapeAlert flag */
		uint64_t fg = TA_HARD | TA_WRITE;
		set_TapeAlert(cmd->lu, fg);
		return 0;
	}

	current_position = current_tape_offset();

	if ((lu_priv->pm->drive_supports_early_warning) &&
			(current_position >= (uint64_t)lu_priv->early_warning_position)) {
		MHVTL_DBG(1, "End of Medium - Early Warning");
		sam_no_sense(SD_EOM, NO_ADDITIONAL_SENSE, sam_stat);
	} else if ((lu_priv->pm->drive_supports_prog_early_warning) &&
			(current_position >= (uint64_t)lu_priv->prog_early_warning_position)) {
		/* FIXME: Need to implement REW bit in Device Configuration Mode Page
		 *	  REW == Report Early Warning
		 */
		MHVTL_DBG(1, "End of Medium - Programmable Early Warning");
		sam_no_sense(SD_EOM, E_PROGRAMMABLE_EARLY_WARNING, sam_stat);
	}
	remaining_capacity = lu_priv->max_capacity - current_position;
	if (remaining_capacity < 0)
		remaining_capacity = 0L;

	put_unaligned_be64(remaining_capacity, &mam.remaining_capacity);

	return src_len;
}
