/*
 * dump_messageQ - A utility to empty & examine a message queue
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
 *                                mark.harvey at veritas dot com
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
 *
 * Modification History:
 *    2010-03-31 hstadler - source code revision, argument checking
 *
 * Dump any existing data in the messageQ.
 */

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include "q.h"

long my_id;
int verbose = 0;
int debug = 0;
char *vtl_driver_name = "dump_messageQ";

static void usage(char *prog)
{
	fprintf(stdout, "Usage  : %s [-h|-help]\n", prog);
	fprintf(stdout, "Version: %s\n\n", MHVTL_VERSION);
	fprintf(stdout, "Dumping message queue content of "
		"library/tape queue.\n");
	fprintf(stdout, "Primarily used for debugging purposes.\n\n");
}

int main(int argc, char **argv)
{
	int r_qid;
	long mcounter = 0;
	int i;
	struct q_entry r_entry;

	my_id = 0;

	/* checking several positions of -h/-help */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h")) {
			usage(argv[0]);
			exit(1);
		}
		if (!strcmp(argv[i], "-?")) {
			usage(argv[0]);
			exit(1);
		}
		if (!strcmp(argv[i], "/h")) {
			usage(argv[0]);
			exit(1);
		}
		if (!strcmp(argv[i], "/?")) {
			usage(argv[0]);
			exit(1);
		}
		if (!strcmp(argv[i], "-help")) {
			usage(argv[0]);
			exit(1);
		}
	}

	if (argc > 1) {
		printf("Invalid option: %s\n", argv[1]);
		printf("Try '%s -h' for more information\n", argv[0]);
		exit(1);
	}

	/* Initialize message queue as necessary */
	r_qid = init_queue();
	if (r_qid == -1)
		exit(1);

	while (msgrcv(r_qid, &r_entry, MAXOBN, 0, IPC_NOWAIT) > 0) {
		mcounter++;
		if (mcounter == 1) {
			printf("\nDump Message Queue Content\n\n");
			printf("%6s %6s %6s %-55s\n", "MessNo", "RcvID",
				"SndID", "MessageText");
		}
		printf("%6ld %6ld %6ld %-55s\n", mcounter, r_entry.rcv_id,
			r_entry.msg.snd_id, r_entry.msg.text);
	}
	if (mcounter == 0)
		printf("Message queue empty\n");

	exit(0);
}

