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
#include "vx.h"
#include "vtltape.h"
#include "vxshared.h"

const char *mktapeVersion = "$Id: mktape.c,v 1.16.0.0 2007-09-26 07:58:44 markh Exp $";

#ifndef Solaris
  loff_t lseek64(int, loff_t, int);
#endif

int verbose = 0;
int debug = 0;

void
usage(char *progname) {

	printf("Usage: %s -m PCL -s size -t type\n", progname);
	printf("       Where 'size' is in Megabytes\n");
	printf("             'type' is data | clean | WORM\n");
	printf("             'PCL' is Physical Cartridge Label (barcode)\n\n");
}

int
main(int argc, char *argv[]) {
	int	file;
	struct	blk_header h;
	struct	MAM mam;
	u8	currentMedia[1024];
	long	nwrite;
	char	*progname = argv[0];
	char	*pcl = NULL;
	char	*mediaType = NULL;
	char	*mediaCapacity = NULL;
	u32	size;

	if (argc < 2) {
		usage(progname);
		exit(1);
	}

	while(argc > 0) {
		if (argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				debug++;
				verbose = 9;	// If debug, make verbose...
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
				printf("%s: version %s\n",
						progname, mktapeVersion);
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

	sscanf(mediaCapacity, "%d", &size);
	if (size == 0)
		size = 8000;

	h.blk_type = B_BOT;
	h.blk_number = 0;
	h.blk_size = size;
	h.curr_blk = 0;
	h.prev_blk = 0;
	h.next_blk = sizeof(mam) + sizeof(h);

	memset((u8 *)&mam, 0, sizeof(mam));

	mam.tape_fmt_version = TAPE_FMT_VERIONS;
	mam.max_capacity = htonll(size * 1048576);
	mam.MAMSpaceRemaining = htonll(sizeof(mam.VendorUnique));
	mam.MediumLength = htonl(384);	// 384 tracks
	mam.MediumWidth = htonl(127);	// 127 x tenths of mm (12.7 mm)
	memcpy(&mam.MediumManufacturer, "Mark    ", 8);
	memcpy(&mam.ApplicationVendor, "Harvey  ", 8);

	if (! strncmp("clean", mediaType, 5)) {
		mam.MediumType = MEDIA_TYPE_CLEAN; // Cleaning cart
		mam.MediumTypeInformation = 20;	// Max cleaning loads
	} else if (! strncmp("WORM", mediaType, 4)) {
		mam.MediumType = MEDIA_TYPE_WORM; // WORM cart
	} else {
		mam.MediumType = MEDIA_TYPE_DATA; // Normal data cart
	}

	sprintf((char *)mam.MediumSerialNumber, "%s_%d", pcl, (int)time(NULL));
	sprintf((char *)mam.Barcode, "%-31s", pcl);

	sprintf((char *)currentMedia, "%s/%s", HOME_PATH, pcl);
	syslog(LOG_DAEMON|LOG_INFO, "%s being created", currentMedia);
	if ((file = creat((char *)currentMedia,S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)) == -1) {
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

