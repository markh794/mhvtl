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
#include <pwd.h>
#include "be_byteshift.h"
#include "list.h"
#include "vtl_common.h"
#include "vtltape.h"
#include "vtllib.h"

#if defined _LARGEFILE64_SOURCE
static void *largefile_support = "large file support";
#else
static void *largefile_support = "No largefile support";
#endif

/* The following variables are needed for the MHVTL_DBG() macro to work. */

char vtl_driver_name[] = "mktape";
int verbose = 0;
int debug = 0;
long my_id = 0;
extern char home_directory[HOME_DIR_PATH_SZ + 1];

static void usage(char *progname)
{
	printf("Usage: %s [OPTIONS] [REQUIRED-PARAMS]", progname);
	printf("Where OPTIONS are from:\n");
	printf("      -h    -- print this message and exit\n");
	printf("      -v    -- verbose\n");
	printf("      -D[N] -- set debug level to N [1]\n");
	printf("      -V    -- print Version and exit\n");
	printf("      -C config-dir -- override default config dir [ %s ]\n",
					MHVTL_CONFIG_PATH);
	printf("      -H home-dir   -- override default home dir [ %s ]\n",
					MHVTL_HOME_PATH);
	printf("And REQUIRED-PARAMS are:\n");
	printf("      -l lib      -- set Library number\n");
	printf("      -m PCL      -- set Physical Cartrige Label (barcode)\n");
	printf("      -s size     -- set size in Megabytes\n");
	printf("      -t type     -- set to data, clean, WORM, or NULL\n");
	printf("      -d density  -- set density to one of:\n");
	printf("           AIT1     AIT2     AIT3     AIT4\n");
	printf("           DDS1     DDS2     DDS3     DDS4\n");
	printf("           DLT3     DLT4\n");
	printf("           SDLT1    SDLT220  SDLT320  SDLT600\n");
	printf("           LTO1     LTO2     LTO3     LTO4\n");
	printf("           LTO5     LTO6     LTO7     LTO8\n");
	printf("           T10KA    T10KB    T10KC\n");
	printf("           9840A    9840B    9840C    9840D\n");
	printf("           9940A    9940B\n");
	printf("           J1A      E05      E06      E07\n");
	printf("\n");
}

int main(int argc, char *argv[])
{
	unsigned char sam_stat;
	char *progname = argv[0];
	char *pcl = NULL;
	char *mediaType = NULL;
	char *mediaCapacity = NULL;
	char *density = NULL;
	char *lib = NULL;
	uint64_t size;
	int libno;
	int opt;
	int res;
	char *param_config_dir = NULL;
	char *param_home_dir = NULL;

	if (sizeof(struct MAM) != 1024) {
		fprintf(stderr, "error: Structure of MAM incorrect size: %d\n",
						(int)sizeof(struct MAM));
		exit(2);
	}

	if (argc < 2) {
		fprintf(stderr, "error: not enough arguments\n");
		usage(progname);
		exit(1);
	}

	while ((opt = getopt(argc, argv, "d:l:m:s:t:VvD::hC:H:")) != -1) {
		switch (opt) {
		case 'h':
			usage(progname);
			exit(0);
		case 'd':
			density = strdup(optarg);
			break;
		case 'l':
			lib = strdup(optarg);
			break;
		case 'm':
			pcl = strdup(optarg);
			break;
		case 's':
			mediaCapacity = strdup(optarg);
			break;
		case 't':
			mediaType = strdup(optarg);
			break;
		case 'C':
			param_config_dir = strdup(optarg);
			break;
		case 'H':
			param_home_dir = strdup(optarg);
			break;
		case 'V':
			printf("%s: version %s\n", progname, MHVTL_VERSION);
			printf("%s\n", (char *)largefile_support);
			exit(0);
		case 'D':
			if (optarg)
				debug = strtol(optarg, NULL, 0);
			else
				debug++;
			break;
		case 'v':
			verbose++;
			break;
		default:
			fprintf(stderr, "error: unknown option: %c\n", opt);
			usage(progname);
			exit(1);
		}
	}

	if (pcl == NULL) {
		fprintf(stderr, "error: Please supply a barcode (-b barcode)\n\n");
		usage(progname);
		exit(1);
	}
	if (mediaCapacity == NULL) {
		fprintf(stderr, "error: Please supply media capacity (-s xx)\n\n");
		usage(progname);
		exit(1);
	}
	if (mediaType == NULL) {
		fprintf(stderr, "error: Please supply cart type (-t data|clean|WORM|NULL)\n\n");
		usage(progname);
		exit(1);
	}
	if (lib == NULL) {
		fprintf(stderr, "error: Please supply Library number (-l xx)\n\n");
		usage(progname);
		exit(1);
	}
	if (density == NULL) {
		fprintf(stderr, "error: Please supply media density (-d xx)\n\n");
		usage(progname);
		exit(1);
	}

	sscanf(mediaCapacity, "%" PRId64, &size);
	if (size == 0)
		size = 8000;

	sscanf(lib, "%d", &libno);
	if (!libno) {
		fprintf(stderr, "error: Invalid library number\n");
		exit(1);
	}

	if (param_home_dir)
		strncpy(home_directory, param_home_dir, HOME_DIR_PATH_SZ);
	else
		find_media_home_directory(param_config_dir, home_directory, libno);

	if (strlen(pcl) > MAX_BARCODE_LEN) {
		fprintf(stderr, "error: Max barcode length (%d) exceeded\n\n", MAX_BARCODE_LEN);
		usage(progname);
		exit(1);
	}

	/* Initialize the contents of the MAM to be used for the new PCL. */
	memset((uint8_t *)&mam, 0, sizeof(mam));

	mam.tape_fmt_version = TAPE_FMT_VERSION;
	mam.mam_fmt_version = MAM_VERSION;
	put_unaligned_be64(size * 1048576, &mam.max_capacity);
	put_unaligned_be64(size * 1048576, &mam.remaining_capacity);
	put_unaligned_be64(sizeof(mam.pad), &mam.MAMSpaceRemaining);

	memcpy(&mam.MediumManufacturer, "linuxVTL", 8);
	memcpy(&mam.ApplicationVendor, "vtl-1.5 ", 8);
	sprintf((char *)mam.ApplicationVersion, "%d", TAPE_FMT_VERSION);

	if (!strncmp("clean", mediaType, 5)) {
		mam.MediumType = MEDIA_TYPE_CLEAN;	/* Cleaning cart */
		mam.MediumTypeInformation = 20;		/* Max cleaning loads */
	} else if (!strncmp("NULL", mediaType, 4)) {
		mam.MediumType = MEDIA_TYPE_NULL;	/* save metadata only */
	} else if (!strncmp("WORM", mediaType, 4)) {
		mam.MediumType = MEDIA_TYPE_WORM;	/* WORM cart */
	} else {
		mam.MediumType = MEDIA_TYPE_DATA;	/* Normal data cart */
	}
	set_media_params(&mam, density);

	sprintf((char *)mam.MediumSerialNumber, "%s_%d", pcl, (int)time(NULL));
	sprintf((char *)mam.MediumManufactureDate, "%d", (int)time(NULL));
	sprintf((char *)mam.Barcode, "%-31s", pcl);

	/* Create the PCL using the initialized MAM. */

	if (verbose)
		printf("Creating tape data ...\n");
	res = create_tape(pcl, &mam, &sam_stat);

	exit(res);
}
