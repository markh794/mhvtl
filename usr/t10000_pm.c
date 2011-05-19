/*
 * This handles any SCSI OP codes defined in the standards as 'STREAM'
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
 *                                mark_harvey at symantec dot com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * See comments in vtltape.c for a more complete version release...
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <syslog.h>
#include <ctype.h>
#include <inttypes.h>
#include <assert.h>
#include <signal.h>
#include "be_byteshift.h"
#include "scsi.h"
#include "list.h"
#include "vtl_common.h"
#include "vtllib.h"
#include "ssc.h"
#include "spc.h"
#include "vtltape.h"
#include "q.h"

/*
 * Mode Pages defined for 'default'

 *** Minimum default requirements is **
 static struct mode sm[] = {
	{0x01, 0x00, 0x00, NULL, }, * RW error recovery - SSC3-8.3.5 *
	{0x02, 0x00, 0x00, NULL, }, * Disconnect Reconnect - SPC3 *
	{0x0a, 0x00, 0x00, NULL, }, * Control Extension - SPC3 *
	{0x0f, 0x00, 0x00, NULL, }, * Data Compression - SSC3-8.3.3
	{0x10, 0x00, 0x00, NULL, }, * Device config - SSC3-8.3.3
	{0x11, 0x00, 0x00, NULL, }, * Medium Partition - SSC3-8.3.4
	{0x1a, 0x00, 0x00, NULL, }, * Power condition - SPC3
	{0x1c, 0x00, 0x00, NULL, }, * Information Exception Ctrl SSC3-8.3.6
	{0x1d, 0x00, 0x00, NULL, }, * Medium configuration - SSC3-8.3.7
	{0x00, 0x00, 0x00, NULL, }, * NULL terminator
	};
 */

static struct mode sm[] = {
/*	Page,  subpage, len, 'pointer to data struct' */
	{0x01, 0x00, 0x00, NULL, }, /* RW error recovery - SSC3-8.3.5 */
	{0x02, 0x00, 0x00, NULL, }, /* Disconnect Reconnect - SPC3 */
	{0x0a, 0x00, 0x00, NULL, }, /* Control Extension - SPC3 */
	{0x0f, 0x00, 0x00, NULL, }, /* Data Compression - SSC3-8.3.3 */
	{0x10, 0x00, 0x00, NULL, }, /* Device config - SSC3-8.3.3 */
	{0x11, 0x00, 0x00, NULL, }, /* Medium Partition - SSC3-8.3.4 */
	{0x1a, 0x00, 0x00, NULL, }, /* Power condition - SPC3 */
	{0x1c, 0x00, 0x00, NULL, }, /* Information Exception Ctrl SSC3-8.3.6 */
	{0x1d, 0x00, 0x00, NULL, }, /* Medium configuration - SSC3-8.3.7 */
	{0x00, 0x00, 0x00, NULL, }, /* NULL terminator */
	};

static struct media_handling t10kA_media_handling[] = {
	{ "T10KA", "RW", Media_T10KA, },
	{ "T10KA Cleaning", "RO", Media_T10KA_CLEAN, },
	{ "T10KB", "RW", Media_T10KB, },
	{ "T10KB Cleaning", "RO", Media_T10KB_CLEAN, },
	{ "T10KC", "RW", Media_T10KC, },
	{ "T10KC Cleaning", "RO", Media_T10KC_CLEAN, },
	};

static struct media_handling t10kB_media_handling[] = {
	{ "T10KA", "RW", Media_T10KA, },
	{ "T10KA Cleaning", "RO", Media_T10KA_CLEAN, },
	{ "T10KB", "RW", Media_T10KB, },
	{ "T10KB Cleaning", "RO", Media_T10KB_CLEAN, },
	{ "T10KC", "RW", Media_T10KC, },
	{ "T10KC Cleaning", "RO", Media_T10KC_CLEAN, },
	};

static struct media_handling t10kC_media_handling[] = {
	{ "T10KA", "RW", Media_T10KA, },
	{ "T10KA Cleaning", "RO", Media_T10KA_CLEAN, },
	{ "T10KB", "RW", Media_T10KB, },
	{ "T10KB Cleaning", "RO", Media_T10KB_CLEAN, },
	{ "T10KC", "RW", Media_T10KC, },
	{ "T10KC Cleaning", "RO", Media_T10KC_CLEAN, },
	};

/*
 * Returns true if blk header has correct encryption key data
 */
#define	UKAD_LENGTH	encr->ukad_length
#define	AKAD_LENGTH	encr->akad_length
#define	KEY_LENGTH	encr->key_length
#define	UKAD		encr->ukad
#define	AKAD		encr->akad
#define	KEY		encr->key
uint8_t valid_encryption_blk_t10k(struct scsi_cmd *cmd)
{
	uint8_t correct_key;
	int i;
	struct lu_phy_attr *lu = cmd->lu;
	struct priv_lu_ssc *lu_priv;
	struct encryption *encr;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(3, "+++ Trace +++");

	lu_priv = lu->lu_private;
	encr = lu_priv->encr;

	/* decryption logic */
	correct_key = TRUE;
	if (c_pos->blk_flags & BLKHDR_FLG_ENCRYPTED) {
		/* compare the keys  - STK requires UKAD back to decrypt */
		if (lu_priv->DECRYPT_MODE > 1) {
			if (c_pos->encryption.key_length != KEY_LENGTH) {
				mkSenseBuf(DATA_PROTECT,
							E_INCORRECT_KEY,
							sam_stat);
				correct_key = FALSE;
				return correct_key;
			}
			for (i = 0; i < c_pos->encryption.key_length; ++i) {
				if (c_pos->encryption.key[i] != KEY[i]) {
					mkSenseBuf(DATA_PROTECT,
							E_INCORRECT_KEY,
							sam_stat);
					correct_key = FALSE;
					break;
				}
			}
			if (c_pos->encryption.ukad_length != UKAD_LENGTH) {
				mkSenseBuf(DATA_PROTECT,
							E_INCORRECT_KEY,
							sam_stat);
				correct_key = FALSE;
				return correct_key;
			}
			for (i = 0; i < c_pos->encryption.ukad_length; ++i) {
				if (c_pos->encryption.ukad[i] != UKAD[i]) {
					mkSenseBuf(DATA_PROTECT,
							E_INCORRECT_KEY,
							sam_stat);
					correct_key = FALSE;
					break;
				}
			}
		} else {
			mkSenseBuf(DATA_PROTECT, E_UNABLE_TO_DECRYPT, sam_stat);
			correct_key = FALSE;
		}
	} else if (lu_priv->DECRYPT_MODE == 2) {
		mkSenseBuf(DATA_PROTECT, E_UNENCRYPTED_DATA, sam_stat);
		correct_key = FALSE;
	}
	return correct_key;
}

static uint8_t clear_t10k_comp(void)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default clear_compression is in libvtlscsi */
	return clear_compression_mode_pg(sm);
}

static uint8_t set_t10k_comp(int lvl)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default set_compression is in libvtlscsi */
	return set_compression_mode_pg(sm, lvl);
}

static uint8_t update_t10k_encryption_mode(void *p, int value)
{
	MHVTL_DBG(3, "+++ Trace +++");

	return SAM_STAT_GOOD;
}

static uint8_t set_t10k_WORM(void)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", sm);
	return set_WORM(sm);
}

static uint8_t clear_t10k_WORM(void)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", sm);
	return clear_WORM(sm);
}

static int encr_capabilities_t10k(struct scsi_cmd *cmd)
{
	uint8_t *buf = cmd->dbuf_p->data;
	struct priv_lu_ssc *lu_priv = cmd->lu->lu_private;

	MHVTL_DBG(3, "+++ Trace +++");

	put_unaligned_be16(ENCR_CAPABILITIES, &buf[0]);
	put_unaligned_be16(40, &buf[2]); /* List length */

	buf[20] = 1;	/* Algorithm index */
	buf[21] = 0;	/* Reserved */
	put_unaligned_be16(0x14, &buf[22]); /* Descriptor length */
	buf[24] = 0x3a;	/* MAC C/DED_C DECRYPT_C = 2 ENCRYPT_C = 2 */
	buf[25] = 0x10;	/* NONCE_C = 1 */
	/* Max unauthenticated key data */
	put_unaligned_be16(0x20, &buf[26]);
	/* Max authenticated  key data */
	put_unaligned_be16(0x0c, &buf[28]);
	/* Key size */
	put_unaligned_be16(0x20, &buf[30]);
	buf[32] = 0x01;	/* EAREM */
	/* buf 12 - 19 reserved */

	buf[40] = 0;	/* Encryption Algorithm Id */
	buf[41] = 0x01;	/* Encryption Algorithm Id */
	buf[42] = 0;	/* Encryption Algorithm Id */
	buf[43] = 0x14;	/* Encryption Algorithm Id */

	buf[4] = 0x1; /* CFG_P == 01b */
	if (lu_priv->tapeLoaded == TAPE_LOADED) {
		buf[24] |= 0x80; /* AVFMV */
		buf[27] = 0x1e; /* Max unauthenticated key data */
		buf[29] = 0x00; /* Max authenticated key data */
		buf[32] |= 0x42; /* DKAD_C == 1, RDMC_C == 1 */
		buf[40] = 0x80; /* Encryption Algorithm Id */
		buf[43] = 0x10; /* Encryption Algorithm Id */
	}

	return 44;
}

static int t10k_kad_validation(int encrypt_mode, int ukad, int akad)
{
	if (ukad > 30 || akad > 0)
		return TRUE;
	return FALSE;

}

/* Some comments before I forget how this is supose to work..
 - cleaning_media_state is either
   0 - Not mounted
   1 - Cleaning media mounted -> return Cleaning cartridge installed
   2 - Cleaning media mounted -> return Cause not reportable
   3 - Cleaning media mounted -> return Initializing command required

 On cleaning media mount, t10k_cleaning() is called which:
   Sets a pointer from priv_lu_ssc -> cleaning_media_state.
   Sets cleaning_media_state to 1.
   Sets a 30 second timer to call inc_cleaning_state()

 inc_cleaning_state()
   Increments cleaning_media_state.
   If cleaning media_state == 2, set another timer for 90 seconds to again
   call inc_cleaning_state.

 If the application issues a TUR, ssc_tur() will return one of the
 above status codes depending on the current value of cleaning_media_state.

 When the cleaning media is unmounted, the pointer in priv_lu_ssc to this
 var will be re-set to NULL so the ssc_tur() will return defautl value.

 */
static volatile sig_atomic_t cleaning_media_state;

static void inc_cleaning_state(int sig);

static void set_cleaning_timer(int t)
{
	MHVTL_DBG(3, "+++ Trace +++ Setting alarm for %d", t);
	signal(SIGALRM, inc_cleaning_state);
	alarm(t);
}

static void inc_cleaning_state(int sig)
{
	MHVTL_DBG(3, "+++ Trace +++");
	signal(sig, inc_cleaning_state);

	cleaning_media_state++;

	if (cleaning_media_state == CLEAN_MOUNT_STAGE2)
		set_cleaning_timer(90);
}

static uint8_t t10k_media_load(int load)
{
	MHVTL_DBG(3, "+++ Trace +++ %s", (load) ? "load" : "unload");
	return 0;
}

static uint8_t t10k_cleaning(void *ssc_priv)
{
	struct priv_lu_ssc *ssc;

	MHVTL_DBG(3, "+++ Trace +++");

	ssc = ssc_priv;

	ssc->cleaning_media_state = &cleaning_media_state;
	cleaning_media_state = CLEAN_MOUNT_STAGE1;

	set_cleaning_timer(30);

	return 0;
}

static char *pm_name_t10kA = "T10000A";
static char *pm_name_t10kB = "T10000B";
static char *pm_name_t10kC = "T10000C";

static struct ssc_personality_template ssc_pm = {
	.valid_encryption_blk	= valid_encryption_blk_t10k,
	.update_encryption_mode	= update_t10k_encryption_mode,
	.encryption_capabilities = encr_capabilities_t10k,
	.kad_validation		= t10k_kad_validation,
	.check_restrictions	= check_restrictions,
	.clear_compression	= clear_t10k_comp,
	.set_compression	= set_t10k_comp,
	.clear_WORM		= clear_t10k_WORM,
	.set_WORM		= set_t10k_WORM,
	.media_load		= t10k_media_load,
	.cleaning_media		= t10k_cleaning,
};

static void init_t10k_inquiry(struct lu_phy_attr *lu)
{
	int pg;
	uint8_t worm = 1;	/* Supports WORM */

	/* Sequential Access device capabilities - Ref: 8.4.2 */
	pg = 0xb0 & 0x7f;
	lu->lu_vpd[pg] = alloc_vpd(VPD_B0_SZ);
	lu->lu_vpd[pg]->vpd_update = update_vpd_b0;
	lu->lu_vpd[pg]->vpd_update(lu, &worm);
}

#define INQUIRY_LEN 74
uint8_t t10k_inquiry(struct scsi_cmd *cmd)
{
	int len = 0;
	struct vpd *vpd_pg;
	unsigned char key = ILLEGAL_REQUEST;
	uint16_t asc = E_INVALID_FIELD_IN_CDB;
	uint8_t *data = cmd->dbuf_p->data;
	uint8_t *cdb = cmd->scb;
	struct lu_phy_attr *lu = cmd->lu;

	MHVTL_DBG(1, "INQUIRY ** (%ld)", (long)cmd->dbuf_p->serialNo);

	if (((cdb[1] & 0x3) == 0x3) || (!(cdb[1] & 0x3) && cdb[2]))
		goto sense;

	memset(data, 0, INQUIRY_LEN);

	if (!(cdb[1] & 0x3)) {
		int i;
		uint16_t *desc;

		data[0] = lu->ptype;
		data[1] = (lu->removable) ? 0x80 : 0;
		data[2] = 5;	/* SPC-3 */
		data[3] = 0x42;

		memset(data + 8, 0x20, 28);
		memcpy(data + 8,  &lu->vendor_id, VENDOR_ID_LEN);
		memcpy(data + 16, &lu->product_id, PRODUCT_ID_LEN);
		memcpy(data + 32, &lu->product_rev, PRODUCT_REV_LEN);

		data[54] = 0x04;	/* Key Management */
		data[55] = 0x12;	/* Support Encryption & Compression */

		desc = (uint16_t *)(data + 58);
		for (i = 0; i < ARRAY_SIZE(lu->version_desc); i++)
			*desc++ = htons(lu->version_desc[i]);

		len = INQUIRY_LEN;
		data[4] = INQUIRY_LEN - 5;	/* Additional Length */
	} else if (cdb[1] & 0x2) {
		/* CmdDt bit is set */
		/* We do not support it now. */
		data[1] = 0x1;
		data[5] = 0;
		len = 6;
	} else if (cdb[1] & 0x1) {
		uint8_t pcode = cdb[2];

		MHVTL_DBG(2, "Page code 0x%02x\n", pcode);

		if (pcode == 0x00) {
			uint8_t *p;
			int i, cnt;

			data[0] = lu->ptype;
			data[1] = 0;
			data[2] = 0;

			cnt = 1;
			p = data + 5;
			for (i = 0; i < ARRAY_SIZE(lu->lu_vpd); i++) {
				if (lu->lu_vpd[i]) {
					*p++ = i | 0x80;
					cnt++;
				}
			}
			data[3] = cnt;
			data[4] = 0x0;
			len = cnt + 4;
		} else if (lu->lu_vpd[PCODE_OFFSET(pcode)]) {
			vpd_pg = lu->lu_vpd[PCODE_OFFSET(pcode)];

			MHVTL_DBG(2, "Found page 0x%x\n", pcode);

			data[0] = lu->ptype;
			data[1] = pcode;
			data[2] = (vpd_pg->sz >> 8);
			data[3] = vpd_pg->sz & 0xff;
			memcpy(&data[4], vpd_pg->data, vpd_pg->sz);
			len = vpd_pg->sz + 4;
		}
	}
	cmd->dbuf_p->sz = len;
	return SAM_STAT_GOOD;

sense:
	mkSenseBuf(key, asc, &cmd->dbuf_p->sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}

void init_t10kA_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace +++");

	ssc_pm.name = pm_name_t10kA;
	ssc_pm.drive_native_density = medium_density_code_10kA;
	ssc_pm.media_capabilities = t10kA_media_handling;
	personality_module_register(&ssc_pm);
	init_default_ssc_mode_pages(sm);
	lu->mode_pages = sm;
	register_ops(lu, SECURITY_PROTOCOL_IN, ssc_spin);
	register_ops(lu, SECURITY_PROTOCOL_OUT, ssc_spout);
	register_ops(lu, INQUIRY, t10k_inquiry);
	register_ops(lu, LOAD_DISPLAY, ssc_load_display);
	init_t10k_inquiry(lu);
}

void init_t10kB_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace +++");

	ssc_pm.name = pm_name_t10kB;
	ssc_pm.drive_native_density = medium_density_code_10kB;
	ssc_pm.media_capabilities = t10kB_media_handling;
	personality_module_register(&ssc_pm);
	init_default_ssc_mode_pages(sm);
	lu->mode_pages = sm;
	register_ops(lu, SECURITY_PROTOCOL_IN, ssc_spin);
	register_ops(lu, SECURITY_PROTOCOL_OUT, ssc_spout);
	register_ops(lu, INQUIRY, t10k_inquiry);
	register_ops(lu, LOAD_DISPLAY, ssc_load_display);
	init_t10k_inquiry(lu);
}

void init_t10kC_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace +++");

	ssc_pm.name = pm_name_t10kC;
	ssc_pm.drive_native_density = medium_density_code_10kC;
	ssc_pm.media_capabilities = t10kC_media_handling;
	personality_module_register(&ssc_pm);
	init_default_ssc_mode_pages(sm);
	lu->mode_pages = sm;
	register_ops(lu, SECURITY_PROTOCOL_IN, ssc_spin);
	register_ops(lu, SECURITY_PROTOCOL_OUT, ssc_spout);
	register_ops(lu, INQUIRY, t10k_inquiry);
	register_ops(lu, LOAD_DISPLAY, ssc_load_display);
	init_t10k_inquiry(lu);
}
