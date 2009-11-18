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
#include "vtllib.h"
#include "vtltape.h"

#ifndef Solaris
  loff_t lseek64(int, loff_t, int);
#endif

int verbose;
int debug;

struct blk_header current_pos;

void print_current_header(void)
{
	printf("Hdr:");
	switch(current_pos.blk_type) {
	case B_DATA:
		if ((current_pos.blk_flags &&
			(BLKHDR_FLG_COMPRESSED | BLKHDR_FLG_ENCRYPTED)) ==
				(BLKHDR_FLG_COMPRESSED | BLKHDR_FLG_ENCRYPTED))
			printf(" Encrypt/Comp data");
		else if (current_pos.blk_flags & BLKHDR_FLG_ENCRYPTED)
			printf("    Encrypted data");
		else if (current_pos.blk_flags & BLKHDR_FLG_COMPRESSED)
			printf("   Compressed data");
			else
		printf("             data");

		printf("(%02x), sz %6d/%-6d, Blk No.: %" PRId64 ", prev %" PRId64
			", cur %" PRId64 ", next %" PRId64 "\n",
			current_pos.blk_type,
			current_pos.disk_blk_size,
			current_pos.blk_size,
			(loff_t)current_pos.blk_number,
			current_pos.prev_blk,
			current_pos.curr_blk,
			current_pos.next_blk);
		if (current_pos.blk_flags & BLKHDR_FLG_ENCRYPTED)
			printf("   => Encr key length %d, ukad length %d, "
				"akad length %d\n",
				current_pos.encryption_key_length,
				current_pos.encryption_ukad_length,
				current_pos.encryption_akad_length);
		break;
	case B_FILEMARK:
		printf("          Filemark");
		printf("(%02x), sz %13d, Blk No.: %" PRId64 ", prev %" PRId64
			", cur %" PRId64 ", next %" PRId64 "\n",
			current_pos.blk_type,
			current_pos.blk_size,
			(loff_t)current_pos.blk_number,
			current_pos.prev_blk,
			current_pos.curr_blk,
			current_pos.next_blk);
		break;
	case B_BOT:
		printf(" Beginning of Tape");
		printf("(%02x), Capacity %6dMbytes"
			", prev %" PRId64
			", cur %" PRId64
			", next %" PRId64 "\n",
			current_pos.blk_type,
			current_pos.blk_size,
			current_pos.prev_blk,
			current_pos.curr_blk,
			current_pos.next_blk);
		return;
		break;
	case B_BOT_V1:
		printf("   Old format Tape");
		break;
	case B_EOD:
		printf("       End of Data");
		printf("(%02x), sz %13d, Blk No.: %" PRId64 ", prev %" PRId64
			", cur %" PRId64 ", next %" PRId64 "\n",
			current_pos.blk_type,
			current_pos.blk_size,
			(loff_t)current_pos.blk_number,
			current_pos.prev_blk,
			current_pos.curr_blk,
			current_pos.next_blk);
		break;
	case B_NOOP:
		printf("      No Operation");
		break;
	default:
		printf("      Unknown type");
		printf("(%02x), %6d/%-6d, Blk No.: %" PRId64 ", prev %" PRId64
			", cur %" PRId64 ", next %" PRId64 "\n",
			current_pos.blk_type,
			current_pos.disk_blk_size,
			current_pos.blk_size,
			(loff_t)current_pos.blk_number,
			current_pos.prev_blk,
			current_pos.curr_blk,
			current_pos.next_blk);
		break;
	}
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

static void print_mam(struct MAM *mam)
{
	printf("Media density code: 0x%02x\n", mam->MediumDensityCode);
	printf("Media type code   : 0x%02x\n", mam->MediaType);
	printf("Media description : %s\n", mam->media_info.description);
}


int main(int argc, char *argv[])
{
	int ofp;
	char *dataFile = MHVTL_HOME_PATH;
	char sense_flg;
	loff_t nread;
	struct MAM mam;

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
	nread = read(ofp, &mam, sizeof(struct MAM));
	print_mam(&mam);
	while (current_pos.blk_type != B_EOD) {
		nread = skip_to_next_header(ofp, &sense_flg);
		if (nread == -1)
			break;
		else
			print_current_header();
	}
return (0);
}
