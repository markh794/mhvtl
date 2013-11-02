/*
 *	Dump headers of 'tape' datafile
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
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
	printf("Media             : %s\n",
				(mam.Flags & MAM_FLAGS_MEDIA_WRITE_PROTECT) ?
					"Write-protected" : "read-write");
	printf("Remaining Tape Capacity : %" PRId64 " (%" PRId64 " %cBytes)\n",
				get_unaligned_be64(&mam.remaining_capacity),
				remaining, remain_mul);
}

void find_media_home_directory(char *home_directory, int lib_id);

int main(int argc, char *argv[])
{
	uint8_t sam_stat;
	char *pcl = NULL;
	int rc;
	int libno = 0;
	int indx;
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

	print_mam_info();

	print_filemark_count();
	if (verbose) {
		printf("Dumping filemark meta info\n");
		print_metadata();
	}

	while (c_pos->blk_type != B_EOD) {
		print_raw_header();
		position_blocks_forw(1, &sam_stat);
	}
	print_raw_header();
	unload_tape(&sam_stat);

	return 0;
}
