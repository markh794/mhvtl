/*
 * AES-256-GCM drive encryption (markh794/mhvtl#304), through the real
 * vtltape on the userland transport and the file backend.
 *
 * Built without OpenSSL (no MHVTL_HAVE_OPENSSL) only the last test runs: an
 * 'Encryption: aes-gcm' drive must then offer no encryption at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#include "acutest.h"
#include "helpers/scsi_harness.h"
#include "helpers/device_conf_gen.h"
#include "vtlcart.h"
#include "mhvtl_crypto.h"

/* One record of indx.N (usr/vtlcart.c) */
struct indx_rec {
	int64_t			  data_offset;
	struct blk_header hdr;
	char			  pad[512 - sizeof(int64_t) - sizeof(struct blk_header)];
};

#define BLK_SZ	 4096
#define KEY_BASE 0x21

static struct harness_daemon	daemon_ctx;
static struct test_env			test_env;
static const struct drive_type *cur_drive;
static const char			   *cur_cart; /* barcode the cart_path() files belong to */
static char						conf_orig[8192];

static const struct drive_type *drive(const char *name) {
	for (const struct drive_type *dt = ALL_DRIVES; dt->name; dt++)
		if (strcmp(dt->name, name) == 0)
			return dt;
	return NULL;
}

/* device.conf as generated, with 'extra' lines added to the Drive stanza */
static int write_conf(const char *extra) {
	static const char anchor[] = " NAA: 50:11:22:33:44:55:66:01\n";
	char			  path[512];
	char			 *at = strstr(conf_orig, anchor);
	FILE			 *fp;

	if (!at)
		return -1;
	snprintf(path, sizeof(path), "%s/device.conf", test_env.conf_dir);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	at += sizeof(anchor) - 1;
	fprintf(fp, "%.*s%s%s", (int)(at - conf_orig), conf_orig, extra, at);
	return fclose(fp);
}

static int run_drive(const char *extra) {
	struct scsi_result r;
	uint8_t			   tur[6] = {0x00};

	if (write_conf(extra) < 0 ||
		harness_start_env(&daemon_ctx, TEST_MINOR_TAPE, "tape", &test_env) < 0)
		return -1;
	harness_clear_ua(&daemon_ctx);
	if (harness_load_tape(TEST_MINOR_TAPE, cur_drive->media_barcode) < 0)
		return -1;
	for (int i = 0; i < 100; i++) {
		harness_cdb(&daemon_ctx, tur, sizeof(tur), &r);
		if (r.sam_stat == SAM_STAT_GOOD)
			return 0;
		usleep(100000);
	}
	return -1;
}

static int start_drive(const char *name, const char *extra) {
	char  path[512];
	FILE *fp;
	size_t n;

	cur_drive = drive(name);
	if (cur_drive)
		cur_cart = cur_drive->media_barcode;
	if (!cur_drive || gen_test_env(&test_env, cur_drive) < 0)
		return -1;
	snprintf(path, sizeof(path), "%s/device.conf", test_env.conf_dir);
	fp = fopen(path, "r");
	if (!fp)
		return -1;
	n			   = fread(conf_orig, 1, sizeof(conf_orig) - 1, fp);
	conf_orig[n] = '\0';
	fclose(fp);
	return run_drive(extra);
}

static void stop_daemon(void) {
	harness_stop(&daemon_ctx);
}

static void stop_drive(void) {
	stop_daemon();
	gen_cleanup_env(&test_env);
}

/* SECURITY PROTOCOL OUT, Set Data Encryption page: 32-byte key, optional A-KAD */
static int set_encryption(uint8_t encrypt_mode, uint8_t decrypt_mode, uint8_t key_base,
						  const char *akad, struct scsi_result *r) {
	uint8_t cdb[12] = {0xb5, 0x20, 0x00, 0x10};
	uint8_t page[96];
	size_t	len = 52;

	memset(page, 0, sizeof(page));
	page[1] = 0x10;
	page[4] = 0x40; /* All I_T nexus */
	page[6] = encrypt_mode;
	page[7] = decrypt_mode;
	page[8] = 1;
	page[19] = 32;
	for (size_t i = 0; i < 32; i++)
		page[20 + i] = key_base + i;
	if (akad) {
		page[len] = 0x01; /* authenticated KAD */
		page[len + 3] = strlen(akad);
		memcpy(&page[len + 4], akad, strlen(akad));
		len += 4 + strlen(akad);
	}
	page[3] = len - 4;
	cdb[9]	= len;
	return harness_send_cdb_with_data_out(&daemon_ctx, cdb, sizeof(cdb), page, len, r);
}

#define CHECK_SENSE(r, key, asc, ascq, what)                                               \
	TEST_CHECK_((r).sam_stat == SAM_STAT_CHECK_CONDITION && ((r).sense[2] & 0x0f) == (key) && \
					(r).sense[12] == (asc) && (r).sense[13] == (ascq),                       \
				"%s: sam_stat %u sense %x %02x/%02x", what, (r).sam_stat, (r).sense[2] & 0x0f, \
				(r).sense[12], (r).sense[13])

#define CHECK_GOOD(r, what)                                                                   \
	TEST_CHECK_((r).sam_stat == SAM_STAT_GOOD, "%s: sam_stat %u sense %x %02x/%02x", what, \
				(r).sam_stat, (r).sense[2] & 0x0f, (r).sense[12], (r).sense[13])

#ifdef MHVTL_HAVE_OPENSSL

static const char marker[] = "MHVTL-GCM-PLAINTEXT-MARKER";

static void cart_path(char *buf, size_t sz, const char *file) {
	snprintf(buf, sz, "%s/%s/%s", test_env.home_dir, cur_cart, file);
}

/* Block n: the marker, the block number, then a pattern */
static void fill_block(uint8_t *b, size_t sz, unsigned n) {
	for (size_t i = 0; i < sz; i++)
		b[i] = (uint8_t)(i * 7 + n);
	memcpy(b, marker, sizeof(marker) - 1);
	b[sizeof(marker)] = (uint8_t)n;
}

static int write_blocks(unsigned first, unsigned count) {
	uint8_t			   b[BLK_SZ];
	struct scsi_result r;

	for (unsigned n = first; n < first + count; n++) {
		fill_block(b, sizeof(b), n);
		if (harness_tape_write(&daemon_ctx, b, sizeof(b), &r) || r.sam_stat != SAM_STAT_GOOD)
			return -1;
	}
	return 0;
}

/* Read the next block and compare it with block n */
static int read_block_matches(unsigned n, struct scsi_result *r) {
	uint8_t want[BLK_SZ], got[BLK_SZ];

	fill_block(want, sizeof(want), n);
	if (harness_tape_read(&daemon_ctx, got, sizeof(got), r) || r->sam_stat != SAM_STAT_GOOD)
		return 0;
	return !memcmp(want, got, sizeof(got));
}

static void rewind_cart(void) {
	struct scsi_result r;

	TEST_ASSERT(harness_tape_rewind(&daemon_ctx, &r) == 0 && r.sam_stat == SAM_STAT_GOOD);
}

static int read_file(const char *file, uint8_t **buf, size_t *len) {
	char  path[512];
	FILE *fp;
	long  sz;

	cart_path(path, sizeof(path), file);
	fp = fopen(path, "r");
	if (!fp)
		return -1;
	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	rewind(fp);
	*buf = malloc(sz + 1);
	*len = fread(*buf, 1, sz, fp);
	fclose(fp);
	return 0;
}

static int read_indx(unsigned n, struct indx_rec *rec) {
	char path[512];
	int	 fd, ok;

	cart_path(path, sizeof(path), "indx.0");
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ok = pread(fd, rec, sizeof(*rec), (off_t)n * sizeof(*rec)) == sizeof(*rec);
	close(fd);
	return ok ? 0 : -1;
}

static int write_at(const char *file, const void *p, size_t len, off_t off) {
	char path[512];
	int	 fd, ok;

	cart_path(path, sizeof(path), file);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	ok = pwrite(fd, p, len, off) == (ssize_t)len;
	close(fd);
	return ok ? 0 : -1;
}

static uint32_t tape_fmt_version(void) {
	uint8_t *b;
	size_t	 len;
	uint32_t v = 0;

	if (read_file("mam", &b, &len) == 0 && len >= 4)
		memcpy(&v, b, 4);
	free(b);
	return v;
}

static int contains(const uint8_t *hay, size_t len, const uint8_t *needle, size_t nlen) {
	for (size_t i = 0; i + nlen <= len; i++)
		if (!memcmp(hay + i, needle, nlen))
			return 1;
	return 0;
}

/* Write with every codec, read everything back */
static void round_trip(const char *name) {
	static const char *codecs[] = {"lzo", "zlib", "lz4", "zstd"};
	struct scsi_result r;
	struct indx_rec	   rec;
	unsigned		   n = 0;

	TEST_CASE(name);
	TEST_ASSERT(start_drive(name, " Compression: factor 1 enabled 1\n") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	CHECK_GOOD(r, "SPOUT");
	for (size_t c = 0; c < sizeof(codecs) / sizeof(codecs[0]); c++) {
		char cmd[32];

		/* 'compression' sends no reply */
		snprintf(cmd, sizeof(cmd), "compression %s", codecs[c]);
		TEST_ASSERT(harness_vtlcmd_no_response(TEST_MINOR_TAPE, cmd) == 0);
		usleep(300000);
		TEST_ASSERT(write_blocks(n, 2) == 0);
		n += 2;
	}
	rewind_cart();
	for (unsigned i = 0; i < n; i++) {
		TEST_CHECK_(read_block_matches(i, &r), "%s block %u sense %02x/%02x", name, i,
					r.sense[12], r.sense[13]);
		TEST_ASSERT(read_indx(i, &rec) == 0);
		TEST_CHECK_((rec.hdr.blk_flags & (BLKHDR_FLG_ENCRYPTED | BLKHDR_FLG_GCM)) ==
						(BLKHDR_FLG_ENCRYPTED | BLKHDR_FLG_GCM),
					"block %u flags 0x%x", i, rec.hdr.blk_flags);
	}
	/* zstd and lz4 shrink this pattern: compression happened before encryption */
	TEST_ASSERT(read_indx(n - 1, &rec) == 0);
	TEST_CHECK(rec.hdr.disk_blk_size < BLK_SZ);
	TEST_CHECK(tape_fmt_version() == TAPE_FMT_VERSION_GCM);
	stop_drive();
}

void test_round_trip_lto8(void) { round_trip("LTO-8"); }
void test_round_trip_3592_e07(void) { round_trip("3592-E07"); }
void test_round_trip_t10000c(void) { round_trip("T10000C"); }

/* Neither the key nor the plain text reaches the cartridge files */
void test_nothing_in_clear(void) {
	static const char *files[] = {"data.0", "indx.0", "meta.0", "mam"};
	uint8_t			   key[32];
	struct scsi_result r;

	TEST_ASSERT(start_drive("LTO-8", "") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, "AKAD-LABEL", &r) == 0);
	CHECK_GOOD(r, "SPOUT");
	TEST_ASSERT(write_blocks(0, 4) == 0);
	harness_unload_tape(TEST_MINOR_TAPE);
	for (int i = 0; i < 32; i++)
		key[i] = KEY_BASE + i;
	for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
		uint8_t *b = NULL;
		size_t	 len;

		if (read_file(files[f], &b, &len) < 0)
			continue;
		TEST_CHECK_(!contains(b, len, (const uint8_t *)marker, sizeof(marker) - 1),
					"plain text in %s", files[f]);
		TEST_CHECK_(!contains(b, len, key, 8), "key in %s", files[f]);
		free(b);
	}
	for (uint32_t i = 0; i < 4; i++) {
		struct indx_rec rec;

		TEST_ASSERT(read_indx(i, &rec) == 0);
		TEST_CHECK_(rec.hdr.blk_flags & BLKHDR_FLG_GCM, "block %u not GCM", i);
		TEST_CHECK_(!(rec.hdr.blk_flags & BLKHDR_FLG_CRC) && !rec.hdr.uncomp_crc,
					"block %u keeps the plain text CRC", i);
	}
	TEST_CHECK(tape_fmt_version() == TAPE_FMT_VERSION_GCM);
	stop_drive();
}

void test_wrong_and_no_key(void) {
	struct scsi_result r;
	uint8_t			   buf[BLK_SZ];

	TEST_ASSERT(start_drive("LTO-8", "") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_ASSERT(write_blocks(0, 1) == 0);

	TEST_ASSERT(set_encryption(0, 2, 0x80, NULL, &r) == 0);
	CHECK_GOOD(r, "SPOUT other key");
	rewind_cart();
	TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
	CHECK_SENSE(r, DATA_PROTECT, 0x74, 0x03, "wrong key");

	TEST_ASSERT(set_encryption(0, 0, KEY_BASE, NULL, &r) == 0);
	rewind_cart();
	TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
	CHECK_SENSE(r, DATA_PROTECT, 0x74, 0x01, "decryption off");

	/* The right key still works afterwards */
	TEST_ASSERT(set_encryption(0, 2, KEY_BASE, NULL, &r) == 0);
	rewind_cart();
	TEST_CHECK(read_block_matches(0, &r));
	stop_drive();
}

/* Write 3 blocks, apply 'tamper', and expect block 1 to fail with MEDIUM
 * ERROR while block 0 still reads */
static void tampered(const char *what, void (*tamper)(void)) {
	struct scsi_result r;
	uint8_t			   buf[BLK_SZ];

	TEST_CASE(what);
	TEST_ASSERT(start_drive("LTO-8", "") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, "AKAD-LABEL", &r) == 0);
	TEST_ASSERT(write_blocks(0, 3) == 0);
	tamper();
	rewind_cart();
	TEST_CHECK_(read_block_matches(0, &r), "%s: block 0", what);
	TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
	CHECK_SENSE(r, MEDIUM_ERROR, 0x11, 0x00, what);
	stop_drive();
}

static void rewrite_rec(unsigned n, void (*change)(struct indx_rec *)) {
	struct indx_rec rec;

	TEST_ASSERT(read_indx(n, &rec) == 0);
	change(&rec);
	TEST_ASSERT(write_at("indx.0", &rec, sizeof(rec), (off_t)n * sizeof(rec)) == 0);
}

static void flip_data(void) {
	struct indx_rec rec;
	uint8_t			b;
	char			path[512];
	int				fd;

	TEST_ASSERT(read_indx(1, &rec) == 0);
	cart_path(path, sizeof(path), "data.0");
	fd = open(path, O_RDWR);
	TEST_ASSERT(fd >= 0);
	TEST_ASSERT(pread(fd, &b, 1, rec.data_offset + 100) == 1);
	b ^= 0x01;
	TEST_ASSERT(pwrite(fd, &b, 1, rec.data_offset + 100) == 1);
	close(fd);
}
static void chg_akad(struct indx_rec *r) { r->hdr.blk_encryption_info.akad[0] ^= 1; }
static void chg_flags(struct indx_rec *r) { r->hdr.blk_flags ^= BLKHDR_FLG_CRC; }
static void chg_size(struct indx_rec *r) { r->hdr.blk_size -= 1; }
static void chg_tag(struct indx_rec *r) { r->hdr.blk_encryption_info.key[GCM_TAG_OFF] ^= 1; }
static void tamper_akad(void) { rewrite_rec(1, chg_akad); }
static void tamper_flags(void) { rewrite_rec(1, chg_flags); }
static void tamper_size(void) { rewrite_rec(1, chg_size); }
static void tamper_tag(void) { rewrite_rec(1, chg_tag); }

/* Block 2's header and data in block 1's place */
static void replay_block(void) {
	struct indx_rec rec;

	TEST_ASSERT(read_indx(2, &rec) == 0);
	rec.hdr.blk_number = 1;
	TEST_ASSERT(write_at("indx.0", &rec, sizeof(rec), sizeof(rec)) == 0);
}

/* Block 2's index record, unchanged, in block 1's place */
static void splice_block(void) {
	struct indx_rec rec;

	TEST_ASSERT(read_indx(2, &rec) == 0);
	TEST_ASSERT(write_at("indx.0", &rec, sizeof(rec), sizeof(rec)) == 0);
}

/* A key-in-header block on a format 7 cartridge: someone stripped GCM */
static void downgrade_block(void) {
	struct indx_rec rec;

	TEST_ASSERT(read_indx(1, &rec) == 0);
	rec.hdr.blk_flags &= ~BLKHDR_FLG_GCM;
	for (int i = 0; i < 32; i++)
		rec.hdr.blk_encryption_info.key[i] = KEY_BASE + i;
	TEST_ASSERT(write_at("indx.0", &rec, sizeof(rec), sizeof(rec)) == 0);
}

void test_tampered_data(void) { tampered("ciphertext", flip_data); }
void test_tampered_akad(void) { tampered("A-KAD", tamper_akad); }
void test_tampered_flags(void) { tampered("flags", tamper_flags); }
void test_tampered_size(void) { tampered("block size", tamper_size); }
void test_tampered_tag(void) { tampered("tag", tamper_tag); }
void test_replayed_block(void) { tampered("replayed block", replay_block); }
void test_spliced_block(void) { tampered("spliced block", splice_block); }
void test_downgraded_block(void) { tampered("key-in-header block on format 7", downgrade_block); }

/* Flagged AES-GCM but not encrypted, and longer on disk than the block: in
 * MIXED mode it passes the key checks, and must still not be decrypted into
 * a buffer sized for the block */
static void chg_gcm_plain(struct indx_rec *r) {
	r->hdr.blk_flags &= ~BLKHDR_FLG_ENCRYPTED;
	r->hdr.disk_blk_size = 1024 * 1024;
}

void test_gcm_not_encrypted(void) {
	uint8_t			   tur[6] = {0x00};
	struct scsi_result r;
	uint8_t			   buf[BLK_SZ];

	TEST_ASSERT(start_drive("LTO-8", "") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_ASSERT(write_blocks(0, 3) == 0);
	rewrite_rec(1, chg_gcm_plain);
	TEST_ASSERT(set_encryption(0, 3, KEY_BASE, NULL, &r) == 0);
	rewind_cart();
	TEST_CHECK(read_block_matches(0, &r));
	TEST_ASSERT(harness_tape_read(&daemon_ctx, buf, sizeof(buf), &r) == 0);
	TEST_CHECK_(r.sam_stat != SAM_STAT_GOOD, "block 1 read");
	harness_cdb(&daemon_ctx, tur, sizeof(tur), &r);
	CHECK_GOOD(r, "drive alive");
	stop_drive();
}

/* Nonces in indx.0 blocks [0, n) */
static int collect_nonces(uint8_t (*seen)[GCM_NONCE_LEN], int count, unsigned n) {
	struct indx_rec rec;

	for (unsigned i = 0; i < n; i++) {
		TEST_ASSERT(read_indx(i, &rec) == 0);
		memcpy(seen[count++], rec.hdr.blk_encryption_info.key + GCM_NONCE_OFF, GCM_NONCE_LEN);
	}
	return count;
}

/* No nonce is used twice: across an overwrite, an unload/reload and a
 * SIGKILL of the drive */
void test_nonce_unique(void) {
	uint8_t			   seen[64][GCM_NONCE_LEN];
	int				   count = 0;
	struct scsi_result r;

	TEST_ASSERT(start_drive("LTO-8", "") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_ASSERT(write_blocks(0, 4) == 0);
	count = collect_nonces(seen, count, 4);

	/* Overwrite from BOT */
	rewind_cart();
	TEST_ASSERT(write_blocks(0, 4) == 0);
	count = collect_nonces(seen, count, 4);

	/* Unload and reload */
	harness_unload_tape(TEST_MINOR_TAPE);
	TEST_ASSERT(harness_load_tape(TEST_MINOR_TAPE, cur_drive->media_barcode) >= 0);
	usleep(500000);
	harness_clear_ua(&daemon_ctx);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	rewind_cart();
	TEST_ASSERT(write_blocks(0, 4) == 0);
	count = collect_nonces(seen, count, 4);

	/* The drive dies without unloading, a new one takes the cartridge */
	kill(daemon_ctx.pid, SIGKILL);
	waitpid(daemon_ctx.pid, NULL, 0);
	daemon_ctx.pid = -1;
	stop_daemon();
	TEST_ASSERT(run_drive("") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	rewind_cart();
	TEST_ASSERT(write_blocks(0, 4) == 0);
	count = collect_nonces(seen, count, 4);

	for (int i = 0; i < count; i++)
		for (int j = i + 1; j < count; j++)
			TEST_CHECK_(memcmp(seen[i], seen[j], GCM_NONCE_LEN) != 0, "nonce %d == nonce %d", i, j);
	/* The new drive made a new reservation, with a new IV fixed field: a
	 * MAM put back to an older copy can't bring back used IVs */
	TEST_CHECK(memcmp(seen[0], seen[12], GCM_FIXED_LEN) != 0);
	rewind_cart();
	for (unsigned i = 0; i < 4; i++)
		TEST_CHECK(read_block_matches(i, &r));
	stop_drive();
}

/* A second cartridge, new, in the same drive after an AES-GCM one: it gets
 * its own salt and starts its own counter, it doesn't carry on the first's */
void test_salt_not_inherited(void) {
	static const char  second[] = "GCM002L8";
	uint8_t			   seen[8][GCM_NONCE_LEN];
	char			   mktape[256], cmd[1024];
	struct indx_rec	   rec;
	struct scsi_result r;
	uint8_t			   counter0[8] = {0};

	TEST_ASSERT(start_drive("LTO-8", "") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_ASSERT(write_blocks(0, 4) == 0);
	collect_nonces(seen, 0, 4);
	harness_unload_tape(TEST_MINOR_TAPE);

	TEST_ASSERT(gen_find_binary("mktape", mktape, sizeof(mktape)) == 0);
	snprintf(cmd, sizeof(cmd), "%s%s -C %s -H %s -l %d -m %s -s 500 -t data -d %s >/dev/null 2>&1",
			 gen_local_lib_prefix(mktape), mktape, test_env.conf_dir, test_env.home_dir,
			 TEST_MINOR_LIBRARY, second, cur_drive->density);
	TEST_ASSERT(system(cmd) == 0);
	TEST_ASSERT(harness_load_tape(TEST_MINOR_TAPE, second) >= 0);
	usleep(500000);
	harness_clear_ua(&daemon_ctx);
	cur_cart = second;
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_ASSERT(write_blocks(0, 1) == 0);
	TEST_ASSERT(read_indx(0, &rec) == 0);
	TEST_CHECK_(!memcmp(rec.hdr.blk_encryption_info.key + GCM_NONCE_OFF + GCM_FIXED_LEN,
						counter0, sizeof(counter0)),
				"the counter carried on from the first cartridge");
	TEST_CHECK(memcmp(rec.hdr.blk_encryption_info.key + GCM_NONCE_OFF, seen[0], GCM_FIXED_LEN) != 0);
	TEST_CHECK(tape_fmt_version() == TAPE_FMT_VERSION_GCM);
	harness_unload_tape(TEST_MINOR_TAPE);
	stop_drive();
}

/* What a release without AES-GCM leaves when it rewrites the MAM: no salt */
static int drop_salt(void) {
	uint8_t *b;
	size_t	 len, i = 0;
	int		 found = 0;

	if (read_file("mhvtl_data", &b, &len) < 0)
		return -1;
	while (i + 4 <= len) {
		uint16_t id, n;

		memcpy(&id, b + i, 2);
		memcpy(&n, b + i + 2, 2);
		if (id == 0x11 && i + 4 + n <= len) {
			memset(b + i + 4, 0, n);
			found = 1;
		}
		i += 4 + n;
	}
	found = found && write_at("mhvtl_data", b, len, 0) == 0;
	free(b);
	return found ? 0 : -1;
}

/* Without the salt the AES-GCM blocks can't be read nor appended to; from
 * the beginning of the tape the cartridge is written again */
void test_lost_salt(void) {
	uint8_t			   space_eod[6] = {0x11, 0x03};
	struct scsi_result r;
	uint8_t			   b[BLK_SZ];

	TEST_ASSERT(start_drive("LTO-8", "") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_ASSERT(write_blocks(0, 3) == 0);
	harness_unload_tape(TEST_MINOR_TAPE);
	TEST_ASSERT(drop_salt() == 0);
	TEST_ASSERT(harness_load_tape(TEST_MINOR_TAPE, cur_drive->media_barcode) >= 0);
	usleep(500000);
	harness_clear_ua(&daemon_ctx);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_ASSERT(harness_tape_read(&daemon_ctx, b, sizeof(b), &r) == 0);
	CHECK_SENSE(r, DATA_PROTECT, 0x74, 0x01, "read without salt");

	rewind_cart();
	harness_cdb(&daemon_ctx, space_eod, sizeof(space_eod), &r);
	CHECK_GOOD(r, "SPACE to EOD");
	fill_block(b, sizeof(b), 3);
	TEST_ASSERT(harness_tape_write(&daemon_ctx, b, sizeof(b), &r) == 0);
	CHECK_SENSE(r, MEDIUM_ERROR, 0x31, 0x00, "append without salt");

	rewind_cart();
	TEST_ASSERT(write_blocks(0, 2) == 0);
	rewind_cart();
	TEST_CHECK(read_block_matches(0, &r));
	TEST_CHECK(read_block_matches(1, &r));
	stop_drive();
}

/* A cartridge of a newer format than this release is not loaded */
void test_newer_format_refused(void) {
	struct scsi_result r;
	uint8_t			   tur[6] = {0x00};
	uint32_t		   fmt	  = TAPE_FMT_VERSION_GCM + 1;

	TEST_ASSERT(start_drive("LTO-8", "") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_ASSERT(write_blocks(0, 1) == 0);
	harness_unload_tape(TEST_MINOR_TAPE);
	TEST_ASSERT(write_at("mam", &fmt, sizeof(fmt), 0) == 0);
	harness_load_tape(TEST_MINOR_TAPE, cur_drive->media_barcode);
	usleep(500000);
	harness_clear_ua(&daemon_ctx);
	harness_cdb(&daemon_ctx, tur, sizeof(tur), &r);
	TEST_CHECK_(r.sam_stat != SAM_STAT_GOOD, "format %u cartridge loaded", fmt);
	stop_drive();
}

/* Simulated cartridges keep format 6 and stay readable; an aes-gcm drive
 * won't append to one, and a simulated drive won't write keys to format 7 */
void test_simulated_and_format(void) {
	struct scsi_result r;
	struct indx_rec	   rec;
	uint8_t			   b[BLK_SZ];

	TEST_ASSERT(start_drive("LTO-8", " Encryption: simulated\n") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_ASSERT(write_blocks(0, 2) == 0);
	TEST_ASSERT(read_indx(0, &rec) == 0);
	TEST_CHECK((rec.hdr.blk_flags & (BLKHDR_FLG_ENCRYPTED | BLKHDR_FLG_GCM)) == BLKHDR_FLG_ENCRYPTED);
	TEST_CHECK(rec.hdr.blk_encryption_info.key[0] == KEY_BASE);
	harness_unload_tape(TEST_MINOR_TAPE);
	TEST_CHECK(tape_fmt_version() == TAPE_FMT_VERSION);
	stop_daemon();

	/* aes-gcm drive: reads the simulated blocks, refuses to append */
	TEST_ASSERT(run_drive("") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_CHECK(read_block_matches(0, &r));
	TEST_CHECK(read_block_matches(1, &r));
	fill_block(b, sizeof(b), 2);
	TEST_ASSERT(harness_tape_write(&daemon_ctx, b, sizeof(b), &r) == 0);
	CHECK_SENSE(r, DATA_PROTECT, 0x27, 0x00, "append to simulated");
	TEST_CHECK(tape_fmt_version() == TAPE_FMT_VERSION);

	/* From BOT it may: the simulated blocks are gone */
	rewind_cart();
	TEST_ASSERT(write_blocks(0, 2) == 0);
	TEST_CHECK(tape_fmt_version() == TAPE_FMT_VERSION_GCM);
	harness_unload_tape(TEST_MINOR_TAPE);
	stop_daemon();

	/* Simulated drive: reads AES-GCM, refuses encrypted writes */
	TEST_ASSERT(run_drive(" Encryption: simulated\n") == 0);
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	TEST_CHECK(read_block_matches(0, &r));
	TEST_CHECK(read_block_matches(1, &r));
	fill_block(b, sizeof(b), 2);
	TEST_ASSERT(harness_tape_write(&daemon_ctx, b, sizeof(b), &r) == 0);
	CHECK_SENSE(r, DATA_PROTECT, 0x27, 0x00, "simulated write to format 7");

	/* Plain blocks are fine on format 7 */
	TEST_ASSERT(set_encryption(0, 3, KEY_BASE, NULL, &r) == 0);
	TEST_ASSERT(harness_tape_write(&daemon_ctx, b, sizeof(b), &r) == 0);
	CHECK_GOOD(r, "plain write to format 7");
	stop_drive();
}

#else /* !MHVTL_HAVE_OPENSSL */

/* Built without OpenSSL: an aes-gcm drive offers no encryption */
void test_no_openssl_no_encryption(void) {
	uint8_t			   spin[12] = {0xa2, 0x00, 0x00, 0x00, 0, 0, 0x00, 0x00, 0x02, 0x00};
	struct scsi_result r;
	uint8_t			   mode_sense[6] = {0x1a, 0x08, 0x24, 0x00, 0xff, 0x00};
	uint8_t			   encr_mode[6]	 = {0x1a, 0x08, 0x30, 0x20, 0xff, 0x00};

	TEST_ASSERT(start_drive("LTO-8", " Encryption: aes-gcm\n") == 0);
	harness_cdb(&daemon_ctx, spin, sizeof(spin), &r);
	CHECK_SENSE(r, ILLEGAL_REQUEST, 0x20, 0x00, "SPIN");
	TEST_ASSERT(set_encryption(2, 2, KEY_BASE, NULL, &r) == 0);
	CHECK_SENSE(r, ILLEGAL_REQUEST, 0x20, 0x00, "SPOUT");
	harness_cdb(&daemon_ctx, mode_sense, sizeof(mode_sense), &r);
	CHECK_GOOD(r, "MODE SENSE 24h");
	TEST_CHECK_(!(harness_data_buf[4 + 7] & 0x01), "ENCR_C set");
	harness_cdb(&daemon_ctx, encr_mode, sizeof(encr_mode), &r);
	CHECK_SENSE(r, ILLEGAL_REQUEST, 0x24, 0x00, "MODE SENSE 30h/20h");
	stop_drive();
}

#endif

TEST_LIST = {
#ifdef MHVTL_HAVE_OPENSSL
	{"round_trip_lto8", test_round_trip_lto8},
	{"round_trip_3592_e07", test_round_trip_3592_e07},
	{"round_trip_t10000c", test_round_trip_t10000c},
	{"nothing_in_clear", test_nothing_in_clear},
	{"wrong_and_no_key", test_wrong_and_no_key},
	{"tampered_data", test_tampered_data},
	{"tampered_akad", test_tampered_akad},
	{"tampered_flags", test_tampered_flags},
	{"tampered_size", test_tampered_size},
	{"tampered_tag", test_tampered_tag},
	{"replayed_block", test_replayed_block},
	{"spliced_block", test_spliced_block},
	{"downgraded_block", test_downgraded_block},
	{"gcm_not_encrypted", test_gcm_not_encrypted},
	{"nonce_unique", test_nonce_unique},
	{"salt_not_inherited", test_salt_not_inherited},
	{"lost_salt", test_lost_salt},
	{"newer_format_refused", test_newer_format_refused},
	{"simulated_and_format", test_simulated_and_format},
#else
	{"no_openssl_no_encryption", test_no_openssl_no_encryption},
#endif
	{NULL, NULL}};
