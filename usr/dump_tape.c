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
#include "scsi.h"
#include "list.h"
#include "vtl_common.h"
#include "vtllib.h"
#include "vtltape.h"

/* The following variables are needed for the MHVTL_DBG() macro to work. */

char vtl_driver_name[] = "dump_tape";
int verbose = 0;
int debug = 0;


struct blk_header *c_pos;

static void print_mam_info(void)
{
	printf("Media density code: 0x%02x\n", mam.MediumDensityCode);
	printf("Media type code   : 0x%02x\n", mam.MediaType);
	printf("Media description : %s\n", mam.media_info.description);
	printf("Tape Capacity     : %" PRId64 "\n", ntohll(mam.max_capacity));
}


int main(int argc, char *argv[])
{
	uint8_t sam_stat;
	char *pcl = NULL;
	int rc;

	if (argc < 2) {
		printf("Usage: dump_file -f <pcl>\n");
		exit(1);
	}

	while(argc > 0) {
		if (argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				debug++;
				verbose = 9;	// If debug, make verbose...
				break;
			case 'f':
				if (argc > 1) {
					pcl = argv[1];
				} else {
					puts("    More args needed for -f\n");
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
		printf("Usage: dump_file -f <pcl>\n");
		exit(1);
	}

	printf("PCL is : %s\n", pcl);

	rc = load_tape(pcl, &sam_stat);
	if (rc) {
		fprintf(stderr,
			"PCL %s cannot be dumped, load_tape() returned %d\n",
			pcl, rc);
		exit(1);
	}

	print_mam_info();

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
