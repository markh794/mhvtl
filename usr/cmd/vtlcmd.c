/*
 * vtlcmd - A utility to send a message queue to the vtltape/vtllibrary
 *	    userspace daemons
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
 *
 * Modification History:
 *    2010-03-15 hstadler - source code revision, argument checking
 *
 *
 * FIXME: Server & Client are writing in the same queue, it would be better
 *        the client opens a "private" queue and the server writes the
 *        answers in this queue. In this case nobody can disturb the answer
 *        from the server. I think this is called connectionless protocol.
 *        But this means a redesign of some c-sources.
 *
 */

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <sys/ipc.h>
#include <errno.h>
#include <string.h>
#include <sys/msg.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <inttypes.h>
#include "q.h"
#include "vtllib.h"

char mhvtl_driver_name[] = "vtlcmd";

#define TYPE_UNKNOWN 0
#define TYPE_LIBRARY 1
#define TYPE_DRIVE	 2

static void usage(char *prog) {
	fprintf(stderr, "Usage  : %s <DeviceNo> <command> [-h|-help]\n", prog);
	fprintf(stderr, "Version: %s %s %s\n", MHVTL_VERSION, MHVTL_GITHASH, MHVTL_GITDATE);
	fprintf(stderr, "   Where 'DeviceNo' is the number"
					" associated with tape/library daemon\n\n");
	fprintf(stderr, "Global commands:\n");
	fprintf(stderr, "   verbose           -> To enable verbose logging\n");
	/*
	fprintf(stderr, "   debug             -> To enable debug logging\n");
	*/
	fprintf(stderr, "   TapeAlert #       -> 64bit TapeAlert mask (hex)\n");
	fprintf(stderr, "   InquiryDataChange -> Set LU state to indicate Inquiry Data Has Changed\n");
	fprintf(stderr, "   exit              -> To shutdown tape/library "
					"daemon/device\n");
	fprintf(stderr, "\nTape specific commands:\n");
	fprintf(stderr, "   Append Only [Yes|No] -> To 'load' media ID\n");
	fprintf(stderr, "   compression [zlib|lzo] -> Use zlib or lzo "
					"compression\n");
	fprintf(stderr, "   load ID        -> To 'load' media ID\n");
	fprintf(stderr, "   unload ID      -> To 'unload' media ID\n");
	fprintf(stderr, "   delay load n   -> Set load delay to n seconds\n");
	fprintf(stderr, "   delay unload n -> Set unload delay to n seconds\n");
	fprintf(stderr, "   delay rewind n -> Set rewind delay to n seconds\n");
	fprintf(stderr, "   delay position n -> Set position delay to n seconds\n");
	fprintf(stderr, "   delay thread n -> Set thread delay to n seconds\n");
	fprintf(stderr, "\nLibrary specific commands:\n");
	fprintf(stderr, "   add slot     -> Add a slot to library\n");
	fprintf(stderr, "   online       -> To enable library\n");
	fprintf(stderr, "   offline      -> To take library offline\n");
	fprintf(stderr, "   list map     -> To list map contents\n");
	fprintf(stderr, "   empty map [ID] -> To remove media (or only media ID) from map\n");
	fprintf(stderr, "   open map     -> Open map to allow media export\n");
	fprintf(stderr, "   close map    -> Close map to allow media import\n");
	fprintf(stderr, "   load map ID  -> Load media ID into map\n");
}

/*
 * Check that media (tape) exists in the library's home directory
 * (/var/lib/mhvtl/..): a directory for a file-backed cartridge, or a link to
 * a block device for a raw cartridge. The cartridge's own files are not
 * looked at, as their names vary: data.0, data.1, ... one per partition.
 */
int check_media(int libno, char *barcode) {
	char		currentMedia[1024];
	struct stat st;
	int			path_len;

	find_media_home_directory(NULL, libno);
	path_len = snprintf((char *)currentMedia, ARRAY_SIZE(currentMedia), "%s/%s", home_directory, barcode);
	if (path_len >= ARRAY_SIZE(currentMedia)) {
		fprintf(stderr, "Warning: path to %s/%s truncated to %" PRIu32 " bytes",
				home_directory, barcode, (uint32_t)ARRAY_SIZE(currentMedia));
	}
	if (stat(currentMedia, &st) < 0) {
		fprintf(stderr, "Could not find %s: %s\n",
				currentMedia, strerror(errno));
		return 1;
	}
	if (!S_ISDIR(st.st_mode) && !S_ISBLK(st.st_mode)) {
		fprintf(stderr, "%s is neither a cartridge directory nor a "
				"block device\n", currentMedia);
		return 1;
	}
	return 0;
}

/* Display the answer from daemon/service */
void DisplayResponse(int msqid, char *s) {
	struct q_entry r_entry;

	if (msgrcv(msqid, &r_entry, MAXOBN, mhvtl_reply_id(), 0) > 0)
		printf("%s%s\n", s, r_entry.msg.text);
}

/*
 * Display the drive's answer to 'request'. If the drive has not read it
 * within DRIVE_REPLY_GRACE seconds, withdraw it, so it is not carried out
 * later, and give up; once the drive has read it, the answer comes.
 */
int DisplayDriveResponse(int msqid, long drive, char *request) {
	struct q_entry r_entry;

	for (int i = 0; i < DRIVE_REPLY_GRACE * 10; i++) {
		if (msgrcv(msqid, &r_entry, MAXOBN, mhvtl_reply_id(), IPC_NOWAIT) > 0) {
			printf("%s\n", r_entry.msg.text);
			return 0;
		}
		usleep(100000);
	}
	my_id = mhvtl_reply_id(); /* withdraw_msg() matches the sender */
	switch (withdraw_msg(msqid, drive, request)) {
	case 0: /* The drive has it: the answer comes */
		DisplayResponse(msqid, "");
		return 0;
	case 1:
		fprintf(stderr, "No answer from the drive: request withdrawn\n");
		return 1;
	default:
		fprintf(stderr, "No answer from the drive\n");
		return 1;
	}
}

int ishex(char *str) {
	while (*str) {
		if (!isxdigit(*str))
			return 0;
		str++;
	}
	return 1;
}

int isnumeric(char *str) {
	while (*str) {
		if (!isdigit(*str))
			return 0;
		str++;
	}
	return 1;
}

/*
 * Validation functions return NULL on success, or an error message string
 * describing the problem. The caller (main) handles printing and exit.
 */

const char *Check_TapeAlert(int argc, char **argv) {
	if (argc > 3) {
		if (!ishex(argv[3]))
			return "TapeAlert: value not hexadecimal";
		if (argc == 4)
			return NULL;
		return "TapeAlert";
	}
	return "TapeAlert";
}

const char *Check_Load(int argc, char **argv) {
	if (argc > 3) {
		if (!strcmp(argv[3], "map")) {
			if (argc == 5)
				return NULL;
			return "load map";
		}
		if (argc == 4)
			return NULL;
		return "load";
	}
	return "load";
}

const char *Check_delay(int argc, char **argv) {
	if (argc > 4) {
		if (argc == 5) {
			if (atoi(argv[4]) >= 0)
				return NULL;
			return "delay: Negative value";
		}
		return "delay";
	}
	return "delay";
}

const char *Check_Unload(int argc, char **argv) {
	if (argc > 3) {
		if (argc == 4)
			return NULL;
		return "unload";
	}
	return "unload";
}

const char *Check_Compression(int argc, char **argv) {
	if (argc > 3) {
		if (argc == 4)
			return NULL;
		return "compression";
	}
	return "compression : missing lzo or zlib";
}

const char *Check_append_only(int argc, char **argv) {
	if (argc > 4) {
		if (argc == 5)
			return NULL;
		return "Append Only";
	}
	return "Append Only : missing Yes / No";
}

const char *Check_List(int argc, char **argv) {
	if (argc != 4)
		return "list map : too many args";
	if (strcmp(argv[3], "map"))
		return "list map : Can only list map";
	return NULL;
}

const char *Check_Empty(int argc, char **argv) {
	if (argc > 3) {
		if (!strcmp(argv[3], "map")) {
			if (argc == 4 || argc == 5)	/* optional barcode */
				return NULL;
		}
		return "empty map";
	}
	return "empty map";
}

const char *Check_Open(int argc, char **argv) {
	if (argc > 3) {
		if (!strcmp(argv[3], "map")) {
			if (argc == 4)
				return NULL;
		}
		return "open map";
	}
	return "open map";
}

const char *Check_Close(int argc, char **argv) {
	if (argc > 3) {
		if (!strcmp(argv[3], "map")) {
			if (argc == 4)
				return NULL;
		}
		return "close map";
	}
	return "close map";
}

const char *Check_Params(int argc, char **argv) {
	const char *err;

	if (argc > 1) {
		if (!isnumeric(argv[1]))
			return "DeviceNo not numeric";
		if (argc > 2) {
			/* global commands */
			if (!strcmp(argv[2], "verbose")) {
				if (argc == 3)
					return NULL;
				return "verbose";
			}
			if (!strcmp(argv[2], "dump")) {
				if (argc == 3)
					return NULL;
				return "dump";
			}
			if (!strcmp(argv[2], "debug")) {
				if (argc == 3)
					return NULL;
				return "debug";
			}
			if (!strcmp(argv[2], "exit")) {
				if (argc == 3)
					return NULL;
				return "exit";
			}
			if (!strncasecmp(argv[2], "InquiryDataChange", 17))
				return NULL;
			if (!strncasecmp(argv[2], "TapeAlert", 9)) {
				err = Check_TapeAlert(argc, argv);
				return err;
			}

			/* Tape commands */
			if (!strncasecmp(argv[2], "load", 4)) {
				err = Check_Load(argc, argv);
				return err;
			}
			if (!strncasecmp(argv[2], "unload", 6)) {
				err = Check_Unload(argc, argv);
				return err;
			}
			if (!strncasecmp(argv[2], "delay", 5)) {
				err = Check_delay(argc, argv);
				return err;
			}
			if (!strncasecmp(argv[2], "compression", 11)) {
				err = Check_Compression(argc, argv);
				return err;
			}
			if (!strncasecmp(argv[2], "Append", 6)) {
				err = Check_append_only(argc, argv);
				return err;
			}

			/* Library commands */
			if (!strcmp(argv[2], "add")) {
				if (argc == 4)
					return NULL;
				return "add slot";
			}
			if (!strcmp(argv[2], "online")) {
				if (argc == 3)
					return NULL;
				return "online";
			}
			if (!strcmp(argv[2], "offline")) {
				if (argc == 3)
					return NULL;
				return "offline";
			}
			if (!strcmp(argv[2], "list"))
				return Check_List(argc, argv);
			if (!strcmp(argv[2], "empty"))
				return Check_Empty(argc, argv);
			if (!strcmp(argv[2], "open"))
				return Check_Open(argc, argv);
			if (!strcmp(argv[2], "close"))
				return Check_Close(argc, argv);

			return "check param";
		}
		return "missing command";
	}
	return "missing arguments";
}

/*
 * Validate command for a specific device type.
 * Returns NULL if the command is allowed, or an error message if not.
 */
const char *Check_DeviceCommand(const char *buf, int device_type) {
	if (device_type == TYPE_LIBRARY) {
		if (!strncmp(buf, "online", 6)) {
		} else if (!strncmp(buf, "add slot", 8)) {
		} else if (!strncmp(buf, "offline", 7)) {
		} else if (!strncmp(buf, "open map", 8)) {
		} else if (!strncmp(buf, "close map", 9)) {
		} else if (!strncmp(buf, "empty map", 9)) {
		} else if (!strncmp(buf, "list map", 8)) {
		} else if (!strncmp(buf, "load map", 8)) {
		} else if (!strncmp(buf, "verbose", 7)) {
		} else if (!strncmp(buf, "debug", 5)) {
		} else if (!strncmp(buf, "exit", 4)) {
		} else if (!strncmp(buf, "TapeAlert", 9)) {
		} else if (!strncmp(buf, "InquiryDataChange", 17)) {
		} else {
			return "Command for library not allowed";
		}
	}

	if (device_type == TYPE_DRIVE) {
		if (!strncmp(buf, "load", 4)) {
		} else if (!strncmp(buf, "unload", 6)) {
		} else if (!strncmp(buf, "verbose", 7)) {
		} else if (!strncmp(buf, "debug", 5)) {
		} else if (!strncmp(buf, "dump", 4)) {
		} else if (!strncmp(buf, "exit", 4)) {
		} else if (!strncmp(buf, "compression", 11)) {
		} else if (!strncmp(buf, "TapeAlert", 9)) {
		} else if (!strncmp(buf, "InquiryDataChange", 17)) {
		} else if (!strncasecmp(buf, "append", 6)) {
		} else if (!strncasecmp(buf, "delay load", 10)) {
		} else if (!strncasecmp(buf, "delay unload", 12)) {
		} else if (!strncasecmp(buf, "delay rewind", 12)) {
		} else if (!strncasecmp(buf, "delay position", 14)) {
		} else if (!strncasecmp(buf, "delay thread", 12)) {
		} else {
			return "Command for tape not allowed";
		}
	}

	return NULL;
}

/* Open a new queue (for answers from server) */
/* Open an alreay opened queue (opened by server) */
int OpenExistingQueue(key_t key) {
	long queue_id;

	/* Attempt to open an existing message queue */
	queue_id = msgget(key, 0);
	if (queue_id == -1)
		fprintf(stderr, "%s: %s\n", __func__, strerror(errno));

	return queue_id;
}

/* Send command to queue */
int SendMsg(long ReceiverQid, long ReceiverMtyp, char *sndbuf) {
	struct q_entry s_entry;

	s_entry.rcv_id		= ReceiverMtyp;
	s_entry.msg.snd_id	= mhvtl_reply_id();
	s_entry.msg.text[0] = '\0';
	strncat(s_entry.msg.text, sndbuf, MAXTEXTLEN);

	int len = strlen(s_entry.msg.text) + 1 + sizeof(s_entry.msg.snd_id);
	if (msgsnd(ReceiverQid, &s_entry, len, 0) == -1)
		return -1;

	return 0;
}

int main(int argc, char **argv) {
	char  device_conf[CONF_FILE_SZ];
	FILE *conf;
	char  b[1024];
	int	  device_type = TYPE_UNKNOWN;
	long  deviceNo, indx;
	int	  count;
	char  buf[1024];
	char *p;

	my_id = VTLCMD_Q;

	if (get_config(device_conf, DEVICE_CONF, my_id) < 0)
		exit(1);

	if ((argc < 2) || (argc > 6)) {
		usage(argv[0]);
		exit(1);
	}

	/* checking several positions of -h/-help */
	for (count = 1; count < argc; count++) {
		if (!strcmp(argv[count], "-h")) {
			usage(argv[0]);
			exit(1);
		}
		if (!strcmp(argv[count], "/h")) {
			usage(argv[0]);
			exit(1);
		}
		if (!strcmp(argv[count], "-help")) {
			usage(argv[0]);
			exit(1);
		}
		if (!strcasecmp(argv[count], "-v")) {
			fprintf(stderr, "Version: %s %s %s\n", MHVTL_VERSION, MHVTL_GITHASH, MHVTL_GITDATE);
			exit(1);
		}
	}

	{
		const char *param_err = Check_Params(argc, argv);
		if (param_err) {
			fprintf(stderr, "Please check command, parameter \'%s\' wrong.\n\n",
					param_err);
			usage(argv[0]);
			exit(1);
		}
	}

	deviceNo = atol(argv[1]);
	if ((deviceNo < 0) || (deviceNo >= VTLCMD_Q)) {
		fprintf(stderr, "Invalid device number for "
						"tape/library: %s\n",
				argv[1]);
		exit(1);
	}

	conf = fopen(device_conf, "r");
	if (!conf) {
		fprintf(stderr, "Can not open config file %s : %s\n",
				device_conf, strerror(errno));
		exit(1);
	}

	p	   = buf;
	buf[0] = '\0';

	/* While read in a line */
	while (fgets(b, sizeof(b), conf) != NULL) {
		if (sscanf(b, "Drive: %ld ", &indx) == 1 &&
			indx == deviceNo) {
			device_type = TYPE_DRIVE;
			break;
		}
		if (sscanf(b, "Library: %ld ", &indx) == 1 &&
			indx == deviceNo) {
			device_type = TYPE_LIBRARY;
			break;
		}
	}
	fclose(conf);

	if (device_type == TYPE_UNKNOWN) {
		fprintf(stderr, "No tape/library (%s) configured with "
						"device number: %ld\n",
				device_conf, deviceNo);
		exit(1);
	}

	/* Concat all args into one string.
	 * Bound each write by remaining buffer space so an oversized argv
	 * cannot overflow buf[] (the outgoing message queue slot is
	 * sizeof(buf) bytes).
	 */
	{
		size_t remaining = sizeof(buf);
		p				 = buf;
		buf[0]			 = '\0';
		for (count = 2; count < argc; count++) {
			int n = snprintf(p, remaining, "%s ", argv[count]);
			if (n < 0 || (size_t)n >= remaining) {
				fprintf(stderr, "Command line too long "
								"(max %zu bytes)\n",
						sizeof(buf) - 1);
				exit(1);
			}
			p += n;
			remaining -= n;
		}
	}

	/* check if command to the specific device is allowed */
	if (device_type == TYPE_LIBRARY) {
		if (!strncmp(buf, "online", 6)) {
		} else if (!strncmp(buf, "add slot", 8)) {
		} else if (!strncmp(buf, "offline", 7)) {
		} else if (!strncmp(buf, "open map", 8)) {
		} else if (!strncmp(buf, "close map", 9)) {
		} else if (!strncmp(buf, "empty map", 9)) {
		} else if (!strncmp(buf, "list map", 8)) {
		} else if (!strncmp(buf, "load map", 8)) {
		} else if (!strncmp(buf, "verbose", 7)) {
		} else if (!strncmp(buf, "debug", 5)) {
		} else if (!strncmp(buf, "exit", 4)) {
		} else if (!strncmp(buf, "TapeAlert", 9)) {
		} else if (!strncmp(buf, "InquiryDataChange", 17)) {
		} else {
			fprintf(stderr, "Command for library not allowed\n");
			exit(1);
		}
	}

	/* check if command to the specific device is allowed */
	{
		const char *cmd_err = Check_DeviceCommand(buf, device_type);
		if (cmd_err) {
			fprintf(stderr, "%s\n", cmd_err);
			exit(1);
		}
	}

	/* Check for the existance of a datafile first - abort if not there */
	if (device_type == TYPE_LIBRARY) {
		if (!strcmp(argv[2], "load") && !strcmp(argv[3], "map")) {
			if (check_media(deviceNo, argv[4])) {
				fprintf(stderr, "Hint: Use command 'mktape' to "
								"create media first\n");
				exit(1);
			}
		}
	}

	long ReceiverQid;
	ReceiverQid = OpenExistingQueue(QKEY);
	if (ReceiverQid == -1) {
		fprintf(stderr, "MessageQueue not available\n");
		exit(1);
	}

	if (SendMsg(ReceiverQid, deviceNo, buf) < 0) {
		fprintf(stderr, "Message Queue Error: send message\n");
		exit(1);
	}

	if (device_type == TYPE_LIBRARY) {
		if (!strcmp(argv[2], "add") && !strcmp(argv[3], "slot"))
			DisplayResponse(ReceiverQid, "");
		if (!strcmp(argv[2], "open") && !strcmp(argv[3], "map"))
			DisplayResponse(ReceiverQid, "");
		if (!strcmp(argv[2], "close") && !strcmp(argv[3], "map"))
			DisplayResponse(ReceiverQid, "");
		if (!strcmp(argv[2], "empty") && !strcmp(argv[3], "map"))
			DisplayResponse(ReceiverQid, "");
		if (!strcmp(argv[2], "list") && !strcmp(argv[3], "map"))
			DisplayResponse(ReceiverQid, "Contents: ");
		if (!strcmp(argv[2], "load") && !strcmp(argv[3], "map"))
			DisplayResponse(ReceiverQid, "");
	}
	/* The drive answers an unload, which it refuses while removal is prevented */
	if (device_type == TYPE_DRIVE && !strcmp(argv[2], "unload"))
		exit(DisplayDriveResponse(ReceiverQid, deviceNo, buf));

	exit(0);
}
