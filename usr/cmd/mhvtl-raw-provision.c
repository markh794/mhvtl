/*
 * mhvtl-raw-provision — static raw-disk bay provisioner (Phase 2, Stage A).
 *
 * A disk bay is a library Import/Export (MAP) slot bound to a stable device
 * node by the bay config (see etc/mhvtl_raw_bays.conf.sample). For each bay
 * that currently holds a formatted raw-disk cartridge, this tool:
 *
 *   1. probes the device for a valid MVTL superblock and reads its barcode
 *      (raw_probe);
 *   2. creates (or refreshes) a home_directory/<barcode> symlink to the STABLE
 *      by-path node, so load_tape() later resolves <barcode> -> S_ISBLK -> raw
 *      backend (design docs/design/raw-disk-backend.md §2);
 *   3. asks the library to place the cartridge into an empty MAP slot by sending
 *      "load map <barcode>" over the SysV message queue (same addressing as
 *      vtlcmd: receiver message-type == the library instance id).
 *
 * Two present bays whose disks report the same barcode are a collision: neither
 * is loaded. Absent/unreadable/unformatted bays are skipped, not errors.
 *
 * The probe/symlink/load-map logic lives in the reusable raw_bay_ops module
 * (compiled into libvtlscsi) so the Stage-B hot-plug daemon shares it. This tool
 * is now a thin driver: parse the bay config, place all bays, print a summary.
 *
 * Usage:
 *   mhvtl-raw-provision [--config <path>] [--verbose] [-h|--help]
 *
 * Default config: $MHVTL_CONFIG_PATH/mhvtl_raw_bays.conf (else /etc/mhvtl/...).
 * Exit status is 0 whenever the run completed (even if some bays were skipped);
 * non-zero only on a hard error such as a config parse failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "raw_bay_conf.h"
#include "raw_bay_ops.h"

/* Every mhvtl program that links libvtlscsi defines this (used by logging). */
char mhvtl_driver_name[] = "mhvtl-raw-provision";

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: mhvtl-raw-provision [--config <path>] [--verbose]\n"
		"\n"
		"  --config <path>  bay map config (default: "
		"$MHVTL_CONFIG_PATH/mhvtl_raw_bays.conf)\n"
		"  --verbose        log per-bay probe decisions to stderr\n"
		"  -h, --help       show this help\n");
}

int main(int argc, char **argv)
{
	const char		*config = NULL;
	char			 default_config[1024];
	struct raw_bay		*bays = NULL;
	struct raw_bay_result	*results = NULL;
	size_t			 count = 0;
	char			 errbuf[256];
	int			 opt_verbose = 0;

	static struct option opts[] = {
		{ "config",  required_argument, NULL, 'c' },
		{ "verbose", no_argument,       NULL, 'v' },
		{ "help",    no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int c;

	while ((c = getopt_long(argc, argv, "c:vh", opts, NULL)) != -1) {
		switch (c) {
		case 'c': config = optarg; break;
		case 'v': opt_verbose = 1; break;
		case 'h': usage(stdout); return 0;
		default:  usage(stderr); return 2;
		}
	}

	if (!config) {
		snprintf(default_config, sizeof(default_config),
			 "%s/mhvtl_raw_bays.conf", raw_bay_config_dir());
		config = default_config;
	}

	if (raw_bay_conf_parse(config, &bays, &count, errbuf, sizeof(errbuf)) < 0) {
		fprintf(stderr, "mhvtl-raw-provision: %s\n", errbuf);
		return 1;
	}

	if (count == 0) {
		printf("No raw-disk bays configured in %s\n", config);
		raw_bay_conf_free(bays);
		return 0;
	}

	results = calloc(count, sizeof(*results));
	if (!results) {
		fprintf(stderr, "mhvtl-raw-provision: out of memory\n");
		raw_bay_conf_free(bays);
		return 1;
	}

	if (raw_bay_place_all(bays, count, results, opt_verbose,
			      errbuf, sizeof(errbuf)) < 0) {
		fprintf(stderr, "mhvtl-raw-provision: %s\n", errbuf);
		free(results);
		raw_bay_conf_free(bays);
		return 1;
	}

	/* Summary. */
	for (size_t i = 0; i < count; i++) {
		if (results[i].placed)
			printf("PLACED %s in library %d MAP\n",
			       results[i].barcode, bays[i].library_id);
		else
			printf("SKIPPED %s: %s\n", bays[i].by_path,
			       results[i].skip_reason ? results[i].skip_reason
						      : "not provisioned");
	}

	free(results);
	raw_bay_conf_free(bays);
	return 0;
}
