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

static struct density_info density_lto1 = {
	4880, 127, 384, 10000, medium_density_code_lto1,
			"LTO-CVE", "U-18", "Ultrium 1/8T" };
static struct density_info density_lto2 = {
	4880, 127, 512, 20000, medium_density_code_lto2,
			"LTO-CVE", "U-28", "Ultrium 2/8T" };
static struct density_info density_lto3 = {
	9638, 127, 704, 381469, medium_density_code_lto3,
			"LTO-CVE", "U-316", "Ultrium 3/16T" };
static struct density_info density_lto4 = {
	12725, 127, 896, 80000, medium_density_code_lto4,
			"LTO-CVE", "U-416", "Ultrium 4/16T" };
static struct density_info density_lto5 = {
	15142, 127, 1280, 1500000, medium_density_code_lto5,
			"LTO-CVE", "U-516", "Ultrium 5/16T" };
static struct density_info density_lto6 = {
	15142, 127, 2176, 2500000, medium_density_code_lto6,
			"LTO-CVE", "U-616", "Ultrium 6/16T" };
static struct density_info density_lto7 = {
	19107, 127, 3584, 6000000, medium_density_code_lto7,
			"LTO-CVE", "U-732", "Ultrium 7/32T" };

static struct name_to_media_info media_info[] = {
	{"LTO1", Media_LTO1,
			media_type_lto1_data, medium_density_code_lto1},
	{"LTO1 Clean", Media_LTO1_CLEAN,
			media_type_lto1_data, medium_density_code_lto1},
	{"LTO2", Media_LTO2,
			media_type_lto2_data, medium_density_code_lto2},
	{"LTO2 Clean", Media_LTO2_CLEAN,
			media_type_lto2_data, medium_density_code_lto2},
	{"LTO3", Media_LTO3,
			media_type_lto3_data, medium_density_code_lto3},
	{"LTO3 Clean", Media_LTO3_CLEAN,
			media_type_lto3_data, medium_density_code_lto3},
	{"LTO3 WORM", Media_LTO3_WORM,
			media_type_lto3_worm, medium_density_code_lto3},
	{"LTO4", Media_LTO4,
			media_type_lto4_data, medium_density_code_lto4},
	{"LTO4 Clean", Media_LTO4_CLEAN,
			media_type_lto4_data, medium_density_code_lto4},
	{"LTO4 WORM", Media_LTO4_WORM,
			media_type_lto4_worm, medium_density_code_lto4},
	{"LTO5", Media_LTO5,
			media_type_lto5_data, medium_density_code_lto5},
	{"LTO5 Clean", Media_LTO5_CLEAN,
			media_type_lto5_data, medium_density_code_lto5},
	{"LTO5 WORM", Media_LTO5_WORM,
			media_type_lto5_worm, medium_density_code_lto5},
	{"LTO6", Media_LTO6,
			media_type_lto6_data, medium_density_code_lto6},
	{"LTO6 Clean", Media_LTO6_CLEAN,
			media_type_lto6_data, medium_density_code_lto6},
	{"LTO6 WORM", Media_LTO6_WORM,
			media_type_lto6_worm, medium_density_code_lto6},
	{"LTO7", Media_LTO7,
			media_type_lto7_data, medium_density_code_lto7},
	{"LTO7 Clean", Media_LTO7_CLEAN,
			media_type_lto7_data, medium_density_code_lto7},
	{"LTO7 WORM", Media_LTO7_WORM,
			media_type_lto7_worm, medium_density_code_lto7},
	{"", 0, 0, 0},
};

static uint8_t clear_ult_compression(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	/* default clear_compression is in libvtlscsi */
	return clear_compression_mode_pg(m);
}

static uint8_t set_ult_compression(struct list_head *m, int lvl)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	/* default set_compression is in libvtlscsi */
	return set_compression_mode_pg(m, lvl);
}

/* As per IBM LTO5 SCSI Programmers Guide..
 * Filling in compile time/date & dummy 'platform' string
 */
static void update_vpd_ult_c0(struct lu_phy_attr *lu)
{
	uint8_t *data;
	struct vpd *vpd_p;
	int h, m, s;
	int day, month, year;

	vpd_p = lu->lu_vpd[PCODE_OFFSET(0xc0)];
	data = vpd_p->data;

	ymd(&year, &month, &day, &h, &m, &s);

	data[1] = 0xc0;
	data[3] = 0x27;

	sprintf((char *)&data[16], "%02d%02d%02d", h, m, s);
	sprintf((char *)&data[23], "%04d%02d%02d", year, month, day);
	sprintf((char *)&data[31], "mhvtl_fl_f");
}

static void update_vpd_ult_c1(struct lu_phy_attr *lu, char *sn)
{
	uint8_t *data;
	struct vpd *vpd_p;

	vpd_p = lu->lu_vpd[PCODE_OFFSET(0xc1)];
	data = vpd_p->data;

	data[1] = 0xc1;
	data[3] = 0x18;
	snprintf((char *)&data[4], 12, "%-12s", sn);
	snprintf((char *)&data[16], 12, "%-12s", sn);
}

static uint8_t set_ult_WORM(struct list_head *lst)
{
	uint8_t *mp;
	struct mode *m;

	set_WORM(lst);	/* Default WORM setup */

	/* Now for the Ultrium unique stuff */

	m = lookup_pcode(lst, MODE_BEHAVIOR_CONFIGURATION, 0);
	MHVTL_DBG(3, "l: %p, m: %p, m->pcodePointer: %p",
			lst, m, m->pcodePointer);
	if (m) {
		mp = m->pcodePointer;
		if (!mp)
			return SAM_STAT_GOOD;

		mp[4] = 0x01; /* WORM Behavior */
	}

	return SAM_STAT_GOOD;
}

static uint8_t clear_ult_WORM(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	return clear_WORM(m);
}

static uint8_t update_ult_encryption_mode(struct list_head *m, void *p, int value)
{
	struct mode *mp;

	MHVTL_DBG(3, "+++ Trace +++");

	mp = lookup_pcode(m, MODE_VENDOR_SPECIFIC_24H, 0);
	if (mp) {
		if (value)
			mp->pcodePointer[5] |= ENCR_E;
		else
			mp->pcodePointer[5] &= ~ENCR_E;
	}
	return SAM_STAT_GOOD;
}

static int encr_capabilities_ult(struct scsi_cmd *cmd)
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
		case Media_LTO4:
			MHVTL_DBG(1, "LTO4 Medium");
			buf[24] |= 0x80; /* AVFMV */
			break;
		case Media_LTO5:
			MHVTL_DBG(1, "LTO5 Medium");
			buf[24] |= 0x80; /* AVFMV */
			break;
		}
	}
	buf[32] |= 0x08; /* RDMC_C == 4 */
	return 44;
}

static void init_ult_inquiry(struct lu_phy_attr *lu)
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
	lu->lu_vpd[pg] = alloc_vpd(43);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_ult_c0(lu);

	/* VPD page 0xC1 */
	pg = PCODE_OFFSET(0xc1);
	lu->lu_vpd[pg] = alloc_vpd(28);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_ult_c1(lu, lu->lu_serial_no);
}

static int td4_kad_validation(int encrypt_mode, int ukad, int akad)
{
	int count = FALSE;
	if (ukad > 32 || akad > 12)
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

 On cleaning media mount, ult_cleaning() is called which:
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

static uint8_t ult_media_load(struct lu_phy_attr *lu, int load)
{
	struct priv_lu_ssc *lu_priv = lu->lu_private;

	MHVTL_DBG(3, "+++ Trace +++ %s", (load) ? "load" : "unload");

	if (load) {
		switch (lu_priv->mamp->MediaType) {
		case Media_LTO1:
			lu->mode_media_type = media_type_lto1_data;
			break;
		case Media_LTO2:
			lu->mode_media_type = media_type_lto2_data;
			break;
		case Media_LTO3:
			lu->mode_media_type = media_type_lto3_data;
			break;
		case Media_LTO4:
			lu->mode_media_type = media_type_lto4_data;
			break;
		case Media_LTO5:
			lu->mode_media_type = media_type_lto5_data;
			break;
		case Media_LTO6:
			lu->mode_media_type = media_type_lto6_data;
			break;
		case Media_LTO7:
			lu->mode_media_type = media_type_lto7_data;
			break;
		default:
			lu->mode_media_type = 0;
		}
		if (lu_priv->mamp->MediumType == MEDIA_TYPE_WORM)
			lu->mode_media_type |= 0x04;
	} else {
		lu->mode_media_type = 0;
	}
	return 0;
}

static uint8_t ult_cleaning(void *ssc_priv)
{
	struct priv_lu_ssc *ssc;

	MHVTL_DBG(3, "+++ Trace +++");

	ssc = ssc_priv;

	ssc->cleaning_media_state = &cleaning_media_state;
	cleaning_media_state = CLEAN_MOUNT_STAGE1;

	set_cleaning_timer(30);

	return 0;
}

static char *pm_name_lto1 = "LTO-1";
static char *pm_name_lto2 = "LTO-2";
static char *pm_name_lto3 = "LTO-3";
static char *pm_name_lto4 = "LTO-4";
static char *pm_name_lto5 = "LTO-5";
static char *pm_name_lto6 = "LTO-6";
static char *pm_name_lto7 = "LTO-7";

static struct ssc_personality_template ssc_pm = {
	.valid_encryption_blk	= valid_encryption_blk, /* default in ssc.c */
	.check_restrictions	= check_restrictions, /* default in ssc.c */
	.clear_compression	= clear_ult_compression,
	.set_compression	= set_ult_compression,
	.media_load		= ult_media_load,
	.cleaning_media		= ult_cleaning,
	.media_handling		= media_info,
};

void init_ult3580_td1(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_lto1;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_lto1;

	/* Drive capabilities need to be defined before mode pages */
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_ult_inquiry(lu);

	/* IBM Ultrium SCSI Reference (5edition - Oct 2001)
	 * lists these mode pages
	 */
	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_information_exception(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	/* Capacity units in MBytes */
	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 20;

	add_density_support(&lu->den_list, &density_lto1, 1);

	add_drive_media_list(lu, LOAD_RW, "LTO1");
	add_drive_media_list(lu, LOAD_RO, "LTO1 Clean");
}

void init_ult3580_td2(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_lto2;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_lto2;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_ult_inquiry(lu);

	/* Based on 9th edition of IBM SCSI Reference */
	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_control(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_information_exception(lu);
	add_mode_medium_configuration(lu);
	add_mode_behavior_configuration(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	/* Capacity units in MBytes */
	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 20;

	add_density_support(&lu->den_list, &density_lto1, 1);
	add_density_support(&lu->den_list, &density_lto2, 1);

	add_drive_media_list(lu, LOAD_RW, "LTO1");
	add_drive_media_list(lu, LOAD_RO, "LTO1 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO2");
	add_drive_media_list(lu, LOAD_RO, "LTO2 Clean");
}

void init_ult3580_td3(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_lto3;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_lto3;
	ssc_pm.clear_WORM = clear_ult_WORM;
	ssc_pm.set_WORM = set_ult_WORM;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_ult_inquiry(lu);

	/* Based on 9th edition of IBM SCSI Reference */
	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_control(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_device_configuration_extention(lu);
	add_mode_information_exception(lu);
	add_mode_medium_configuration(lu);
	add_mode_behavior_configuration(lu);
	add_mode_vendor_25h_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	/* Capacity units in MBytes */
	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 20;

	add_density_support(&lu->den_list, &density_lto1, 0);
	add_density_support(&lu->den_list, &density_lto2, 1);
	add_density_support(&lu->den_list, &density_lto3, 1);

	add_drive_media_list(lu, LOAD_RO, "LTO1");
	add_drive_media_list(lu, LOAD_RO, "LTO1 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO2");
	add_drive_media_list(lu, LOAD_RO, "LTO2 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO3");
	add_drive_media_list(lu, LOAD_RO, "LTO3 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO3 WORM");
}

void init_ult3580_td4(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_lto4;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_lto4;
	ssc_pm.update_encryption_mode = update_ult_encryption_mode;
	ssc_pm.encryption_capabilities = encr_capabilities_ult;
	ssc_pm.kad_validation = td4_kad_validation;
	ssc_pm.clear_WORM = clear_ult_WORM;
	ssc_pm.set_WORM = set_ult_WORM;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_ult_inquiry(lu);

	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_control(lu);
	add_mode_control_extension(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_device_configuration_extention(lu);
	add_mode_medium_partition(lu);
	add_mode_power_condition(lu);
	add_mode_information_exception(lu);
	add_mode_medium_configuration(lu);
	add_mode_ult_encr_mode_pages(lu);	/* Extra for LTO-4 */
	add_mode_vendor_25h_mode_pages(lu);
	add_mode_encryption_mode_attribute(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	/* Capacity units in MBytes */
	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 20;

	add_density_support(&lu->den_list, &density_lto2, 0);
	add_density_support(&lu->den_list, &density_lto3, 1);
	add_density_support(&lu->den_list, &density_lto4, 1);

	add_drive_media_list(lu, LOAD_RO, "LTO2");
	add_drive_media_list(lu, LOAD_RO, "LTO2 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO3");
	add_drive_media_list(lu, LOAD_RO, "LTO3 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO3 WORM");
	add_drive_media_list(lu, LOAD_RW, "LTO4");
	add_drive_media_list(lu, LOAD_RO, "LTO4 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO4 WORM");
}

void init_ult3580_td5(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_lto5;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_lto5;
	ssc_pm.update_encryption_mode = update_ult_encryption_mode;
	ssc_pm.encryption_capabilities = encr_capabilities_ult;
	ssc_pm.kad_validation = td4_kad_validation;
	ssc_pm.clear_WORM = clear_ult_WORM;
	ssc_pm.set_WORM = set_ult_WORM;
	ssc_pm.drive_supports_append_only_mode = TRUE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = TRUE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_ult_inquiry(lu);

	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_control(lu);
	add_mode_control_extension(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_device_configuration_extention(lu);
	add_mode_medium_partition(lu);
	add_mode_power_condition(lu);
	add_mode_information_exception(lu);
	add_mode_medium_configuration(lu);
	add_mode_ult_encr_mode_pages(lu);	/* Extra for LTO-5 */
	add_mode_vendor_25h_mode_pages(lu);
	add_mode_encryption_mode_attribute(lu);

	/* Supports non-zero programable early warning */
	update_prog_early_warning(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	/* Capacity units in MBytes */
	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 20;

	add_density_support(&lu->den_list, &density_lto3, 0);
	add_density_support(&lu->den_list, &density_lto4, 1);
	add_density_support(&lu->den_list, &density_lto5, 1);

	add_drive_media_list(lu, LOAD_RO, "LTO3");
	add_drive_media_list(lu, LOAD_RO, "LTO3 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO4");
	add_drive_media_list(lu, LOAD_RO, "LTO4 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO4 WORM");
	add_drive_media_list(lu, LOAD_RW, "LTO4 ENCR");
	add_drive_media_list(lu, LOAD_RW, "LTO5");
	add_drive_media_list(lu, LOAD_RO, "LTO5 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO5 WORM");
	add_drive_media_list(lu, LOAD_RW, "LTO5 ENCR");
}

void init_ult3580_td6(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_lto6;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_lto6;
	ssc_pm.update_encryption_mode = update_ult_encryption_mode;
	ssc_pm.encryption_capabilities = encr_capabilities_ult;
	ssc_pm.kad_validation = td4_kad_validation;
	ssc_pm.clear_WORM = clear_ult_WORM;
	ssc_pm.set_WORM = set_ult_WORM;
	ssc_pm.drive_supports_append_only_mode = TRUE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = TRUE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_ult_inquiry(lu);

	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_control(lu);
	add_mode_control_extension(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_device_configuration_extention(lu);
	add_mode_medium_partition(lu);
	add_mode_power_condition(lu);
	add_mode_information_exception(lu);
	add_mode_medium_configuration(lu);
	add_mode_ult_encr_mode_pages(lu);	/* Extra for LTO-5 */
	add_mode_vendor_25h_mode_pages(lu);
	add_mode_encryption_mode_attribute(lu);

	/* Supports non-zero programable early warning */
	update_prog_early_warning(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	/* Capacity units in MBytes */
	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 20;

	add_density_support(&lu->den_list, &density_lto4, 0);
	add_density_support(&lu->den_list, &density_lto5, 1);
	add_density_support(&lu->den_list, &density_lto6, 1);

	add_drive_media_list(lu, LOAD_RO, "LTO4");
	add_drive_media_list(lu, LOAD_RO, "LTO4 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO4 WORM");
	add_drive_media_list(lu, LOAD_RW, "LTO5");
	add_drive_media_list(lu, LOAD_RO, "LTO5 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO5 WORM");
	add_drive_media_list(lu, LOAD_RW, "LTO5 ENCR");
	add_drive_media_list(lu, LOAD_RW, "LTO6");
	add_drive_media_list(lu, LOAD_RO, "LTO6 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO6 WORM");
	add_drive_media_list(lu, LOAD_RW, "LTO6 ENCR");
}

void init_ult3580_td7(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_lto7;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_lto7;
	ssc_pm.update_encryption_mode = update_ult_encryption_mode;
	ssc_pm.encryption_capabilities = encr_capabilities_ult;
	ssc_pm.kad_validation = td4_kad_validation;
	ssc_pm.clear_WORM = clear_ult_WORM;
	ssc_pm.set_WORM = set_ult_WORM;
	ssc_pm.drive_supports_append_only_mode = TRUE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = TRUE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = TRUE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_ult_inquiry(lu);

	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_control(lu);
	add_mode_control_extension(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_device_configuration_extention(lu);
	add_mode_medium_partition(lu);
	add_mode_power_condition(lu);
	add_mode_information_exception(lu);
	add_mode_medium_configuration(lu);
	add_mode_ult_encr_mode_pages(lu);	/* Extra for LTO-5 */
	add_mode_vendor_25h_mode_pages(lu);
	add_mode_encryption_mode_attribute(lu);

	/* Supports non-zero programable early warning */
	update_prog_early_warning(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	/* Capacity units in MBytes */
	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 20;

	add_density_support(&lu->den_list, &density_lto5, 0);
	add_density_support(&lu->den_list, &density_lto6, 1);
	add_density_support(&lu->den_list, &density_lto7, 1);

	add_drive_media_list(lu, LOAD_RO, "LTO5");
	add_drive_media_list(lu, LOAD_RO, "LTO5 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO5 WORM");
	add_drive_media_list(lu, LOAD_RW, "LTO5 ENCR");
	add_drive_media_list(lu, LOAD_RW, "LTO6");
	add_drive_media_list(lu, LOAD_RO, "LTO6 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO6 WORM");
	add_drive_media_list(lu, LOAD_RW, "LTO6 ENCR");
	add_drive_media_list(lu, LOAD_RW, "LTO7");
	add_drive_media_list(lu, LOAD_RO, "LTO7 Clean");
	add_drive_media_list(lu, LOAD_RW, "LTO7 WORM");
	add_drive_media_list(lu, LOAD_RW, "LTO7 ENCR");
}
