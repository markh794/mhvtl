/*
 * test_fabrics — unit tests for mhvtl-fabrics (usr/fabrics_conf.c and
 * usr/fabrics_cfs.c): fabrics.conf parsing, the LUN plan from device.conf,
 * and apply / remove / status against a plain directory standing in for
 * /sys/kernel/config/target.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "acutest.h"
#include "fabrics.h"

static const char DEVICE_CONF[] =
	"VERSION: 5\n"
	"\n"
	"Library: 10 CHANNEL: 00 TARGET: 00 LUN: 00\n"
	" Vendor identification: STK\n"
	" NAA: 10:22:33:44:ab:00:00:00\n"
	"\n"
	"Drive: 11 CHANNEL: 00 TARGET: 01 LUN: 00\n"
	" Library ID: 10 Slot: 01\n"
	"\n"
	"Drive: 12 CHANNEL: 00 TARGET: 02 LUN: 00\n"
	" Library ID: 10 Slot: 02\n"
	"\n"
	"Library: 30 CHANNEL: 00 TARGET: 08 LUN: 00\n"
	"\n"
	"Drive: 31 CHANNEL: 00 TARGET: 09 LUN: 00\n"
	" Library ID: 30 Slot: 04\n"
	"\n"
	"Drive: 50 CHANNEL: 00 TARGET: 10 LUN: 00\n"
	" Vendor identification: IBM\n";

static FILE *mem(const char *s)
{
	return fmemopen((void *)s, strlen(s), "r");
}

static int parse(const char *s, struct fab_conf *c, char *err, size_t len)
{
	FILE	*fp = mem(s);
	int	 r = fab_conf_parse(fp, c, err, len);

	fclose(fp);
	return r;
}

/* Parse fabrics.conf text and plan it against DEVICE_CONF */
static int plan(const char *s, struct fab_conf *c, char *err, size_t len)
{
	static struct fab_topology topo;
	FILE *fp;

	if (parse(s, c, err, len))
		return -1;
	fp = mem(DEVICE_CONF);
	TEST_ASSERT(fab_topology_parse(fp, &topo, err, len) == 0);
	fclose(fp);
	return fab_plan(c, &topo, err, len);
}

/* The prefix the shipped fabrics.conf.sample uses: LIO's targetcli default */
#define BASE "iqn.2003-01.org.linux-iscsi.host-a:mhvtl"
#define W10 BASE ".lib10"
#define W30 BASE ".lib30"

static struct fab_conf conf;
static char err[512];

void test_parse_full(void)
{
	const char *s =
		"VERSION: 1\n"
		"# comment\n"
		"iSCSI:\n"
		" Portal: 192.0.2.10:3261\n"
		" Portal: [2001:db8::1]\n"
		" Base name: iqn.2024-01.com.example:vtl\n"
		"\n"
		"iSCSI target: 10\n"
		" Initiator name: iqn.2024-01.com.example:pbs1   # PBS\n"
		" Initiator name: iqn.2024-01.com.example:pbs2\n"
		" CHAP name: pbs\n"
		" CHAP secret: abcdefghijkl#x\n"
		" Mutual CHAP name: vtl\n"
		" Mutual CHAP secret: mnopqrstuvwx\n";

	TEST_ASSERT_(parse(s, &conf, err, sizeof(err)) == 0, "%s", err);
	TEST_CHECK(conf.iscsi);
	TEST_CHECK(conf.n_portals == 2);
	TEST_CHECK(!strcmp(conf.portals[0].addr, "192.0.2.10") &&
		   conf.portals[0].port == 3261 && !conf.portals[0].ipv6);
	TEST_CHECK(!strcmp(conf.portals[1].addr, "2001:db8::1") &&
		   conf.portals[1].port == 3260 && conf.portals[1].ipv6);
	TEST_CHECK(!strcmp(conf.base_name, "iqn.2024-01.com.example:vtl"));
	TEST_CHECK(conf.n_targets == 1 && conf.targets[0].id == 10);
	TEST_CHECK(conf.targets[0].n_initiators == 2);
	TEST_CHECK(!strcmp(conf.targets[0].initiators[0],
			   "iqn.2024-01.com.example:pbs1"));
	/* '#' without a blank before it is part of the value */
	TEST_CHECK(!strcmp(conf.targets[0].auth.secret, "abcdefghijkl#x"));
	TEST_CHECK(!strcmp(conf.targets[0].auth.mutual_name, "vtl"));
	TEST_CHECK(conf.has_secret);
}

void test_parse_field_names_are_loose(void)
{
	/* Field names ignore case and runs of blanks, values keep case */
	const char *s =
		"iscsi:\n"
		"\tportal:\t192.0.2.1\n"
		"ISCSI   TARGET: 10\n"
		"  initiator   NAME: iqn.2024-01.com.example:a\n"
		"  chap name: PBS\n"
		"  chap secret: Secret123456\n";

	TEST_ASSERT_(parse(s, &conf, err, sizeof(err)) == 0, "%s", err);
	TEST_CHECK(!strcmp(conf.targets[0].auth.name, "PBS"));
	TEST_CHECK(!strcmp(conf.targets[0].auth.secret, "Secret123456"));
}

static void expect_error(const char *s, const char *what)
{
	TEST_CHECK_(parse(s, &conf, err, sizeof(err)) != 0,
		    "should fail: %s", what);
	TEST_MSG("error was: %s", err);
}

void test_parse_errors(void)
{
	expect_error("iSCSI:\n Portl: 1.2.3.4\n", "typo in field");
	expect_error("iSCSI:\niSCSI target: 10\n Initator name: iqn.2024-01.x:y\n",
		     "typo in initiator field");
	expect_error("FC:\n", "unknown record");
	expect_error("iSCSI:\n Portal: 1.2.3.4:0\n", "port 0");
	expect_error("iSCSI:\n Portal: 1.2.3.4:70000\n", "port too large");
	expect_error("iSCSI:\n Portal: host.example:3260\n", "host name portal");
	expect_error("iSCSI:\n Portal: 2001:db8::1\n", "IPv6 without brackets");
	expect_error("iSCSI target: 10\n Initiator name: iqn.2024-01.x:y\n",
		     "target without iSCSI record");
	expect_error("iSCSI:\niSCSI target: x\n", "non numeric id");
	expect_error("iSCSI:\niSCSI target: 10\niSCSI target: 10\n",
		     "duplicate target");
	expect_error("iSCSI:\niSCSI target: 10\n Initiator name: IQN.2024-01.x:y\n",
		     "upper case iSCSI name");
	expect_error("iSCSI:\niSCSI target: 10\n Initiator name: iqn.2024-01.x/../y\n",
		     "slash in iSCSI name");
	expect_error("iSCSI:\niSCSI target: 10\n Initiator name: iqn.2024-01.x:y\n"
		     " Initiator name: iqn.2024-01.x:y\n", "duplicate initiator");
	expect_error("iSCSI:\niSCSI target: 10\n CHAP name: a\n",
		     "CHAP name without secret");
	expect_error("iSCSI:\niSCSI target: 10\n CHAP secret: a b\n",
		     "blank in secret");
	expect_error("iSCSI:\niSCSI target: 10\n CHAP name: NULLuser\n CHAP secret: abcdefghijkl\n",
		     "NULL prefix clears the value in LIO");
	expect_error("iSCSI:\niSCSI target: 10\n CHAP name: a\n CHAP secret: abcdefghijkl\n"
		     " Mutual CHAP name: b\n Mutual CHAP secret: abcdefghijkl\n",
		     "same secret both directions");
	expect_error("iSCSI:\niSCSI target: 10\n Mutual CHAP name: b\n"
		     " Mutual CHAP secret: abcdefghijkl\n", "mutual without CHAP");
	expect_error("VERSION: 2\n", "future version");
	expect_error(" Portal: 1.2.3.4\n", "field outside a record");
	expect_error("iSCSI:\n Portal 1.2.3.4\n", "no colon");
	expect_error("iSCSI:\n Portal: 1.2.3.4\n Portal: 1.2.3.4:3260\n",
		     "same portal twice");
	expect_error("iSCSI:\n Portal: 0.0.0.0\n Portal: 192.0.2.1\n",
		     "wildcard and specific address on one port");
	expect_error("iSCSI:\n Portal: [2001:db8::1]\n Portal: [::]:3260\n",
		     "IPv6 wildcard and specific address on one port");
}

void test_parse_portals_that_coexist(void)
{
	const char *s =
		"iSCSI:\n"
		" Portal: 0.0.0.0:3260\n"
		" Portal: 192.0.2.1:3261\n"
		" Portal: [2001:db8::1]:3260\n";

	TEST_CHECK_(parse(s, &conf, err, sizeof(err)) == 0, "%s", err);
	TEST_CHECK(conf.n_portals == 3);
}

void test_secret_never_in_error(void)
{
	TEST_CHECK(parse("iSCSI:\niSCSI target: 10\n CHAP secret: hunter2 x\n",
			 &conf, err, sizeof(err)) != 0);
	TEST_CHECK_(strstr(err, "hunter2") == NULL, "error leaks secret: %s", err);
}

void test_plan_library(void)
{
	const char *s =
		"iSCSI:\n"
		" Base name: " BASE "\n"
		"iSCSI target: 10\n"
		" Initiator name: iqn.2024-01.com.example:pbs1\n"
		"iSCSI target: 30\n"
		" Target name: iqn.2024-01.com.example:custom\n"
		" Initiator name: iqn.2024-01.com.example:pbs1\n"
		"iSCSI target: 50\n"
		" Initiator name: iqn.2024-01.com.example:pbs1\n";
	struct fab_target *t;

	TEST_ASSERT_(plan(s, &conf, err, sizeof(err)) == 0, "%s", err);

	t = &conf.targets[0];
	TEST_CHECK_(!strcmp(t->wwn, W10), "wwn %s", t->wwn);
	TEST_CHECK(t->n_luns == 3);
	TEST_CHECK(t->luns[0].lun == 0 && !strcmp(t->luns[0].backstore, "lib10"));
	TEST_CHECK(t->luns[1].lun == 1 && !strcmp(t->luns[1].backstore, "tape11"));
	TEST_CHECK(t->luns[2].lun == 2 && !strcmp(t->luns[2].backstore, "tape12"));

	/* LUN follows the drive slot, not the record order */
	t = &conf.targets[1];
	TEST_CHECK(!strcmp(t->wwn, "iqn.2024-01.com.example:custom"));
	TEST_CHECK(t->n_luns == 2 && t->luns[1].lun == 4 &&
		   !strcmp(t->luns[1].backstore, "tape31"));

	/* Standalone drive: LUN 0 */
	t = &conf.targets[2];
	TEST_CHECK(!strcmp(t->wwn, BASE ".tape50"));
	TEST_CHECK(t->n_luns == 1 && t->luns[0].lun == 0 &&
		   !strcmp(t->luns[0].backstore, "tape50"));
}

void test_plan_errors(void)
{
	TEST_CHECK(plan("iSCSI:\niSCSI target: 99\n", &conf, err, sizeof(err)));
	TEST_MSG("%s", err);
	/* A drive inside a library is exported with its library */
	TEST_CHECK(plan("iSCSI:\niSCSI target: 11\n", &conf, err, sizeof(err)));
	TEST_MSG("%s", err);
	/* Two targets ending up with the same name */
	TEST_CHECK(plan("iSCSI:\n Base name: " BASE "\niSCSI target: 10\n"
			"iSCSI target: 30\n Target name: " W10 "\n",
			&conf, err, sizeof(err)));
	TEST_MSG("%s", err);
	/* ... also when the later one is a standalone drive */
	TEST_CHECK(plan("iSCSI:\n Base name: " BASE "\n"
			"iSCSI target: 10\n Target name: " BASE ".tape50\n"
			"iSCSI target: 50\n", &conf, err, sizeof(err)));
	TEST_MSG("%s", err);
}

void test_plan_base_name_required(void)
{
	/* No default: the naming authority is the administrator's domain */
	TEST_CHECK(plan("iSCSI:\niSCSI target: 10\n", &conf, err, sizeof(err)));
	TEST_CHECK_(strstr(err, "Base name") != NULL, "error: %s", err);
	/* Not needed when every target is named */
	TEST_CHECK_(plan("iSCSI:\niSCSI target: 10\n Target name: " W10 "\n",
			 &conf, err, sizeof(err)) == 0, "%s", err);
	TEST_CHECK(!strcmp(conf.targets[0].wwn, W10));
}

/* ---- configfs engine against a fake tree ---- */

static char root[256], state[300];

static void fake_setup(void)
{
	const char *bs[] = { "lib10", "tape11", "tape12", "lib30", "tape31" };
	char p[512];
	size_t i;

	snprintf(root, sizeof(root), "/tmp/mhvtl-fabrics-XXXXXX");
	TEST_ASSERT(mkdtemp(root) != NULL);
	snprintf(state, sizeof(state), "%s/state", root);
	snprintf(cfs_root, sizeof(cfs_root), "%s/target", root);
	snprintf(fab_state_path, sizeof(fab_state_path), "%s", state);
	cfs_fake = 1;
	fab_wait_seconds = 0;

	/* The handler's backstores, enabled */
	for (i = 0; i < sizeof(bs) / sizeof(bs[0]); i++) {
		FILE *fp;

		snprintf(p, sizeof(p), "%s/target", root);
		mkdir(p, 0755);
		snprintf(p, sizeof(p), "%s/target/core", root);
		mkdir(p, 0755);
		snprintf(p, sizeof(p), "%s/target/core/user_0", root);
		mkdir(p, 0755);
		snprintf(p, sizeof(p), "%s/target/core/user_0/%s", root, bs[i]);
		mkdir(p, 0755);
		snprintf(p, sizeof(p), "%s/target/core/user_0/%s/enable", root, bs[i]);
		fp = fopen(p, "w");
		fputs("1\n", fp);
		fclose(fp);
	}
}

static void fake_teardown(void)
{
	char cmd[300];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
	(void)!system(cmd);
}

static int exists(const char *fmt, const char *a, const char *b)
{
	char		p[1024], q[512];
	struct stat	st;

	snprintf(q, sizeof(q), fmt, a, b);
	snprintf(p, sizeof(p), "%s/%s", cfs_root, q);
	return lstat(p, &st) == 0;
}

static char *read_file(const char *fmt, const char *a, const char *b)
{
	static char	v[256];
	char		p[1024], q[512];
	FILE		*fp;

	snprintf(q, sizeof(q), fmt, a, b);
	snprintf(p, sizeof(p), "%s/%s", cfs_root, q);
	v[0] = '\0';
	fp = fopen(p, "r");
	if (fp) {
		if (!fgets(v, sizeof(v), fp))
			v[0] = '\0';
		fclose(fp);
	}
	return v;
}

#define DATAOUT	"iscsi/%s/tpgt_1/acls/%s/attrib/dataout_timeout"

/* Session parameters apply_tpg sets for tape on every target */
static void check_tape_params(const char *wwn)
{
	static const struct {
		const char *param, *val;
	} want[] = {
		{ "InitialR2T", "No" },
		{ "ImmediateData", "Yes" },
		{ "FirstBurstLength", "262144" },
		{ "MaxBurstLength", "1048576" },
		{ "MaxRecvDataSegmentLength", "262144" },
	};
	size_t	i;

	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++)
		TEST_CHECK_(!strcmp(read_file("iscsi/%s/tpgt_1/param/%s", wwn,
					      want[i].param), want[i].val),
			    "%s %s", wwn, want[i].param);
}

#define I1  "iqn.2024-01.com.example:pbs1"
#define I2  "iqn.2024-01.com.example:pbs2"

static const char CONF_TWO[] =
	"iSCSI:\n"
	" Portal: 192.0.2.10:3260\n"
	" Base name: " BASE "\n"
	"iSCSI target: 10\n"
	" Initiator name: " I1 "\n"
	" Initiator name: " I2 "\n"
	" CHAP name: pbs\n"
	" CHAP secret: abcdefghijkl\n"
	"iSCSI target: 30\n"
	" Initiator name: " I1 "\n";

void test_apply_builds_tree(void)
{
	fake_setup();
	TEST_ASSERT_(plan(CONF_TWO, &conf, err, sizeof(err)) == 0, "%s", err);
	TEST_ASSERT_(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0,
		     "%s", err);

	TEST_CHECK(exists("iscsi/%s/tpgt_1/lun/lun_0/lib10%s", W10, ""));
	TEST_CHECK(exists("iscsi/%s/tpgt_1/lun/lun_2/tape12%s", W10, ""));
	TEST_CHECK(exists("iscsi/%s/tpgt_1/acls/%s/lun_1/lun", W10, I2));
	TEST_CHECK(exists("iscsi/%s/tpgt_1/np/192.0.2.10:3260%s", W10, ""));
	TEST_CHECK(!strcmp(read_file("iscsi/%s/tpgt_1/enable%s", W10, ""), "1"));
	TEST_CHECK(!strcmp(read_file("iscsi/%s/tpgt_1/attrib/generate_node_acls%s",
				     W10, ""), "0"));
	TEST_CHECK(!strcmp(read_file("iscsi/%s/tpgt_1/attrib/demo_mode_discovery%s",
				     W10, ""), "0"));
	TEST_CHECK(!strcmp(read_file("iscsi/%s/tpgt_1/attrib/authentication%s",
				     W10, ""), "1"));
	TEST_CHECK(!strcmp(read_file("iscsi/%s/tpgt_1/acls/%s/auth/password",
				     W10, I1), "abcdefghijkl"));
	TEST_CHECK(!strcmp(read_file("iscsi/%s/tpgt_1/acls/%s/auth/userid_mutual",
				     W10, I1), "NULL"));
	check_tape_params(W10);
	check_tape_params(W30);
	TEST_CHECK(!strcmp(read_file(DATAOUT, W10, I1), "30"));
	TEST_CHECK(!strcmp(read_file(DATAOUT, W10, I2), "30"));
	TEST_CHECK(!strcmp(read_file("iscsi/%s/tpgt_1/attrib/authentication%s",
				     W30, ""), "0"));
	TEST_CHECK(!strcmp(read_file("iscsi/%s/tpgt_1/param/AuthMethod%s",
				     W30, ""), "None"));
	TEST_CHECK(exists("iscsi/%s/tpgt_1/lun/lun_4/tape31%s", W30, ""));

	/* Second apply changes nothing and succeeds */
	TEST_CHECK_(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0,
		    "%s", err);
	TEST_CHECK(exists("iscsi/%s/tpgt_1/acls/%s/lun_1/lun", W10, I2));

	TEST_CHECK(fab_remove(&fabric_iscsi, &conf) == 0);
	TEST_CHECK(!exists("iscsi/%s%s", W10, ""));
	TEST_CHECK(!exists("iscsi/%s%s", W30, ""));
	/* Backstores belong to the handler and stay */
	TEST_CHECK(exists("core/user_0/lib10%s%s", "", ""));
	TEST_CHECK(access(state, F_OK) != 0);
	fake_teardown();
}

void test_apply_reconciles(void)
{
	const char *smaller =
		"iSCSI:\n"
		" Portal: 192.0.2.11\n"
		" Base name: " BASE "\n"
		"iSCSI target: 10\n"
		" Initiator name: " I1 "\n";

	fake_setup();
	TEST_ASSERT(plan(CONF_TWO, &conf, err, sizeof(err)) == 0);
	TEST_ASSERT(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0);

	/* Drop initiator 2, library 30, CHAP, and move the portal */
	TEST_ASSERT_(plan(smaller, &conf, err, sizeof(err)) == 0, "%s", err);
	TEST_ASSERT_(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0,
		     "%s", err);
	TEST_CHECK(exists("iscsi/%s/tpgt_1/acls/%s", W10, I1));
	TEST_CHECK(!exists("iscsi/%s/tpgt_1/acls/%s", W10, I2));
	TEST_CHECK_(!exists("iscsi/%s%s", W30, ""),
		    "target removed from the config is torn down");
	TEST_CHECK(exists("iscsi/%s/tpgt_1/np/192.0.2.11:3260%s", W10, ""));
	TEST_CHECK(!exists("iscsi/%s/tpgt_1/np/192.0.2.10:3260%s", W10, ""));
	TEST_CHECK(!strcmp(read_file("iscsi/%s/tpgt_1/acls/%s/auth/userid",
				     W10, I1), "NULL"));
	TEST_CHECK(!strcmp(read_file("iscsi/%s/tpgt_1/attrib/authentication%s",
				     W10, ""), "0"));
	/* A re-apply keeps the tape parameters and the Data-Out timeout */
	check_tape_params(W10);
	TEST_CHECK(!strcmp(read_file(DATAOUT, W10, I1), "30"));

	/* remove with an empty configuration still uses the state file */
	memset(&conf, 0, sizeof(conf));
	TEST_CHECK(fab_remove(&fabric_iscsi, &conf) == 0);
	TEST_CHECK(!exists("iscsi/%s%s", W10, ""));
	fake_teardown();
}

void test_apply_prunes_lun(void)
{
	fake_setup();
	TEST_ASSERT(plan(CONF_TWO, &conf, err, sizeof(err)) == 0);
	TEST_ASSERT(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0);

	/* A drive removed from device.conf: its LUN and mapped LUNs go */
	conf.targets[0].n_luns = 2;
	TEST_ASSERT_(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0,
		     "%s", err);
	TEST_CHECK(!exists("iscsi/%s/tpgt_1/lun/lun_2%s", W10, ""));
	TEST_CHECK(!exists("iscsi/%s/tpgt_1/acls/%s/lun_2", W10, I1));
	TEST_CHECK(exists("iscsi/%s/tpgt_1/acls/%s/lun_1/lun", W10, I1));
	TEST_CHECK(fab_remove(&fabric_iscsi, &conf) == 0);
	fake_teardown();
}

static void touch(const char *fmt, const char *a, const char *b)
{
	char	p[1024], q[512];
	FILE	*fp;

	snprintf(q, sizeof(q), fmt, a, b);
	snprintf(p, sizeof(p), "%s/%s", cfs_root, q);
	fp = fopen(p, "w");
	if (fp)
		fclose(fp);
}

static int links_to(const char *fmt, const char *a, const char *b,
		    const char *base)
{
	char	p[1024], q[512], l[1024], *c;
	ssize_t	n;

	snprintf(q, sizeof(q), fmt, a, b);
	snprintf(p, sizeof(p), "%s/%s", cfs_root, q);
	n = readlink(p, l, sizeof(l) - 1);
	if (n < 0)
		return 0;
	l[n] = '\0';
	c = strrchr(l, '/');
	return !strcmp(c ? c + 1 : l, base);
}

void test_apply_lun_moves_backstore(void)
{
	char tmp[FAB_BS_MAX];

	fake_setup();
	TEST_ASSERT(plan(CONF_TWO, &conf, err, sizeof(err)) == 0);
	TEST_ASSERT(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0);
	/* Marks the mapped LUN group, so its rebuild can be seen */
	touch("iscsi/%s/tpgt_1/acls/%s/lun_1/marker", W10, I1);

	/* Drives 11 and 12 swap Slot values in device.conf */
	strcpy(tmp, conf.targets[0].luns[1].backstore);
	strcpy(conf.targets[0].luns[1].backstore, conf.targets[0].luns[2].backstore);
	strcpy(conf.targets[0].luns[2].backstore, tmp);
	TEST_ASSERT_(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0,
		     "%s", err);

	TEST_CHECK(links_to("iscsi/%s/tpgt_1/lun/lun_1/tape12%s", W10, "", "tape12"));
	TEST_CHECK(!exists("iscsi/%s/tpgt_1/lun/lun_1/tape11%s", W10, ""));
	/* The kernel drops mapped LUNs when the TPG LUN is unlinked, so the
	 * ACL's group must have been rebuilt, in every ACL */
	TEST_CHECK_(!exists("iscsi/%s/tpgt_1/acls/%s/lun_1/marker", W10, I1),
		    "mapped LUN 1 was not rebuilt");
	TEST_CHECK(links_to("iscsi/%s/tpgt_1/acls/%s/lun_1/lun", W10, I1, "lun_1"));
	TEST_CHECK(links_to("iscsi/%s/tpgt_1/acls/%s/lun_2/lun", W10, I2, "lun_2"));
	TEST_CHECK(fab_remove(&fabric_iscsi, &conf) == 0);
	fake_teardown();
}

void test_apply_keeps_unremovable_stale_target(void)
{
	const char *only10 =
		"iSCSI:\n"
		" Base name: " BASE "\n"
		"iSCSI target: 10\n"
		" Initiator name: " I1 "\n";
	char		blocker[1024];
	struct fab_conf	empty;

	fake_setup();
	TEST_ASSERT(plan(CONF_TWO, &conf, err, sizeof(err)) == 0);
	TEST_ASSERT(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0);

	/* A symlink left in the TPG makes its removal fail, as a busy
	 * configfs group would */
	snprintf(blocker, sizeof(blocker), "%s/iscsi/%s/tpgt_1/blocker",
		 cfs_root, W30);
	TEST_ASSERT(symlink("/", blocker) == 0);

	TEST_ASSERT(plan(only10, &conf, err, sizeof(err)) == 0);
	TEST_ASSERT_(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0,
		     "%s", err);
	TEST_CHECK(exists("iscsi/%s%s", W30, ""));

	/* Still owned: once it can go, remove with no configuration takes it */
	unlink(blocker);
	memset(&empty, 0, sizeof(empty));
	TEST_CHECK(fab_remove(&fabric_iscsi, &empty) == 0);
	TEST_CHECK_(!exists("iscsi/%s%s", W30, ""),
		    "stale target dropped from the state file");
	TEST_CHECK(!exists("iscsi/%s%s", W10, ""));
	fake_teardown();
}

void test_apply_waits_for_backstore(void)
{
	char p[1024];

	fake_setup();
	TEST_ASSERT(plan(CONF_TWO, &conf, err, sizeof(err)) == 0);
	snprintf(p, sizeof(p), "%s/core/user_0/tape31/enable", cfs_root);
	unlink(p);
	TEST_CHECK(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) != 0);
	TEST_CHECK_(strstr(err, "tape31") != NULL, "error: %s", err);
	/* Nothing is created while a backstore is missing */
	TEST_CHECK(!exists("iscsi/%s%s", W10, ""));
	fake_teardown();
}

void test_status_json(void)
{
	char	*buf = NULL;
	size_t	 len = 0;
	FILE	*out;

	fake_setup();
	TEST_ASSERT(plan(CONF_TWO, &conf, err, sizeof(err)) == 0);
	TEST_ASSERT(fab_apply(&fabric_iscsi, &conf, err, sizeof(err)) == 0);

	out = open_memstream(&buf, &len);
	fab_status(out, &fabric_iscsi, &conf);
	fclose(out);
	TEST_CHECK(strstr(buf, "\"wwn\":\"" W10 "\",\"present\":true,\"enabled\":true") != NULL);
	TEST_CHECK(strstr(buf, "{\"portal\":\"192.0.2.10:3260\",\"mtu\":null}") != NULL);
	TEST_CHECK(strstr(buf, "{\"lun\":2,\"backstore\":\"tape12\",\"backstore_ready\":true,\"mapped\":true}") != NULL);
	TEST_CHECK(strstr(buf, "\"chap\":true") != NULL);
	TEST_CHECK_(strstr(buf, "abcdefghijkl") == NULL, "status leaks secret");
	TEST_CHECK(strstr(buf, "\"stale\":[]") != NULL);
	TEST_MSG("%s", buf);
	free(buf);
	TEST_CHECK(fab_remove(&fabric_iscsi, &conf) == 0);
	fake_teardown();
}

TEST_LIST = {
	{ "parse_full", test_parse_full },
	{ "parse_field_names_are_loose", test_parse_field_names_are_loose },
	{ "parse_errors", test_parse_errors },
	{ "parse_portals_that_coexist", test_parse_portals_that_coexist },
	{ "secret_never_in_error", test_secret_never_in_error },
	{ "plan_library", test_plan_library },
	{ "plan_errors", test_plan_errors },
	{ "plan_base_name_required", test_plan_base_name_required },
	{ "apply_builds_tree", test_apply_builds_tree },
	{ "apply_reconciles", test_apply_reconciles },
	{ "apply_prunes_lun", test_apply_prunes_lun },
	{ "apply_lun_moves_backstore", test_apply_lun_moves_backstore },
	{ "apply_keeps_unremovable_stale_target", test_apply_keeps_unremovable_stale_target },
	{ "apply_waits_for_backstore", test_apply_waits_for_backstore },
	{ "status_json", test_status_json },
	{ NULL, NULL }
};
