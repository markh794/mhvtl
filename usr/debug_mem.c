/*
 * This utility is used to set SSC serial number.
 *
 * The vtl package consists of:
 *   a kernel module (vlt.ko) - Currently on 2.6.x Linux kernel support.
 *   SCSI target daemons for both SMC and SSC devices.
 *
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
#include <syslog.h>
#include <inttypes.h>
#include "scsi.h"
#include "vx.h"
#include "vxshared.h"

/*
 * Define DEBUG to 0 and recompile to remove most debug messages.
 * or DEFINE TO 1 to make the -d (debug operation) mode more chatty
 */

#define DEBUG 0

#if DEBUG

#define DEB(a) a
#define DEBC(a) if (debug) { a ; }

#else

#define DEB(a)
#define DEBC(a)

#endif

#ifndef Solaris
  int ioctl(int, int, void *);
#endif

int verbose = 0;
int debug = 0;
int reset = 1;		/* Tape drive has been 'reset' */
char blockDescriptorBlock[8];
uint8_t sense[36];

/* Null function */
int init_queue(void)
{
	return 0;
}

static void usage(char *progname)
{
	printf("Usage: %s -q <Q number> [-d] [-v]\n",
						 progname);
	printf("       Where file == data file\n");
	printf("              'q number' is the queue priority number\n");
	printf("              'd' == debug\n");
	printf("              'v' == verbose\n");
}

int main(int argc, char *argv[])
{
	int cdev;
	int index = 0;
	char sn[] = "0";

	char *progname = argv[0];

	char *name = "vtl";
	int minor = 0;

	/* Output file pointer (data file) */
	if (argc < 2) {
		usage(argv[0]);
		printf("  -- Not enough parameters --\n");
		exit(1);
	}

	while (argc > 0) {
		if (argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				debug++;
				verbose = 9;	// If debug, make verbose...
				break;
			case 'v':
				verbose++;
				break;
			case 'q':
				if (argc > 1)
					index = atoi(argv[1]);
				break;
			default:
				usage(progname);
				printf("    Unknown option %c\n", argv[0][1]);
				exit(1);
				break;
			}
		}
		argv++;
		argc--;
	}

	if (!index) {
		printf("Require drive queue number\n");
		fflush(NULL);
		usage(progname);
	}
	cdev = chrdev_open(name, index);
	if (cdev < 0) {
		syslog(LOG_DAEMON|LOG_ERR,
				"Could not open /dev/%s%d: %m", name, minor);
		fflush(NULL);
		exit(1);
	}

	if (ioctl(cdev, VTL_DEBUG_MEM, sn) < 0) {
		perror("Failed setting serial number");
		exit(1);
	} else {
		syslog(LOG_DAEMON|LOG_INFO, "Dump mem linked list\n");
	}

	exit(0);
}

