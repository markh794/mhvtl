/*
 * mhvtl-fabrics - export mhVTL's TCMU backstores to remote initiators.
 *
 *   mhvtl-fabrics [options] check|apply|remove|status
 *
 * check   parse fabrics.conf and device.conf and print the plan
 * apply   create or update the targets (idempotent)
 * remove  remove every target this tool created
 * status  JSON status on stdout, for scripts and mhvtl-control
 *
 * The backstores belong to mhvtl_tcmu_handler; this tool only links LUNs to
 * them. See mhvtl-fabrics(1) and fabrics.conf(5).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fabrics.h"

#ifndef MHVTL_CONFIG_PATH
#define MHVTL_CONFIG_PATH "/etc/mhvtl"
#endif

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-c fabrics.conf] [-d device.conf] [-w seconds] check|apply|remove|status\n"
		"  -c FILE  fabric configuration (default " MHVTL_CONFIG_PATH "/fabrics.conf)\n"
		"  -d FILE  device configuration (default " MHVTL_CONFIG_PATH "/device.conf)\n"
		"  -w SECS  apply: wait this long for the TCMU backstores\n"
		"           (default 3 s per LUN, at least 60)\n",
		prog);
}

/*
 * fabrics.conf decides who reaches the tapes, so only its owner may write
 * it; with CHAP secrets in it, as with /etc/iscsi/iscsid.conf, nobody else
 * may read it either. Checked on the descriptor that was parsed.
 */
static int check_conf_file(FILE *fp, const char *path,
			   const struct fab_conf *conf)
{
	struct stat st;

	if (fstat(fileno(fp), &st))
		return -1;
	if (st.st_uid != 0 && st.st_uid != geteuid()) {
		fprintf(stderr, "%s is owned by uid %u, not root\n",
			path, (unsigned)st.st_uid);
		return -1;
	}
	if (st.st_mode & 022) {
		fprintf(stderr,
			"%s is writable by group or others (mode %04o); run chmod go-w %s\n",
			path, (unsigned)(st.st_mode & 07777), path);
		return -1;
	}
	if (conf->has_secret && (st.st_mode & 077)) {
		fprintf(stderr,
			"%s holds CHAP secrets but is readable by group or others (mode %04o); run chmod 600 %s\n",
			path, (unsigned)(st.st_mode & 07777), path);
		return -1;
	}
	return 0;
}

static int load(const char *conf_path, const char *dev_path,
		struct fab_conf *conf, int quiet)
{
	struct fab_topology	*topo;
	FILE			*fp;
	char			 err[512] = "";
	int			 i, n;

	fp = fopen(conf_path, "re");
	if (!fp) {
		fprintf(stderr, "cannot open %s: %s\n", conf_path, strerror(errno));
		return -1;
	}
	if (fab_conf_parse(fp, conf, err, sizeof(err))) {
		fprintf(stderr, "%s: %s\n", conf_path, err);
		fclose(fp);
		return -1;
	}
	if (check_conf_file(fp, conf_path, conf)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	topo = malloc(sizeof(*topo));
	if (!topo)
		return -1;
	fp = fopen(dev_path, "re");
	if (!fp) {
		fprintf(stderr, "cannot open %s: %s\n", dev_path, strerror(errno));
		free(topo);
		return -1;
	}
	n = fab_topology_parse(fp, topo, err, sizeof(err));
	fclose(fp);
	if (n) {
		fprintf(stderr, "%s: %s\n", dev_path, err);
		free(topo);
		return -1;
	}

	n = fab_plan(conf, topo, err, sizeof(err));
	free(topo);
	if (n) {
		fprintf(stderr, "%s: %s\n", conf_path, err);
		return -1;
	}

	/* No ACL means no access: the target is not exported at all */
	for (i = n = 0; i < conf->n_targets; i++) {
		const struct fab_auth *a = &conf->targets[i].auth;

		/* RFC 7143 section 9.2.1: a CHAP secret of at least 96 bits */
		if (!quiet && ((a->secret[0] && strlen(a->secret) < 12) ||
			       (a->mutual_secret[0] && strlen(a->mutual_secret) < 12)))
			fprintf(stderr,
				"%s: iSCSI target %ld: CHAP secret shorter than 12 characters; initiators may refuse it\n",
				conf_path, conf->targets[i].id);

		if (!conf->targets[i].n_initiators) {
			if (!quiet)
				fprintf(stderr,
					"%s: iSCSI target %ld lists no Initiator name; not exported\n",
					conf_path, conf->targets[i].id);
			continue;
		}
		if (n != i)
			conf->targets[n] = conf->targets[i];
		n++;
	}
	conf->n_targets = n;
	return 0;
}

static void print_plan(const struct fab_conf *conf)
{
	int i, j;

	if (!conf->n_portals)
		printf("portal 0.0.0.0:3260\n");
	for (i = 0; i < conf->n_portals; i++)
		printf(conf->portals[i].ipv6 ? "portal [%s]:%d\n" : "portal %s:%d\n",
		       conf->portals[i].addr, conf->portals[i].port);
	for (i = 0; i < conf->n_targets; i++) {
		const struct fab_target *t = &conf->targets[i];

		printf("target %s (device.conf %ld)%s%s\n", t->wwn, t->id,
		       t->auth.name[0] ? ", CHAP" : "",
		       t->auth.mutual_name[0] ? ", mutual CHAP" : "");
		for (j = 0; j < t->n_luns; j++)
			printf("  lun %u -> core/user_0/%s\n", t->luns[j].lun,
			       t->luns[j].backstore);
		for (j = 0; j < t->n_initiators; j++)
			printf("  initiator %s\n", t->initiators[j]);
	}
}

/* apply and remove from systemd and from mhvtl-control must not interleave */
static int lock_state(void)
{
	char	path[600], *slash;
	int	fd;

	/* /run/mhvtl comes from tmpfiles.d or the handler; don't depend on it */
	snprintf(path, sizeof(path), "%s", fab_state_path);
	slash = strrchr(path, '/');
	if (slash && slash != path) {
		*slash = '\0';
		mkdir(path, 0755);
	}
	snprintf(path, sizeof(path), "%s.lock", fab_state_path);
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0 || flock(fd, LOCK_EX)) {
		fprintf(stderr, "cannot lock %s: %s\n", path, strerror(errno));
		return -1;
	}
	return fd;
}

int main(int argc, char *argv[])
{
	char		 conf_path[512] = MHVTL_CONFIG_PATH "/fabrics.conf";
	char		 dev_path[512] = MHVTL_CONFIG_PATH "/device.conf";
	char		 err[512] = "";
	const char	*cmd, *env;
	struct fab_conf	*conf;
	int		 opt, ret = 0, lock = -1;

	while ((opt = getopt(argc, argv, "c:d:w:h")) != -1) {
		switch (opt) {
		case 'c':
			snprintf(conf_path, sizeof(conf_path), "%s", optarg);
			break;
		case 'd':
			snprintf(dev_path, sizeof(dev_path), "%s", optarg);
			break;
		case 'w': {
			char *end;
			long w = strtol(optarg, &end, 10);

			if (!*optarg || *end || w < 0 || w > 3600) {
				fprintf(stderr, "-w takes 0 to 3600 seconds\n");
				return 1;
			}
			fab_wait_seconds = (int)w;
			break;
		}
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}
	if (optind != argc - 1) {
		usage(argv[0]);
		return 1;
	}
	cmd = argv[optind];

	/*
	 * Test hooks: point the engine at a plain directory. Never as root, so
	 * a privileged caller passing its environment through can't redirect
	 * writes.
	 */
	if (geteuid() != 0) {
		env = getenv("MHVTL_FABRICS_CONFIGFS");
		if (env) {
			snprintf(cfs_root, sizeof(cfs_root), "%s", env);
			cfs_fake = 1;
		}
		env = getenv("MHVTL_FABRICS_STATE");
		if (env)
			snprintf(fab_state_path, sizeof(fab_state_path), "%s", env);
	}

	conf = calloc(1, sizeof(*conf));
	if (!conf)
		return 1;

	if (!strcmp(cmd, "check")) {
		if (load(conf_path, dev_path, conf, 0))
			ret = 1;
		else
			print_plan(conf);
	} else if (!strcmp(cmd, "apply")) {
		/* The kernel module backend has no TCMU backstores to export */
		env = getenv("MHVTL_BACKEND");
		if (env && strcmp(env, "tcmu")) {
			fprintf(stderr, "MHVTL_BACKEND is '%s', not 'tcmu'; nothing to export\n",
				env);
		} else if (load(conf_path, dev_path, conf, 0) ||
			   (lock = lock_state()) < 0) {
			ret = 1;
		} else if (fab_apply(&fabric_iscsi, conf, err, sizeof(err))) {
			fprintf(stderr, "apply failed: %s\n", err);
			ret = 1;
		}
	} else if (!strcmp(cmd, "remove")) {
		/* Without a readable configuration the state file still lists
		 * what to remove */
		if (load(conf_path, dev_path, conf, 1))
			memset(conf, 0, sizeof(*conf));
		if ((lock = lock_state()) < 0 || fab_remove(&fabric_iscsi, conf)) {
			fprintf(stderr, "remove left targets behind\n");
			ret = 1;
		}
	} else if (!strcmp(cmd, "status")) {
		if (load(conf_path, dev_path, conf, 1))
			ret = 1;
		else
			fab_status(stdout, &fabric_iscsi, conf);
	} else {
		usage(argv[0]);
		ret = 1;
	}

	if (lock >= 0)
		close(lock);
	free(conf);
	return ret;
}
