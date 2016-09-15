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
#include "q.h"
#include "mode.h"
#include "log.h"

static struct density_info density_ait1 = {
	0x17d6, 0x50, 1, 100, medium_density_code_ait1,
		"SONY", "AIT-1", "Adv Intellgent Tape" };

static struct density_info density_ait2 = {
	0x17d6, 0x50, 1, 200, medium_density_code_ait2,
		"SONY", "AIT-2", "Adv Intellgent Tape" };

static struct density_info density_ait3 = {
	0x17d6, 0x50, 1, 300, medium_density_code_ait3,
		"SONY", "AIT-3", "Adv Intellgent Tape" };

static struct density_info density_ait4 = {
	0x17d6, 0x50, 1, 400, medium_density_code_ait4,
		"SONY", "AIT-4", "Adv Intellgent Tape" };

static struct name_to_media_info media_info[] = {
	{"AIT1", Media_AIT1,
			media_type_unknown, medium_density_code_ait1},
	{"AIT1 Clean", Media_AIT1_CLEAN,
			media_type_unknown, medium_density_code_ait1},
	{"AIT2", Media_AIT2,
			media_type_unknown, medium_density_code_ait2},
	{"AIT2 Clean", Media_AIT2_CLEAN,
			media_type_unknown, medium_density_code_ait2},
	{"AIT3", Media_AIT3,
			media_type_unknown, medium_density_code_ait3},
	{"AIT3 Clean", Media_AIT3_CLEAN,
			media_type_unknown, medium_density_code_ait3},
	{"AIT4",  Media_AIT4,
			media_type_unknown, medium_density_code_ait4},
	{"AIT4 Clean", Media_AIT4_CLEAN,
			media_type_unknown, medium_density_code_ait4},
	{"AIT4 WORM", Media_AIT4_WORM,
			media_type_unknown, medium_density_code_ait4},
	{"", 0, 0, 0},
};

static uint8_t clear_ait_WORM(struct list_head *l)
{
	uint8_t *smp_dp;
	struct mode *smp;

	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", l);

	smp = lookup_pcode(l, MODE_AIT_DEVICE_CONFIGURATION, 0);
	if (smp) {
		smp_dp = smp->pcodePointer;
		smp_dp[4] = 0x0;
	}

	return SAM_STAT_GOOD;
}

static uint8_t set_ait_WORM(struct list_head *l)
{
	uint8_t *smp_dp;
	struct mode *smp;

	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", l);

	smp = lookup_pcode(l, MODE_AIT_DEVICE_CONFIGURATION, 0);
	if (smp) {
		smp_dp = smp->pcodePointer;
		smp_dp[4] = 0x40;
	}

	return SAM_STAT_GOOD;
}

static uint8_t clear_ait_compression(struct list_head *l)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", l);
	/* default clear_compression is in libvtlscsi */
	return clear_compression_mode_pg(l);
}

static uint8_t set_ait_compression(struct list_head *l, int lvl)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", l);
	/* default set_compression is in libvtlscsi */
	return set_compression_mode_pg(l, lvl);
}

static uint8_t update_ait_encryption_mode(struct list_head *m, void *p, int value)
{
	MHVTL_DBG(3, "+++ Trace +++");

	return SAM_STAT_GOOD;
}

/* FIXME: This is a copy of LTO4/5 encryption capabilities.
	Need to adjust to suit AIT4
*/
static int encr_capabilities_ait(struct scsi_cmd *cmd)
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

	/* adjustments for each emulated drive type */
	buf[4] = 0x1; /* CFG_P == 01b */
	if (lu_priv->tapeLoaded == TAPE_LOADED) {
		switch (mam.MediaType) {
		case Media_AIT4:
			MHVTL_DBG(1, "AIT4 Medium");
			buf[24] |= 0x80; /* AVFMV */
			break;
		}
	}
	buf[32] |= 0x08; /* RDMC_C == 4 */
	return 44;
}

static void init_ait_inquiry(struct lu_phy_attr *lu)
{
	int pg;
	uint8_t worm;
	uint8_t local_TapeAlert[8] = {
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

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

/* Dummy routine. Always return false */
static int ait_kad_validation(int mode, int ukad, int akad)
{
	return FALSE;
}

/* Some comments before I forget how this is supose to work..
 - cleaning_media_state is either
   0 - Not mounted
   1 - Cleaning media mounted -> return Cleaning cartridge installed
   2 - Cleaning media mounted -> return Cause not reportable
   3 - Cleaning media mounted -> return Initializing command required

 On cleaning media mount, ait_cleaning() is called which:
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

static uint8_t ait_cleaning(void *ssc_priv)
{
	struct priv_lu_ssc *ssc;

	MHVTL_DBG(3, "+++ Trace +++");

	ssc = ssc_priv;

	ssc->cleaning_media_state = &cleaning_media_state;
	cleaning_media_state = CLEAN_MOUNT_STAGE1;

	set_cleaning_timer(30);

	return 0;
}

/* Table 6-29 Supported Mode pages
 * Sony SDX-900V v2.1 SCSI Reference Guide
 */
static void init_ait_mode_pages(struct lu_phy_attr *lu)
{
	add_mode_disconnect_reconnect(lu);
	add_mode_control(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_medium_partition(lu);
	add_mode_information_exception(lu);
	add_mode_ait_device_configuration(lu);
}

/* load set to 1 for load, 0 for unload */
static uint8_t ait_media_load(struct lu_phy_attr *lu, int load)
{
	uint8_t *smp_dp;
	struct mode *smp;

	MHVTL_DBG(3, "+++ Trace +++");

	smp = lookup_pcode(&lu->mode_pg, MODE_AIT_DEVICE_CONFIGURATION, 0);
	if (smp) {
		smp_dp = smp->pcodePointer;
		if (load)
			smp_dp[3] = 0x0a;	/* SPAN -> Set to 0Ah on load */
		else
			smp_dp[3] = 0x0;	/* SPAN -> Set to 0 on unload */
	}

	return 0;
}

static char *name_ait_1 = "AIT";
static char *name_ait_2 = "AIT-2";
static char *name_ait_3 = "AIT-3";
static char *name_ait_4 = "AIT-4";

static struct ssc_personality_template ssc_pm = {
	.valid_encryption_blk	= valid_encryption_blk,
	.update_encryption_mode	= update_ait_encryption_mode,
	.encryption_capabilities = encr_capabilities_ait,
	.kad_validation		= ait_kad_validation,
	.check_restrictions	= check_restrictions,
	.clear_compression	= clear_ait_compression,
	.set_compression	= set_ait_compression,
	.media_load		= ait_media_load,
	.cleaning_media		= ait_cleaning,
	.media_handling		= media_info,
};

void init_ait1_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", &lu->mode_pg);

	ssc_pm.name = name_ait_1;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_ait1;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = FALSE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 2;

	ssc_personality_module_register(&ssc_pm);

	init_ait_inquiry(lu);

	init_ait_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	add_density_support(&lu->den_list, &density_ait1, 1);
	add_drive_media_list(lu, LOAD_RW, "AIT1");
	add_drive_media_list(lu, LOAD_RO, "AIT1 Clean");
}

void init_ait2_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", &lu->mode_pg);

	ssc_pm.name = name_ait_2;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_ait2;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_ait_inquiry(lu);

	init_ait_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	add_density_support(&lu->den_list, &density_ait1, 1);
	add_density_support(&lu->den_list, &density_ait2, 1);

	add_drive_media_list(lu, LOAD_RW, "AIT1");
	add_drive_media_list(lu, LOAD_RO, "AIT1 Clean");
	add_drive_media_list(lu, LOAD_RW, "AIT2");
	add_drive_media_list(lu, LOAD_RO, "AIT2 Clean");
}

void init_ait3_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", &lu->mode_pg);

	ssc_pm.name = name_ait_3;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_ait3;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_ait_inquiry(lu);

	init_ait_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	add_density_support(&lu->den_list, &density_ait1, 0);
	add_density_support(&lu->den_list, &density_ait2, 1);
	add_density_support(&lu->den_list, &density_ait3, 1);

	add_drive_media_list(lu, LOAD_RO, "AIT1");
	add_drive_media_list(lu, LOAD_RO, "AIT1 Clean");
	add_drive_media_list(lu, LOAD_RW, "AIT2");
	add_drive_media_list(lu, LOAD_RO, "AIT2 Clean");
	add_drive_media_list(lu, LOAD_RW, "AIT3");
	add_drive_media_list(lu, LOAD_RO, "AIT3 Clean");
}

void init_ait4_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", &lu->mode_pg);

	ssc_pm.name = name_ait_4;
	ssc_pm.lu = lu;
	ssc_pm.clear_WORM = clear_ait_WORM;
	ssc_pm.set_WORM	= set_ait_WORM;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_ait_inquiry(lu);

	init_ait_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	ssc_pm.native_drive_density = &density_ait4;

	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 10; /* Capacity units in KBytes */

	add_density_support(&lu->den_list, &density_ait2, 0);
	add_density_support(&lu->den_list, &density_ait3, 1);
	add_density_support(&lu->den_list, &density_ait4, 1);

	add_drive_media_list(lu, LOAD_RO, "AIT2");
	add_drive_media_list(lu, LOAD_RO, "AIT2 Clean");
	add_drive_media_list(lu, LOAD_RW, "AIT3");
	add_drive_media_list(lu, LOAD_RO, "AIT3 Clean");
	add_drive_media_list(lu, LOAD_RW, "AIT4");
	add_drive_media_list(lu, LOAD_RO, "AIT4 Clean");
	add_drive_media_list(lu, LOAD_RW, "AIT4 WORM");
}
