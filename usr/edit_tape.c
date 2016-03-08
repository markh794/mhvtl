/*
 * edit_tape is portion of the mhVTL package.
 *
 * The vtl package consists of:
 *   a kernel module (vlt.ko) - Currently on 2.6.x Linux kernel support.
 *   SCSI target daemons for both SMC and SSC devices.
 *
 * Copyright (C) 2005 - 2013 Mark Harvey       markh794@gmail.com
 *                                          mark.harvey at veritas.com
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
static void *largefile_support = "large file support";
#else
static void *largefile_support = "No largefile support";
#endif

/* The following variables are needed for the MHVTL_DBG() macro to work. */

char vtl_driver_name[] = "edit_tape";
int verbose;
int debug;
int wp;	/* Write protect flag */
long my_id;
extern char home_directory[HOME_DIR_PATH_SZ + 1];

#define WRITE_PROTECT_OFF 1
#define WRITE_PROTECT_ON  2

void usage(char *progname)
{
	printf("Usage: %s -l lib -m PCL [-s size] [-t type] [-d density]"
		" [-w on|off]\n",
					progname);
	printf("       Where 'size' is in Megabytes\n");
	printf("             'lib' is Library number\n");
	printf("             'type' is data | clean | WORM\n");
	printf("             'PCL' is Physical Cartridge Label (barcode)\n");
	printf("             '-w on|off' enable/disable write protect flag\n");
	printf("             'density' can be on of the following:\n");
	printf("           AIT1     AIT2     AIT3     AIT4\n");
	printf("           DDS1     DDS2     DDS3     DDS4\n");
	printf("           DLT3     DLT4\n");
	printf("           SDLT1    SDLT220  SDLT320  SDLT600\n");
	printf("           LTO1     LTO2     LTO3     LTO4\n");
	printf("           LTO5     LTO6     LTO7\n");
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
	wp = 0;

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
				printf("%s: version %s\n%s\n\n",
						progname, MHVTL_VERSION,
						(char *)largefile_support);
				break;
			case 'v':
				verbose++;
				break;
			case 'w':
				if (argc > 1) {
					if (!strncasecmp("yes", argv[1], 3))
						wp = WRITE_PROTECT_ON;
					else if (!strncasecmp("on", argv[1], 3))
						wp = WRITE_PROTECT_ON;
					else
						wp = WRITE_PROTECT_OFF;
				} else {
					puts("    More args needed for -m\n");
					exit(1);
				}
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
	s = zalloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = zalloc(MALLOC_SZ);
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
		printf("New capacity for %s: %ldMB\n",
					pcl, (unsigned long)size);
	}

	if (mediaType) {
		if (!strncasecmp("clean", mediaType, 5)) {
			MHVTL_DBG(1, "Setting media type to CLEAN");
			new_mam.MediumType = MEDIA_TYPE_CLEAN;
			new_mam.MediumTypeInformation = 20;
		} else if (!strncasecmp("data", mediaType, 4)) {
			MHVTL_DBG(1, "Setting media type to DATA");
			new_mam.MediumType = MEDIA_TYPE_DATA;
		} else if (!strncasecmp("null", mediaType, 4)) {
			MHVTL_DBG(1, "Setting media type to NULL");
			new_mam.MediumType = MEDIA_TYPE_NULL;
		} else if (!strncasecmp("WORM", mediaType, 4)) {
			MHVTL_DBG(1, "Setting media type to WORM");
			new_mam.MediumType = MEDIA_TYPE_WORM;
		} else {
			printf("Unknown media type: %s\n", mediaType);
			usage(progname);
			exit(1);
		}
	}
	if (density) {
		printf("Setting density to %s\n", density);
		if (set_media_params(&new_mam, density)) {
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
	switch (wp) {
	case WRITE_PROTECT_ON:
		new_mam.Flags |= MAM_FLAGS_MEDIA_WRITE_PROTECT;
		printf("Setting write-protect for %s\n", pcl);
		break;
	case WRITE_PROTECT_OFF:
		new_mam.Flags &= ~MAM_FLAGS_MEDIA_WRITE_PROTECT;
		printf("Turning off write-protect for %s\n", pcl);
		break;
	}

	put_unaligned_be64(sizeof(mam.pad), &new_mam.MAMSpaceRemaining);

	memcpy(&mam, &new_mam, sizeof(mam));
	rewriteMAM(&sam_stat);
	unload_tape(&sam_stat);

	exit(0);
}
