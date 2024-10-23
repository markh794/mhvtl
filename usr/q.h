/*
 * q.h -- Message queue for vtltape/vtllibrary
 *
 * Copyright (C) 2005 Mark Harvey markh794 at gmail dot com
 *                                mark.harvey at nutanix dot com
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
 * Message key - My 2 seconds of fame :-)
 */

#ifndef _Q_H_
#define _Q_H_

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <string.h>
#include <errno.h>

#define	MAXTEXTLEN	1024

struct	q_msg {
	long snd_id;
	char text[MAXTEXTLEN+1];
};

#define QKEY	(key_t)0x4d61726b	/* Identifying key for queue */
#define QPERM	0660		/* Permissions for queue */
#define MAXOBN	sizeof(struct q_msg)	/* Maximum length of message for Q. */
#define MAXPRIOR 1024		/* max priority level */
#define VTLCMD_Q 32768		/* Priority for vtlcmd */

struct q_entry {
	long rcv_id;
	struct q_msg msg;
};


int enter(char *, long rcv_id);
int send_msg(char *cmd, long rcv_id);
int serve(void);
int init_queue(void);

extern long my_id;

/* Message strings passed between vtllibrary & vtltape */
#define msg_not_occupied	"Not occupied"
#define msg_occupied		"occupied"
#define msg_unload_ok		"Unloaded OK"
#define msg_load_failed		"Load failed"
#define msg_load_ok		"Loaded OK"
#define msg_mount_state		"mount_state"
#define msg_eject		"eject"
#define msg_set_empty		"set_empty"

#endif /* _Q_H_ */
