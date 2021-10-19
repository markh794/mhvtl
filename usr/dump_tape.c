/*
 *	Dump headers of 'tape' datafile
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
 *                                mark.harvey at nutanix dot com
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
#include "mhvtl_scsi.h"
#include "mhvtl_list.h"
#include "vtl_common.h"
#include "vtllib.h"
#include "vtltape.h"
#include "q.h"
#include "ssc.h"
#include "ccan/crc32c/crc32c.h"

char mhvtl_driver_name[] = "dump_tape";
int verbose = 0;
int debug = 0;
long my_id = 0;
int lib_id;
struct priv_lu_ssc lu_ssc;
struct lu_phy_attr lunit;
struct encryption encryption;

extern char home_directory[HOME_DIR_PATH_SZ + 1];

static char *progname;

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
		fprintf(stderr, "Unable to allocate %d bytes\n", c_pos->blk_size);
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

void find_media_home_directory(char *config_directory, char *home_directory, long lib_id);

static void usage(char *errmsg)
{
	if (errmsg)
		printf("%s\n", errmsg);
	printf("Usage: %s OPTIONS\n", progname);
	printf("Where OPTIONS are from:\n");
	printf("  -h               Print this message and exit\n");
	printf("  -d               Enable debugging\n");
	printf("  -v               Be verbose\n");
	printf("  -D               Dump data\n");
	printf("  -l lib_no        Look in specified library\n");
	printf("  -f pcl           Look for specified PCL\n");
	exit(errmsg ? 1 : 0);
}

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

	progname = argv[0];

	if (argc < 2)
		usage("Not enough arguments");

#ifdef __x86_64__
	if (__builtin_cpu_supports("sse4.2")) {
		printf("crc32c using Intel sse4.2 hardware optimization\n");
	} else {
		printf("crc32c not using Intel sse4.2 optimization\n");
	}
#endif

	while (argc > 1) {
		if (argv[1][0] == '-') {
			switch (argv[1][1]) {
			case 'h':
				usage(NULL);
				break;
			case 'd':
				debug++;
				verbose = 9;	/* If debug, make verbose... */
				break;
			case 'f':
				if (argc > 1)
					pcl = argv[2];
				else
					usage("More args needed for -f");
				break;
			case 'l':
				if (argc > 1)
					libno = atoi(argv[1]);
				else
					usage("More args needed for -l");
				break;
			case 'D':
				dump_data = TRUE;
				break;
			case 'v':
				verbose++;
				break;
			default:
				usage("Unknown option");
				break;
			}
		}
		argv++;
		argc--;
	}

	if (pcl == NULL)
		usage("No PCL number supplied");

	conf = fopen(config , "r");
	if (!conf) {
		fprintf(stderr, "Cannot open config file %s: %s\n", config,
					strerror(errno));
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
		find_media_home_directory(NULL, home_directory, libno);
		rc = load_tape(pcl, &sam_stat);
	} else { /* Walk thru all defined libraries looking for media */
		while (readline(b, MALLOC_SZ, conf) != NULL) {
			if (b[0] == '#')	/* Ignore comments */
				continue;
			/* If found a library: Attempt to load media
			 * Break out of loop if found. Otherwise try next lib.
			 */
			if (sscanf(b, "Library: %d CHANNEL:", &indx)) {
				find_media_home_directory(NULL, home_directory, indx);
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
		fprintf(stderr, "internal error - lzo_init() failed !!!\n");
		fprintf(stderr,
			"(this usually indicates a compiler bug - try recompiling\nwithout optimizations, and enable '-DLZO_DEBUG' for diagnostics)\n");
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
