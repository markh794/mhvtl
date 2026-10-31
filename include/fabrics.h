/*
 * Fabric provisioner (mhvtl-fabrics): exports the TCMU backstores that
 * mhvtl_tcmu_handler creates to remote initiators through a LIO fabric.
 *
 * Three parts, each usable on its own so they can be unit tested:
 *   - fabrics.conf and device.conf parsing (usr/fabrics_conf.c)
 *   - the LIO configfs apply / remove / status engine (usr/fabrics_cfs.c)
 *   - one struct fabric_ops per LIO fabric; iSCSI is the first
 *
 * Every fabric links its LUNs to the handler's TCMU user backstores by name
 * (core/user_0/lib<ID>, core/user_0/tape<ID>), never to a pscsi backstore.
 */
#ifndef _FABRICS_H_
#define _FABRICS_H_

#include <stdio.h>
#include <stddef.h>

/* RFC 7143 section 4.2.7.1: an iSCSI name is at most 223 bytes */
#define FAB_NAME_MAX		224
/* MAX_USER_LEN and MAX_PASS_LEN in LIO's iscsi_target_core.h: 256 with NUL */
#define FAB_SECRET_MAX		256
#define FAB_ADDR_MAX		64
#define FAB_BS_MAX		32

#define FAB_MAX_PORTALS		16
#define FAB_MAX_INITIATORS	32
#define FAB_MAX_TARGETS		32
#define FAB_MAX_LUNS		64	/* MAX_DEVICES in the handler */
#define FAB_MAX_DEVS		256

/* ---- fabrics.conf ---- */

struct fab_portal {
	char	addr[FAB_ADDR_MAX];	/* numeric address, no brackets */
	int	port;
	int	ipv6;
};

struct fab_auth {
	char	name[FAB_SECRET_MAX];
	char	secret[FAB_SECRET_MAX];
	char	mutual_name[FAB_SECRET_MAX];
	char	mutual_secret[FAB_SECRET_MAX];
};

struct fab_lun {
	unsigned	lun;
	char		backstore[FAB_BS_MAX];	/* lib<ID> or tape<ID> */
};

struct fab_target {
	long		id;		/* device.conf Library or Drive ID */
	int		line;		/* record line in fabrics.conf */
	char		wwn[FAB_NAME_MAX];	/* empty until fab_plan() */
	int		n_initiators;
	char		initiators[FAB_MAX_INITIATORS][FAB_NAME_MAX];
	struct fab_auth	auth;
	/* Filled by fab_plan() from device.conf */
	int		n_luns;
	struct fab_lun	luns[FAB_MAX_LUNS];
};

struct fab_conf {
	int			iscsi;		/* an "iSCSI:" record was seen */
	char			base_name[FAB_NAME_MAX];	/* "" if not set */
	int			n_portals;
	struct fab_portal	portals[FAB_MAX_PORTALS];
	int			n_targets;
	struct fab_target	targets[FAB_MAX_TARGETS];
	int			has_secret;	/* any CHAP secret present */
};

/*
 * Parse fabrics.conf. Returns 0 on success, -1 with a message naming the line
 * in errbuf. Unknown records and fields are errors: a mistyped "Initiator
 * name" must not silently leave a library closed or, worse, open.
 */
int fab_conf_parse(FILE *fp, struct fab_conf *conf, char *errbuf, size_t errlen);

/* An iSCSI name in the iqn., eui. or naa. format, safe as a directory name */
int fab_valid_iscsi_name(const char *s);

/* ---- device.conf ---- */

struct fab_dev {
	long	id;
	int	is_library;
	long	lib_id;		/* drives only; -1 when standalone */
	int	slot;		/* drives only; drive slot in that library */
};

struct fab_topology {
	int		n;
	struct fab_dev	devs[FAB_MAX_DEVS];
};

int fab_topology_parse(FILE *fp, struct fab_topology *topo,
		       char *errbuf, size_t errlen);

/*
 * Resolve every target: its WWN (from "Target name:" or base name + id) and
 * its LUNs. A library gets the changer on LUN 0 and each drive on the LUN of
 * its drive slot, so LUN numbers depend on device.conf content, not on the
 * order daemons register or records appear. A standalone drive gets LUN 0.
 * There is no default base name: an IQN's naming authority must be a domain
 * its user owns (RFC 7143 section 4.2.7.4), which only the administrator
 * knows, so a target without "Target name:" needs "Base name:".
 */
int fab_plan(struct fab_conf *conf, const struct fab_topology *topo,
	     char *errbuf, size_t errlen);

/* ---- LIO configfs engine ---- */

/*
 * What differs between LIO fabrics (docs/design/iscsi-transport.md 3.1).
 * Everything else - LUNs, ACLs, mapped LUNs, waiting for backstores,
 * ownership and status - is shared.
 */
struct fabric_ops {
	const char	*name;		/* directory under target/, e.g. "iscsi" */
	/* Create configured endpoints under tpg and remove others */
	int		(*apply_endpoints)(const char *tpg,
					   const struct fab_conf *conf);
	/* TPG attributes and parameters, before the TPG is enabled */
	int		(*apply_tpg)(const char *tpg, const struct fab_target *t);
	/* Per-ACL authentication */
	int		(*apply_acl_auth)(const char *acl,
					  const struct fab_target *t);
	/* Write the endpoints of tpg as a JSON array value */
	void		(*status_endpoints)(FILE *out, const char *tpg);
};

extern const struct fabric_ops fabric_iscsi;

/*
 * configfs root, normally /sys/kernel/config/target. Tests point it at a
 * plain directory; cfs_fake then makes writes create files and rmdir remove
 * the attribute files configfs would remove by itself.
 */
extern char cfs_root[512];
extern int cfs_fake;

/*
 * Seconds fab_apply() waits for the handler to create the backstores; -1
 * (the default) means 3 s per LUN, at least 60 s.
 */
extern int fab_wait_seconds;

/* State file listing the targets this tool created, for remove */
extern char fab_state_path[512];

int fab_apply(const struct fabric_ops *ops, const struct fab_conf *conf,
	      char *errbuf, size_t errlen);
int fab_remove(const struct fabric_ops *ops, const struct fab_conf *conf);
void fab_status(FILE *out, const struct fabric_ops *ops,
		const struct fab_conf *conf);

#endif /* _FABRICS_H_ */
