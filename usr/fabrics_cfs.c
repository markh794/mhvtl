/*
 * LIO configfs engine for mhvtl-fabrics: apply, remove and status, shared by
 * every fabric. See include/fabrics.h and docs on the configfs layout in the
 * kernel's drivers/target/target_core_fabric_configfs.c.
 *
 * For each target this builds:
 *
 *   <fabric>/<wwn>/tpgt_1/
 *     lun/lun_<N>/<backstore>        -> core/user_0/<backstore>
 *     acls/<initiator>/lun_<N>/lun   -> tpgt_1/lun/lun_<N>
 *     np/<portal>                       (iSCSI)
 *     enable = 1                        (last)
 *
 * An explicit node ACL gets no mapped LUNs from the kernel (only dynamic,
 * demo mode ACLs do, in core_tpg_check_initiator_node_acl()), so they are
 * created here.
 *
 * apply is idempotent: it creates what is missing and removes LUNs, ACLs and
 * portals the configuration no longer lists, so a running session to an
 * unchanged LUN is not dropped. Targets are tracked in a state file, so a
 * target whose record was deleted from fabrics.conf is still removed.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fabrics.h"

char cfs_root[512] = "/sys/kernel/config/target";
int cfs_fake;
int fab_wait_seconds = -1;
char fab_state_path[512] = "/run/mhvtl/fabrics.state";

#define TPG	"tpgt_1"
#define PATHLEN	1024

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (!errbuf || !errlen)
		return;
	va_start(ap, fmt);
	vsnprintf(errbuf, errlen, fmt, ap);
	va_end(ap);
}

/* snprintf into a path buffer; a truncated path is never used */
static int mkpath(char *buf, const char *fmt, ...)
{
	va_list	ap;
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(buf, PATHLEN, fmt, ap);
	va_end(ap);
	if (n < 0 || n >= PATHLEN) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

/* ---- configfs primitives ---- */

static int cfs_mkdir(const char *path);

static int cfs_write(const char *path, const char *val)
{
	int	fd, e;
	ssize_t	r;

	/* attrib/, param/ and auth/ are default groups in configfs */
	if (cfs_fake) {
		char dir[PATHLEN], *slash;

		snprintf(dir, sizeof(dir), "%s", path);
		slash = strrchr(dir, '/');
		if (slash) {
			*slash = '\0';
			if (cfs_mkdir(dir))
				return -1;
		}
	}
	fd = open(path, cfs_fake ? O_WRONLY | O_CREAT | O_TRUNC : O_WRONLY, 0644);
	if (fd < 0)
		return -1;
	r = write(fd, val, strlen(val));
	e = errno;
	close(fd);
	if (r < 0) {
		errno = e;
		return -1;
	}
	return 0;
}

static int cfs_read(const char *path, char *buf, size_t len)
{
	int	fd;
	ssize_t	r;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	r = read(fd, buf, len - 1);
	close(fd);
	if (r < 0)
		return -1;
	buf[r] = '\0';
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
		buf[--r] = '\0';
	return 0;
}

static int is_dir(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * configfs creates a group's default subdirectories itself; a plain test
 * directory does not, so there parents are created as needed.
 */
static int cfs_mkdir(const char *path)
{
	if (cfs_fake) {
		char	tmp[PATHLEN], *p;

		snprintf(tmp, sizeof(tmp), "%s", path);
		for (p = tmp + strlen(cfs_root) + 1; (p = strchr(p, '/')); p++) {
			*p = '\0';
			if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(path, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/*
 * Remove a configfs group. configfs drops attribute files and default groups
 * by itself and refuses while a symlink is still inside; the fake tree does
 * the same, so tests catch teardown done in the wrong order.
 */
static int fake_clear(const char *path)
{
	DIR		*d = opendir(path);
	struct dirent	*e;
	int		 ret = 0;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char		p[PATHLEN];
		struct stat	st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (mkpath(p, "%s/%s", path, e->d_name) || lstat(p, &st)) {
			ret = -1;
			continue;
		}
		if (S_ISLNK(st.st_mode)) {
			errno = EBUSY;
			ret = -1;
		} else if (S_ISDIR(st.st_mode)) {
			if (fake_clear(p) || rmdir(p))
				ret = -1;
		} else {
			unlink(p);
		}
	}
	closedir(d);
	return ret;
}

static int cfs_rmdir(const char *path)
{
	if (!is_dir(path))
		return 0;
	if (cfs_fake && fake_clear(path))
		return -1;
	return rmdir(path) < 0 && errno != ENOENT ? -1 : 0;
}

/* Remove every symlink directly inside dir */
static void unlink_links(const char *dir)
{
	DIR		*d = opendir(dir);
	struct dirent	*e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		char		p[PATHLEN];
		struct stat	st;

		if (!mkpath(p, "%s/%s", dir, e->d_name) && !lstat(p, &st) &&
		    S_ISLNK(st.st_mode))
			unlink(p);
	}
	closedir(d);
}

/*
 * Point dir/name at target, replacing any other link in dir. Returns 0 when
 * the link was already right.
 */
static int ensure_link(const char *dir, const char *name, const char *target,
		       const char *want_base)
{
	DIR		*d;
	struct dirent	*e;
	char		 p[PATHLEN], l[PATHLEN];
	int		 ok = 0;

	d = opendir(dir);
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		struct stat	st;
		ssize_t		n;
		const char	*base;

		if (mkpath(p, "%s/%s", dir, e->d_name) || lstat(p, &st) ||
		    !S_ISLNK(st.st_mode))
			continue;
		n = readlink(p, l, sizeof(l) - 1);
		if (n < 0)
			continue;
		l[n] = '\0';
		base = strrchr(l, '/');
		base = base ? base + 1 : l;
		if (!ok && !strcmp(base, want_base))
			ok = 1;
		else
			unlink(p);
	}
	closedir(d);
	if (ok)
		return 0;
	if (mkpath(p, "%s/%s", dir, name))
		return -1;
	return symlink(target, p);
}

/* Whether dir holds a symlink whose target's last component is not want_base */
static int links_elsewhere(const char *dir, const char *want_base)
{
	DIR		*d = opendir(dir);
	struct dirent	*e;
	int		 other = 0;

	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char		 p[PATHLEN], l[PATHLEN];
		const char	*base;
		ssize_t		 n;

		if (mkpath(p, "%s/%s", dir, e->d_name))
			continue;
		n = readlink(p, l, sizeof(l) - 1);
		if (n < 0)
			continue;
		l[n] = '\0';
		base = strrchr(l, '/');
		base = base ? base + 1 : l;
		if (strcmp(base, want_base))
			other = 1;
	}
	closedir(d);
	return other;
}

static int parse_lun_dir(const char *name, unsigned *lun)
{
	char *end;
	unsigned long n;

	if (strncmp(name, "lun_", 4) || !name[4])
		return -1;
	n = strtoul(name + 4, &end, 10);
	if (*end || n > UINT_MAX)
		return -1;
	*lun = (unsigned)n;
	return 0;
}

static int target_has_lun(const struct fab_target *t, unsigned lun)
{
	int i;

	for (i = 0; i < t->n_luns; i++)
		if (t->luns[i].lun == lun)
			return 1;
	return 0;
}

/* Remove the lun_<N> groups in dir (a TPG lun/ or an ACL) the target lacks */
static void prune_luns(const char *dir, const struct fab_target *t)
{
	DIR		*d = opendir(dir);
	struct dirent	*e;
	unsigned	 lun;

	if (!d)
		return;
	while ((e = readdir(d))) {
		char p[PATHLEN];

		if (parse_lun_dir(e->d_name, &lun) ||
		    (t && target_has_lun(t, lun)))
			continue;
		if (mkpath(p, "%s/%s", dir, e->d_name))
			continue;
		unlink_links(p);
		cfs_rmdir(p);
	}
	closedir(d);
}

static void remove_acl(const char *acl)
{
	prune_luns(acl, NULL);
	cfs_rmdir(acl);
}

/* ---- backstores ---- */

static int backstore_ready(const char *bs)
{
	char p[PATHLEN], v[16];

	if (mkpath(p, "%s/core/user_0/%s/enable", cfs_root, bs) ||
	    cfs_read(p, v, sizeof(v)))
		return 0;
	return !strcmp(v, "1");
}

static time_t now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

/*
 * The handler creates a backstore when its daemon registers, about 2 s per
 * device on the phase 1 VMs. Wait for all of them so a target never goes live
 * with drives missing; by default 3 s per LUN, at least 60 s.
 */
static int wait_backstores(const struct fab_conf *conf, char *errbuf,
			   size_t errlen)
{
	time_t	deadline;
	int	i, j, wait = fab_wait_seconds, n = 0;

	if (wait < 0) {
		for (i = 0; i < conf->n_targets; i++)
			n += conf->targets[i].n_luns;
		wait = 3 * n > 60 ? 3 * n : 60;
	}
	deadline = now() + wait;

	for (;;) {
		const char *missing = NULL;

		for (i = 0; i < conf->n_targets && !missing; i++)
			for (j = 0; j < conf->targets[i].n_luns; j++)
				if (!backstore_ready(conf->targets[i].luns[j].backstore)) {
					missing = conf->targets[i].luns[j].backstore;
					break;
				}
		if (!missing)
			return 0;
		if (now() >= deadline) {
			set_err(errbuf, errlen,
				"backstore core/user_0/%s is not enabled after %d s; is mhvtl_tcmu_handler running and the device's daemon started?",
				missing, wait);
			return -1;
		}
		usleep(500000);
	}
}

/* ---- state file ---- */

struct owned {
	int	n;
	char	wwn[FAB_MAX_TARGETS * 2][FAB_NAME_MAX];
};

static int conf_has_wwn(const struct fab_conf *conf, const char *wwn)
{
	int i;

	for (i = 0; i < conf->n_targets; i++)
		if (!strcmp(conf->targets[i].wwn, wwn))
			return 1;
	return 0;
}

/* The state and lock files live in /run/mhvtl; never follow a symlink there */
static FILE *open_nofollow(const char *path, int flags, const char *mode)
{
	int	 fd = open(path, flags | O_NOFOLLOW | O_CLOEXEC, 0600);
	FILE	*fp;

	if (fd < 0)
		return NULL;
	fp = fdopen(fd, mode);
	if (!fp)
		close(fd);
	return fp;
}

static void state_read(const struct fabric_ops *ops, struct owned *o)
{
	FILE	*fp = open_nofollow(fab_state_path, O_RDONLY, "r");
	char	 fabric[32], wwn[FAB_NAME_MAX];

	o->n = 0;
	if (!fp)
		return;
	while (o->n < (int)(sizeof(o->wwn) / sizeof(o->wwn[0])) &&
	       fscanf(fp, "%31s %223s", fabric, wwn) == 2)
		if (!strcmp(fabric, ops->name) && fab_valid_iscsi_name(wwn))
			strcpy(o->wwn[o->n++], wwn);
	fclose(fp);
}

static int target_exists(const struct fabric_ops *ops, const char *wwn)
{
	char p[PATHLEN];

	return !mkpath(p, "%s/%s/%s", cfs_root, ops->name, wwn) && is_dir(p);
}

/*
 * Record the configured targets, plus any owned target that is still in
 * configfs although it should be gone, so a later remove retries it.
 */
static int state_write(const struct fabric_ops *ops, const struct fab_conf *conf,
		       const struct owned *old)
{
	char	 tmp[PATHLEN];
	FILE	*fp;
	int	 i;

	if (mkpath(tmp, "%s.tmp", fab_state_path))
		return -1;
	unlink(tmp);
	fp = open_nofollow(tmp, O_WRONLY | O_CREAT | O_EXCL, "w");
	if (!fp)
		return -1;
	for (i = 0; i < conf->n_targets; i++)
		fprintf(fp, "%s %s\n", ops->name, conf->targets[i].wwn);
	for (i = 0; old && i < old->n; i++)
		if (!conf_has_wwn(conf, old->wwn[i]) &&
		    target_exists(ops, old->wwn[i]))
			fprintf(fp, "%s %s\n", ops->name, old->wwn[i]);
	if (fclose(fp))
		return -1;
	return rename(tmp, fab_state_path);
}

/* ---- remove ---- */

static void remove_target(const struct fabric_ops *ops, const char *wwn)
{
	char		 tgt[PATHLEN], tpg[PATHLEN], p[PATHLEN];
	DIR		*d;
	struct dirent	*e;

	if (mkpath(tgt, "%s/%s/%s", cfs_root, ops->name, wwn) ||
	    mkpath(tpg, "%s/" TPG, tgt) || !is_dir(tgt))
		return;

	/* Disable first so initiators see the target go away, not I/O errors */
	if (!mkpath(p, "%s/enable", tpg) && is_dir(tpg))
		cfs_write(p, "0");

	if (!mkpath(p, "%s/acls", tpg) && (d = opendir(p))) {
		while ((e = readdir(d))) {
			char acl[PATHLEN];

			if (e->d_name[0] == '.' ||
			    mkpath(acl, "%s/acls/%s", tpg, e->d_name) ||
			    !is_dir(acl))
				continue;
			remove_acl(acl);
		}
		closedir(d);
	}
	if (!mkpath(p, "%s/lun", tpg))
		prune_luns(p, NULL);
	if (!mkpath(p, "%s/np", tpg) && (d = opendir(p))) {
		while ((e = readdir(d))) {
			char np[PATHLEN];

			if (e->d_name[0] == '.' ||
			    mkpath(np, "%s/np/%s", tpg, e->d_name))
				continue;
			cfs_rmdir(np);
		}
		closedir(d);
	}
	cfs_rmdir(tpg);
	cfs_rmdir(tgt);
}

int fab_remove(const struct fabric_ops *ops, const struct fab_conf *conf)
{
	struct owned	o;
	int		i, ret = 0;

	state_read(ops, &o);
	for (i = 0; i < o.n; i++)
		remove_target(ops, o.wwn[i]);
	for (i = 0; i < conf->n_targets; i++)
		remove_target(ops, conf->targets[i].wwn);

	for (i = 0; i < o.n; i++)
		if (target_exists(ops, o.wwn[i]))
			ret = -1;
	for (i = 0; i < conf->n_targets; i++)
		if (target_exists(ops, conf->targets[i].wwn))
			ret = -1;
	if (!ret)
		unlink(fab_state_path);
	return ret;
}

/* ---- apply ---- */

/* Remove mapped LUN <lun> from every ACL of tpg */
static void drop_mapped_lun(const char *tpg, unsigned lun)
{
	char		 p[PATHLEN];
	DIR		*d;
	struct dirent	*e;

	if (mkpath(p, "%s/acls", tpg) || !(d = opendir(p)))
		return;
	while ((e = readdir(d))) {
		char m[PATHLEN];

		if (e->d_name[0] == '.' ||
		    mkpath(m, "%s/acls/%s/lun_%u", tpg, e->d_name, lun) ||
		    !is_dir(m))
			continue;
		unlink_links(m);
		cfs_rmdir(m);
	}
	closedir(d);
}

static int apply_target(const struct fabric_ops *ops, const struct fab_target *t,
			const struct fab_conf *conf, char *errbuf, size_t errlen)
{
	char		 tpg[PATHLEN], p[PATHLEN], q[PATHLEN];
	DIR		*d;
	struct dirent	*e;
	int		 i, j;

	if (mkpath(tpg, "%s/%s/%s/" TPG, cfs_root, ops->name, t->wwn) ||
	    mkpath(p, "%s/%s/%s", cfs_root, ops->name, t->wwn) ||
	    cfs_mkdir(p) || cfs_mkdir(tpg)) {
		set_err(errbuf, errlen, "%s: cannot create target: %s",
			t->wwn, strerror(errno));
		return -1;
	}

	if (ops->apply_tpg && ops->apply_tpg(tpg, t)) {
		set_err(errbuf, errlen, "%s: cannot set TPG attributes: %s",
			t->wwn, strerror(errno));
		return -1;
	}

	for (i = 0; i < t->n_luns; i++) {
		const struct fab_lun *l = &t->luns[i];

		/*
		 * A LUN moving to another backstore (drive Slot values changed
		 * in device.conf): unlinking the TPG LUN removes it from every
		 * ACL in the kernel (core_clear_lun_from_tpg()), and relinking
		 * does not restore explicit ACLs. Drop their mapped LUNs so the
		 * ACL loop below creates them again.
		 */
		if (!mkpath(p, "%s/lun/lun_%u", tpg, l->lun) &&
		    links_elsewhere(p, l->backstore))
			drop_mapped_lun(tpg, l->lun);

		if (mkpath(p, "%s/lun/lun_%u", tpg, l->lun) || cfs_mkdir(p) ||
		    mkpath(q, "%s/core/user_0/%s", cfs_root, l->backstore) ||
		    ensure_link(p, l->backstore, q, l->backstore)) {
			set_err(errbuf, errlen, "%s: cannot map LUN %u to %s: %s",
				t->wwn, l->lun, l->backstore, strerror(errno));
			return -1;
		}
	}

	for (i = 0; i < t->n_initiators; i++) {
		char acl[PATHLEN];

		if (mkpath(acl, "%s/acls/%s", tpg, t->initiators[i]) ||
		    cfs_mkdir(acl)) {
			set_err(errbuf, errlen, "%s: cannot add ACL %s: %s",
				t->wwn, t->initiators[i], strerror(errno));
			return -1;
		}
		for (j = 0; j < t->n_luns; j++) {
			unsigned lun = t->luns[j].lun;
			char name[32];

			snprintf(name, sizeof(name), "lun_%u", lun);
			if (mkpath(p, "%s/lun_%u", acl, lun) || cfs_mkdir(p) ||
			    mkpath(q, "%s/lun/lun_%u", tpg, lun) ||
			    ensure_link(p, "lun", q, name)) {
				set_err(errbuf, errlen,
					"%s: cannot map LUN %u for %s: %s",
					t->wwn, lun, t->initiators[i],
					strerror(errno));
				return -1;
			}
		}
		prune_luns(acl, t);
		if (ops->apply_acl_auth && ops->apply_acl_auth(acl, t)) {
			set_err(errbuf, errlen, "%s: cannot set CHAP for %s: %s",
				t->wwn, t->initiators[i], strerror(errno));
			return -1;
		}
	}

	/* ACLs the configuration no longer lists */
	if (!mkpath(p, "%s/acls", tpg) && (d = opendir(p))) {
		while ((e = readdir(d))) {
			char acl[PATHLEN];
			int keep = 0;

			if (e->d_name[0] == '.')
				continue;
			for (i = 0; i < t->n_initiators; i++)
				keep |= !strcmp(e->d_name, t->initiators[i]);
			if (!keep && !mkpath(acl, "%s/acls/%s", tpg, e->d_name) &&
			    is_dir(acl))
				remove_acl(acl);
		}
		closedir(d);
	}
	if (!mkpath(p, "%s/lun", tpg))
		prune_luns(p, t);

	if (ops->apply_endpoints && ops->apply_endpoints(tpg, conf)) {
		set_err(errbuf, errlen, "%s: cannot add portal: %s",
			t->wwn, strerror(errno));
		return -1;
	}

	/* Kernels before 5.16 reject enabling an enabled iSCSI TPG */
	if (mkpath(p, "%s/enable", tpg) ||
	    ((cfs_read(p, q, sizeof(q)) || strcmp(q, "1")) &&
	     cfs_write(p, "1"))) {
		set_err(errbuf, errlen, "%s: cannot enable: %s",
			t->wwn, strerror(errno));
		return -1;
	}
	return 0;
}

int fab_apply(const struct fabric_ops *ops, const struct fab_conf *conf,
	      char *errbuf, size_t errlen)
{
	struct owned	o;
	char		p[PATHLEN];
	int		i;

	if (wait_backstores(conf, errbuf, errlen))
		return -1;

	if (mkpath(p, "%s/%s", cfs_root, ops->name) || cfs_mkdir(p)) {
		set_err(errbuf, errlen, "cannot create %s (is the %s fabric module available?): %s",
			p, ops->name, strerror(errno));
		return -1;
	}

	/* Targets created by an earlier apply whose record is gone */
	state_read(ops, &o);
	for (i = 0; i < o.n; i++)
		if (!conf_has_wwn(conf, o.wwn[i]))
			remove_target(ops, o.wwn[i]);

	/* Record ownership before creating anything, so remove finds a target
	 * even when apply fails half way */
	if (state_write(ops, conf, &o)) {
		set_err(errbuf, errlen, "cannot write %s: %s", fab_state_path,
			strerror(errno));
		return -1;
	}

	for (i = 0; i < conf->n_targets; i++)
		if (apply_target(ops, &conf->targets[i], conf, errbuf, errlen))
			return -1;
	return 0;
}

/* ---- status ---- */

static void json_str(FILE *out, const char *s)
{
	fputc('"', out);
	for (; *s; s++) {
		unsigned char c = *s;

		if (c == '"' || c == '\\')
			fprintf(out, "\\%c", c);
		else if (c < 0x20)
			fprintf(out, "\\u%04x", c);
		else
			fputc(c, out);
	}
	fputc('"', out);
}

static const char *json_bool(int b)
{
	return b ? "true" : "false";
}

static int acl_logged_in(const char *acl)
{
	char p[PATHLEN], info[4096];

	if (mkpath(p, "%s/info", acl) || cfs_read(p, info, sizeof(info)))
		return 0;
	return strstr(info, "SessionType: Normal") != NULL;
}

static void status_target(FILE *out, const struct fabric_ops *ops,
			  const struct fab_target *t)
{
	char	tpg[PATHLEN], p[PATHLEN], v[16];
	int	i, present, enabled = 0;

	if (mkpath(tpg, "%s/%s/%s/" TPG, cfs_root, ops->name, t->wwn))
		return;
	present = is_dir(tpg);
	if (present && !mkpath(p, "%s/enable", tpg) && !cfs_read(p, v, sizeof(v)))
		enabled = !strcmp(v, "1");

	fprintf(out, "{\"id\":%ld,\"wwn\":", t->id);
	json_str(out, t->wwn);
	fprintf(out, ",\"present\":%s,\"enabled\":%s,\"endpoints\":",
		json_bool(present), json_bool(enabled));
	if (present && ops->status_endpoints)
		ops->status_endpoints(out, tpg);
	else
		fputs("[]", out);

	fputs(",\"luns\":[", out);
	for (i = 0; i < t->n_luns; i++) {
		const struct fab_lun *l = &t->luns[i];
		char link[PATHLEN];
		struct stat st;

		fprintf(out, "%s{\"lun\":%u,\"backstore\":", i ? "," : "", l->lun);
		json_str(out, l->backstore);
		fprintf(out, ",\"backstore_ready\":%s,\"mapped\":%s}",
			json_bool(backstore_ready(l->backstore)),
			json_bool(!mkpath(link, "%s/lun/lun_%u/%s", tpg, l->lun,
					  l->backstore) &&
				  !lstat(link, &st) && S_ISLNK(st.st_mode)));
	}

	fputs("],\"initiators\":[", out);
	for (i = 0; i < t->n_initiators; i++) {
		char acl[PATHLEN];
		int acl_present = !mkpath(acl, "%s/acls/%s", tpg,
					  t->initiators[i]) && is_dir(acl);

		fprintf(out, "%s{\"name\":", i ? "," : "");
		json_str(out, t->initiators[i]);
		/* Report whether CHAP is configured, never the secret */
		fprintf(out, ",\"present\":%s,\"chap\":%s,\"mutual_chap\":%s,\"logged_in\":%s}",
			json_bool(acl_present), json_bool(t->auth.name[0]),
			json_bool(t->auth.mutual_name[0]),
			json_bool(acl_present && acl_logged_in(acl)));
	}
	fputs("]}", out);
}

void fab_status(FILE *out, const struct fabric_ops *ops,
		const struct fab_conf *conf)
{
	struct owned	o;
	int		i, n = 0;

	fprintf(out, "{\"fabric\":");
	json_str(out, ops->name);
	fputs(",\"targets\":[", out);
	for (i = 0; i < conf->n_targets; i++) {
		fputs(i ? "," : "", out);
		status_target(out, ops, &conf->targets[i]);
	}
	/* Owned targets no longer configured: the next apply removes them */
	fputs("],\"stale\":[", out);
	state_read(ops, &o);
	for (i = 0; i < o.n; i++) {
		if (conf_has_wwn(conf, o.wwn[i]))
			continue;
		fputs(n++ ? "," : "", out);
		json_str(out, o.wwn[i]);
	}
	fputs("]}\n", out);
}

/* ---- iSCSI ---- */

static int portal_name(const struct fab_portal *pt, char *buf, size_t len)
{
	int n = snprintf(buf, len, pt->ipv6 ? "[%s]:%d" : "%s:%d",
			 pt->addr, pt->port);

	return n < 0 || (size_t)n >= len ? -1 : 0;
}

static int iscsi_apply_endpoints(const char *tpg, const struct fab_conf *conf)
{
	struct fab_portal	 any = { "0.0.0.0", 3260, 0 };
	const struct fab_portal	*pts = conf->n_portals ? conf->portals : &any;
	int			 n = conf->n_portals ? conf->n_portals : 1;
	char			 np[PATHLEN], name[FAB_ADDR_MAX + 16];
	DIR			*d;
	struct dirent		*e;
	int			 i;

	for (i = 0; i < n; i++)
		if (portal_name(&pts[i], name, sizeof(name)) ||
		    mkpath(np, "%s/np/%s", tpg, name) || cfs_mkdir(np))
			return -1;

	if (mkpath(np, "%s/np", tpg) || !(d = opendir(np)))
		return 0;
	while ((e = readdir(d))) {
		int keep = 0;

		if (e->d_name[0] == '.')
			continue;
		for (i = 0; i < n; i++)
			keep |= !portal_name(&pts[i], name, sizeof(name)) &&
				!strcmp(name, e->d_name);
		if (!keep && !mkpath(np, "%s/np/%s", tpg, e->d_name))
			cfs_rmdir(np);
	}
	closedir(d);
	return 0;
}

static int iscsi_apply_tpg(const char *tpg, const struct fab_target *t)
{
	static const struct {
		const char *attr, *val;
	} attrs[] = {
		/* Explicit ACLs only: no demo mode, no dynamic ACLs */
		{ "attrib/generate_node_acls", "0" },
		{ "attrib/cache_dynamic_acls", "0" },
		{ "attrib/demo_mode_write_protect", "1" },
		/* Unlisted initiators don't see the target in SendTargets */
		{ "attrib/demo_mode_discovery", "0" },
	};
	char	p[PATHLEN];
	size_t	i;
	int	chap = t->auth.name[0] != '\0';

	for (i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++)
		if (mkpath(p, "%s/%s", tpg, attrs[i].attr) ||
		    cfs_write(p, attrs[i].val))
			return -1;
	if (mkpath(p, "%s/attrib/authentication", tpg) ||
	    cfs_write(p, chap ? "1" : "0"))
		return -1;
	/* AuthMethod, RFC 7143 section 12.1: offer only what the TPG enforces */
	if (mkpath(p, "%s/param/AuthMethod", tpg) ||
	    cfs_write(p, chap ? "CHAP" : "None"))
		return -1;
	return 0;
}

static int iscsi_apply_acl_auth(const char *acl, const struct fab_target *t)
{
	const struct {
		const char *attr, *val;
	} auth[] = {
		/* "NULL" clears a value in LIO */
		{ "auth/userid", t->auth.name[0] ? t->auth.name : "NULL" },
		{ "auth/password", t->auth.secret[0] ? t->auth.secret : "NULL" },
		{ "auth/userid_mutual",
		  t->auth.mutual_name[0] ? t->auth.mutual_name : "NULL" },
		{ "auth/password_mutual",
		  t->auth.mutual_secret[0] ? t->auth.mutual_secret : "NULL" },
	};
	char	p[PATHLEN];
	size_t	i;

	for (i = 0; i < sizeof(auth) / sizeof(auth[0]); i++)
		if (mkpath(p, "%s/%s", acl, auth[i].attr) ||
		    cfs_write(p, auth[i].val))
			return -1;
	return 0;
}

/* MTU of the interface that holds addr, or -1 (wildcard or not local) */
static int portal_mtu(const char *addr)
{
	struct ifaddrs	*ifa, *i;
	unsigned char	 want[sizeof(struct in6_addr)];
	int		 fam, mtu = -1;

	if (inet_pton(AF_INET, addr, want) == 1)
		fam = AF_INET;
	else if (inet_pton(AF_INET6, addr, want) == 1)
		fam = AF_INET6;
	else
		return -1;
	if (getifaddrs(&ifa))
		return -1;
	for (i = ifa; i; i = i->ifa_next) {
		const void	*a;
		char		 p[PATHLEN], v[16];

		if (!i->ifa_addr || i->ifa_addr->sa_family != fam)
			continue;
		a = fam == AF_INET ?
			(const void *)&((struct sockaddr_in *)i->ifa_addr)->sin_addr :
			(const void *)&((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr;
		if (memcmp(a, want, fam == AF_INET ? 4 : 16))
			continue;
		if (!mkpath(p, "/sys/class/net/%s/mtu", i->ifa_name) &&
		    !cfs_read(p, v, sizeof(v)))
			mtu = atoi(v);
		break;
	}
	freeifaddrs(ifa);
	return mtu;
}

static void iscsi_status_endpoints(FILE *out, const char *tpg)
{
	char		 np[PATHLEN];
	DIR		*d;
	struct dirent	*e;
	int		 n = 0;

	fputc('[', out);
	if (!mkpath(np, "%s/np", tpg) && (d = opendir(np))) {
		while ((e = readdir(d))) {
			char addr[FAB_ADDR_MAX + 16], *c;
			int mtu;

			if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(addr))
				continue;
			/* "a.b.c.d:port" or "[v6]:port" */
			strcpy(addr, e->d_name[0] == '[' ? e->d_name + 1 : e->d_name);
			c = strrchr(addr, e->d_name[0] == '[' ? ']' : ':');
			if (c)
				*c = '\0';
			mtu = portal_mtu(addr);

			fprintf(out, "%s{\"portal\":", n++ ? "," : "");
			json_str(out, e->d_name);
			/* MTU is reported, never set: docs/design 3.6 */
			if (mtu > 0)
				fprintf(out, ",\"mtu\":%d}", mtu);
			else
				fputs(",\"mtu\":null}", out);
		}
		closedir(d);
	}
	fputc(']', out);
}

const struct fabric_ops fabric_iscsi = {
	.name			= "iscsi",
	.apply_endpoints	= iscsi_apply_endpoints,
	.apply_tpg		= iscsi_apply_tpg,
	.apply_acl_auth		= iscsi_apply_acl_auth,
	.status_endpoints	= iscsi_status_endpoints,
};
