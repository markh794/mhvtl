/*
 * vtlcmd - A utility to send a message queue to the vtltape/vtllibrary
 *	    userspace daemons
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
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <inttypes.h>
#include "list.h"
#include "q.h"
#include "vtl_common.h"
#include "vtllib.h"

long my_id = VTLCMD_Q;
char vtl_driver_name[] = "vtlcmd";
int verbose = 0;
int debug = 0;

#define TYPE_UNKNOWN 0
#define TYPE_LIBRARY 1
#define TYPE_DRIVE 2

extern char home_directory[HOME_DIR_PATH_SZ + 1];

void find_media_home_directory(char *home_directory, long lib_id);

static void usage(char *prog)
{
	fprintf(stderr, "Usage  : %s <DeviceNo> <command> [-h|-help]\n", prog);
	fprintf(stderr, "Version: %s\n\n", MHVTL_VERSION);
	fprintf(stderr, "   Where 'DeviceNo' is the number"
			" associated with tape/library daemon\n\n");
	fprintf(stderr, "Global commands:\n");
	fprintf(stderr, "   verbose     -> To enable verbose logging\n");
	/*
	fprintf(stderr, "   debug       -> To enable debug logging\n");
	*/
	fprintf(stderr, "   TapeAlert # -> 64bit TapeAlert mask (hex)\n");
	fprintf(stderr, "   exit        -> To shutdown tape/library "
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
	fprintf(stderr, "   empty map    -> To remove media from map\n");
	fprintf(stderr, "   open map     -> Open map to allow media export\n");
	fprintf(stderr, "   close map    -> Close map to allow media import\n");
	fprintf(stderr, "   load map ID  -> Load media ID into map\n");
}

/* check if media (tape) exists in directory (/opt/mhvtl/..) */
int check_media(int libno, char *barcode)
{
	char currentMedia[1024];
	int datafile;

	find_media_home_directory(home_directory, libno);
	snprintf((char *)currentMedia, ARRAY_SIZE(currentMedia), "%s/%s/data", home_directory, barcode);
	datafile = open(currentMedia, O_RDWR|O_LARGEFILE);
	if (datafile < 0) {
		fprintf(stderr, "Could not open %s: %s\n",
			currentMedia, strerror(errno));
		return 1;
	}
	close(datafile);
	return 0;
}

/* Display the answer from daemon/service */
void DisplayResponse(int msqid, char *s)
{
	struct q_entry	r_entry;

	if (msgrcv(msqid, &r_entry, MAXOBN, VTLCMD_Q, 0) > 0)
		printf("%s%s\n", s, r_entry.msg.text);
}

int ishex(char *str)
{
	while (*str) {
		if (!isxdigit(*str))
			return 0;
		str++;
	}
	return 1;
}

int isnumeric(char *str)
{
	while (*str) {
		if (!isdigit(*str))
			return 0;
		str++;
	}
	return 1;
}

void PrintErrorExit(char *prog, char *s)
{
	fprintf(stderr, "Please check command, parameter \'%s\' wrong.\n\n", s);
	usage(prog);
	exit(1);
}

void Check_TapeAlert(int argc, char **argv)
{
	if (argc > 3) {
		if (!ishex(argv[3])) {
			fprintf(stderr, "Value not hexadecimal: %s\n", argv[3]);
			exit(1);
		}
		if (argc == 4)
			return;

		PrintErrorExit(argv[0], "TapeAlert");
	}
	PrintErrorExit(argv[0], "TapeAlert");
}

void Check_Load(int argc, char **argv)
{
	if (argc > 3) {
		if (!strcmp(argv[3], "map")) {
			if (argc == 5)
				return;

			PrintErrorExit(argv[0], "load map");
		}

		if (argc == 4)
			return;

		PrintErrorExit(argv[0], "load");
	}
	PrintErrorExit(argv[0], "load");
}

void Check_delay(int argc, char **argv)
{
	if (argc > 4) {
		if (argc == 5) {
			if (atoi(argv[4]) >= 0)
				return;
			PrintErrorExit(argv[0], "delay: Negative value");
		}
		PrintErrorExit(argv[0], "delay");
	}
	PrintErrorExit(argv[0], "delay");
}

void Check_Unload(int argc, char **argv)
{
	if (argc > 3) {
		if (argc == 4)
			return;

		PrintErrorExit(argv[0], "unload");
	}
	PrintErrorExit(argv[0], "unload");
}

void Check_Compression(int argc, char **argv)
{
	if (argc > 3) {
		if (argc == 4)
			return;

		PrintErrorExit(argv[0], "compression");
	}
	PrintErrorExit(argv[0], "compression : missing lzo or zlib");
}

void Check_append_only(int argc, char **argv)
{
	if (argc > 4) {
		if (argc == 5)
			return;

		PrintErrorExit(argv[0], "Append Only");
	}
	PrintErrorExit(argv[0], "Append Only : missing Yes / No");
}

void Check_List(int argc, char **argv)
{
	if (argc != 4)
		PrintErrorExit(argv[0], "list map : too many args");
	else
		if (strcmp(argv[3], "map"))
			PrintErrorExit(argv[0], "list map : Can only list map");
}

void Check_Empty(int argc, char **argv)
{
	if (argc > 3) {
		if (!strcmp(argv[3], "map")) {
			if (argc == 4)
				return;
		}
		PrintErrorExit(argv[0], "empty map");
	}
	PrintErrorExit(argv[0], "empty map");
}

void Check_Open(int argc, char **argv)
{
	if (argc > 3) {
		if (!strcmp(argv[3], "map")) {
			if (argc == 4)
				return;
		}
		PrintErrorExit(argv[0], "open map");
	}
	PrintErrorExit(argv[0], "open map");
}

void Check_Close(int argc, char **argv)
{
	if (argc > 3) {
		if (!strcmp(argv[3], "map")) {
			if (argc == 4)
				return;
		}
		PrintErrorExit(argv[0], "close map");
	}
	PrintErrorExit(argv[0], "close map");
}

void Check_Params(int argc, char **argv)
{
	if (argc > 1) {
		if (!isnumeric(argv[1])) {
			fprintf(stderr, "DeviceNo not numeric: %s\n", argv[1]);
			exit(1);
		}
		if (argc > 2) {
			/* global commands */
			if (!strcmp(argv[2], "verbose")) {
				if (argc == 3)
					return;
				PrintErrorExit(argv[0], "verbose");
			}
			if (!strcmp(argv[2], "dump")) {
				if (argc == 3)
					return;
				PrintErrorExit(argv[0], "dump");
			}
			if (!strcmp(argv[2], "debug")) {
				if (argc == 3)
					return;
				PrintErrorExit(argv[0], "debug");
			}
			if (!strcmp(argv[2], "exit")) {
				if (argc == 3)
					return;
				PrintErrorExit(argv[0], "exit");
			}
			if (!strncasecmp(argv[2], "TapeAlert", 9)) {
				Check_TapeAlert(argc, argv);
				return;
			}

			/* Tape commands */
			if (!strncasecmp(argv[2], "load", 4)) {
				Check_Load(argc, argv);
				return;
			}
			if (!strncasecmp(argv[2], "unload", 6)) {
				Check_Unload(argc, argv);
				return;
			}
			if (!strncasecmp(argv[2], "delay", 5)) {
				Check_delay(argc, argv);
				return;
			}
			if (!strncasecmp(argv[2], "compression", 11)) {
				Check_Compression(argc, argv);
				return;
			}
			if (!strncasecmp(argv[2], "Append", 6)) {
				Check_append_only(argc, argv);
				return;
			}

			/* Library commands */
			if (!strcmp(argv[2], "add")) {
				if (argc == 4)
					return;
				PrintErrorExit(argv[0], "add slot");
			}
			if (!strcmp(argv[2], "online")) {
				if (argc == 3)
					return;
				PrintErrorExit(argv[0], "online");
			}
			if (!strcmp(argv[2], "offline")) {
				if (argc == 3)
					return;
				PrintErrorExit(argv[0], "offline");
			}
			if (!strcmp(argv[2], "list")) {
				Check_List(argc, argv);
				return;
			}
			if (!strcmp(argv[2], "empty")) {
				Check_Empty(argc, argv);
				return;
			}
			if (!strcmp(argv[2], "open")) {
				Check_Open(argc, argv);
				return;
			}
			if (!strcmp(argv[2], "close")) {
				Check_Close(argc, argv);
				return;
			}
			PrintErrorExit(argv[0], "check param");
		}
		PrintErrorExit(argv[0], "");
	}
	PrintErrorExit(argv[0], "");
}

/* Open a new queue (for answers from server) */
int CreateNewQueue(void)
{
	long queue_id;

	/* Attempt to create a message queue */
	queue_id = msgget(IPC_PRIVATE,
			IPC_CREAT|S_IRUSR|S_IWUSR|S_IWGRP|S_IWOTH);
	if (queue_id == -1)
		fprintf(stderr, "%s: %s\n", __func__, strerror(errno));

	return queue_id;
}

/* Open an alreay opened queue (opened by server) */
int OpenExistingQueue(key_t key)
{
	long queue_id;

	/* Attempt to open an existing message queue */
	queue_id = msgget(key, 0);
	if (queue_id == -1)
		fprintf(stderr, "%s: %s\n", __func__, strerror(errno));

	return queue_id;
}

/* Send command to queue */
int SendMsg(long ReceiverQid, long ReceiverMtyp, char *sndbuf)
{
	struct q_entry s_entry;

	s_entry.rcv_id = ReceiverMtyp;
	s_entry.msg.snd_id = VTLCMD_Q;
	s_entry.msg.text[0] = '\0';
	strncat(s_entry.msg.text, sndbuf, MAXTEXTLEN);

	int len = strlen(s_entry.msg.text) + 1 + sizeof(s_entry.msg.snd_id);
	if (msgsnd(ReceiverQid, &s_entry, len, 0) == -1)
		return -1;

	return 0;
}

int main(int argc, char **argv)
{
	char *config = MHVTL_CONFIG_PATH"/device.conf";
	FILE *conf;
	char b[1024];
	int device_type = TYPE_UNKNOWN;
	long deviceNo, indx;
	int count;
	char buf[1024];
	char *p;

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
			printf("Version: %s\n", MHVTL_VERSION);
			exit(1);
		}
	}

	Check_Params(argc, argv);

	deviceNo = atol(argv[1]);
	if ((deviceNo < 0) || (deviceNo >= VTLCMD_Q)) {
		fprintf(stderr, "Invalid device number for "
			"tape/library: %s\n", argv[1]);
		exit(1);
	}

	conf = fopen(config, "r");
	if (!conf) {
		fprintf(stderr, "Can not open config file %s : %s\n",
			config, strerror(errno));
		exit(1);
	}

	p = buf;
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
			"device number: %ld\n", config, deviceNo);
		exit(1);
	}

	/* Concat all args into one string */
	p = buf;
	buf[0] = '\0';

	for (count = 2; count < argc; count++) {
		strcat(p, argv[count]);
		p += strlen(argv[count]);
		strcat(p, " ");
		p += strlen(" ");
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
		} else {
			fprintf(stderr, "Command for library not allowed\n");
			exit(1);
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
		} else if (!strncasecmp(buf, "append", 6)) {
		} else if (!strncasecmp(buf, "delay load", 10)) {
		} else if (!strncasecmp(buf, "delay unload", 12)) {
		} else if (!strncasecmp(buf, "delay rewind", 12)) {
		} else if (!strncasecmp(buf, "delay position", 14)) {
		} else if (!strncasecmp(buf, "delay thread", 12)) {
		} else {
			fprintf(stderr, "Command for tape not allowed\n");
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

	exit(0);
}

