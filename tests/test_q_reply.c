/*
 * test_q_reply.c — recv_reply_from() and recv_msg(): waiting for one drive's
 * reply on the shared mhvtl message queue without losing other messages.
 *
 * A library waiting for a drive's "Loaded OK" used to take whatever reached
 * its id first, so a vtlcmd command arriving mid-move was read as the drive's
 * answer and lost. These tests use a private queue; no daemons are started.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>
#include "acutest.h"
#include "q.h"

/* Globals q.c expects from the daemon it is linked into */
long my_id = 10;
int debug = 1;
char mhvtl_driver_name[] = "test_q_reply";

static const char *const load_replies[] = { msg_load_ok, msg_load_failed, NULL };

static int new_queue(void)
{
	int qid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);

	TEST_ASSERT(qid >= 0);
	return qid;
}

/* Put a message from 'from' to the library on qid */
static void put(int qid, long from, const char *text)
{
	struct q_entry e;
	int len;

	memset(&e, 0, sizeof(e));
	e.rcv_id = my_id;
	e.msg.snd_id = from;
	strncpy(e.msg.text, text, MAXTEXTLEN);
	len = offsetof(struct q_msg, text) + strlen(e.msg.text) + 1;
	TEST_ASSERT(msgsnd(qid, &e, len, 0) == 0);
}

/* The next message for the library, as its main loop reads it */
static int next(int qid, struct q_entry *e)
{
	memset(e, 0, sizeof(*e));
	return recv_msg(qid, e, IPC_NOWAIT);
}

static void expect(int qid, long from, const char *text)
{
	struct q_entry e;

	TEST_CHECK(next(qid, &e) > 0);
	TEST_CHECK(e.msg.snd_id == from);
	TEST_CHECK(strcmp(e.msg.text, text) == 0);
	TEST_MSG("got \"%s\" from %ld, want \"%s\" from %ld",
		 e.msg.text, e.msg.snd_id, text, from);
}

static void expect_empty(int qid)
{
	struct q_entry e;

	TEST_CHECK(next(qid, &e) < 0 && errno == ENOMSG);
}

static void test_reply_first(void)
{
	int qid = new_queue();
	struct q_entry r;

	put(qid, 11, "Loaded OK: E01001L8");
	TEST_CHECK(recv_reply_from(qid, 11, load_replies, &r) > 0);
	TEST_CHECK(r.msg.snd_id == 11);
	TEST_CHECK(strcmp(r.msg.text, "Loaded OK: E01001L8") == 0);
	expect_empty(qid);

	msgctl(qid, IPC_RMID, NULL);
}

/* Other senders' messages, and the awaited drive's own unsolicited eject
 * notice, are not taken as the reply. They are handed to the main loop
 * afterwards in arrival order, ahead of anything that came later.
 */
static void test_others_held_in_order(void)
{
	int qid = new_queue();
	struct q_entry r;

	put(qid, 32768, "open map");
	put(qid, 11, msg_eject);
	put(qid, 12, msg_eject);
	put(qid, 11, "Load failed: E01001L8");

	TEST_CHECK(recv_reply_from(qid, 11, load_replies, &r) > 0);
	TEST_CHECK(strcmp(r.msg.text, "Load failed: E01001L8") == 0);

	put(qid, 32768, "list map");
	expect(qid, 32768, "open map");
	expect(qid, 11, msg_eject);
	expect(qid, 12, msg_eject);
	expect(qid, 32768, "list map");
	expect_empty(qid);

	msgctl(qid, IPC_RMID, NULL);
}

static void *send_late(void *arg)
{
	int qid = *(int *)arg;

	put(qid, 32768, "list map");
	usleep(100 * 1000);
	put(qid, 11, "Loaded OK: E01001L8");
	return NULL;
}

/* A message arriving while the wait is in progress does not end it */
static void test_wait_spans_other_messages(void)
{
	int qid = new_queue();
	struct q_entry r;
	pthread_t t;

	TEST_ASSERT(pthread_create(&t, NULL, send_late, &qid) == 0);
	TEST_CHECK(recv_reply_from(qid, 11, load_replies, &r) > 0);
	pthread_join(t, NULL);
	TEST_CHECK(strcmp(r.msg.text, "Loaded OK: E01001L8") == 0);
	expect(qid, 32768, "list map");
	expect_empty(qid);

	msgctl(qid, IPC_RMID, NULL);
}

static void on_signal(int sig)
{
	(void)sig;
}

struct interrupt {
	int qid;
	pthread_t waiter;
};

static void *interrupt_then_reply(void *arg)
{
	struct interrupt *in = arg;

	usleep(100 * 1000);
	pthread_kill(in->waiter, SIGUSR1);
	usleep(100 * 1000);
	put(in->qid, 11, "Loaded OK: E01001L8");
	return NULL;
}

/* A signal interrupting the wait does not end it: msgrcv() fails with EINTR
 * and the wait goes on until the drive replies.
 */
static void test_signal_does_not_end_wait(void)
{
	struct interrupt in = { .qid = new_queue(), .waiter = pthread_self() };
	struct sigaction sa;
	struct q_entry r;
	pthread_t t;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal; /* no SA_RESTART: msgrcv() returns EINTR */
	TEST_ASSERT(sigaction(SIGUSR1, &sa, NULL) == 0);

	TEST_ASSERT(pthread_create(&t, NULL, interrupt_then_reply, &in) == 0);
	TEST_CHECK(recv_reply_from(in.qid, 11, load_replies, &r) > 0);
	pthread_join(t, NULL);
	TEST_CHECK(strcmp(r.msg.text, "Loaded OK: E01001L8") == 0);

	signal(SIGUSR1, SIG_DFL);
	msgctl(in.qid, IPC_RMID, NULL);
}

/* A message too short to carry a sender id and text is not the reply,
 * whatever was in memory before it was received.
 */
static void test_short_message_not_a_reply(void)
{
	int qid = new_queue();
	struct q_entry e, r;

	memset(&e, 0, sizeof(e));
	e.rcv_id = my_id;
	TEST_ASSERT(msgsnd(qid, &e, 4, 0) == 0); /* half a sender id, no text */
	put(qid, 11, "Loaded OK: E01001L8");

	TEST_CHECK(recv_reply_from(qid, 11, load_replies, &r) > 0);
	TEST_CHECK(strcmp(r.msg.text, "Loaded OK: E01001L8") == 0);
	TEST_CHECK(next(qid, &r) > 0); /* the short one, held for the main loop */
	TEST_CHECK(r.msg.snd_id == 0 && r.msg.text[0] == '\0');
	expect_empty(qid);

	msgctl(qid, IPC_RMID, NULL);
}

/* A failed receive is reported as before; held messages are not lost */
static void test_receive_failure(void)
{
	int qid = new_queue();
	struct q_entry r;

	put(qid, 32768, "open map");
	put(qid, 12, msg_eject);
	msgctl(qid, IPC_RMID, NULL);
	TEST_CHECK(recv_reply_from(qid, 11, load_replies, &r) == -1);
	TEST_CHECK(errno == EINVAL || errno == EIDRM);

	/* The queue is gone, so only what was never taken off it is lost */
	TEST_CHECK(next(qid, &r) < 0);
}

/* A message of the largest size the queue carries survives being held */
static void test_max_length_message(void)
{
	int qid = new_queue();
	struct q_entry r;
	char big[MAXTEXTLEN + 1];

	memset(big, 'x', MAXTEXTLEN);
	big[MAXTEXTLEN] = '\0';
	put(qid, 32768, big);
	put(qid, 11, "Loaded OK: E01001L8");
	TEST_CHECK(recv_reply_from(qid, 11, load_replies, &r) > 0);
	expect(qid, 32768, big);
	expect_empty(qid);

	msgctl(qid, IPC_RMID, NULL);
}

/* Put a message from the library to drive 'drv' on qid, as send_msg() does */
static void to_drive(int qid, long drv, const char *text)
{
	struct q_entry e;
	int len;

	memset(&e, 0, sizeof(e));
	e.rcv_id = drv;
	e.msg.snd_id = my_id;
	strncpy(e.msg.text, text, MAXTEXTLEN);
	len = offsetof(struct q_msg, text) + strlen(e.msg.text) + 1;
	TEST_ASSERT(msgsnd(qid, &e, len, 0) == 0);
}

/* The next message waiting for drive 'drv', as the drive reads it */
static int drive_next(int qid, long drv, struct q_entry *e)
{
	memset(e, 0, sizeof(*e));
	return msgrcv(qid, e, MAXOBN, drv, IPC_NOWAIT | MSG_NOERROR);
}

static void expect_for_drive(int qid, long drv, const char *text)
{
	struct q_entry e;

	TEST_CHECK(drive_next(qid, drv, &e) > 0);
	TEST_CHECK(strcmp(e.msg.text, text) == 0);
	TEST_MSG("drive %ld got \"%s\", want \"%s\"", drv, e.msg.text, text);
}

/* No daemon reads drive 11's messages: after the grace period the request
 * is taken back and the wait fails with ETIMEDOUT. What else was waiting for
 * the drive stays, in order, and the library's own messages are kept.
 */
static void test_no_reader_withdrawn(void)
{
	int qid = new_queue();
	struct q_entry r;

	to_drive(qid, 11, "Register");
	to_drive(qid, 11, "lload E01001L8");
	to_drive(qid, 11, "debug");
	to_drive(qid, 11, "lload E01001L8"); /* left over from an earlier try */
	put(qid, 32768, "open map");

	TEST_CHECK(recv_reply_within(qid, 11, "lload E01001L8", 1,
				     load_replies, &r) == -1);
	TEST_CHECK(errno == ETIMEDOUT);

	expect_for_drive(qid, 11, "Register");
	expect_for_drive(qid, 11, "debug");
	TEST_CHECK(drive_next(qid, 11, &r) < 0 && errno == ENOMSG);
	expect(qid, 32768, "open map");
	expect_empty(qid);

	msgctl(qid, IPC_RMID, NULL);
}

struct slow_drive {
	int qid;
	useconds_t take_after, reply_after;
};

/* Drive 11: take the request off the queue, then reply some time later */
static void *slow_drive(void *arg)
{
	struct slow_drive *d = arg;
	struct q_entry e;

	usleep(d->take_after);
	TEST_CHECK(drive_next(d->qid, 11, &e) > 0);
	usleep(d->reply_after);
	put(d->qid, 11, "Loaded OK: E01001L8");
	return NULL;
}

/* A drive that has taken the request may take longer than the grace period
 * to reply: once it has the request, the wait goes on until the reply.
 */
static void test_slow_reply_after_take(void)
{
	struct slow_drive d = { new_queue(), 200 * 1000, 3500 * 1000 };
	struct q_entry r;
	pthread_t t;

	to_drive(d.qid, 11, "lload E01001L8");
	TEST_ASSERT(pthread_create(&t, NULL, slow_drive, &d) == 0);
	TEST_CHECK(recv_reply_within(d.qid, 11, "lload E01001L8", 3,
				     load_replies, &r) > 0);
	pthread_join(t, NULL);
	TEST_CHECK(strcmp(r.msg.text, "Loaded OK: E01001L8") == 0);
	expect_empty(d.qid);

	msgctl(d.qid, IPC_RMID, NULL);
}

/* A reply inside the grace period is returned at once */
static void test_reply_within_grace(void)
{
	struct slow_drive d = { new_queue(), 50 * 1000, 50 * 1000 };
	struct q_entry r;
	pthread_t t;

	to_drive(d.qid, 11, "lload E01001L8");
	TEST_ASSERT(pthread_create(&t, NULL, slow_drive, &d) == 0);
	TEST_CHECK(recv_reply_within(d.qid, 11, "lload E01001L8", 5,
				     load_replies, &r) > 0);
	pthread_join(t, NULL);
	TEST_CHECK(strcmp(r.msg.text, "Loaded OK: E01001L8") == 0);

	msgctl(d.qid, IPC_RMID, NULL);
}

/* withdraw_msg(): 0 once the drive has read the request, and only the
 * library's own copy of the request is taken back
 */
static void test_withdraw_msg(void)
{
	int qid = new_queue();
	struct q_entry e;

	to_drive(qid, 11, "unload E01001L8");
	TEST_CHECK(drive_next(qid, 11, &e) > 0);
	TEST_CHECK(withdraw_msg(qid, 11, "unload E01001L8") == 0);

	/* the same text from another sender is not ours */
	memset(&e, 0, sizeof(e));
	e.rcv_id = 11;
	e.msg.snd_id = 32768;
	strcpy(e.msg.text, "unload E01001L8");
	TEST_ASSERT(msgsnd(qid, &e, offsetof(struct q_msg, text) + strlen(e.msg.text) + 1, 0) == 0);
	to_drive(qid, 12, "unload E01001L8");
	TEST_CHECK(withdraw_msg(qid, 11, "unload E01001L8") == 0);
	TEST_CHECK(drive_next(qid, 11, &e) > 0 && e.msg.snd_id == 32768);
	expect_for_drive(qid, 12, "unload E01001L8");

	msgctl(qid, IPC_RMID, NULL);
}

TEST_LIST = {
	{ "no_reader_withdrawn",       test_no_reader_withdrawn },
	{ "slow_reply_after_take",     test_slow_reply_after_take },
	{ "reply_within_grace",        test_reply_within_grace },
	{ "withdraw_msg",              test_withdraw_msg },
	{ "reply_first",               test_reply_first },
	{ "others_held_in_order",      test_others_held_in_order },
	{ "wait_spans_other_messages", test_wait_spans_other_messages },
	{ "signal_does_not_end_wait",  test_signal_does_not_end_wait },
	{ "short_message_not_a_reply", test_short_message_not_a_reply },
	{ "receive_failure",           test_receive_failure },
	{ "max_length_message",        test_max_length_message },
	{ NULL, NULL }
};
