/*
 *	Dump headers of 'tape' datafile
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
 *                                mark.harvey at veritas dot com
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
 */

#define _FILE_OFFSET_BITS 64

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <inttypes.h>
#include <zlib.h>
#include "minilzo.h"
#include "be_byteshift.h"
#include "scsi.h"
#include "list.h"
#include "vtl_common.h"
#include "vtllib.h"
#include "vtltape.h"

char vtl_driver_name[] = "dump_tape";
int verbose = 0;
int debug = 0;
long my_id = 0;
int lib_id;

extern char home_directory[HOME_DIR_PATH_SZ + 1];

struct blk_header *c_pos;

static void print_mam_info(void)
{
	uint64_t size;
	uint64_t remaining;
	char size_mul;		/* K/M/G/T/P multiplier */
	char remain_mul;	/* K/M/G/T/P multiplier */
	int a;
	static const char mul[] = " KMGT";

	size = get_unaligned_be64(&mam.max_capacity);
	remaining = get_unaligned_be64(&mam.remaining_capacity);

	size_mul = remain_mul = ' ';
	for (a = 0; a < 4; a++) {
		if (size > 5121) {
			size >>= 10;	/* divide by 1024 */
			size_mul = mul[a+1];
		}
	}
	for (a = 0; a < 4; a++) {
		if (remaining > 5121) {
			remaining >>= 10;	/* divide by 1024 */
			remain_mul = mul[a+1];
		}
	}

	printf("Media density code: 0x%02x\n", mam.MediumDensityCode);
	printf("Media type code   : 0x%02x\n", mam.MediaType);
	printf("Media description : %s\n", mam.media_info.description);
	printf("Tape Capacity     : %" PRId64 " (%" PRId64 " %cBytes)\n",
					get_unaligned_be64(&mam.max_capacity),
					size, size_mul);
	switch (mam.MediumType) {
	case MEDIA_TYPE_CLEAN:
		printf("Media type        : Cleaning media\n");
		break;
	case MEDIA_TYPE_DATA:
		printf("Media type        : Normal data\n");
		break;
	case MEDIA_TYPE_NULL:
		printf("Media type        : NULL - Use with caution !!\n");
		break;
	case MEDIA_TYPE_WORM:
		printf("Media type        : Write Once Read Many (WORM)\n");
		break;
	default:
		printf("Media type        : Unknown - please report !!\n");
		break;
	}

	printf("Media             : %s\n",
				(mam.Flags & MAM_FLAGS_MEDIA_WRITE_PROTECT) ?
					"Write-protected" : "read-write");
	printf("Remaining Tape Capacity : %" PRId64 " (%" PRId64 " %cBytes)\n",
				get_unaligned_be64(&mam.remaining_capacity),
				remaining, remain_mul);
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
		printf("Out of memory: %d\n", __LINE__);
		sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
		return 0;
	}

	/* c_buf is now invalid for this current block */
	nread = read_tape_block(cbuf, disk_blk_size, sam_stat);
//	printf("Reading - disk_blk_size: %d\n", disk_blk_size);
	if (nread != disk_blk_size) {
		printf("read failed, %s\n", strerror(errno));
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

	printf("tgtsize: %d, blk_size: %d\n", tgtsize, blk_size);

	if (tgtsize >= blk_size) {
		/* block sizes match, uncompress directly into buf */
		z = lzo1x_decompress(cbuf, disk_blk_size, buf, &uncompress_sz, NULL);
	} else {
		/* Initiator hasn't requested same size as data block */
		c2buf = (uint8_t *)malloc(uncompress_sz);
		if (c2buf == NULL) {
			printf("Out of memory: %d\n", __LINE__);
			sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
			free(cbuf);
			return 0;
		}
		z = lzo1x_decompress(cbuf, disk_blk_size, c2buf, &uncompress_sz, NULL);
		/* Now copy 'requested size' of data into buffer */
		memcpy(buf, c2buf, tgtsize);
		free(c2buf);
	}

	if (z == LZO_E_OK) {
		printf("Read %u bytes of lzo compressed"
				" data, have %u bytes for result\n",
				(uint32_t)nread, blk_size);
	} else {
		printf("Decompression error\n");
		sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
		rc = 0;
	}

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
		printf("Out of memory: %d\n", __LINE__);
		sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
		return 0;
	}

	nread = read_tape_block(cbuf, disk_blk_size, sam_stat);
	if (nread != disk_blk_size) {
		printf("read failed, %s\n", strerror(errno));
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
			printf("Out of memory: %d\n", __LINE__);
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
		printf("Read %u bytes of zlib compressed"
			" data, have %u bytes for result\n",
			(uint32_t)nread, blk_size);
		break;
	case Z_MEM_ERROR:
		printf("Not enough memory to decompress\n");
		sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
		rc = 0;
		break;
	case Z_DATA_ERROR:
		printf("Block corrupt or incomplete\n");
		sam_medium_error(E_DECOMPRESSION_CRC, sam_stat);
		rc = 0;
		break;
	case Z_BUF_ERROR:
		printf("Not enough memory in destination buf\n");
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
	uint32_t blk_size;
	uint32_t tgtsize, rc;

	printf("Request to read: %d bytes, SILI: %d\n", request_sz, sili);

	/* check for a zero length read
	 * This is not an error, and shouldn't change the tape position */
	if (request_sz == 0)
		return 0;

	switch (c_pos->blk_type) {
	case B_DATA:
		break;
	case B_FILEMARK:
		printf("Expected to find DATA header, found: FILEMARK\n");
		position_blocks_forw(1, sam_stat);
		return 0;
		break;
	case B_EOD:
		printf("Expected to find DATA header, found: EOD\n");
		return 0;
		break;
	default:
		printf("Unknown blk header at offset %u"
				" - Abort read cmd\n", c_pos->blk_number);
		sam_medium_error(E_UNRECOVERED_READ, sam_stat);
		return 0;
		break;
	}

	/* The tape block is compressed.  Save field values we will need after
	   the read causes the tape block to advance.
	*/
	blk_size = c_pos->blk_size;

	/* We have a data block to read.
	   Only read upto size of allocated buffer by initiator
	*/
	tgtsize = min(request_sz, blk_size);

	if (c_pos->blk_flags && BLKHDR_FLG_LZO_COMPRESSED)
		rc = uncompress_lzo_block(buf, tgtsize, sam_stat);
	else if (c_pos->blk_flags && BLKHDR_FLG_ZLIB_COMPRESSED)
		rc = uncompress_zlib_block(buf, tgtsize, sam_stat);
	else {
	/* If the tape block is uncompressed, we can read the number of bytes
	   we need directly into the scsi read buffer and we are done.
	*/
		if (read_tape_block(buf, tgtsize, sam_stat) != tgtsize) {
			printf("read failed, %s\n", strerror(errno));
			sam_medium_error(E_UNRECOVERED_READ, sam_stat);
			return 0;
		}
		rc = tgtsize;
	}

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

	return rc;
}
static int read_data(uint8_t *sam_stat)
{
	uint8_t *p;
	uint32_t ret;

	printf("c_pos->blk_size: %d\n", c_pos->blk_size);

	if (c_pos->blk_size <= 0) {
		printf("Data size: %d - skipping read\n", c_pos->blk_size);
		return 0;
	}
	p = malloc(c_pos->blk_size);
	if (!p) {
		printf("Unable to allocate %d bytes\n", c_pos->blk_size);
		return -ENOMEM;
	}
	ret = readBlock(p, c_pos->blk_size, 1, sam_stat);
	if (ret != c_pos->blk_size) {
		printf("Requested %d bytes, received %d\n",
				c_pos->blk_size, ret);
	}
	free(p);
	puts("\n");
	return ret;
}

void find_media_home_directory(char *home_directory, long lib_id);

int main(int argc, char *argv[])
{
	uint8_t sam_stat;
	char *pcl = NULL;
	int rc;
	int libno = 0;
	int indx;
	int dump_data = FALSE;
	char *config = MHVTL_CONFIG_PATH"/device.conf";
	FILE *conf;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */

	if (argc < 2) {
		printf("Usage: %s [-l lib_no] -f <pcl>\n", argv[0]);
		exit(1);
	}

	while (argc > 0) {
		if (argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				debug++;
				verbose = 9;	/* If debug, make verbose... */
				break;
			case 'f':
				if (argc > 1) {
					pcl = argv[1];
				} else {
					puts("    More args needed for -f\n");
					exit(1);
				}
				break;
			case 'l':
				if (argc > 1) {
					libno = atoi(argv[1]);
				} else {
					puts("    More args needed for -l\n");
					exit(1);
				}
				break;

			case 'D':
				dump_data = TRUE;
				break;

			case 'v':
				verbose++;
				break;
			}
		}
		argv++;
		argc--;
	}

	if (pcl == NULL) {
		printf("Usage: %s -f <pcl>\n", argv[0]);
		exit(1);
	}

	conf = fopen(config , "r");
	if (!conf) {
		printf("Can not open config file %s : %s", config,
					strerror(errno));
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

	rc = ENOENT;

	if (libno) {
		printf("Looking for PCL: %s in library %d\n", pcl, libno);
		find_media_home_directory(home_directory, libno);
		rc = load_tape(pcl, &sam_stat);
	} else { /* Walk thru all defined libraries looking for media */
		while (readline(b, MALLOC_SZ, conf) != NULL) {
			if (b[0] == '#')	/* Ignore comments */
				continue;
			/* If found a library: Attempt to load media
			 * Break out of loop if found. Otherwise try next lib.
			 */
			if (sscanf(b, "Library: %d CHANNEL:", &indx)) {
				find_media_home_directory(home_directory, indx);
				rc = load_tape(pcl, &sam_stat);
				if (!rc)
					break;
			}
		}
	}

	fclose(conf);
	free(s);
	free(b);

	if (rc) {
		fprintf(stderr, "PCL %s cannot be dumped, "
				"load_tape() returned %d\n",
					pcl, rc);
		exit(1);
	}

	if (lzo_init() != LZO_E_OK) {
		printf("internal error - lzo_init() failed !!!\n");
		printf("(this usually indicates a compiler bug - try recompiling\nwithout optimizations, and enable '-DLZO_DEBUG' for diagnostics)\n");
		exit(3);
	}
	print_mam_info();

	print_filemark_count();
	if (verbose) {
		printf("Dumping filemark meta info\n");
		print_metadata();
	}

	while (c_pos->blk_type != B_EOD) {
		print_raw_header();
		if (dump_data) {
			read_data(&sam_stat);
		}
		position_blocks_forw(1, &sam_stat);
	}
	print_raw_header();
	unload_tape(&sam_stat);

	return 0;
}
