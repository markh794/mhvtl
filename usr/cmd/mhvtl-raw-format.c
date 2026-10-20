/*
 * mhvtl-raw-format — format a block device as an mhvtl raw cartridge.
 *
 * Usage:
 *   mhvtl-raw-format <device> --barcode <id> [--density <density>]
 *                    [--partition-data <pct[,pct...]>] [--min-record-size <size>]
 *
 *   --density          media density written to the MAM, e.g. LTO8. Defaults
 *                      to the one the barcode's media suffix names (VOL001L8 is
 *                      LTO-8); required for a 6-character barcode.
 *   --partition-data   per-partition size percentages that total exactly 100
 *                      (default "100" = single partition). e.g. 92,8 or 85,5,5,5
 *   --min-record-size  index capacity-planning record size (default 64KiB).
 *                      Accepts a plain byte count or a K/M/G[i] suffix.
 *   --partitions N     optional; if given, must equal the --partition-data count.
 *
 * Requires write access to the device (run as root). One disk = one cartridge.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdint.h>
#include <errno.h>
#include <syslog.h>

#include "cart_backend_raw.h"

/* Every mhvtl program that links libvtlscsi defines this (used by logging). */
char mhvtl_driver_name[] = "mhvtl-raw-format";

static uint64_t parse_size(const char *s, int *ok)
{
	char *end = NULL;
	unsigned long long v = strtoull(s, &end, 10);

	*ok = 1;
	if (end == s) { *ok = 0; return 0; }
	/* optional unit suffix: K/M/G, optionally with 'i' and/or 'B' */
	if (*end) {
		uint64_t mul = 1;
		switch (*end) {
		case 'k': case 'K': mul = 1024ull; break;
		case 'm': case 'M': mul = 1024ull * 1024; break;
		case 'g': case 'G': mul = 1024ull * 1024 * 1024; break;
		default: *ok = 0; return 0;
		}
		end++;
		if (*end == 'i' || *end == 'I') end++;
		if (*end == 'b' || *end == 'B') end++;
		if (*end) { *ok = 0; return 0; }
		v *= mul;
	}
	return (uint64_t)v;
}

static int count_partspec(const char *spec)
{
	int n = 1;
	for (const char *p = spec; *p; p++)
		if (*p == ',') n++;
	return n;
}

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: mhvtl-raw-format <device> --barcode <id> [--density <density>]\n"
		"                       [--partition-data <pct[,pct...]>]\n"
		"                       [--min-record-size <size>] [--partitions N]\n");
}

int main(int argc, char **argv)
{
	const char *barcode = NULL;
	const char *density = NULL;
	const char *partspec = "100";
	uint64_t min_record = 64ull * 1024;
	long want_partitions = -1;

	static struct option opts[] = {
		{ "barcode",         required_argument, NULL, 'b' },
		{ "density",         required_argument, NULL, 'd' },
		{ "partition-data",  required_argument, NULL, 'p' },
		{ "min-record-size", required_argument, NULL, 'm' },
		{ "partitions",      required_argument, NULL, 'n' },
		{ "help",            no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int c, ok;

	/* raw_format() reports why it refused a device through syslog; copy
	 * those messages to stderr so the caller sees the reason as well. */
	openlog(mhvtl_driver_name, LOG_PERROR, LOG_DAEMON);

	while ((c = getopt_long(argc, argv, "b:d:p:m:n:h", opts, NULL)) != -1) {
		switch (c) {
		case 'b': barcode = optarg; break;
		case 'd': density = optarg; break;
		case 'p': partspec = optarg; break;
		case 'm':
			min_record = parse_size(optarg, &ok);
			if (!ok || min_record == 0) {
				fprintf(stderr, "invalid --min-record-size: %s\n", optarg);
				return 1;
			}
			break;
		case 'n': want_partitions = strtol(optarg, NULL, 10); break;
		case 'h': usage(stdout); return 0;
		default:  usage(stderr); return 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "error: missing <device>\n");
		usage(stderr);
		return 1;
	}
	const char *device = argv[optind];

	if (!barcode || !barcode[0]) {
		fprintf(stderr, "error: --barcode is required\n");
		return 1;
	}
	if (strlen(barcode) == 8 && !strcmp(barcode + 6, "M8")) {
		fprintf(stderr, "error: M8 (LTO-8 Type M) cartridges are not "
			"supported yet\n");
		return 1;
	}
	if (!density && strlen(barcode) == 6) {
		fprintf(stderr, "error: --density is required for a barcode "
			"without a media suffix (e.g. VOL001L8 is LTO-8)\n");
		return 1;
	}
	if (want_partitions >= 0 && want_partitions != count_partspec(partspec)) {
		fprintf(stderr, "error: --partitions %ld does not match "
			"--partition-data count %d\n",
			want_partitions, count_partspec(partspec));
		return 1;
	}

	if (raw_format(device, barcode, density, partspec, min_record) != 0) {
		fprintf(stderr, "error: failed to format %s: %s\n",
			device, strerror(errno));
		return 1;
	}

	printf("Formatted %s as raw cartridge '%s' (partitions: %s, "
	       "min record %llu bytes)\n",
	       device, barcode, partspec, (unsigned long long)min_record);
	return 0;
}
