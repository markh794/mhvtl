/*
 * test_compression_select — runtime write-time codec selection (US-C2).
 *
 * Drives the REAL selection path: it builds a tape LU with the Data
 * Compression (0x0F) and Device Configuration (0x10) mode pages, then calls
 * set_mode_compression() with a crafted MODE SELECT page selecting each codec
 * and asserts:
 *   (a) lu_priv->compressionType actually changes to the selected codec, and
 *   (b) the stored Data Compression page's Compression Algorithm field is
 *       updated so a subsequent MODE SENSE reports the codec back, and
 *   (c) for ZSTD the carried level (compressionFactor / device-config p[14])
 *       is clamped into 1-3.
 *
 * When a formatted block device + barcode are supplied (argv[1], argv[2];
 * driven by tests/test_raw_loopback.sh), it additionally proves the selected
 * codec is actually USED on the write path: it loads the raw cartridge, selects
 * LZ4 then ZSTD, writes a compressible block through write_tape_block() with the
 * selected comp_type, and asserts the stored block header flag shows
 * BLKHDR_FLG_LZ4_COMPRESSED (resp. ZSTD) — i.e. the chosen codec's flag appears.
 *
 * Usage: ./test_compression_select [block-device barcode]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "mhvtl_scsi.h"		/* SAM_STAT_GOOD, struct scsi_cmd */
#include "mhvtl_list.h"
#include "vtllib.h"			/* lunit, lookup_mode_pg, c_pos, mam, home_directory */
#include "vtlcart.h"		/* LZ4/ZSTD, MHVTL_COMP_ALGO_*, BLKHDR_FLG_*, tape ops */
#include "ssc.h"			/* struct priv_lu_ssc, ssc_personality_template */
#include "mode.h"			/* set_mode_compression, add_mode_* */
#include "be_byteshift.h"
#include "mhvtl_compress.h"

/* libvtlscsi expects each executable to supply this symbol. */
char mhvtl_driver_name[] = "test_compression_select";

static int fails;
#define CHECK(cond, msg) do { \
	if (cond) { printf("  ok   : %s\n", msg); } \
	else { printf("  FAIL : %s\n", msg); fails++; } \
} while (0)

/* The LU state the selection path operates on. */
static struct priv_lu_ssc		   priv;
static struct ssc_personality_template pm_tmpl;
static struct scsi_cmd			   cmd;

/* Build a tape LU with the two compression-related mode pages wired up, exactly
 * as the real personality setup does (add_mode_data_compression +
 * add_mode_device_configuration), so set_mode_compression() has real pages to
 * act on. */
static void setup_lu(void)
{
	memset(&lunit, 0, sizeof(lunit));
	INIT_LIST_HEAD(&lunit.mode_pg);
	INIT_LIST_HEAD(&lunit.log_pg);
	INIT_LIST_HEAD(&lunit.den_list);

	memset(&priv, 0, sizeof(priv));
	priv.configCompressionFactor = 9; /* legacy zlib level; ZSTD must clamp it */
	priv.compressionType	 = ZLIB;   /* a configured legacy codec to start */

	memset(&pm_tmpl, 0, sizeof(pm_tmpl));
	pm_tmpl.lu				= &lunit;
	pm_tmpl.set_compression	= set_compression_mode_pg;
	pm_tmpl.clear_compression = clear_compression_mode_pg;
	priv.pm = &pm_tmpl;

	lunit.lu_private = &priv;

	add_mode_data_compression(&lunit);	 /* page 0x0F */
	add_mode_device_configuration(&lunit); /* page 0x10; sets priv.compressionFactor */

	memset(&cmd, 0, sizeof(cmd));
	cmd.lu = &lunit;
}

/* Craft a MODE SELECT Data Compression page selecting `algo` with DCE+DDE set,
 * hand it to set_mode_compression(), and return the resulting codec. */
static uint8_t select_codec(uint32_t algo)
{
	uint8_t p[16];

	memset(p, 0, sizeof(p));
	p[0] = MODE_DATA_COMPRESSION; /* page code 0x0f */
	p[1] = 0x0e;				  /* page length */
	p[2] = 0x80;				  /* DCE: data compression enable */
	p[3] = 0x80;				  /* DDE: data decompression enable */
	put_unaligned_be32(algo, &p[4]);  /* compression algorithm */
	put_unaligned_be32(algo, &p[8]);  /* decompression algorithm */

	set_mode_compression(&cmd, p);
	return priv.compressionType;
}

/* The algorithm id the mode page should now report for a given codec. */
static uint32_t reported_algo(void)
{
	struct mode *m = lookup_mode_pg(&lunit.mode_pg, MODE_DATA_COMPRESSION, 0);
	return m ? get_unaligned_be32(&m->pcodePointer[4]) : 0xffffffff;
}

static void test_selection(void)
{
	printf("== selection path (set_mode_compression) ==\n");
	setup_lu();

	/* Select LZ4. */
	CHECK(select_codec(MHVTL_COMP_ALGO_LZ4) == LZ4,
	      "MODE SELECT 0x0F LZ4 -> compressionType == LZ4");
	CHECK(reported_algo() == MHVTL_COMP_ALGO_LZ4,
	      "MODE SENSE reports LZ4 algorithm id back");
	CHECK(*priv.compressionFactor != MHVTL_NO_COMPRESSION,
	      "LZ4: compression factor non-zero (write path compresses)");

	/* Select ZSTD — the carried level must clamp to 1-3. */
	CHECK(select_codec(MHVTL_COMP_ALGO_ZSTD) == ZSTD,
	      "MODE SELECT 0x0F ZSTD -> compressionType == ZSTD");
	CHECK(reported_algo() == MHVTL_COMP_ALGO_ZSTD,
	      "MODE SENSE reports ZSTD algorithm id back");
	CHECK(*priv.compressionFactor >= 1 && *priv.compressionFactor <= 3,
	      "ZSTD: carried level clamped into 1-3");

	/* A legacy / default algorithm id keeps the configured codec (here the
	 * ZSTD we just selected) rather than clobbering it. */
	CHECK(select_codec(MHVTL_COMP_ALGO_DEFAULT) == ZSTD,
	      "MODE SELECT 0x0F default algo keeps current codec");

	/* DCE clear disables compression (legacy behaviour preserved). */
	{
		uint8_t p[16];
		memset(p, 0, sizeof(p));
		p[0] = MODE_DATA_COMPRESSION;
		p[1] = 0x0e;
		p[2] = 0x00; /* DCE clear */
		set_mode_compression(&cmd, p);
		CHECK(*priv.compressionFactor == MHVTL_NO_COMPRESSION,
		      "DCE clear -> compression disabled (factor 0)");
	}
}

#define PAYLOAD_SZ 8192

/* Optional end-to-end: prove the SELECTED codec is actually used on the write
 * path, by writing a compressible block to a real raw cartridge and checking
 * the stored block header flag. */
static void test_write_uses_codec(const char *dev, const char *barcode)
{
	char	home[] = "/tmp/mhvtl-selhome-XXXXXX";
	char	link[512];
	uint8_t ss = SAM_STAT_GOOD;
	uint8_t payload[PAYLOAD_SZ];
	uint8_t dst[PAYLOAD_SZ * 2];
	int		rc;
	struct {
		uint32_t algo;
		uint8_t	 codec;
		uint8_t	 flag;
		const char *name;
	} cases[] = {
		{ MHVTL_COMP_ALGO_LZ4,  LZ4,  BLKHDR_FLG_LZ4_COMPRESSED,  "LZ4" },
		{ MHVTL_COMP_ALGO_ZSTD, ZSTD, BLKHDR_FLG_ZSTD_COMPRESSED, "ZSTD" },
	};

	printf("== write path uses selected codec (raw %s) ==\n", dev);

	if (!mkdtemp(home)) {
		CHECK(0, "mkdtemp");
		return;
	}
	strncpy(home_directory, home, HOME_DIR_PATH_SZ);
	home_directory[HOME_DIR_PATH_SZ] = '\0';
	snprintf(link, sizeof(link), "%s/%s", home, barcode);
	if (symlink(dev, link) < 0) {
		CHECK(0, "symlink barcode -> device");
		rmdir(home);
		return;
	}

	/* Low-entropy, highly compressible payload. */
	for (int i = 0; i < PAYLOAD_SZ; i++)
		payload[i] = (uint8_t)('A' + (i % 16 == 0 ? 1 : 0));

	rc = load_tape(barcode, &ss);
	CHECK(rc == 0 && ss == SAM_STAT_GOOD, "load_tape() loads the raw cartridge");

	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		char msg[128];
		int	 comp_len;

		/* Select the codec through the real MODE SELECT path. */
		snprintf(msg, sizeof(msg), "%s: select -> compressionType", cases[c].name);
		CHECK(select_codec(cases[c].algo) == cases[c].codec, msg);

		/* Compress with the chosen codec (as writeBlock_lz4/zstd do) and store
		 * at BOT with the selected comp_type. */
		comp_len = mhvtl_compress_block(cases[c].codec, payload, PAYLOAD_SZ,
						dst, sizeof(dst), 3);
		snprintf(msg, sizeof(msg), "%s: payload compressed (comp=%d)",
			 cases[c].name, comp_len);
		CHECK(comp_len > 0, msg);
		if (comp_len <= 0)
			continue;

		rewind_tape(&ss);
		rc = write_tape_block(dst, PAYLOAD_SZ, (uint32_t)comp_len, NULL,
				      priv.compressionType, 0, 0, &ss);
		snprintf(msg, sizeof(msg), "%s: write_tape_block ok", cases[c].name);
		CHECK(rc == 0 && ss == SAM_STAT_GOOD, msg);

		/* Reposition to the block so c_pos holds its stored header. */
		rewind_tape(&ss);
		snprintf(msg, sizeof(msg),
			 "%s: stored block header shows the codec's flag", cases[c].name);
		CHECK((c_pos->blk_flags & cases[c].flag) != 0, msg);
	}

	unload_tape(&ss);
	unlink(link);
	rmdir(home);
}

int main(int argc, char **argv)
{
	printf("test_compression_select\n");

	test_selection();

	if (argc >= 3)
		test_write_uses_codec(argv[1], argv[2]);
	else
		printf("  (write-path check skipped: no <device> <barcode> args)\n");

	printf("%s: %d check(s) failed\n", fails ? "FAILED" : "SUCCESS", fails);
	return fails ? 1 : 0;
}
