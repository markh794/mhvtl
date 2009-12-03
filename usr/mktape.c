/*
 * mktape
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
#include "vtl_common.h"
#include "vtltape.h"
#include "vtllib.h"

#ifndef Solaris
  loff_t lseek64(int, loff_t, int);
#endif

int verbose = 0;
int debug = 0;

void usage(char *progname) {
	printf("Usage: %s -m PCL -s size -t type -d density\n", progname);
	printf("       Where 'size' is in Megabytes\n");
	printf("             'type' is data | clean | WORM\n");
	printf("             'PCL' is Physical Cartridge Label (barcode)\n");
	printf("             'density' is\n");
	printf("                   LTO1\n");
	printf("                   LTO2\n");
	printf("                   LTO3\n");
	printf("                   LTO4\n");
	printf("                   DLT3\n");
	printf("                   DLT4\n");
	printf("                   SDLT1\n");
	printf("                   SDLT2\n");
	printf("                   SDLT3\n");
	printf("                   SDLT4\n");
	printf("                   T10KA\n");
	printf("                   T10KB\n");
	printf("                   AIT1\n");
	printf("                   AIT2\n");
	printf("                   AIT3\n");
	printf("                   AIT4\n\n");
}

static unsigned int set_params(struct MAM *mam, char *density)
{
	mam->MediaType = Media_undefined;
	if (!(strncmp(density, "LTO1", 4))) {
		mam->MediumDensityCode = medium_density_code_lto1;
		mam->MediaType = Media_LTO1;
		mam->MediumLength = htonl(384);	// 384 tracks
		mam->MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mam->media_info.description, "Ultrium 1/8T", 12);
		memcpy(&mam->media_info.density_name, "U-18  ", 6);
		memcpy(&mam->AssigningOrganization_1, "LTO-CVE", 7);
		mam->media_info.bits_per_mm = htonl(4880);
	}
	if (!(strncmp(density, "LTO2", 4))) {
		mam->MediumDensityCode = medium_density_code_lto2;
		mam->MediaType = Media_LTO2;
		mam->MediumLength = htonl(512);	// 512 tracks
		mam->MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mam->media_info.description, "Ultrium 2/8T", 12);
		memcpy(&mam->media_info.density_name, "U-28  ", 6);
		memcpy(&mam->AssigningOrganization_1, "LTO-CVE", 7);
		mam->media_info.bits_per_mm = htonl(7398);
	}
	if (!(strncmp(density, "LTO3", 4))) {
		mam->MediumDensityCode = medium_density_code_lto3;
		mam->MediaType = Media_LTO3;
		mam->MediumLength = htonl(704);	// 704 tracks
		mam->MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mam->media_info.description, "Ultrium 3/8T", 12);
		memcpy(&mam->media_info.density_name, "U-316 ", 6);
		memcpy(&mam->AssigningOrganization_1, "LTO-CVE", 7);
		mam->media_info.bits_per_mm = htonl(9638);
	}
	if (!(strncmp(density, "LTO4", 4))) {
		mam->MediumDensityCode = medium_density_code_lto4;
		mam->MediaType = Media_LTO4;
		mam->MediumLength = htonl(896);	// 896 tracks
		mam->MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mam->media_info.description, "Ultrium 4/8T", 12);
		memcpy(&mam->media_info.density_name, "U-416  ", 6);
		memcpy(&mam->AssigningOrganization_1, "LTO-CVE", 7);
		mam->media_info.bits_per_mm = htonl(12725);
	}
	/* Vaules for AIT taken from "Product Manual SDX-900V v1.0" */
	if (!(strncmp(density, "AIT1", 4))) {
		mam->MediumDensityCode = 0x30;
		mam->MediaType = Media_AIT1;
		mam->MediumLength = htonl(384);	// 384 tracks
		mam->MediumWidth = htonl(0x50);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mam->media_info.description, "AdvIntelligentTape1", 20);
		memcpy(&mam->media_info.density_name, "AIT-1 ", 6);
		memcpy(&mam->AssigningOrganization_1, "SONY", 4);
		mam->media_info.bits_per_mm = htonl(0x11d7);
	}
	if (!(strncmp(density, "AIT2", 4))) {
		mam->MediumDensityCode = 0x31;
		mam->MediaType = Media_AIT2;
		mam->MediumLength = htonl(384);	// 384 tracks
		mam->MediumWidth = htonl(0x50);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mam->media_info.description, "AdvIntelligentTape2", 20);
		memcpy(&mam->media_info.density_name, "AIT-2  ", 6);
		memcpy(&mam->AssigningOrganization_1, "SONY", 4);
		mam->media_info.bits_per_mm = htonl(0x17d6);
	}
	if (!(strncmp(density, "AIT3", 4))) {
		mam->MediumDensityCode = 0x32;
		mam->MediaType = Media_AIT3;
		mam->MediumLength = htonl(384);	// 384 tracks
		mam->MediumWidth = htonl(0x50);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mam->media_info.description, "AdvIntelligentTape3", 20);
		memcpy(&mam->media_info.density_name, "AIT-3  ", 6);
		memcpy(&mam->AssigningOrganization_1, "SONY", 4);
		mam->media_info.bits_per_mm = htonl(0x17d6);
	}
	if (!(strncmp(density, "AIT4", 4))) {
		mam->MediumDensityCode = 0x33;
		mam->MediaType = Media_AIT4;
		mam->MediumLength = htonl(384);	// 384 tracks
		mam->MediumWidth = htonl(0x50);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mam->media_info.description, "AdvIntelligentTape4", 20);
		memcpy(&mam->media_info.density_name, "AIT-4  ", 6);
		memcpy(&mam->AssigningOrganization_1, "SONY", 4);
		mam->media_info.bits_per_mm = htonl(0x17d6);
	}
	if (!(strncmp(density, "DLT3", 4))) {
		mam->MediumDensityCode = 0x0;
		mam->MediaType = Media_DLT3;
		memcpy(&mam->media_info.description, "DLT4000 media", 13);
		memcpy(&mam->media_info.density_name, "DLT-III", 7);
		memcpy(&mam->AssigningOrganization_1, "QUANTUM", 7);
	}
	if (!(strncmp(density, "DLT4", 4))) {
		mam->MediumDensityCode = 0x0;
		mam->MediaType = Media_DLT4;
		memcpy(&mam->media_info.description, "DLT7000 media", 13);
		memcpy(&mam->media_info.density_name, "DLT-IV", 6);
		memcpy(&mam->AssigningOrganization_1, "QUANTUM", 7);
	}
	if (!(strncmp(density, "SDLT1", 5))) {
		mam->MediumDensityCode = 0x48;
		mam->MediaType = Media_SDLT;
		memcpy(&mam->media_info.description, "SDLT I media", 12);
		memcpy(&mam->media_info.density_name, "SDLT-1", 6);
		memcpy(&mam->AssigningOrganization_1, "QUANTUM", 7);
		mam->media_info.bits_per_mm = htonl(133000);
	}
	if (!(strncmp(density, "SDLT2", 5))) {
		mam->MediumDensityCode = 0x48;
		mam->MediaType = Media_SDLT220;
		memcpy(&mam->media_info.description, "SDLT I media", 12);
		memcpy(&mam->media_info.density_name, "SDLT220", 7);
		memcpy(&mam->AssigningOrganization_1, "QUANTUM", 7);
		mam->media_info.bits_per_mm = htonl(133000);
	}
	if (!(strncmp(density, "SDLT3", 5))) {
		mam->MediumDensityCode = 0x49;
		mam->MediaType = Media_SDLT320;
		memcpy(&mam->media_info.description, "SDLT I media", 12);
		memcpy(&mam->media_info.density_name, "SDLT320", 7);
		memcpy(&mam->AssigningOrganization_1, "QUANTUM", 7);
		mam->media_info.bits_per_mm = htonl(190000);
	}
	if (!(strncmp(density, "SDLT4", 5))) {
		mam->MediumDensityCode = 0x4a;
		mam->MediaType = Media_SDLT600;
		memcpy(&mam->media_info.description, "SDLT II media", 13);
		memcpy(&mam->media_info.density_name, "SDLT600", 7);
		memcpy(&mam->AssigningOrganization_1, "QUANTUM", 7);
		mam->media_info.bits_per_mm = htonl(233000);
	}
	if (!(strncmp(density, "T10KA", 5))) {
		mam->MediumDensityCode = 0x4a;
		mam->MediaType = Media_T10KA;
		memcpy(&mam->media_info.description, "STK T10KA media", 15);
		memcpy(&mam->media_info.density_name, "T10000A", 7);
		memcpy(&mam->AssigningOrganization_1, "STK", 3);
		mam->media_info.bits_per_mm = htonl(233000);
	}
	if (!(strncmp(density, "T10KB", 5))) {
		mam->MediumDensityCode = 0x4b;
		mam->MediaType = Media_T10KB;
		memcpy(&mam->media_info.description, "STK T10Kb media", 15);
		memcpy(&mam->media_info.density_name, "T10000B", 7);
		memcpy(&mam->AssigningOrganization_1, "STK", 3);
		mam->media_info.bits_per_mm = htonl(233000);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int file;
	struct blk_header h;
	uint8_t currentMedia[1024];
	long nwrite;
	char *progname = argv[0];
	char *pcl = NULL;
	char *mediaType = NULL;
	char *mediaCapacity = NULL;
	char *density = NULL;
	uint32_t size;
	struct MAM mam;

	if (sizeof(struct MAM) != 1024) {
		printf("Structure of MAM incorrect size: %d\n",
						(int)sizeof(struct MAM));
		exit(2);
	}

	if (argc < 2) {
		usage(progname);
		exit(1);
	}

	while(argc > 0) {
		if (argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				if (argc > 1)
					density = argv[1];
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
		usage(progname);
		exit(1);
	}
	if (mediaCapacity == NULL) {
		usage(progname);
		exit(1);
	}
	if (mediaType == NULL) {
		usage(progname);
		exit(1);
	}

	if (density == NULL) {
		usage(progname);
		exit(1);
	}

	sscanf(mediaCapacity, "%d", &size);
	if (size == 0)
		size = 8000;

	h.blk_type = B_BOT;
	h.blk_number = 0;
	h.blk_size = size;
	h.curr_blk = 0;
	h.prev_blk = 0;
	h.next_blk = sizeof(mam) + sizeof(h);

	memset((uint8_t *)&mam, 0, sizeof(mam));

	mam.tape_fmt_version = TAPE_FMT_VERSION;
	mam.mam_fmt_version = MAM_VERSION;
	mam.max_capacity = htonll(size * 1048576);

	mam.MAMSpaceRemaining = htonll(sizeof(mam.pad));
	memcpy(&mam.MediumManufacturer, "VERITAS ", 8);
	memcpy(&mam.ApplicationVendor, "vtl-0.16", 8);
	sprintf((char *)mam.ApplicationVersion, "%d", TAPE_FMT_VERSION);

	if (! strncmp("clean", mediaType, 5)) {
		mam.MediumType = MEDIA_TYPE_CLEAN; // Cleaning cart
		mam.MediumTypeInformation = 20;	// Max cleaning loads
	} else if (! strncmp("WORM", mediaType, 4)) {
		mam.MediumType = MEDIA_TYPE_WORM; // WORM cart
	} else {
		mam.MediumType = MEDIA_TYPE_DATA; // Normal data cart
	}
	set_params(&mam, density);

	sprintf((char *)mam.MediumSerialNumber, "%s_%d", pcl, (int)time(NULL));
	sprintf((char *)mam.MediumManufactureDate, "%d", (int)time(NULL));
	sprintf((char *)mam.Barcode, "%-31s", pcl);

	sprintf((char *)currentMedia, "%s/%s", MHVTL_HOME_PATH, pcl);
	syslog(LOG_DAEMON|LOG_INFO, "%s being created", currentMedia);
	file = creat((char *)currentMedia,S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	if (file == -1) {
		perror("Failed creating file");
		exit(2);
	}
	nwrite = write(file, &h, sizeof(h));
	if (nwrite <= 0) {
		perror("Unable to write header");
		exit(1);
	}
	nwrite = write(file, &mam, sizeof(mam));
	if (nwrite <= 0) {
		perror("Unable to write MAM");
		exit(1);
	}
	memset(&h, 0, sizeof(h));
	h.blk_type = B_EOD;
	h.blk_number = 0;
	h.curr_blk = lseek64(file, 0, SEEK_CUR);
	h.prev_blk = 0;
	h.next_blk = h.curr_blk;

	nwrite = write(file, &h, sizeof(h));
	if (nwrite <= 0) {
		perror("Unable to write header");
		exit(1);
	}
	close(file);

exit(0);
}

