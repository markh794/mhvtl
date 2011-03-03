/*
 * From Unix System Programming -
 * Starting from Pg 195
 *
 * Advanced inter-process communications
 */

#include <stddef.h>
#include <stdio.h>
#include <syslog.h>
#include "q.h"
extern int debug;
extern char *vtl_driver_name;

#define MHVTL_LOG(format, arg...) {			\
	if (debug)						\
		printf("%s: %s: " format "\n",			\
			vtl_driver_name, __func__, ## arg); 	\
	else							\
		syslog(LOG_DAEMON|LOG_ERR, "%s: " format,	\
			__func__, ## arg); 			\
}

static void warn(char *s)
{
	fprintf(stderr, "Warning: %s\n", s);
}

int init_queue(void)
{
	int queue_id;

	/* Attempt to create or open message queue */
	queue_id = msgget(QKEY, IPC_CREAT | QPERM);
	if (queue_id == -1) {
		char s[245];
		switch(errno) {
		case EACCES:
			strcpy(s, "Operation not permitted");
			break;
		case EEXIST:
			strcpy(s, "Message Q already exists");
			break;
		case ENOENT:
			strcpy(s, "Message Q does not exist");
			break;
		case ENOSPC:
			strcpy(s, "Exceeded max num of message queues");
			break;
		default:
			strcpy(s, "errno not valid");
			break;
		}
		MHVTL_LOG("msgget(%d) failed %s, %s",
				QKEY, strerror(errno), s);
	}

	return queue_id;
}

int send_msg(char *cmd, long rcv_id)
{
	int len, s_qid;
	struct q_entry s_entry;

	s_qid = init_queue();
	if (s_qid == -1)
		return -1;

	s_entry.rcv_id = rcv_id;
	s_entry.msg.snd_id = my_id;
	strcpy(s_entry.msg.text, cmd);
	len = strlen(s_entry.msg.text) + 1 + offsetof(struct q_entry, msg.text);

	if (msgsnd(s_qid, &s_entry, len, 0) == -1) {
		MHVTL_LOG("msgsnd failed: %s", strerror(errno));
		return -1;
	}

	return 0;
}

static void proc_obj(struct q_entry *q_entry)
{
	printf("rcv_id: %ld, snd_id: %ld, text: %s\n",
		q_entry->rcv_id, q_entry->msg.snd_id, q_entry->msg.text);
}

int enter(char *objname, long rcv_id)
{
	int len, s_qid;
	struct q_entry s_entry;	/* Structure to hold message */

	/* Validate name length, rcv_id */
	if (strlen(objname) > MAXTEXTLEN) {
		warn("Name too long");
		return -1;
	}

	if (rcv_id > 32764 || rcv_id < 0) {
		warn("Invalid rcv_id");
		return -1;
	}

	/* Initialize message queue as nessary */
	s_qid = init_queue();
	if (s_qid == -1)
		return -1;

	/* Initialize s_entry */
	s_entry.rcv_id = rcv_id;
	s_entry.msg.snd_id = my_id;
	strcpy(s_entry.msg.text, objname);
	len = strlen(s_entry.msg.text) + 1 + offsetof(struct q_msg, text);

	/* Send message, waiting if nessary */
	if (msgsnd(s_qid, &s_entry, len, 0) == -1) {
		perror("msgsnd failed");
		return -1;
	}

	return 0;
}

int serve(void)
{
	int mlen, r_qid;
	struct q_entry r_entry;

	/* Initialise message queue as necessary */
	r_qid = init_queue();
	if (r_qid == -1)
		return -1;

	/* Get and process next message, waiting if necessary */
	for (;;) {
		if ((mlen = msgrcv(r_qid, &r_entry, MAXOBN,
					(-1 * MAXPRIOR), MSG_NOERROR)) == -1) {
			perror("msgrcv failed");
			return -1;
		} else {
			/* Process object name */
			proc_obj(&r_entry);
		}
	}
}

