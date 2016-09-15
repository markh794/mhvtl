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

static struct density_info density_9840A = {
	0, 127, 288, 0x4e20, medium_density_code_9840A,
			"STK", "R-20", "Raven 20 GB" };

static struct density_info density_9840B = {
	0, 127, 288, 0x4e20, medium_density_code_9840B,
			"STK", "R-20", "Raven 20 GB" };

static struct density_info density_9840C = {
	0, 127, 288, 0x9c40, medium_density_code_9840C,
			"STK", "R-40", "Raven 40 GB" };

static struct density_info density_9840D = {
	0, 127, 576, 0x124f8, medium_density_code_9840D,
			"STK", "R-75", "Raven 75 GB" };

static struct density_info density_9940A = {
	0, 127, 288, 0xea60, medium_density_code_9940A,
			"STK", "P-60", "PeakCapacity 60 GB" };

static struct density_info density_9940B = {
	0, 127, 576, 0x30d40, medium_density_code_9940B,
			"STK", "P-200", "PeakCapacity 200 GB" };

static struct name_to_media_info media_info[] = {
	{"9840A", Media_9840A,
			media_type_unknown, medium_density_code_9840A},
	{"9840A Clean", Media_9840A_CLEAN,
			media_type_unknown, medium_density_code_9840A},
	{"9840B", Media_9840B,
			media_type_unknown, medium_density_code_9840B},
	{"9840B Clean", Media_9840B_CLEAN,
			media_type_unknown, medium_density_code_9840B},
	{"9840C", Media_9840C,
			media_type_unknown, medium_density_code_9840C},
	{"9840C Clean", Media_9840C_CLEAN,
			media_type_unknown, medium_density_code_9840C},
	{"9840D", Media_9840D,
			media_type_unknown, medium_density_code_9840D},
	{"9840D Clean", Media_9840D_CLEAN,
			media_type_unknown, medium_density_code_9840D},

	{"9940A", Media_9940A,
			media_type_unknown, medium_density_code_9940A},
	{"9940A Clean", Media_9940A_CLEAN,
			media_type_unknown, medium_density_code_9940A},
	{"9940B", Media_9940B,
			media_type_unknown, medium_density_code_9940B},
	{"9940B Clean", Media_9940B_CLEAN,
			media_type_unknown, medium_density_code_9940B},
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
uint8_t valid_encryption_blk_9840(struct scsi_cmd *cmd)
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

static uint8_t clear_9840_comp(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default clear_compression is in libvtlscsi */
	return clear_compression_mode_pg(m);
}

static uint8_t set_9840_comp(struct list_head *m, int lvl)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default set_compression is in libvtlscsi */
	return set_compression_mode_pg(m, lvl);
}

static uint8_t update_9840_encryption_mode(struct list_head *m, void *p, int value)
{
	MHVTL_DBG(3, "+++ Trace +++");

	return SAM_STAT_GOOD;
}

static uint8_t set_9840_WORM(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	return set_WORM(m);
}

static uint8_t clear_9840_WORM(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	return clear_WORM(m);
}

static int encr_capabilities_9840(struct scsi_cmd *cmd)
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

static int T9840_kad_validation(int encrypt_mode, int ukad, int akad)
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

 On cleaning media mount, 9840_cleaning() is called which:
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

static uint8_t T9840_media_load(struct lu_phy_attr *lu, int load)
{
	MHVTL_DBG(3, "+++ Trace +++ %s", (load) ? "load" : "unload");
	return 0;
}

static uint8_t T9840_cleaning(void *ssc_priv)
{
	struct priv_lu_ssc *ssc;

	MHVTL_DBG(3, "+++ Trace +++");

	ssc = ssc_priv;

	ssc->cleaning_media_state = &cleaning_media_state;
	cleaning_media_state = CLEAN_MOUNT_STAGE1;

	set_cleaning_timer(30);

	return 0;
}

static void init_9840_mode_pages(struct lu_phy_attr *lu)
{
	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_information_exception(lu);
}

static char *pm_name_9840A = "T9840A";
static char *pm_name_9840B = "T9840B";
static char *pm_name_9840C = "T9840C";
static char *pm_name_9840D = "T9840D";
static char *pm_name_9940A = "T9940A";
static char *pm_name_9940B = "T9940B";

static struct ssc_personality_template ssc_pm = {
	.valid_encryption_blk	= valid_encryption_blk_9840,
	.update_encryption_mode	= update_9840_encryption_mode,
	.encryption_capabilities = encr_capabilities_9840,
	.kad_validation		= T9840_kad_validation,
	.check_restrictions	= check_restrictions,
	.clear_compression	= clear_9840_comp,
	.set_compression	= set_9840_comp,
	.clear_WORM		= clear_9840_WORM,
	.set_WORM		= set_9840_WORM,
	.media_load		= T9840_media_load,
	.cleaning_media		= T9840_cleaning,
	.media_handling		= media_info,
};

#define INQUIRY_LEN 74
static void init_9840_inquiry(struct lu_phy_attr *lu)
{
	int pg;
	uint8_t worm;

	worm = ((struct priv_lu_ssc *)lu->lu_private)->pm->drive_supports_WORM;
	lu->inquiry[2] =
		((struct priv_lu_ssc *)lu->lu_private)->pm->drive_ANSI_VERSION;

	lu->inquiry[3] = 0x42;
	lu->inquiry[4] = INQUIRY_LEN - 5;	/* Additional Length */
	lu->inquiry[54] = 0x04;	/* Key Management */
	lu->inquiry[55] = 0x12;	/* Support Encryption & Compression */

	/* Sequential Access device capabilities - Ref: 8.4.2 */
	pg = PCODE_OFFSET(0xb0);
	lu->lu_vpd[pg] = alloc_vpd(VPD_B0_SZ);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_b0(lu, &worm);
}

void init_9840A_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_9840A;
	ssc_pm.lu = lu;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;
	ssc_pm.native_drive_density = &density_9840A;

	ssc_personality_module_register(&ssc_pm);

	init_9840_inquiry(lu);

	init_9840_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	register_ops(lu, LOAD_DISPLAY, ssc_load_display, NULL, NULL);

	add_density_support(&lu->den_list, &density_9840A, 1);

	add_drive_media_list(lu, LOAD_RW, "9840A");
	add_drive_media_list(lu, LOAD_RO, "9840A Clean");
}

void init_9840B_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_9840B;
	ssc_pm.lu = lu;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;
	ssc_pm.native_drive_density = &density_9840B;

	ssc_personality_module_register(&ssc_pm);

	init_9840_inquiry(lu);

	init_9840_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	register_ops(lu, LOAD_DISPLAY, ssc_load_display, NULL, NULL);

	add_density_support(&lu->den_list, &density_9840A, 1);
	add_density_support(&lu->den_list, &density_9840B, 1);

	add_drive_media_list(lu, LOAD_RW, "9840A");
	add_drive_media_list(lu, LOAD_RO, "9840A Clean");
	add_drive_media_list(lu, LOAD_RW, "9840B");
	add_drive_media_list(lu, LOAD_RO, "9840B Clean");
}

void init_9840C_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_9840C;
	ssc_pm.lu = lu;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_9840_inquiry(lu);

	init_9840_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	ssc_pm.native_drive_density = &density_9840C;

	register_ops(lu, LOAD_DISPLAY, ssc_load_display, NULL, NULL);

	add_density_support(&lu->den_list, &density_9840A, 0);
	add_density_support(&lu->den_list, &density_9840B, 1);
	add_density_support(&lu->den_list, &density_9840C, 1);

	add_drive_media_list(lu, LOAD_RO, "9840A");
	add_drive_media_list(lu, LOAD_RO, "9840A Clean");
	add_drive_media_list(lu, LOAD_RW, "9840B");
	add_drive_media_list(lu, LOAD_RO, "9840B Clean");
	add_drive_media_list(lu, LOAD_RW, "9840C");
	add_drive_media_list(lu, LOAD_RO, "9840C Clean");
}

void init_9840D_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_9840D;
	ssc_pm.lu = lu;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_ANSI_VERSION = 5;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.native_drive_density = &density_9840D;

	ssc_personality_module_register(&ssc_pm);

	init_9840_inquiry(lu);

	init_9840_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	register_ops(lu, LOAD_DISPLAY, ssc_load_display, NULL, NULL);

	add_density_support(&lu->den_list, &density_9840A, 0);
	add_density_support(&lu->den_list, &density_9840B, 0);
	add_density_support(&lu->den_list, &density_9840C, 1);
	add_density_support(&lu->den_list, &density_9840D, 1);

	add_drive_media_list(lu, LOAD_RO, "9840B");
	add_drive_media_list(lu, LOAD_RO, "9840B Clean");
	add_drive_media_list(lu, LOAD_RW, "9840C");
	add_drive_media_list(lu, LOAD_RO, "9840C Clean");
	add_drive_media_list(lu, LOAD_RW, "9840D");
	add_drive_media_list(lu, LOAD_RO, "9840D Clean");
}

void init_9940A_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_9940A;
	ssc_pm.lu = lu;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;
	ssc_pm.native_drive_density = &density_9940A;

	ssc_personality_module_register(&ssc_pm);

	init_9840_inquiry(lu);

	init_9840_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	register_ops(lu, LOAD_DISPLAY, ssc_load_display, NULL, NULL);

	add_density_support(&lu->den_list, &density_9940A, 1);

	add_drive_media_list(lu, LOAD_RW, "9840A");
	add_drive_media_list(lu, LOAD_RO, "9840A Clean");
	add_drive_media_list(lu, LOAD_RW, "9840A");
	add_drive_media_list(lu, LOAD_RO, "9840A Clean");
	add_drive_media_list(lu, LOAD_RW, "9840A");
	add_drive_media_list(lu, LOAD_RO, "9840A Clean");
}

void init_9940B_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", &lu->mode_pg);

	ssc_pm.name = pm_name_9940B;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_9940B;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_9840_inquiry(lu);

	init_9840_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	register_ops(lu, LOAD_DISPLAY, ssc_load_display, NULL, NULL);

	add_density_support(&lu->den_list, &density_9940A, 1);
	add_density_support(&lu->den_list, &density_9940B, 1);

	add_drive_media_list(lu, LOAD_RW, "9940A");
	add_drive_media_list(lu, LOAD_RO, "9940A Clean");
	add_drive_media_list(lu, LOAD_RW, "9940B");
	add_drive_media_list(lu, LOAD_RO, "9940B Clean");
}
