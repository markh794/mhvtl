/*
 * q.h -- Message queue for vtltape/vtllibrary
 *
 * Copyright (C) 2005 - 2025 Mark Harvey markh794 at gmail dot com
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

#include <stdlib.h>

#define MAXTEXTLEN 1024

struct q_msg {
	long snd_id;
	char text[MAXTEXTLEN + 1];
};

/* Default SysV IPC queue key — ASCII "Mark" (0x4d61726b) */
#define MHVTL_DEFAULT_QKEY	((key_t)0x4d61726b)
/* Base key for per-instance test isolation (high bits of "Mark") */
#define MHVTL_QKEY_BASE	((unsigned)0x4d000000)

/* Queue key: use MHVTL_QKEY env var if set, else default.
 * Allows multiple instances (e.g. tests) to run in parallel
 * without conflicting on the same system-wide queue. */
static inline key_t mhvtl_qkey(void) {
	const char *env = getenv("MHVTL_QKEY");
	return (env && env[0]) ? (key_t)strtoul(env, NULL, 0) : MHVTL_DEFAULT_QKEY;
}
#define QKEY	 mhvtl_qkey()
#define QPERM	 0660				  /* Permissions for queue */
#define MAXOBN	 sizeof(struct q_msg) /* Maximum length of message for Q. */
#define MAXPRIOR 1024				  /* max priority level */
#define VTLCMD_Q 32768				  /* Priority for vtlcmd */

struct q_entry {
	long		 rcv_id;
	struct q_msg msg;
};

int enter(char *, long rcv_id);
int send_msg(char *cmd, long rcv_id);
int serve(void);
int init_queue(void);

extern long my_id;

/* Message strings passed between vtllibrary & vtltape */
#define msg_not_occupied "Not occupied"
#define msg_occupied	 "occupied"
#define msg_unload_ok	 "Unloaded OK"
#define msg_load_failed	 "Load failed"
#define msg_load_ok		 "Loaded OK"
#define msg_mount_state	 "mount_state"
#define msg_eject		 "eject"
#define msg_set_empty	 "set_empty"

#endif /* _Q_H_ */
