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

long my_id = 0;

int
main(int argc, char **argv) {
int	mlen, r_qid;
struct q_entry r_entry;

	/* Initialize message queue as nessary */
	if ( (r_qid = init_queue()) == -1)
		return (-1);

	mlen = msgrcv(r_qid, &r_entry, MAXOBN, 0, IPC_NOWAIT);
	if (mlen > 0) {
		printf("Rcv_id : %ld, Snd_id : %ld, Message : %s\n",
			r_entry.rcv_id, r_entry.msg.snd_id, r_entry.msg.text);
	} else {
		printf("Nothing found in message Q\n");
	}
exit(0);
}

