/*
 * From Unix System Programming -
 * Starting from Pg 195
 *
 * Advanced inter-process communications
 */

#include <stdio.h>
#include "q.h"

void
warn(char *s) {
	fprintf(stderr, "Warning: %s\n", s);
}

int
init_queue(void) {
int	queue_id;

/* Attempt to create or open message queue */
if ( (queue_id = msgget(QKEY, IPC_CREAT | QPERM)) == -1)
	perror("msgget failed");

return (queue_id);
}

void
proc_obj(struct q_entry *msg) {
	printf("Priority: %ld name: %s\n", msg->mtype, msg->mtext);
}

int
enter(char *objname, int priority) {
	int len, s_qid;
	struct q_entry s_entry;	/* Structure to hold message */

	/* Validate name length, priority level */
	if ((len = strlen(objname)) > MAXOBN) {
		warn("Name too long");
		return (-1);
	}

	if (priority > 32764 || priority < 0) {
		warn("Invalid priority level");
		return(-1);
	}

	/* Initialize message queue as nessary */
	if ( (s_qid = init_queue()) == -1)
		return (-1);

	/* Initialize s_entry */
	s_entry.mtype = (long)priority;
	strncpy(s_entry.mtext, objname, MAXOBN);

	/* Send message, waiting if nessary */
	if (msgsnd(s_qid, &s_entry, len, 0) == -1) {
		perror("msgsnd failed");
		return (-1);
	} else {
		return (0);
	}
}

int
serve(void) {
int	mlen, r_qid;
struct q_entry r_entry;

/* Initialise message queue as necessary */
if ((r_qid = init_queue()) == -1)
	return (-1);

/* Get and process next message, waiting if necessary */
for (;;) {
	if ((mlen = msgrcv(r_qid, &r_entry, MAXOBN,
				(-1 * MAXPRIOR), MSG_NOERROR)) == -1) {
		perror("msgrcv failed");
		return (-1);
	} else {
		/* Make sure we've a string */
		r_entry.mtext[mlen]='\0';

		/* Process object name */
		proc_obj(&r_entry);
	}
  }
}

