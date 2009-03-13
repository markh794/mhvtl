/*
 * dump_messageQ - A utility to empty & examine a message queue
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
 *
 *	FIXME: Make into user friendly utility :-)
 *
 * Dump any existing data in the messageQ.
 */

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include "q.h"

int
init_queue(void) {
int	queue_id;

/* Attempt to create or open message queue */
if ( (queue_id = msgget(QKEY, IPC_CREAT | QPERM)) == -1)
	perror("msgget failed");

return (queue_id);
}

int
main(int argc, char **argv) {
int	mlen, r_qid;
struct q_entry r_entry;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s MessageQ.\n", argv[0]);
		exit(1);
	}

	/* Initialize message queue as nessary */
	if ( (r_qid = init_queue()) == -1)
		return (-1);

	mlen = msgrcv(r_qid, &r_entry, MAXOBN, 0, IPC_NOWAIT);
	if (mlen > 0) {
		r_entry.mtext[mlen] = '\0';
		printf("Message : %s\n", r_entry.mtext);
	} else {
		printf("Nothing found in message Q\n");
	}
exit(0);
}

