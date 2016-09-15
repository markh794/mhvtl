/*
 * This handles any SCSI OP codes defined in the standards as 'STREAM'
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
 *                                mark.harvey at veritas dot com
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
#include "logging.h"
#include "q.h"
#include "ssc.h"
#include "spc.h"
#include "vtltape.h"
#include "mode.h"
#include "log.h"

static struct density_info density_t10kA = {
	0, 127, 0x300, 0x7a120, medium_density_code_10kA,
			"STK", "T1 - 500", "T1 - 500 GB" };

static struct density_info density_t10kB = {
	0, 127, 0x480, 0x1d4c0, medium_density_code_10kB,
			"STK", "T1 - 1000", "T1 - 1000 GB" };

static struct density_info density_t10kC = {
	0, 127, 0x600, 0x30000, medium_density_code_10kC,
			"STK", "T2 - 5000", "T1 - 5000 GB" };

static struct name_to_media_info media_info[] = {
	{"T10KA", Media_T10KA,
			media_type_unknown, medium_density_code_10kA},
	{"T10KA WORM", Media_T10KA_WORM,
			media_type_unknown, medium_density_code_10kA},
	{"T10KA Clean", Media_T10KA_CLEAN,
			media_type_unknown, medium_density_code_10kA},
	{"T10KB", Media_T10KB,
			media_type_unknown, medium_density_code_10kB},
	{"T10KB WORM", Media_T10KB_WORM,
			media_type_unknown, medium_density_code_10kB},
	{"T10KB Clean", Media_T10KB_CLEAN,
			media_type_unknown, medium_density_code_10kB},
	{"T10KC", Media_T10KC,
			media_type_unknown, medium_density_code_10kC},
	{"T10KC WORM", Media_T10KC_WORM,
			media_type_unknown, medium_density_code_10kC},
	{"T10KC Clean", Media_T10KC_CLEAN,
			media_type_unknown, medium_density_code_10kC},
	{"", 0, 0, 0},
};

/*
 * Returns true if blk header has correct encryption key data
 */
#define	UKAD_LENGTH	(encr->ukad_length)
#define	AKAD_LENGTH	(encr->akad_length)
#define	KEY_LENGTH	(encr->key_length)
#define	UKAD		(encr->ukad)
#define	AKAD		(encr->akad)
#define	KEY		(encr->key)
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
				sam_data_protect(E_INCORRECT_KEY, sam_stat);
				correct_key = FALSE;
				return correct_key;
			}
			for (i = 0; i < c_pos->encryption.key_length; ++i) {
				if (c_pos->encryption.key[i] != KEY[i]) {
					sam_data_protect(E_INCORRECT_KEY,
							sam_stat);
					correct_key = FALSE;
					break;
				}
			}
			if (c_pos->encryption.ukad_length != UKAD_LENGTH) {
				sam_data_protect(E_INCORRECT_KEY, sam_stat);
				correct_key = FALSE;
				return correct_key;
			}
			for (i = 0; i < c_pos->encryption.ukad_length; ++i) {
				if (c_pos->encryption.ukad[i] != UKAD[i]) {
					sam_data_protect(E_INCORRECT_KEY,
							sam_stat);
					correct_key = FALSE;
					break;
				}
			}
		} else {
			sam_data_protect(E_UNABLE_TO_DECRYPT, sam_stat);
			correct_key = FALSE;
		}
	} else if (lu_priv->DECRYPT_MODE == 2) {
		sam_data_protect(E_UNENCRYPTED_DATA, sam_stat);
		correct_key = FALSE;
	}
	return correct_key;
}

static uint8_t clear_t10k_comp(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default clear_compression is in libvtlscsi */
	return clear_compression_mode_pg(m);
}

static uint8_t set_t10k_comp(struct list_head *m, int lvl)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default set_compression is in libvtlscsi */
	return set_compression_mode_pg(m, lvl);
}

static uint8_t update_t10k_encryption_mode(struct list_head *m, void *p, int value)
{
	MHVTL_DBG(3, "+++ Trace +++");

	return SAM_STAT_GOOD;
}

static uint8_t set_t10k_WORM(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	return set_WORM(m);
}

static uint8_t clear_t10k_WORM(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	return clear_WORM(m);
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

static uint8_t t10k_media_load(struct lu_phy_attr *lu, int load)
{
	uint8_t *sense_p = lu->sense_p;
	struct priv_lu_ssc *ssc;
	ssc = lu->lu_private;

	MHVTL_DBG(3, "+++ Trace +++ %s", (load) ? "load" : "unload");

	if (load) {
		switch (ssc->mamp->MediumType) {
		case MEDIA_TYPE_WORM:
			sense_p[24] |= 0x02;	/* Data + Append-only */
			/* Now fall thru to 'Data' */
		case MEDIA_TYPE_DATA:
			sense_p[24] |= 0x10;
			break;
		case MEDIA_TYPE_CLEAN:
			sense_p[24] |= 0x80;	/* Cleaning cart */
			break;
		case MEDIA_TYPE_FIRMWARE:
			sense_p[24] |= 0x20;
			break;
		default:
			sense_p[24] &= 0x0d;	/* Unknown type */
			break;
		}
	} else {
		sense_p[24] &= 0x0d;	/* Unknown type & mask out Volsafe */
	}

	if (ssc->append_only_mode)
		sense_p[24] |= 0x02;

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

static void init_t10k_mode_pages(struct lu_phy_attr *lu)
{
	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_control(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_medium_partition(lu);
	add_mode_power_condition(lu);
	add_mode_information_exception(lu);
	add_mode_medium_configuration(lu);
}

/* T10K return tape type in request sense information
 *
 * Reference: T10000 Interface Reference Manual * August 2009 * Revision M
 *
 * sense[24] 7 6 5 4 3 2 1 0
 *                   | | | +- TapeEOL - Tape loaded is End-Of-Life
 *                   | | +--- Volsafe - Current tape is append-only
 *                   | +----- MIRBad  - Metadata on tape is defective
 *           | | | | +------- DAvail  - Diagnostic info available
 * Tape Type +-+-+-+
 * 1000b = Cleaning tape
 * 0100b = Dump tape
 * 0010b = Code load tape
 * 0001b = Data Tape
 * 0000b = Unknown type
 */
static void t10k_init_sense(struct scsi_cmd *cmd)
{
	uint8_t *sense_buf = (uint8_t *)cmd->dbuf_p->sense_buf;
	struct priv_lu_ssc *lu_priv = cmd->lu->lu_private;

	if (lu_priv->tapeLoaded) {
		if (lu_priv->append_only_mode)
			sense_buf[24] |= 0x02;

		switch (lu_priv->mamp->MediumType) {
		case MEDIA_TYPE_WORM:
			sense_buf[24] |= 0x02;	/* Append-only */
			/* Fall thru to MEDIA_TYPE_DATA */
		case MEDIA_TYPE_DATA:
			sense_buf[24] |= 0x10;
			break;
		case MEDIA_TYPE_CLEAN:
			sense_buf[24] |= 0x80;	/* Cleaning cart */
			break;
		case MEDIA_TYPE_FIRMWARE:
			sense_buf[24] |= 0x20;
			break;
		default:
			sense_buf[24] &= 0x0d;	/* Unknown type */
			break;
		}
	}
	if (lu_priv->inLibrary)
		sense_buf[25] = 0x02;	/* LibAtt */
}

uint8_t t10k_sense(struct scsi_cmd *cmd)
{
	t10k_init_sense(cmd);
	return spc_request_sense(cmd);
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
	.media_handling		= media_info,
};

#define INQUIRY_LEN 74
static void init_t10k_inquiry(struct lu_phy_attr *lu)
{
	int pg;
	uint8_t worm;

	worm = ((struct priv_lu_ssc *)lu->lu_private)->pm->drive_supports_WORM;
	lu->inquiry[2] =
		((struct priv_lu_ssc *)lu->lu_private)->pm->drive_ANSI_VERSION;

	lu->inquiry[3] = 0x42;
	lu->inquiry[4] = INQUIRY_LEN - 5;	/* Additional Length */

	if (ssc_pm.drive_supports_SP) {	/* Security Protocols */
		lu->inquiry[54] |= 0x04; /* Key management - DPKM SPIN/SPOUT */
		lu->inquiry[55] |= 0x10; /* Encrypt */
	}

	/* FIXME: Need to add 'LibAtt' too */

	/* WORM... */
	if (ssc_pm.drive_supports_WORM)
		lu->inquiry[55] |= 0x04; /* VolSafe set */

	/* Set Data Compression enabled */
	lu->inquiry[55] |= 0x02;	/* DCMP bit enabled */

	/* Version Descriptor */
	put_unaligned_be16(0x0077, &lu->inquiry[58]);
	put_unaligned_be16(0x0314, &lu->inquiry[60]);
	put_unaligned_be16(0x0403, &lu->inquiry[62]);
	put_unaligned_be16(0x0a11, &lu->inquiry[64]);

	/* Sequential Access device capabilities - Ref: 8.4.2 */
	pg = PCODE_OFFSET(0xb0);
	lu->lu_vpd[pg] = alloc_vpd(VPD_B0_SZ);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_b0(lu, &worm);
}

void init_t10kA_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_t10kA;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_t10kA;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_t10k_inquiry(lu);

	init_t10k_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	register_ops(lu, LOAD_DISPLAY, ssc_load_display, NULL, NULL);
	register_ops(lu, REQUEST_SENSE, t10k_sense, NULL, NULL);

	add_density_support(&lu->den_list, &density_t10kA, 1);

	add_drive_media_list(lu, LOAD_RW, "T10KA");
	add_drive_media_list(lu, LOAD_RO, "T10KA Clean");
}

void init_t10kB_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_t10kB;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_t10kB;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_t10k_inquiry(lu);

	init_t10k_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	register_ops(lu, LOAD_DISPLAY, ssc_load_display, NULL, NULL);
	register_ops(lu, REQUEST_SENSE, t10k_sense, NULL, NULL);

	add_density_support(&lu->den_list, &density_t10kA, 1);
	add_density_support(&lu->den_list, &density_t10kB, 1);

	add_drive_media_list(lu, LOAD_RW, "T10KA");
	add_drive_media_list(lu, LOAD_RO, "T10KA Clean");
	add_drive_media_list(lu, LOAD_RW, "T10KB");
	add_drive_media_list(lu, LOAD_RO, "T10KB Clean");
}

void init_t10kC_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", &lu->mode_pg);

	ssc_pm.name = pm_name_t10kC;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_t10kC;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_t10k_inquiry(lu);

	init_t10k_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	register_ops(lu, LOAD_DISPLAY, ssc_load_display, NULL, NULL);
	register_ops(lu, REQUEST_SENSE, t10k_sense, NULL, NULL);

	add_density_support(&lu->den_list, &density_t10kA, 0);
	add_density_support(&lu->den_list, &density_t10kB, 1);
	add_density_support(&lu->den_list, &density_t10kC, 1);

	add_drive_media_list(lu, LOAD_RW, "T10KA");
	add_drive_media_list(lu, LOAD_RO, "T10KA Clean");
	add_drive_media_list(lu, LOAD_RW, "T10KB");
	add_drive_media_list(lu, LOAD_RO, "T10KB Clean");
	add_drive_media_list(lu, LOAD_RW, "T10KC");
	add_drive_media_list(lu, LOAD_RO, "T10KC Clean");
}
