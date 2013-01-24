/*
 * edit_tape is portion of the mhVTL package.
 *
 * The vtl package consists of:
 *   a kernel module (vlt.ko) - Currently on 2.6.x Linux kernel support.
 *   SCSI target daemons for both SMC and SSC devices.
 *
 * Copyright (C) 2005 - 2013 Mark Harvey       markh794@gmail.com
 *                                          mark_harvey@symantec.com
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
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "be_byteshift.h"
#include "list.h"
#include "vtl_common.h"
#include "vtltape.h"
#include "vtllib.h"
#include "logging.h"

#if defined _LARGEFILE64_SOURCE
void *largefile_support = "large file support";
#else
void *largefile_support = "No largefile support";
#endif

/* The following variables are needed for the MHVTL_DBG() macro to work. */

char vtl_driver_name[] = "edit_tape";
int verbose;
int debug;
long my_id;
extern char home_directory[HOME_DIR_PATH_SZ + 1];

void usage(char *progname)
{
	printf("Usage: %s -l lib -m PCL -s size -t type -d density\n",
					progname);
	printf("       Where 'size' is in Megabytes\n");
	printf("             'lib' is Library number\n");
	printf("             'type' is data | clean | WORM\n");
	printf("             'PCL' is Physical Cartridge Label (barcode)\n");
	printf("             'density' can be on of the following:\n");
	printf("           AIT1     AIT2     AIT3     AIT4\n");
	printf("           DDS1     DDS2     DDS3     DDS4\n");
	printf("           DLT3     DLT4\n");
	printf("           SDLT1    SDLT220  SDLT320  SDLT600\n");
	printf("           LTO1     LTO2     LTO3     LTO4\n");
	printf("           LTO5     LTO6\n");
	printf("           T10KA    T10KB    T10KC\n");
	printf("           9840A    9840B    9840C    9840D\n");
	printf("           9940A    9940B\n");
	printf("           J1A      E05      E06\n\n");
}

static unsigned int set_params(struct MAM *mamp, char *density)
{
	/* Invent some defaults */
	mamp->MediaType = Media_undefined;
	put_unaligned_be32(2048, &mamp->media_info.bits_per_mm);
	put_unaligned_be16(1, &mamp->media_info.tracks);
	put_unaligned_be32(127, &mamp->MediumWidth);
	put_unaligned_be32(1024, &mamp->MediumLength);
	memcpy(&mamp->media_info.description, "mhvtl", 5);

	if (!(strncmp(density, "LTO1", 4))) {
		mamp->MediumDensityCode = medium_density_code_lto1;
		mamp->MediaType = Media_LTO1;
		put_unaligned_be32(384, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 1/8T", 12);
		memcpy(&mamp->media_info.density_name, "U-18  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(4880, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "LTO2", 4))) {
		mamp->MediumDensityCode = medium_density_code_lto2;
		mamp->MediaType = Media_LTO2;
		put_unaligned_be32(512, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 2/8T", 12);
		memcpy(&mamp->media_info.density_name, "U-28  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(7398, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "LTO3", 4))) {
		if (mamp->MediumType == MEDIA_TYPE_WORM)
			mamp->MediumDensityCode = medium_density_code_lto3_WORM;
		else
			mamp->MediumDensityCode = medium_density_code_lto3;
		mamp->MediaType = Media_LTO3;
		put_unaligned_be32(704, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 3/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-316 ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(9638, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "LTO4", 4))) {
		if (mamp->MediumType == MEDIA_TYPE_WORM)
			mamp->MediumDensityCode = medium_density_code_lto4_WORM;
		else
			mamp->MediumDensityCode = medium_density_code_lto4;
		mamp->MediaType = Media_LTO4;
		put_unaligned_be32(896, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 4/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-416  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(12725, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "LTO5", 4))) {
		if (mamp->MediumType == MEDIA_TYPE_WORM)
			mamp->MediumDensityCode = medium_density_code_lto5_WORM;
		else
			mamp->MediumDensityCode = medium_density_code_lto5;
		mamp->MediaType = Media_LTO5;
		put_unaligned_be32(1280, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 5/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-516  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(15142, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "LTO6", 4))) { /* FIXME */
		mamp->MediumDensityCode = medium_density_code_lto6;
		mamp->MediaType = Media_LTO6;
		put_unaligned_be32(2176, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 6/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-616  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(18441, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "AIT1", 4))) {
	/* Vaules for AIT taken from "Product Manual SDX-900V v1.0" */
		mamp->MediumDensityCode = medium_density_code_ait1;
		mamp->MediaType = Media_AIT1;
		put_unaligned_be32(384, &mamp->MediumLength);
		put_unaligned_be32(0x50, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "AdvIntelligentTape1", 20);
		memcpy(&mamp->media_info.density_name, "AIT-1 ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		put_unaligned_be32(0x11d7, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "AIT2", 4))) {
		mamp->MediumDensityCode = medium_density_code_ait2;
		mamp->MediaType = Media_AIT2;
		put_unaligned_be32(384, &mamp->MediumLength);
		put_unaligned_be32(0x50, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "AdvIntelligentTape2", 20);
		memcpy(&mamp->media_info.density_name, "AIT-2  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		put_unaligned_be32(0x17d6, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "AIT3", 4))) {
		mamp->MediumDensityCode = medium_density_code_ait3;
		mamp->MediaType = Media_AIT3;
		put_unaligned_be32(384, &mamp->MediumLength);
		put_unaligned_be32(0x50, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "AdvIntelligentTape3", 20);
		memcpy(&mamp->media_info.density_name, "AIT-3  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		put_unaligned_be32(0x17d6, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "AIT4", 4))) {
		mamp->MediumDensityCode = medium_density_code_ait4;
		mamp->MediaType = Media_AIT4;
		put_unaligned_be32(384, &mamp->MediumLength);
		put_unaligned_be32(0x50, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "AdvIntelligentTape4", 20);
		memcpy(&mamp->media_info.density_name, "AIT-4  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		put_unaligned_be32(0x17d6, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "DLT3", 4))) {
		mamp->MediumDensityCode = 0x0;
		mamp->MediaType = Media_DLT3;
		memcpy(&mamp->media_info.description, "DLT4000 media", 13);
		memcpy(&mamp->media_info.density_name, "DLT-III", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
	} else if (!(strncmp(density, "DLT4", 4))) {
		mamp->MediumDensityCode = 0x0;
		mamp->MediaType = Media_DLT4;
		memcpy(&mamp->media_info.description, "DLT7000 media", 13);
		memcpy(&mamp->media_info.density_name, "DLT-IV", 6);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
	} else if (!(strncmp(density, "SDLT1", 5))) {
		mamp->MediumDensityCode = 0x48;
		mamp->MediaType = Media_SDLT;
		memcpy(&mamp->media_info.description, "SDLT I media", 12);
		memcpy(&mamp->media_info.density_name, "SDLT-1", 6);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		put_unaligned_be32(133000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "SDLT220", 7))) {
		mamp->MediumDensityCode = medium_density_code_220;
		mamp->MediaType = Media_SDLT220;
		memcpy(&mamp->media_info.description, "SDLT I media", 12);
		memcpy(&mamp->media_info.density_name, "SDLT220", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		put_unaligned_be32(133000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "SDLT320", 7))) {
		mamp->MediumDensityCode = medium_density_code_320;
		mamp->MediaType = Media_SDLT320;
		memcpy(&mamp->media_info.description, "SDLT I media", 12);
		memcpy(&mamp->media_info.density_name, "SDLT320", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		put_unaligned_be32(190000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "SDLT600", 7))) {
		mamp->MediumDensityCode = medium_density_code_600;
		mamp->MediaType = Media_SDLT600;
		memcpy(&mamp->media_info.description, "SDLT II media", 13);
		memcpy(&mamp->media_info.density_name, "SDLT600", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "9840A", 5))) {
		mamp->MediumDensityCode = medium_density_code_9840A;
		mamp->MediaType = Media_9840A;
		memcpy(&mamp->media_info.description, "Raven 20 GB", 11);
		memcpy(&mamp->media_info.density_name, "R-20", 4);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(288, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "9840B", 5))) {
		mamp->MediumDensityCode = medium_density_code_9840B;
		mamp->MediaType = Media_9840B;
		memcpy(&mamp->media_info.description, "Raven 20 GB", 11);
		memcpy(&mamp->media_info.density_name, "R-20", 4);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(288, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "9840C", 5))) {
		mamp->MediumDensityCode = medium_density_code_9840C;
		mamp->MediaType = Media_9840C;
		memcpy(&mamp->media_info.description, "Raven 40 GB", 11);
		memcpy(&mamp->media_info.density_name, "R-40", 4);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(288, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "9840D", 5))) {
		mamp->MediumDensityCode = medium_density_code_9840D;
		mamp->MediaType = Media_9840D;
		memcpy(&mamp->media_info.description, "Raven 75 GB", 11);
		memcpy(&mamp->media_info.density_name, "R-75", 4);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(576, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "9940A", 5))) {
		mamp->MediumDensityCode = medium_density_code_9940A;
		mamp->MediaType = Media_9940A;
		memcpy(&mamp->media_info.description, "PeakCapacity 60 GB", 18);
		memcpy(&mamp->media_info.density_name, "P-60", 4);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(288, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "9940B", 5))) {
		mamp->MediumDensityCode = medium_density_code_9940B;
		mamp->MediaType = Media_9940B;
		memcpy(&mamp->media_info.description,
						"PeakCapacity 200 GB", 19);
		memcpy(&mamp->media_info.density_name, "P-200", 5);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(576, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "T10KA", 5))) {
		mamp->MediumDensityCode = medium_density_code_10kA;
		mamp->MediaType = Media_T10KA;
		memcpy(&mamp->media_info.description, "STK T10KA media", 15);
		memcpy(&mamp->media_info.density_name, "T10000A", 7);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "T10KB", 5))) {
		mamp->MediumDensityCode = medium_density_code_10kB;
		mamp->MediaType = Media_T10KB;
		memcpy(&mamp->media_info.description, "STK T10KB media", 15);
		memcpy(&mamp->media_info.density_name, "T10000B", 7);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "T10KC", 5))) {
		mamp->MediumDensityCode = medium_density_code_10kC;
		mamp->MediaType = Media_T10KC;
		memcpy(&mamp->media_info.description, "STK T10KC media", 15);
		memcpy(&mamp->media_info.density_name, "T10000C", 7);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "DDS1", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS1;
		mamp->MediaType = Media_DDS1;
		memcpy(&mamp->media_info.description, "4MM DDS-1 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS1", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "DDS2", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS2;
		mamp->MediaType = Media_DDS2;
		memcpy(&mamp->media_info.description, "4MM DDS-2 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS2", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "DDS3", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS3;
		mamp->MediaType = Media_DDS3;
		memcpy(&mamp->media_info.description, "4MM DDS-3 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS3", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "DDS4", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS4;
		mamp->MediaType = Media_DDS4;
		memcpy(&mamp->media_info.description, "4MM DDS-4 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS4", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "J1A", 3))) {
		mamp->MediumDensityCode = medium_density_code_j1a;
		mamp->MediaType = Media_3592_JA;
		memcpy(&mamp->media_info.description, "3592 J1A media", 14);
		memcpy(&mamp->media_info.density_name, "3592J1A", 7);
		memcpy(&mamp->AssigningOrganization_1, "IBM", 3);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "E05", 3))) {
		mamp->MediumDensityCode = medium_density_code_e05;
		mamp->MediaType = Media_3592_JB;
		memcpy(&mamp->media_info.description, "3592 E05 media", 14);
		memcpy(&mamp->media_info.density_name, "3592E05", 7);
		memcpy(&mamp->AssigningOrganization_1, "IBM", 3);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "E06", 3))) {
		mamp->MediumDensityCode = medium_density_code_e06;
		mamp->MediaType = Media_3592_JX;
		memcpy(&mamp->media_info.description, "3592 E06 media", 14);
		memcpy(&mamp->media_info.density_name, "3592E06", 7);
		memcpy(&mamp->AssigningOrganization_1, "IBM", 3);
	} else
		printf("'%s' is an invalid density\n", density);

	if (mamp->MediaType == Media_undefined)	{
		printf("mamp->MediaType is still Media_undefined, exiting\n");
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	unsigned char sam_stat;
	char *progname = argv[0];
	char *pcl = NULL;
	char *mediaType = NULL;
	char *mediaCapacity = NULL;
	char *density = NULL;
	uint64_t size;
	struct MAM new_mam;
	char *lib = NULL;
	int libno = 0;
	int indx;
	int rc;
	char *config = MHVTL_CONFIG_PATH"/device.conf";
	FILE *conf;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */

	if (sizeof(struct MAM) != 1024) {
		printf("Structure of MAM incorrect size: %d\n",
						(int)sizeof(struct MAM));
		exit(2);
	}

	if (argc < 2) {
		usage(progname);
		exit(1);
	}

	debug = 0;
	my_id = 0;
	verbose = 0;

	while (argc > 0) {
		if (argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				if (argc > 1)
					density = argv[1];
				break;
			case 'l':
				if (argc > 1) {
					lib = argv[1];
				} else {
					puts("    More args needed for -l\n");
					exit(1);
				}
				break;
			case 'm':
				if (argc > 1) {
					pcl = argv[1];
				} else {
					puts("    More args needed for -m\n");
					exit(1);
				}
				break;
			case 's':
				if (argc > 1) {
					mediaCapacity = argv[1];
				} else {
					puts("    More args needed for -s\n");
					exit(1);
				}
				break;
			case 't':
				if (argc > 1) {
					mediaType = argv[1];
				} else {
					puts("    More args needed for -t\n");
					exit(1);
				}
				break;
			case 'V':
				printf("%s: version %s\n\n",
						progname, MHVTL_VERSION);
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
		printf("Please supply a barcode (-b barcode)\n\n");
		usage(progname);
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

	if (lib) {
		sscanf(lib, "%d", &libno);
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

	/* Copy media MAM into temp location */
	memcpy(&new_mam, &mam, sizeof(mam));

	size = 0L;
	if (mediaCapacity) {
		sscanf(mediaCapacity, "%" PRId64, &size);
		printf("New capacity %ldMB\n", (unsigned long)size);
	}

	if (mediaType) {
		if (strncmp("clean", mediaType, 5)) {
			MHVTL_DBG(1, "Setting media type to CLEAN\n");
			new_mam.MediumType = MEDIA_TYPE_CLEAN;
			new_mam.MediumTypeInformation = 20;
		} else if (!strncmp("WORM", mediaType, 4)) {
			MHVTL_DBG(1, "Setting media type to WORM\n");
			new_mam.MediumType = MEDIA_TYPE_WORM;
		} else {
			MHVTL_DBG(1, "Setting media type to DATA\n");
			new_mam.MediumType = MEDIA_TYPE_DATA;
		}
	}
	if (density) {
		printf("Setting density to %s\n", density);
		if (set_params(&new_mam, density)) {
			printf("Could not determine media density: %s\n",
					density);
			unload_tape(&sam_stat);
			exit(1);
		}
	}
	if (size) {
		put_unaligned_be64(size * 1048576, &new_mam.max_capacity);
		/* This will set incorrect value to start with but next media
		 * Usage, this will be recalculated correctly
		 */
		put_unaligned_be64(size * 1048576, &new_mam.remaining_capacity);
	}
	put_unaligned_be64(sizeof(mam.pad), &new_mam.MAMSpaceRemaining);

	memcpy(&mam, &new_mam, sizeof(mam));
	rewriteMAM(&sam_stat);
	unload_tape(&sam_stat);

	printf("Successfully updated %s\n", pcl);
	exit(0);
}
