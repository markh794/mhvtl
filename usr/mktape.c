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

void usage(char *progname) {
	printf("Usage: %s -l lib -m PCL -s size -t type -d density\n",
					progname);
	printf("       Where 'size' is in Megabytes\n");
	printf("             'type' is data | clean | WORM\n");
	printf("             'PCL' is Physical Cartridge Label (barcode)\n");
	printf("             'lib' is Library number\n");
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
	struct stat statb;
	struct passwd *pw;

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
				printf("%s: version %s\n%s\n\n",
						progname, MHVTL_VERSION,
						(char *)largefile_support);
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
	if (lib == NULL) {
		printf("Please supply Library number (-l xx)\n\n");
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

	sscanf(lib, "%d", &libno);
	if (!libno) {
		printf("Invalid library number\n");
		exit(1);
	}

	find_media_home_directory(home_directory, libno);

	if (strlen(pcl) > MAX_BARCODE_LEN) {
		printf("Max barcode length (%d) exceeded\n\n", MAX_BARCODE_LEN);
		usage(progname);
		exit(1);
	}

	pw = getpwnam(USR);	/* Find UID for user 'vtl' */

	/* Verify that the MHVTL home directory exists. */
	if (stat(home_directory, &statb) < 0 && errno == ENOENT) {
		umask(0007);
		if (mkdir(home_directory, 02770) < 0) {
			printf("Cannot create PCL %s, directory %s:"
				"Doesn't exist and cannot be created\n",
						pcl, home_directory);
			exit(1);
		}
	}

	/* Don't really care if this fails or not..
	 * But lets try anyway
	 */
	if (chown(home_directory, pw->pw_uid, pw->pw_gid))
		;

	/* Initialize the contents of the MAM to be used for the new PCL. */
	bzero((uint8_t *)&mam, sizeof(mam));

	mam.tape_fmt_version = TAPE_FMT_VERSION;
	mam.mam_fmt_version = MAM_VERSION;
	put_unaligned_be64(size * 1048576, &mam.max_capacity);
	put_unaligned_be64(size * 1048576, &mam.remaining_capacity);
	put_unaligned_be64(sizeof(mam.pad), &mam.MAMSpaceRemaining);

	memcpy(&mam.MediumManufacturer, "linuxVTL", 8);
	memcpy(&mam.ApplicationVendor, "vtl-1.4 ", 8);
	sprintf((char *)mam.ApplicationVersion, "%d", TAPE_FMT_VERSION);

	if (! strncmp("clean", mediaType, 5)) {
		mam.MediumType = MEDIA_TYPE_CLEAN;	/* Cleaning cart */
		mam.MediumTypeInformation = 20;		/* Max cleaning loads */
	} else if (! strncmp("WORM", mediaType, 4)) {
		mam.MediumType = MEDIA_TYPE_WORM;	/* WORM cart */
	} else {
		mam.MediumType = MEDIA_TYPE_DATA;	/* Normal data cart */
	}
	set_media_params(&mam, density);

	sprintf((char *)mam.MediumSerialNumber, "%s_%d", pcl, (int)time(NULL));
	sprintf((char *)mam.MediumManufactureDate, "%d", (int)time(NULL));
	sprintf((char *)mam.Barcode, "%-31s", pcl);

	/* Create the PCL using the initialized MAM. */

	exit(create_tape(pcl, &mam, &sam_stat));
}
