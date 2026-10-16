/*
 * test_q_send.c — send_msg() puts exactly the message on the queue: the
 * sender id and the text with its NUL, and nothing past the end of the
 * message buffer.
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>
#include "acutest.h"
#include "q.h"

/* Globals q.c expects from the daemon it is linked into */
long my_id = 11;
int debug = 1;
char mhvtl_driver_name[] = "test_q_send";

static int qid = -1;

static void use_private_key(void)
{
	char key[32];

	snprintf(key, sizeof(key), "0x%x", MHVTL_QKEY_BASE + (unsigned)getpid());
	setenv("MHVTL_QKEY", key, 1);
	qid = init_queue();
	TEST_ASSERT(qid >= 0);
}

static void check_size(const char *text)
{
	struct q_entry r;
	int mlen;

	TEST_ASSERT(send_msg((char *)text, 10) == 0);
	memset(&r, 0, sizeof(r));
	mlen = msgrcv(qid, &r, MAXOBN, 10, IPC_NOWAIT);
	TEST_CHECK(mlen == (int)(offsetof(struct q_msg, text) + strlen(text) + 1));
	TEST_MSG("received %d bytes for a %zu character message", mlen, strlen(text));
	TEST_CHECK(r.msg.snd_id == 11);
	TEST_CHECK(strcmp(r.msg.text, text) == 0);
}

static void test_message_size(void)
{
	char big[MAXTEXTLEN + 1];

	use_private_key();
	check_size("Loaded OK: E01001L8");

	/* The largest message must fit the buffer it is sent from */
	memset(big, 'x', MAXTEXTLEN);
	big[MAXTEXTLEN] = '\0';
	check_size(big);

	msgctl(qid, IPC_RMID, NULL);
}

TEST_LIST = {
	{ "message_size", test_message_size },
	{ NULL, NULL }
};
