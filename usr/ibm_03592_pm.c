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

/* Note need to change 'medium density' if encryption is enabled / disabled */
static struct density_info density_j1a = {
	0x2e18, 0x0d, 0x200, 0x493e0, medium_density_code_j1a,
			"IBM", "3592A1", "" };

static struct density_info density_e05 = {
	0x2e18, 0x0d, 0x380, 0x7a120, medium_density_code_e05,
			"IBM", "3592A2", "" };

static struct density_info density_e06 = {
	0x348c, 0x0d, 0x480, 0x7a120, medium_density_code_e06,
			"IBM", "3592A3", "" };

static struct density_info density_e07 = {
	0x348c, 0x0d, 0x480, 0x7a120, medium_density_code_e07,
			"IBM", "3592A4", "" };

static struct name_to_media_info media_info[] = {
	{"03592 JA", Media_3592_JA,
			media_type_unknown, medium_density_code_j1a},
	{"03592 JA Clean", Media_3592_JA_CLEAN,
			media_type_unknown, medium_density_code_j1a},
	{"03592 JA WORM", Media_3592_JW,
			media_type_unknown, medium_density_code_j1a},

	{"03592 JB", Media_3592_JB,
			media_type_unknown, medium_density_code_e05},
	{"03592 JB Clean", Media_3592_JB_CLEAN,
			media_type_unknown, medium_density_code_e05},
	{"03592 JB ENCR", Media_3592_JB,
			media_type_unknown, medium_density_code_e05_ENCR},

	{"03592 JC", Media_3592_JX,
			media_type_unknown, medium_density_code_e06},
	{"03592 JC Clean", Media_3592_JX_CLEAN,
			media_type_unknown, medium_density_code_e06},
	{"03592 JC ENCR", Media_3592_JX,
			media_type_unknown, medium_density_code_e06_ENCR},

	{"03592 JK", Media_3592_JK,
			media_type_unknown, medium_density_code_e07},
	{"03592 JK Clean", Media_3592_JK_CLEAN,
			media_type_unknown, medium_density_code_e07},
	{"03592 JK ENCR", Media_3592_JK,
			media_type_unknown, medium_density_code_e07_ENCR},
	{"", 0, 0, 0},
};

static uint8_t valid_encryption_media_E06(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct lu_phy_attr *lu;
	struct priv_lu_ssc *lu_priv;

	MHVTL_DBG(3, "+++ Trace +++");

	lu = cmd->lu;
	lu_priv = lu->lu_private;

	if (c_pos->blk_number == 0) {
		/* 3590 media must be formatted to allow encryption.
		 * This is done by writting an ANSI like label
		 * (NBU label is close enough) to the tape while
		 * an encryption key is in place. The drive doesn't
		 * actually use the key, but sets the tape format
		 */
		if (lu_priv->pm->drive_type == drive_3592_E06) {
			if (lu_priv->ENCRYPT_MODE == 2) {
				lu_priv->cryptop = NULL;
				mam.Flags |= MAM_FLAGS_ENCRYPTION_FORMAT;
			} else
				mam.Flags &= ~MAM_FLAGS_ENCRYPTION_FORMAT;
		}
		modeBlockDescriptor[0] = lu_priv->pm->native_drive_density->density;
		mam.MediumDensityCode = modeBlockDescriptor[0];
		mam.FormattedDensityCode = modeBlockDescriptor[0];
		rewriteMAM(sam_stat);
	} else {
		/* Extra check for 3592 to be sure the cartridge is
		 * formatted for encryption
		 */
		if ((lu_priv->pm->drive_type == drive_3592_E06) &&
				lu_priv->ENCRYPT_MODE &&
				!(mam.Flags & MAM_FLAGS_ENCRYPTION_FORMAT)) {
			sam_data_protect(E_WRITE_PROTECT, sam_stat);
			return 0;
		}
		if (mam.MediumDensityCode !=
				lu_priv->pm->native_drive_density->density) {
			switch (lu_priv->pm->drive_type) {
			case drive_3592_E05:
				if (mam.MediumDensityCode ==
						medium_density_code_j1a)
					break;
				sam_data_protect(E_WRITE_PROTECT, sam_stat);
				return SAM_STAT_CHECK_CONDITION;
				break;
			case drive_3592_E06:
				if (mam.MediumDensityCode ==
						medium_density_code_e05)
					break;
				sam_data_protect(E_WRITE_PROTECT, sam_stat);
				return SAM_STAT_CHECK_CONDITION;
				break;
			case drive_3592_E07:
				if (mam.MediumDensityCode ==
						medium_density_code_e06)
					break;
				sam_data_protect(E_WRITE_PROTECT, sam_stat);
				return SAM_STAT_CHECK_CONDITION;
				break;
			default:
				sam_data_protect(E_WRITE_PROTECT, sam_stat);
				return SAM_STAT_CHECK_CONDITION;
				break;
			}
		}
	}

	return SAM_STAT_GOOD;
}

static uint8_t clear_3592_comp(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default clear_compression is in libvtlscsi */
	return clear_compression_mode_pg(m);
}

static uint8_t set_3592_comp(struct list_head *m, int lvl)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default set_compression is in libvtlscsi */
	return set_compression_mode_pg(m, lvl);
}

static uint8_t update_3592_encryption_mode(struct list_head *m, void *p, int value)
{
	MHVTL_DBG(3, "+++ Trace +++");

	return SAM_STAT_GOOD;
}

static uint8_t set_3592_WORM(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	return set_WORM(m);
}

static uint8_t clear_3592_WORM(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	return clear_WORM(m);
}

static int encr_capabilities_3592(struct scsi_cmd *cmd)
{
	uint8_t *buf = cmd->dbuf_p->data;
	struct priv_lu_ssc *lu_priv = cmd->lu->lu_private;

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

/*	MHVTL_DBG(1, "Drive type: %s, Media type: %s",
				drive_name(lunit.drive_type),
				lookup_media_type(mam.MediaType));
*/

	if (lu_priv->tapeLoaded == TAPE_LOADED) {
		buf[24] |= 0x80; /* AVFMV */
		buf[27] = 0x00; /* Max unauthenticated key data */
		buf[32] |= 0x0e; /* RDMC_C == 7 */
	}
	return 44;
}

static void init_3592_inquiry(struct lu_phy_attr *lu)
{
	int pg;
	uint8_t worm;
	uint8_t local_TapeAlert[8] =
			{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	worm = ((struct priv_lu_ssc *)lu->lu_private)->pm->drive_supports_WORM;
	lu->inquiry[2] =
		((struct priv_lu_ssc *)lu->lu_private)->pm->drive_ANSI_VERSION;

	/* Sequential Access device capabilities - Ref: 8.4.2 */
	pg = PCODE_OFFSET(0xb0);
	lu->lu_vpd[pg] = alloc_vpd(VPD_B0_SZ);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_b0(lu, &worm);

	/* Manufacture-assigned serial number - Ref: 8.4.3 */
	pg = PCODE_OFFSET(0xb1);
	lu->lu_vpd[pg] = alloc_vpd(VPD_B1_SZ);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_b1(lu, lu->lu_serial_no);

	/* TapeAlert supported flags - Ref: 8.4.4 */
	pg = PCODE_OFFSET(0xb2);
	lu->lu_vpd[pg] = alloc_vpd(VPD_B2_SZ);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_b2(lu, &local_TapeAlert);

	/* VPD page 0xC0 */
	pg = PCODE_OFFSET(0xc0);
	lu->lu_vpd[pg] = alloc_vpd(VPD_C0_SZ);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_c0(lu, "10-03-2008 19:38:00");

	/* VPD page 0xC1 */
	pg = PCODE_OFFSET(0xc1);
	lu->lu_vpd[pg] = alloc_vpd(strlen("Security"));
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_c1(lu, "Security");
}

static int e06_kad_validation(int encrypt_mode, int ukad, int akad)
{
	int count = FALSE;
	if (ukad > 0 || akad > 12)
		count = TRUE;
	if (!encrypt_mode && (ukad || akad))
		count = TRUE;

	return count;
}

/* Some comments before I forget how this is supose to work..
 - cleaning_media_state is either
   0 - Not mounted
   1 - Cleaning media mounted -> return Cleaning cartridge installed
   2 - Cleaning media mounted -> return Cause not reportable
   3 - Cleaning media mounted -> return Initializing command required

 On cleaning media mount, ibm_cleaning() is called which:
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

static uint8_t ibm_media_load(struct lu_phy_attr *lu, int load)
{
	MHVTL_DBG(3, "+++ Trace +++ %s", (load) ? "load" : "unload");
	return 0;
}

static uint8_t ibm_cleaning(void *ssc_priv)
{
	struct priv_lu_ssc *ssc;

	MHVTL_DBG(3, "+++ Trace +++");

	ssc = ssc_priv;

	ssc->cleaning_media_state = &cleaning_media_state;
	cleaning_media_state = CLEAN_MOUNT_STAGE1;

	set_cleaning_timer(30);

	return 0;
}

static void init_03592_mode_pages(struct lu_phy_attr *lu)
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

static char *pm_name_j1a = "03592J1A";
static char *pm_name_e05 = "03592E05";
static char *pm_name_e06 = "03592E06";
static char *pm_name_e07 = "03592E07";

static struct ssc_personality_template ssc_pm = {
	.valid_encryption_blk	= valid_encryption_blk,
	.valid_encryption_media	= valid_encryption_media_E06,
	.update_encryption_mode	= update_3592_encryption_mode,
	.kad_validation		= e06_kad_validation,
	.check_restrictions	= check_restrictions,
	.clear_compression	= clear_3592_comp,
	.set_compression	= set_3592_comp,
	.clear_WORM		= clear_3592_WORM,
	.set_WORM		= set_3592_WORM,
	.media_load		= ibm_media_load,
	.cleaning_media		= ibm_cleaning,
	.media_handling		= media_info,
};

void init_3592_j1a(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_j1a;
	ssc_pm.lu = lu;
	ssc_pm.drive_type = drive_3592_J1A;
	ssc_pm.native_drive_density = &density_j1a;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_3592_inquiry(lu);

	init_03592_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);
	add_density_support(&lu->den_list, &density_j1a, 1);
	add_drive_media_list(lu, LOAD_RW, "03592 JA");
	add_drive_media_list(lu, LOAD_RO, "03592 JA Clean");
	add_drive_media_list(lu, LOAD_RW, "03592 JA WORM");
}

void init_3592_E05(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_e05;
	ssc_pm.lu = lu;
	ssc_pm.drive_type = drive_3592_E05;
	ssc_pm.native_drive_density = &density_e05;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_3592_inquiry(lu);

	init_03592_mode_pages(lu);
	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);
	add_density_support(&lu->den_list, &density_j1a, 1);
	add_density_support(&lu->den_list, &density_e05, 1);
	add_drive_media_list(lu, LOAD_RW, "03592 JA");
	add_drive_media_list(lu, LOAD_RO, "03592 JA Clean");
	add_drive_media_list(lu, LOAD_RW, "03592 JA WORM");
	add_drive_media_list(lu, LOAD_RW, "03592 JB");
	add_drive_media_list(lu, LOAD_RO, "03592 JB Clean");
	add_drive_media_list(lu, LOAD_RW, "03592 JB WORM");
}

void init_3592_E06(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", &lu->mode_pg);

	ssc_pm.name = pm_name_e06;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_e06;
	ssc_pm.encryption_capabilities = encr_capabilities_3592;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_3592_inquiry(lu);

	init_03592_mode_pages(lu);
	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);
	ssc_pm.drive_type = drive_3592_E06;
	register_ops(lu, SECURITY_PROTOCOL_IN, ssc_spin, NULL, NULL);
	register_ops(lu, SECURITY_PROTOCOL_OUT, ssc_spout, NULL, NULL);
	add_density_support(&lu->den_list, &density_j1a, 0);
	add_density_support(&lu->den_list, &density_e05, 1);
	add_density_support(&lu->den_list, &density_e06, 1);
	add_drive_media_list(lu, LOAD_RW, "03592 JA");
	add_drive_media_list(lu, LOAD_RO, "03592 JA Clean");
	add_drive_media_list(lu, LOAD_RW, "03592 JA WORM");
	add_drive_media_list(lu, LOAD_RW, "03592 JB");
	add_drive_media_list(lu, LOAD_RO, "03592 JB Clean");
	add_drive_media_list(lu, LOAD_RW, "03592 JB WORM");
	add_drive_media_list(lu, LOAD_RW, "03592 JC");
	add_drive_media_list(lu, LOAD_RO, "03592 JC Clean");
	add_drive_media_list(lu, LOAD_RW, "03592 JC WORM");
}

void init_3592_E07(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_e07;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_e07;
	ssc_pm.encryption_capabilities = encr_capabilities_3592;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_3592_inquiry(lu);

	init_03592_mode_pages(lu);
	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);
	ssc_pm.drive_type = drive_3592_E07;
	register_ops(lu, SECURITY_PROTOCOL_IN, ssc_spin, NULL, NULL);
	register_ops(lu, SECURITY_PROTOCOL_OUT, ssc_spout, NULL, NULL);
	add_density_support(&lu->den_list, &density_e05, 0);
	add_density_support(&lu->den_list, &density_e06, 1);
	add_density_support(&lu->den_list, &density_e07, 1);
	add_drive_media_list(lu, LOAD_RW, "03592 JB");
	add_drive_media_list(lu, LOAD_RO, "03592 JB Clean");
	add_drive_media_list(lu, LOAD_RW, "03592 JB WORM");
	add_drive_media_list(lu, LOAD_RW, "03592 JC");
	add_drive_media_list(lu, LOAD_RO, "03592 JC Clean");
	add_drive_media_list(lu, LOAD_RW, "03592 JC WORM");
	add_drive_media_list(lu, LOAD_RW, "03592 JK");
	add_drive_media_list(lu, LOAD_RO, "03592 JK Clean");
	add_drive_media_list(lu, LOAD_RW, "03592 JK WORM");
}

