/*
 * q.h -- Message queue for vx_tape/vx_library
 *
 * $Id: q.h,v 1.5.2.1 2006-08-06 07:58:44 markh Exp $
 *
 * Copyright (C) 2005 Mark Harvey markh794 at gmail dot com
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
 * Message key - My 2 seconds of fame :-)
 */

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <string.h>
#include <errno.h>

#define QKEY	(key_t)0x4d61726b	// Identifying key for queue
#define QPERM	0660		// Permissions for queue
#define MAXOBN	1024		// Maxmum lenght of message for Q.
#define MAXPRIOR 32		// max priority level
#define LIBRARY_Q 32768		// Priority for Library controller

struct q_entry {
	long mtype;
	char mtext[MAXOBN+1];
};

// void warn(char *);
int enter(char *, int);
int send_msg(char *cmd, int q_id);
int serve(void);
int init_queue(void);

