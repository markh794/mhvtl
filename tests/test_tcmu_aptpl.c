/*
 * test_tcmu_aptpl — unit tests for the APTPL state replay (usr/tcmu_aptpl.c):
 * LIO's aptpl_<serial> file split into res_aptpl_metadata writes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acutest.h"
#include "tcmu_aptpl.h"

/* Records passed to the callback, joined with '\n' */
static char got[8192];
static int calls;
static int fail_call = -1;	/* refuse this call (0-based) */

static int collect(const char *record, void *arg)
{
	(void)arg;
	strcat(got, record);
	strcat(got, "\n");
	return calls++ == fail_call ? -1 : 0;
}

static int count_only(const char *record, void *arg)
{
	(void)record;
	(void)arg;
	calls++;
	return 0;
}

static int run(const char *buf)
{
	got[0] = '\0';
	calls = 0;
	return aptpl_replay(buf, collect, NULL);
}

/* As LIO writes it: a reservation holder and a plain registration */
static const char two_regs[] =
	"PR_REG_START: 0\n"
	"initiator_fabric=iSCSI\n"
	"initiator_node=iqn.1993-08.org.debian:01:aaaa\n"
	"initiator_sid=0x23d000000\n"
	"sa_res_key=1234\n"
	"res_holder=1\n"
	"res_type=08\n"
	"res_scope=00\n"
	"res_all_tg_pt=0\n"
	"mapped_lun=1\n"
	"target_fabric=iSCSI\n"
	"target_node=iqn.2003-01.org.linux-iscsi.host:mhvtl:lib10\n"
	"tpgt=1\n"
	"port_rtpi=1\n"
	"target_lun=1\n"
	"PR_REG_END: 0\n"
	"PR_REG_START: 1\n"
	"initiator_fabric=iSCSI\n"
	"initiator_node=iqn.1993-08.org.debian:01:bbbb\n"
	"sa_res_key=5678\n"
	"res_holder=0\n"
	"res_all_tg_pt=0\n"
	"mapped_lun=1\n"
	"target_fabric=iSCSI\n"
	"target_node=iqn.2003-01.org.linux-iscsi.host:mhvtl:lib10\n"
	"tpgt=1\n"
	"port_rtpi=1\n"
	"target_lun=1\n"
	"PR_REG_END: 1\n";

void test_two_registrations(void)
{
	TEST_CHECK(run(two_regs) == 2);
	TEST_CHECK(calls == 2);
	TEST_CHECK(strcmp(got,
		"initiator_fabric=iSCSI,initiator_node=iqn.1993-08.org.debian:01:aaaa,"
		"initiator_sid=0x23d000000,sa_res_key=1234,res_holder=1,res_type=8,"
		"res_scope=0,res_all_tg_pt=0,mapped_lun=1,target_fabric=iSCSI,"
		"target_node=iqn.2003-01.org.linux-iscsi.host:mhvtl:lib10,tpgt=1,"
		"port_rtpi=1,target_lun=1\n"
		"initiator_fabric=iSCSI,initiator_node=iqn.1993-08.org.debian:01:bbbb,"
		"sa_res_key=5678,res_holder=0,res_all_tg_pt=0,mapped_lun=1,"
		"target_fabric=iSCSI,"
		"target_node=iqn.2003-01.org.linux-iscsi.host:mhvtl:lib10,tpgt=1,"
		"port_rtpi=1,target_lun=1\n") == 0);
	TEST_MSG("got:\n%s", got);
}

/* LIO matches "%d" with base 0, so "08" is refused; send every type in decimal */
void test_res_type_decimal(void)
{
	static const struct { const char *hex, *dec; } t[] = {
		{ "01", "res_type=1," }, { "03", "res_type=3," },
		{ "05", "res_type=5," }, { "06", "res_type=6," },
		{ "07", "res_type=7," }, { "08", "res_type=8," },
	};
	char buf[512];

	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		snprintf(buf, sizeof(buf),
			 "PR_REG_START: 0\ninitiator_node=i\nsa_res_key=1\n"
			 "res_holder=1\nres_type=%s\nres_scope=00\n"
			 "target_node=t\nPR_REG_END: 0\n", t[i].hex);
		TEST_CHECK(run(buf) == 1);
		TEST_CHECK_(strstr(got, t[i].dec) != NULL, "%s -> %s", t[i].hex, got);
	}
}

/* What LIO writes when APTPL is cleared, with the trailing newline */
void test_no_registrations(void)
{
	TEST_CHECK(run("No Registrations or Reservations\n") == 0);
	TEST_CHECK(run("No Registrations or Reservations") == 0);
	TEST_CHECK(run("") == 0);
	TEST_CHECK(calls == 0);
}

/* A refused registration does not stop the others */
void test_refused_registration(void)
{
	fail_call = 0;
	TEST_CHECK(run(two_regs) == 1);
	TEST_CHECK(calls == 2);
	fail_call = -1;
}

void test_malformed(void)
{
	/* No PR_REG_END */
	TEST_CHECK(run("PR_REG_START: 0\ninitiator_node=i\n") == -1);
	/* Field outside a record */
	TEST_CHECK(run("initiator_node=i\n") == -1);
	/* Nested start, stray end */
	TEST_CHECK(run("PR_REG_START: 0\nPR_REG_START: 1\n") == -1);
	TEST_CHECK(run("PR_REG_END: 0\n") == -1);
	/* Line without '=' */
	TEST_CHECK(run("PR_REG_START: 0\ngarbage\nPR_REG_END: 0\n") == -1);
	/* res_type not hex */
	TEST_CHECK(run("PR_REG_START: 0\nres_type=zz\nPR_REG_END: 0\n") == -1);
	TEST_CHECK(run("PR_REG_START: 0\nres_type=-1\nPR_REG_END: 0\n") == -1);
	TEST_CHECK(run("PR_REG_START: 0\nres_type=100\nPR_REG_END: 0\n") == -1);
	TEST_CHECK(calls == 0);
}

/* A bad record anywhere means nothing is applied, not the records before it */
void test_malformed_late(void)
{
	char buf[sizeof(two_regs) + 64];

	snprintf(buf, sizeof(buf), "%sPR_REG_START: 2\ngarbage\nPR_REG_END: 2\n",
		 two_regs);
	TEST_CHECK(run(buf) == -1);
	TEST_CHECK(calls == 0);
}

/* LIO grows its buffer past 256 KiB when there are many registrations */
void test_many_registrations(void)
{
	const size_t nrec = 1500;
	size_t size = nrec * 300, len = 0;
	char *buf = malloc(size);

	TEST_ASSERT(buf != NULL);
	for (size_t i = 0; i < nrec; i++)
		len += snprintf(buf + len, size - len,
				"PR_REG_START: %zu\ninitiator_fabric=iSCSI\n"
				"initiator_node=iqn.2026-10.test:i%zu\nsa_res_key=%zu\n"
				"res_holder=0\nres_all_tg_pt=0\nmapped_lun=1\n"
				"target_fabric=iSCSI\ntarget_node=iqn.2026-10.test:t\n"
				"tpgt=1\nport_rtpi=1\ntarget_lun=1\nPR_REG_END: %zu\n",
				i, i, i + 1, i);
	TEST_ASSERT(len > 262144 && len < size);
	calls = 0;
	TEST_CHECK(aptpl_replay(buf, count_only, NULL) == (int)nrec);
	TEST_CHECK(calls == (int)nrec);
	free(buf);
}

/* Longer than any record LIO writes */
void test_record_too_long(void)
{
	static char buf[4096];
	char *p = buf;

	p += sprintf(p, "PR_REG_START: 0\ninitiator_node=");
	memset(p, 'a', 3000);
	p += 3000;
	strcpy(p, "\nPR_REG_END: 0\n");
	TEST_CHECK(run(buf) == -1);
	TEST_CHECK(calls == 0);
}

TEST_LIST = {
	{ "two_registrations",   test_two_registrations },
	{ "res_type_decimal",    test_res_type_decimal },
	{ "no_registrations",    test_no_registrations },
	{ "refused_registration", test_refused_registration },
	{ "malformed",           test_malformed },
	{ "malformed_late",      test_malformed_late },
	{ "many_registrations",  test_many_registrations },
	{ "record_too_long",     test_record_too_long },
	{ NULL, NULL }
};
