/*
 * fabrics.conf and device.conf parsing for mhvtl-fabrics. See include/fabrics.h.
 *
 * fabrics.conf follows device.conf: a record starts in column 1, its fields
 * are indented "Name: value" lines, '#' starts a comment.
 *
 *   iSCSI:
 *    Portal: 192.0.2.10:3260
 *
 *   iSCSI target: 10
 *    Initiator name: iqn.2024-01.com.example:pbs1
 *    CHAP name: pbs1
 *    CHAP secret: 0123456789abcdef
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fabrics.h"

enum record { REC_NONE, REC_ISCSI, REC_ISCSI_TARGET };

static void set_err(char *errbuf, size_t errlen, int line, const char *fmt, ...)
{
	va_list	ap;
	int	n = 0;

	if (!errbuf || !errlen)
		return;
	if (line > 0)
		n = snprintf(errbuf, errlen, "line %d: ", line);
	if (n < 0 || (size_t)n >= errlen)
		return;
	va_start(ap, fmt);
	vsnprintf(errbuf + n, errlen - n, fmt, ap);
	va_end(ap);
}

static char *rtrim(char *s)
{
	char *end = s + strlen(s);

	while (end > s && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return s;
}

/*
 * Split "Name: value" into a lower-cased name with single spaces and the
 * trimmed value. A '#' after a blank starts a comment, as in device.conf, so
 * a value cannot contain " #". Returns 0, or -1 when there is no ':'.
 */
static int split_field(char *line, char *name, size_t nlen, char **value)
{
	char	*colon, *c, *v;
	size_t	 n = 0;
	int	 blank = 0;

	for (c = line; c[0] && c[1]; c++)
		if ((c[0] == ' ' || c[0] == '\t') && c[1] == '#') {
			c[0] = '\0';
			break;
		}

	colon = strchr(line, ':');
	if (!colon)
		return -1;

	for (c = line; c < colon && n + 1 < nlen; c++) {
		if (isspace((unsigned char)*c)) {
			blank = n > 0;
			continue;
		}
		if (blank && n + 2 < nlen)
			name[n++] = ' ';
		blank = 0;
		name[n++] = tolower((unsigned char)*c);
	}
	name[n] = '\0';

	v = colon + 1;
	while (isspace((unsigned char)*v))
		v++;
	*value = rtrim(v);
	return 0;
}

static int copy_value(char *dst, size_t len, const char *src, int line,
		      const char *what, char *errbuf, size_t errlen)
{
	if (strlen(src) >= len) {
		set_err(errbuf, errlen, line, "%s is longer than %zu bytes",
			what, len - 1);
		return -1;
	}
	strcpy(dst, src);
	return 0;
}

/*
 * An iSCSI name (RFC 7143 section 4.2.7) in the iqn., eui. or naa. format.
 * Names become configfs directory names, so only the characters RFC 3722
 * stringprep leaves in a name are accepted, and never '/'.
 */
int fab_valid_iscsi_name(const char *s)
{
	const char *c;

	if (strncmp(s, "iqn.", 4) && strncmp(s, "eui.", 4) &&
	    strncmp(s, "naa.", 4))
		return 0;
	if (strlen(s) > FAB_NAME_MAX - 1 || strlen(s) <= 4)
		return 0;
	for (c = s; *c; c++)
		if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) ||
		      *c == '.' || *c == '-' || *c == ':'))
			return 0;
	return 1;
}

/*
 * A CHAP name or secret is written to configfs as is; whitespace would not
 * survive the file format and a leading "NULL" clears the value in LIO.
 */
static int valid_chap(const char *s)
{
	const char *c;

	if (!*s || !strncmp(s, "NULL", 4))
		return 0;
	for (c = s; *c; c++)
		if (!isgraph((unsigned char)*c))
			return 0;
	return 1;
}

/* "192.0.2.10", "192.0.2.10:3260", "[2001:db8::1]:3260" or "[2001:db8::1]" */
static int parse_portal(const char *s, struct fab_portal *p)
{
	char		 buf[FAB_ADDR_MAX + 8], *port = NULL, *end;
	unsigned char	 a[sizeof(struct in6_addr)];
	long		 n;

	if (strlen(s) >= sizeof(buf))
		return -1;
	strcpy(buf, s);

	if (buf[0] == '[') {
		char *close = strchr(buf, ']');

		if (!close)
			return -1;
		*close = '\0';
		if (close[1] == ':')
			port = close + 2;
		else if (close[1])
			return -1;
		if (inet_pton(AF_INET6, buf + 1, a) != 1)
			return -1;
		strcpy(p->addr, buf + 1);
		p->ipv6 = 1;
	} else {
		port = strchr(buf, ':');
		if (port)
			*port++ = '\0';
		if (inet_pton(AF_INET, buf, a) != 1)
			return -1;
		strcpy(p->addr, buf);
		p->ipv6 = 0;
	}

	p->port = 3260;		/* RFC 7143 section 15: the IANA iSCSI port */
	if (port) {
		n = strtol(port, &end, 10);
		if (!*port || *end || n < 1 || n > 65535)
			return -1;
		p->port = (int)n;
	}
	return 0;
}

static int portal_any(const struct fab_portal *p)
{
	return !strcmp(p->addr, p->ipv6 ? "::" : "0.0.0.0");
}

/*
 * The kernel listens on each portal; a wildcard and a specific address of
 * the same family and port cannot both be bound.
 */
static int portal_conflict(const struct fab_conf *conf, const struct fab_portal *p)
{
	int i;

	for (i = 0; i < conf->n_portals; i++) {
		const struct fab_portal *q = &conf->portals[i];

		if (q->port != p->port || q->ipv6 != p->ipv6)
			continue;
		if (!strcmp(q->addr, p->addr) || portal_any(q) || portal_any(p))
			return 1;
	}
	return 0;
}

static int parse_id(const char *s, long *id)
{
	char *end;

	*id = strtol(s, &end, 10);
	return (!*s || *rtrim(end) || *id < 0) ? -1 : 0;
}

static int iscsi_field(struct fab_conf *conf, const char *name, char *value,
		       int line, char *errbuf, size_t errlen)
{
	if (!strcmp(name, "portal")) {
		struct fab_portal *p;

		if (conf->n_portals == FAB_MAX_PORTALS) {
			set_err(errbuf, errlen, line, "more than %d portals",
				FAB_MAX_PORTALS);
			return -1;
		}
		p = &conf->portals[conf->n_portals];
		if (parse_portal(value, p) < 0) {
			set_err(errbuf, errlen, line,
				"portal '%s' is not ADDRESS[:PORT] or [IPv6]:PORT",
				value);
			return -1;
		}
		if (portal_conflict(conf, p)) {
			set_err(errbuf, errlen, line,
				"portal '%s' repeats or overlaps another portal on port %d",
				value, p->port);
			return -1;
		}
		conf->n_portals++;
		return 0;
	}
	if (!strcmp(name, "base name")) {
		if (!fab_valid_iscsi_name(value)) {
			set_err(errbuf, errlen, line,
				"base name '%s' is not an iSCSI name", value);
			return -1;
		}
		return copy_value(conf->base_name, sizeof(conf->base_name),
				  value, line, "base name", errbuf, errlen);
	}
	set_err(errbuf, errlen, line, "unknown iSCSI field '%s'", name);
	return -1;
}

static int target_field(struct fab_conf *conf, struct fab_target *t,
			const char *name, char *value, int line,
			char *errbuf, size_t errlen)
{
	struct {
		const char	*name;
		char		*dst;
	} chap[] = {
		{ "chap name", t->auth.name },
		{ "chap secret", t->auth.secret },
		{ "mutual chap name", t->auth.mutual_name },
		{ "mutual chap secret", t->auth.mutual_secret },
	};
	size_t i;

	if (!strcmp(name, "initiator name")) {
		if (!fab_valid_iscsi_name(value)) {
			set_err(errbuf, errlen, line,
				"initiator name '%s' is not an iSCSI name (lower case iqn., eui. or naa.)",
				value);
			return -1;
		}
		for (i = 0; i < (size_t)t->n_initiators; i++)
			if (!strcmp(t->initiators[i], value)) {
				set_err(errbuf, errlen, line,
					"initiator %s is listed twice", value);
				return -1;
			}
		if (t->n_initiators == FAB_MAX_INITIATORS) {
			set_err(errbuf, errlen, line, "more than %d initiators",
				FAB_MAX_INITIATORS);
			return -1;
		}
		strcpy(t->initiators[t->n_initiators++], value);
		return 0;
	}
	if (!strcmp(name, "target name")) {
		if (!fab_valid_iscsi_name(value)) {
			set_err(errbuf, errlen, line,
				"target name '%s' is not an iSCSI name", value);
			return -1;
		}
		return copy_value(t->wwn, sizeof(t->wwn), value, line,
				  "target name", errbuf, errlen);
	}
	for (i = 0; i < sizeof(chap) / sizeof(chap[0]); i++) {
		if (strcmp(name, chap[i].name))
			continue;
		if (!valid_chap(value)) {
			set_err(errbuf, errlen, line,
				"%s must be printable, without blanks, and not start with NULL",
				chap[i].name);
			return -1;
		}
		if (strstr(chap[i].name, "secret"))
			conf->has_secret = 1;
		return copy_value(chap[i].dst, FAB_SECRET_MAX, value, line,
				  chap[i].name, errbuf, errlen);
	}
	set_err(errbuf, errlen, line, "unknown iSCSI target field '%s'", name);
	return -1;
}

static int check_target(const struct fab_target *t, char *errbuf, size_t errlen)
{
	const struct fab_auth *a = &t->auth;

	if (!!a->name[0] != !!a->secret[0]) {
		set_err(errbuf, errlen, t->line,
			"target %ld: CHAP name and CHAP secret go together", t->id);
		return -1;
	}
	if (!!a->mutual_name[0] != !!a->mutual_secret[0]) {
		set_err(errbuf, errlen, t->line,
			"target %ld: mutual CHAP name and secret go together", t->id);
		return -1;
	}
	if (a->mutual_name[0] && !a->name[0]) {
		set_err(errbuf, errlen, t->line,
			"target %ld: mutual CHAP needs CHAP", t->id);
		return -1;
	}
	/* RFC 7143 section 9.2.1: the two directions must use different secrets */
	if (a->mutual_secret[0] && !strcmp(a->secret, a->mutual_secret)) {
		set_err(errbuf, errlen, t->line,
			"target %ld: mutual CHAP secret must differ from the CHAP secret",
			t->id);
		return -1;
	}
	return 0;
}

int fab_conf_parse(FILE *fp, struct fab_conf *conf, char *errbuf, size_t errlen)
{
	char		 buf[1024], name[64], *value;
	enum record	 rec = REC_NONE;
	struct fab_target *t = NULL;
	int		 line = 0, i, j;

	memset(conf, 0, sizeof(*conf));

	while (fgets(buf, sizeof(buf), fp)) {
		char *p = buf;

		line++;
		if (!strchr(buf, '\n') && !feof(fp)) {
			set_err(errbuf, errlen, line, "line too long");
			return -1;
		}
		while (*p == ' ' || *p == '\t')
			p++;
		if (*rtrim(p) == '\0' || *p == '#')
			continue;

		if (split_field(p, name, sizeof(name), &value) < 0) {
			set_err(errbuf, errlen, line, "expected 'Name: value'");
			return -1;
		}

		if (p == buf) {		/* column 1: a new record */
			t = NULL;
			if (!strcmp(name, "version")) {
				if (strcmp(value, "1")) {
					set_err(errbuf, errlen, line,
						"unsupported VERSION %s", value);
					return -1;
				}
				rec = REC_NONE;
			} else if (!strcmp(name, "iscsi") && !*value) {
				if (conf->iscsi) {
					set_err(errbuf, errlen, line,
						"second iSCSI record");
					return -1;
				}
				conf->iscsi = 1;
				rec = REC_ISCSI;
			} else if (!strcmp(name, "iscsi target")) {
				long id;

				if (parse_id(value, &id) < 0) {
					set_err(errbuf, errlen, line,
						"iSCSI target needs a device.conf Library or Drive ID");
					return -1;
				}
				for (i = 0; i < conf->n_targets; i++)
					if (conf->targets[i].id == id) {
						set_err(errbuf, errlen, line,
							"second iSCSI target record for %ld", id);
						return -1;
					}
				if (conf->n_targets == FAB_MAX_TARGETS) {
					set_err(errbuf, errlen, line,
						"more than %d targets", FAB_MAX_TARGETS);
					return -1;
				}
				t = &conf->targets[conf->n_targets++];
				t->id = id;
				t->line = line;
				rec = REC_ISCSI_TARGET;
			} else {
				set_err(errbuf, errlen, line,
					"unknown record '%s'", name);
				return -1;
			}
			continue;
		}

		switch (rec) {
		case REC_ISCSI:
			if (iscsi_field(conf, name, value, line, errbuf, errlen))
				return -1;
			break;
		case REC_ISCSI_TARGET:
			if (!*value) {
				set_err(errbuf, errlen, line, "%s has no value", name);
				return -1;
			}
			if (target_field(conf, t, name, value, line, errbuf, errlen))
				return -1;
			break;
		default:
			set_err(errbuf, errlen, line,
				"field '%s' outside a record", name);
			return -1;
		}
	}

	for (i = 0; i < conf->n_targets; i++) {
		if (check_target(&conf->targets[i], errbuf, errlen))
			return -1;
		for (j = 0; j < i; j++)
			if (conf->targets[i].wwn[0] &&
			    !strcmp(conf->targets[i].wwn, conf->targets[j].wwn)) {
				set_err(errbuf, errlen, conf->targets[i].line,
					"target name %s is used twice",
					conf->targets[i].wwn);
				return -1;
			}
	}
	if (conf->n_targets && !conf->iscsi) {
		set_err(errbuf, errlen, 0, "iSCSI target records need an iSCSI record");
		return -1;
	}
	return 0;
}

/* ---- device.conf ---- */

int fab_topology_parse(FILE *fp, struct fab_topology *topo,
		       char *errbuf, size_t errlen)
{
	char		 buf[1024];
	struct fab_dev	*cur = NULL;
	int		 line = 0;

	memset(topo, 0, sizeof(*topo));

	while (fgets(buf, sizeof(buf), fp)) {
		long	id, lib;
		int	slot;

		line++;
		if (sscanf(buf, "Library: %ld", &id) == 1 ||
		    sscanf(buf, "Drive: %ld", &id) == 1) {
			if (topo->n == FAB_MAX_DEVS) {
				set_err(errbuf, errlen, line,
					"device.conf has more than %d devices",
					FAB_MAX_DEVS);
				return -1;
			}
			cur = &topo->devs[topo->n++];
			cur->id = id;
			cur->is_library = buf[0] == 'L';
			cur->lib_id = -1;
			cur->slot = 0;
		} else if (buf[0] != ' ' && buf[0] != '\t') {
			cur = NULL;
		} else if (cur && !cur->is_library &&
			   sscanf(buf, " Library ID: %ld Slot: %d", &lib, &slot) == 2) {
			cur->lib_id = lib;
			cur->slot = slot;
		}
	}
	return 0;
}

static const struct fab_dev *find_dev(const struct fab_topology *topo, long id)
{
	int i;

	for (i = 0; i < topo->n; i++)
		if (topo->devs[i].id == id)
			return &topo->devs[i];
	return NULL;
}

static int add_lun(struct fab_target *t, unsigned lun, const char *prefix,
		   long id, char *errbuf, size_t errlen)
{
	int i;

	for (i = 0; i < t->n_luns; i++)
		if (t->luns[i].lun == lun) {
			set_err(errbuf, errlen, 0,
				"target %ld: two devices on LUN %u (check the drive Slot values in device.conf)",
				t->id, lun);
			return -1;
		}
	if (t->n_luns == FAB_MAX_LUNS) {
		set_err(errbuf, errlen, 0, "target %ld: more than %d LUNs",
			t->id, FAB_MAX_LUNS);
		return -1;
	}
	t->luns[t->n_luns].lun = lun;
	snprintf(t->luns[t->n_luns].backstore, FAB_BS_MAX, "%s%ld", prefix, id);
	t->n_luns++;
	return 0;
}

int fab_plan(struct fab_conf *conf, const struct fab_topology *topo,
	     char *errbuf, size_t errlen)
{
	int	i, j, k;

	for (i = 0; i < conf->n_targets; i++) {
		struct fab_target	*t = &conf->targets[i];
		const struct fab_dev	*d = find_dev(topo, t->id);

		t->n_luns = 0;
		if (!d) {
			set_err(errbuf, errlen, t->line,
				"iSCSI target %ld: no Library or Drive %ld in device.conf",
				t->id, t->id);
			return -1;
		}
		if (!d->is_library && d->lib_id >= 0) {
			set_err(errbuf, errlen, t->line,
				"iSCSI target %ld: drive belongs to library %ld; export the library",
				t->id, d->lib_id);
			return -1;
		}
		if (!t->wwn[0] && !conf->base_name[0]) {
			set_err(errbuf, errlen, t->line,
				"iSCSI target %ld has no Target name and the iSCSI record no Base name",
				t->id);
			return -1;
		}
		if (!t->wwn[0]) {
			int n = snprintf(t->wwn, sizeof(t->wwn), "%s.%s%ld",
					 conf->base_name,
					 d->is_library ? "lib" : "tape", t->id);

			if (n < 0 || (size_t)n >= sizeof(t->wwn) ||
			    !fab_valid_iscsi_name(t->wwn)) {
				set_err(errbuf, errlen, t->line,
					"iSCSI target %ld: derived name is too long or invalid",
					t->id);
				return -1;
			}
		}

		for (k = 0; k < i; k++)
			if (!strcmp(conf->targets[k].wwn, t->wwn)) {
				set_err(errbuf, errlen, t->line,
					"target name %s is used twice", t->wwn);
				return -1;
			}

		if (!d->is_library) {
			if (add_lun(t, 0, "tape", d->id, errbuf, errlen))
				return -1;
			continue;
		}
		if (add_lun(t, 0, "lib", d->id, errbuf, errlen))
			return -1;
		for (j = 0; j < topo->n; j++) {
			const struct fab_dev *drv = &topo->devs[j];

			if (drv->is_library || drv->lib_id != d->id)
				continue;
			if (drv->slot < 1) {
				set_err(errbuf, errlen, 0,
					"drive %ld in library %ld has no Slot",
					drv->id, d->id);
				return -1;
			}
			if (add_lun(t, (unsigned)drv->slot, "tape", drv->id,
				    errbuf, errlen))
				return -1;
		}

	}
	return 0;
}
