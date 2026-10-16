/*
 * From Unix System Programming -
 * Starting from Pg 195
 *
 * Advanced inter-process communications
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <sys/msg.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "q.h"

extern int	debug;
extern char mhvtl_driver_name[];

#define MHVTL_ERR(format, arg...)                             \
	{                                                         \
		if (debug)                                            \
			printf("%s: ERROR %s: " format "\n",              \
				   mhvtl_driver_name, __func__, ##arg);       \
		else                                                  \
			syslog(LOG_DAEMON | LOG_ERR, "ERROR %s: " format, \
				   __func__, ##arg);                          \
	}

static void warn(char *s) {
	fprintf(stderr, "Warning: %s\n", s);
}

int init_queue(void) {
	int queue_id;

	/* Attempt to create or open message queue */
	queue_id = msgget(QKEY, IPC_CREAT | QPERM);
	if (queue_id == -1) {
		char s[245];
		switch (errno) {
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
		MHVTL_ERR("msgget(%d) failed %s, %s",
				  QKEY, strerror(errno), s);
	}

	return queue_id;
}

int send_msg(char *cmd, long rcv_id) {
	int			   len, s_qid;
	struct q_entry s_entry;

	/* Match enter()'s length guard: refuse messages that would not
	 * fit into s_entry.msg.text[MAXTEXTLEN + 1] and prevent an
	 * unbounded strcpy into the fixed-size queue message slot.
	 */
	if (strlen(cmd) > MAXTEXTLEN) {
		MHVTL_ERR("send_msg: command too long (%zu > %d)",
				  strlen(cmd), MAXTEXTLEN);
		return -1;
	}

	memset(&s_entry, 0, sizeof(struct q_entry));

	s_qid = init_queue();
	if (s_qid == -1)
		return -1;

	s_entry.rcv_id	   = rcv_id;
	s_entry.msg.snd_id = my_id;
	strncpy(s_entry.msg.text, cmd, MAXTEXTLEN);
	s_entry.msg.text[MAXTEXTLEN] = '\0';
	len = strlen(s_entry.msg.text) + 1 + offsetof(struct q_msg, text);

	if (msgsnd(s_qid, &s_entry, len, 0) == -1) {
		MHVTL_ERR("msgsnd failed: %s", strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * Messages for this daemon that were taken off the queue while it waited for
 * a reply, oldest first. recv_msg() hands them out before reading the queue
 * again, so they keep their place ahead of anything that arrived later.
 */
struct pending_msg {
	struct pending_msg *next;
	struct q_entry		entry;
};
static struct pending_msg *pending_head, *pending_tail;

static void hold(struct pending_msg *p) {
	p->next = NULL;
	if (pending_tail)
		pending_tail->next = p;
	else
		pending_head = p;
	pending_tail = p;
}

static int is_reply(const char *text, const char *const *replies) {
	for (; *replies; replies++)
		if (!strncmp(text, *replies, strlen(*replies)))
			return 1;
	return 0;
}

/*
 * Wait on queue qid for the reply that 'from' sends to this daemon: a message
 * from 'from' whose text starts with one of the NULL-terminated 'replies'.
 *
 * Everything addressed to a daemon shares one message type, its id, so a
 * plain msgrcv(my_id) takes whatever arrives first: a vtlcmd command, a
 * management daemon's request, another drive's notice, or an unsolicited
 * notice from the very drive being waited on. Anything that is not the reply
 * is held for recv_msg(), which the daemon's main loop reads its messages
 * with, so it is processed afterwards in the order it arrived. Room to hold a
 * message is made before it is taken off the queue, so none is dropped.
 *
 * Like the msgrcv() it replaces, this waits until the reply comes. Returns
 * the size of the reply, or -1 with errno set by msgrcv().
 */
int recv_reply_from(int qid, long from, const char *const *replies,
					struct q_entry *reply) {
	struct pending_msg *p = NULL;
	int					mlen;

	for (;;) {
		if (!p) {
			p = malloc(sizeof(*p));
			if (!p) {
				MHVTL_ERR("Out of memory holding messages while waiting for %ld",
						  from);
				sleep(1);
				continue;
			}
		}
		/* msgrcv() writes only what was sent: start from a clean entry */
		memset(&p->entry, 0, sizeof(p->entry));
		mlen = msgrcv(qid, &p->entry, MAXOBN, my_id, MSG_NOERROR);
		if (mlen < 0) {
			if (errno == EINTR)
				continue;
			free(p);
			return -1;
		}
		p->entry.msg.text[MAXTEXTLEN] = '\0';
		/* Too short to carry a sender and text, it cannot be the reply */
		if (mlen > (int)offsetof(struct q_msg, text) &&
			p->entry.msg.snd_id == from && is_reply(p->entry.msg.text, replies)) {
			*reply = p->entry;
			free(p);
			return mlen;
		}
		hold(p);
		p = NULL;
	}
}

/*
 * Receive the next message for this daemon: one held while it waited for a
 * reply, else msgrcv() with the given flags. Returns as msgrcv() does.
 */
int recv_msg(int qid, struct q_entry *e, int flags) {
	struct pending_msg *p = pending_head;

	if (p) {
		pending_head = p->next;
		if (!pending_head)
			pending_tail = NULL;
		*e = p->entry;
		free(p);
		return offsetof(struct q_msg, text) + strlen(e->msg.text) + 1;
	}
	return msgrcv(qid, e, MAXOBN, my_id, flags | MSG_NOERROR);
}

static void proc_obj(struct q_entry *q_entry) {
	printf("rcv_id: %ld, snd_id: %ld, text: %s\n",
		   q_entry->rcv_id, q_entry->msg.snd_id, q_entry->msg.text);
}

int enter(char *objname, long rcv_id) {
	int			   len, s_qid;
	struct q_entry s_entry; /* Structure to hold message */

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
	s_entry.rcv_id	   = rcv_id;
	s_entry.msg.snd_id = my_id;
	strcpy(s_entry.msg.text, objname);
	len = strlen(s_entry.msg.text) + 1 + offsetof(struct q_msg, text);

	/* Send message, waiting if nessary */
	if (msgsnd(s_qid, &s_entry, len, 0) == -1) {
		MHVTL_ERR("msgsnd failed: %s", strerror(errno));
		return -1;
	}

	return 0;
}

int serve(void) {
	int			   mlen, r_qid;
	struct q_entry r_entry;

	/* Initialise message queue as necessary */
	r_qid = init_queue();
	if (r_qid == -1)
		return -1;

	/* Get and process next message, waiting if necessary */
	for (;;) {
		mlen = msgrcv(r_qid, &r_entry, MAXOBN,
					  (-1 * MAXPRIOR), MSG_NOERROR);
		if (mlen == -1) {
			MHVTL_ERR("msgsnd failed: %s", strerror(errno));
			return -1;
		} else {
			/* Process object name */
			proc_obj(&r_entry);
		}
	}
}
