/*
 *	Dump headers of 'tape' datafile
 *
 * Copyright (C) 2005 - 2008 Mark Harvey markh794 at gmail dot com
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
#include "vtl_common.h"
#include "vx.h"
#include "vtltape.h"

#ifndef Solaris
  loff_t lseek64(int, loff_t, int);
#endif

int verbose;
int debug;

struct blk_header current_pos;

void print_current_header(void) {
	printf("Hdr:");
	switch(current_pos.blk_type) {
		case B_UNCOMPRESS_DATA:
			printf(" Uncompressed data");
			break;
		case B_COMPRESSED_DATA:
			printf("   Compressed data");
			break;
		case B_FILEMARK:
			printf("          Filemark");
			break;
		case B_BOT:
			printf(" Beginning of Tape");
			break;
		case B_EOD:
			printf("       End of Data");
			break;
		case B_EOM_WARN:
			printf("End of Media - Early Warning");
			break;
		case B_EOM:
			printf("      End of Media");
			break;
		case B_NOOP:
			printf("      No Operation");
			break;
		default:
			printf("      Unknown type");
			break;
	}
	printf("(%d), %s %d, Blk No.: %" PRId64 ", prev %" PRId64
			", cur %" PRId64 ", next %" PRId64 "\n",
			current_pos.blk_type,
			(current_pos.blk_type == B_BOT) ? "Capacity" : "sz",
			current_pos.blk_size,
			(loff_t)current_pos.blk_number,
			current_pos.prev_blk,
			current_pos.curr_blk,
			current_pos.next_blk);
}

int skip_to_next_header(int datafile, char * sense_flg) {
	loff_t nread;
	loff_t pos;

	pos = lseek64(datafile, current_pos.next_blk, SEEK_SET);
	if (pos != current_pos.next_blk) {
		printf("Error reading datafile while forward SPACEing!!\n");
		return -1;
	}
	nread = read(datafile, &current_pos, sizeof(current_pos));
	if (nread <= 0) {
		printf("Error reading datafile while forward SPACEing!!\n");
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int ofp;
	char *dataFile = HOME_PATH;
	char sense_flg;
	loff_t nread;

	if (argc < 2) {
		printf("Usage: dump_file -f <media>\n");
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
					printf("argv: -f %s\n", argv[1]);
					dataFile = argv[1];
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

	printf("Data file is : %s\n", dataFile);

	if ((ofp = open(dataFile, O_RDWR|O_LARGEFILE)) == -1) {
		fprintf(stderr, "%s, ", dataFile);
		perror("Could not open");
		exit(1);
	}
	nread = read(ofp, &current_pos, sizeof(current_pos));
	print_current_header();
	while (current_pos.blk_type != B_EOD) {
		nread = skip_to_next_header(ofp, &sense_flg);
		if (nread == -1)
			break;
		else
			print_current_header();
	}
return (0);
}
