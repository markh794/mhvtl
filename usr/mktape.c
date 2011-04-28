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
#include "list.h"
#include "vtl_common.h"
#include "vtltape.h"
#include "vtllib.h"

#if defined _LARGEFILE64_SOURCE
void *largefile_support = "large file support";
#else
void *largefile_support = "No largefile support";
#endif

/* The following variables are needed for the MHVTL_DBG() macro to work. */

char vtl_driver_name[] = "mktape";
int verbose = 0;
int debug = 0;
long my_id = 0;

void usage(char *progname) {
	printf("Usage: %s -m PCL -s size -t type -d density\n", progname);
	printf("       Where 'size' is in Megabytes\n");
	printf("             'type' is data | clean | WORM\n");
	printf("             'PCL' is Physical Cartridge Label (barcode)\n");
	printf("             'density' is\n");
	printf("                   AIT1\n");
	printf("                   AIT2\n");
	printf("                   AIT3\n");
	printf("                   AIT4\n");
	printf("                   DDS1\n");
	printf("                   DDS2\n");
	printf("                   DDS3\n");
	printf("                   DDS4\n");
	printf("                   DLT3\n");
	printf("                   DLT4\n");
	printf("                   LTO1\n");
	printf("                   LTO2\n");
	printf("                   LTO3\n");
	printf("                   LTO4\n");
	printf("                   LTO5\n");
	printf("                   SDLT1\n");
	printf("                   SDLT2\n");
	printf("                   SDLT3\n");
	printf("                   SDLT4\n");
	printf("                   T10KA\n");
	printf("                   T10KB\n");
	printf("                   T10KC\n");
	printf("                   J1A\n");
	printf("                   E05\n");
	printf("                   E06\n\n");
}

static unsigned int set_params(struct MAM *mamp, char *density)
{
	mamp->MediaType = Media_undefined;
	if (!(strncmp(density, "LTO1", 4))) {
		mamp->MediumDensityCode = medium_density_code_lto1;
		mamp->MediaType = Media_LTO1;
		mamp->MediumLength = htonl(384);	// 384 tracks
		mamp->MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mamp->media_info.description, "Ultrium 1/8T", 12);
		memcpy(&mamp->media_info.density_name, "U-18  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		mamp->media_info.bits_per_mm = htonl(4880);
	} else if (!(strncmp(density, "LTO2", 4))) {
		mamp->MediumDensityCode = medium_density_code_lto2;
		mamp->MediaType = Media_LTO2;
		mamp->MediumLength = htonl(512);	// 512 tracks
		mamp->MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mamp->media_info.description, "Ultrium 2/8T", 12);
		memcpy(&mamp->media_info.density_name, "U-28  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		mamp->media_info.bits_per_mm = htonl(7398);
	} else if (!(strncmp(density, "LTO3", 4))) {
		if (mamp->MediumType == MEDIA_TYPE_WORM)
			mamp->MediumDensityCode = medium_density_code_lto3_WORM;
		else
			mamp->MediumDensityCode = medium_density_code_lto3;
		mamp->MediaType = Media_LTO3;
		mamp->MediumLength = htonl(704);	// 704 tracks
		mamp->MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mamp->media_info.description, "Ultrium 3/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-316 ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		mamp->media_info.bits_per_mm = htonl(9638);
	} else if (!(strncmp(density, "LTO4", 4))) {
		if (mamp->MediumType == MEDIA_TYPE_WORM)
			mamp->MediumDensityCode = medium_density_code_lto4_WORM;
		else
			mamp->MediumDensityCode = medium_density_code_lto4;
		mamp->MediaType = Media_LTO4;
		mamp->MediumLength = htonl(896);	// 896 tracks
		mamp->MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mamp->media_info.description, "Ultrium 4/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-416  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		mamp->media_info.bits_per_mm = htonl(12725);
	} else if (!(strncmp(density, "LTO5", 4))) {
		if (mamp->MediumType == MEDIA_TYPE_WORM)
			mamp->MediumDensityCode = medium_density_code_lto5_WORM;
		else
			mamp->MediumDensityCode = medium_density_code_lto5;
		mamp->MediaType = Media_LTO5;
		mamp->MediumLength = htonl(1280);	/* 1280 tracks */
		mamp->MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mamp->media_info.description, "Ultrium 5/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-516  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		mamp->media_info.bits_per_mm = htonl(15142);
	} else if (!(strncmp(density, "LTO6", 4))) { /* FIXME */
		mamp->MediumDensityCode = medium_density_code_lto6;
		mamp->MediaType = Media_LTO5;
		mamp->MediumLength = htonl(1280);	// 896 tracks
		mamp->MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mamp->media_info.description, "Ultrium 6/8T", 12);
		memcpy(&mamp->media_info.density_name, "U-616  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		mamp->media_info.bits_per_mm = htonl(12725);
	} else if (!(strncmp(density, "AIT1", 4))) {
	/* Vaules for AIT taken from "Product Manual SDX-900V v1.0" */
		mamp->MediumDensityCode = medium_density_code_ait1;
		mamp->MediaType = Media_AIT1;
		mamp->MediumLength = htonl(384);	// 384 tracks
		mamp->MediumWidth = htonl(0x50);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mamp->media_info.description, "AdvIntelligentTape1", 20);
		memcpy(&mamp->media_info.density_name, "AIT-1 ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		mamp->media_info.bits_per_mm = htonl(0x11d7);
	} else if (!(strncmp(density, "AIT2", 4))) {
		mamp->MediumDensityCode = medium_density_code_ait2;
		mamp->MediaType = Media_AIT2;
		mamp->MediumLength = htonl(384);	// 384 tracks
		mamp->MediumWidth = htonl(0x50);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mamp->media_info.description, "AdvIntelligentTape2", 20);
		memcpy(&mamp->media_info.density_name, "AIT-2  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		mamp->media_info.bits_per_mm = htonl(0x17d6);
	} else if (!(strncmp(density, "AIT3", 4))) {
		mamp->MediumDensityCode = medium_density_code_ait3;
		mamp->MediaType = Media_AIT3;
		mamp->MediumLength = htonl(384);	// 384 tracks
		mamp->MediumWidth = htonl(0x50);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mamp->media_info.description, "AdvIntelligentTape3", 20);
		memcpy(&mamp->media_info.density_name, "AIT-3  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		mamp->media_info.bits_per_mm = htonl(0x17d6);
	} else if (!(strncmp(density, "AIT4", 4))) {
		mamp->MediumDensityCode = medium_density_code_ait4;
		mamp->MediaType = Media_AIT4;
		mamp->MediumLength = htonl(384);	// 384 tracks
		mamp->MediumWidth = htonl(0x50);	// 127 x tenths of mm (12.7 mm)
		memcpy(&mamp->media_info.description, "AdvIntelligentTape4", 20);
		memcpy(&mamp->media_info.density_name, "AIT-4  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		mamp->media_info.bits_per_mm = htonl(0x17d6);
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
		mamp->media_info.bits_per_mm = htonl(133000);
	} else if (!(strncmp(density, "SDLT2", 5))) {
		mamp->MediumDensityCode = medium_density_code_220;
		mamp->MediaType = Media_SDLT220;
		memcpy(&mamp->media_info.description, "SDLT I media", 12);
		memcpy(&mamp->media_info.density_name, "SDLT220", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		mamp->media_info.bits_per_mm = htonl(133000);
	} else if (!(strncmp(density, "SDLT3", 5))) {
		mamp->MediumDensityCode = medium_density_code_320;
		mamp->MediaType = Media_SDLT320;
		memcpy(&mamp->media_info.description, "SDLT I media", 12);
		memcpy(&mamp->media_info.density_name, "SDLT320", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		mamp->media_info.bits_per_mm = htonl(190000);
	} else if (!(strncmp(density, "SDLT4", 5))) {
		mamp->MediumDensityCode = medium_density_code_600;
		mamp->MediaType = Media_SDLT600;
		memcpy(&mamp->media_info.description, "SDLT II media", 13);
		memcpy(&mamp->media_info.density_name, "SDLT600", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		mamp->media_info.bits_per_mm = htonl(233000);
	} else if (!(strncmp(density, "T10KA", 5))) {
		mamp->MediumDensityCode = medium_density_code_10kA;
		mamp->MediaType = Media_T10KA;
		memcpy(&mamp->media_info.description, "STK T10KA media", 15);
		memcpy(&mamp->media_info.density_name, "T10000A", 7);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		mamp->media_info.bits_per_mm = htonl(233000);
	} else if (!(strncmp(density, "T10KB", 5))) {
		mamp->MediumDensityCode = medium_density_code_10kB;
		mamp->MediaType = Media_T10KB;
		memcpy(&mamp->media_info.description, "STK T10KB media", 15);
		memcpy(&mamp->media_info.density_name, "T10000B", 7);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		mamp->media_info.bits_per_mm = htonl(233000);
	} else if (!(strncmp(density, "T10KC", 5))) {
		mamp->MediumDensityCode = medium_density_code_10kC;
		mamp->MediaType = Media_T10KC;
		memcpy(&mamp->media_info.description, "STK T10KC media", 15);
		memcpy(&mamp->media_info.density_name, "T10000C", 7);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		mamp->media_info.bits_per_mm = htonl(233000);
	} else if (!(strncmp(density, "DDS1", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS1;
		mamp->MediaType = Media_DDS1;
		memcpy(&mamp->media_info.description, "4MM DDS-1 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS1", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		mamp->media_info.bits_per_mm = htonl(233000);
	} else if (!(strncmp(density, "DDS2", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS2;
		mamp->MediaType = Media_DDS2;
		memcpy(&mamp->media_info.description, "4MM DDS-2 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS2", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		mamp->media_info.bits_per_mm = htonl(233000);
	} else if (!(strncmp(density, "DDS3", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS3;
		mamp->MediaType = Media_DDS3;
		memcpy(&mamp->media_info.description, "4MM DDS-3 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS3", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		mamp->media_info.bits_per_mm = htonl(233000);
	} else if (!(strncmp(density, "DDS4", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS4;
		mamp->MediaType = Media_DDS4;
		memcpy(&mamp->media_info.description, "4MM DDS-4 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS4", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		mamp->media_info.bits_per_mm = htonl(233000);
	} else if (!(strncmp(density, "J1A", 3))) {
		mamp->MediumDensityCode = medium_density_code_j1a;
		mamp->MediaType = Media_3592_JA;
		memcpy(&mamp->media_info.description, "3592 J1A media", 14);
		memcpy(&mamp->media_info.density_name, "3592J1A", 7);
		memcpy(&mamp->AssigningOrganization_1, "IBM", 3);
		mamp->media_info.bits_per_mm = htonl(233000);
	} else if (!(strncmp(density, "E05", 3))) {
		mamp->MediumDensityCode = medium_density_code_e05;
		mamp->MediaType = Media_3592_JB;
		memcpy(&mamp->media_info.description, "3592 E05 media", 14);
		memcpy(&mamp->media_info.density_name, "3592E05", 7);
		memcpy(&mamp->AssigningOrganization_1, "IBM", 3);
		mamp->media_info.bits_per_mm = htonl(233000);
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
		exit(1);
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
	struct stat statb;

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
		printf("Please supply a barcode (-b barcode)\n\n");
		usage(progname);
		exit(1);
	}
	if (mediaCapacity == NULL) {
		printf("Please supply media capacity (-s xx)\n\n");
		usage(progname);
		exit(1);
	}
	if (mediaType == NULL) {
		printf("Please supply cart type (-t data|clean|WORM)\n\n");
		usage(progname);
		exit(1);
	}

	if (density == NULL) {
		printf("Please supply media density (-d xx)\n\n");
		usage(progname);
		exit(1);
	}

	sscanf(mediaCapacity, "%" PRId64, &size);
	if (size == 0)
		size = 8000;

	if (strlen(pcl) > MAX_BARCODE_LEN) {
		printf("Max barcode length (%d) exceeded\n\n", MAX_BARCODE_LEN);
		usage(progname);
		exit(1);
	}

	/* Verify that the MHVTL home directory exists. */

	if (stat(MHVTL_HOME_PATH, &statb) < 0 && errno == ENOENT) {
		if (mkdir(MHVTL_HOME_PATH, 0770) < 0) {
			printf("Cannot create PCL %s, directory " MHVTL_HOME_PATH
				" does not exist and cannot be created\n", pcl);
			exit(1);
		}
	}

	/* Initialize the contents of the MAM to be used for the new PCL. */

	memset((uint8_t *)&mam, 0, sizeof(mam));

	mam.tape_fmt_version = TAPE_FMT_VERSION;
	mam.mam_fmt_version = MAM_VERSION;
	mam.max_capacity = htonll(size * 1048576);

	mam.MAMSpaceRemaining = htonll(sizeof(mam.pad));
	memcpy(&mam.MediumManufacturer, "VERITAS ", 8);
	memcpy(&mam.ApplicationVendor, "vtl-0.18", 8);
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

	/* Create the PCL using the initialized MAM. */

	exit(create_tape(pcl, &mam, &sam_stat));
}
