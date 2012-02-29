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
#include "be_byteshift.h"
#include "scsi.h"
#include "list.h"
#include "vtl_common.h"
#include "vtllib.h"
#include "ssc.h"
#include "spc.h"
#include "vtltape.h"
#include "q.h"
#include "mode.h"
#include "log.h"

static struct density_info density_lto1 = {
	4880, 127, 384, 80000, medium_density_code_lto1,
			"LTO-CVE", "U-18", "Ultrium 1/8T" };
static struct density_info density_lto2 = {
	4880, 127, 512, 80000, medium_density_code_lto2,
			"LTO-CVE", "U-28", "Ultrium 2/8T" };
static struct density_info density_lto3 = {
	9638, 127, 704, 80000, medium_density_code_lto3,
			"LTO-CVE", "U-316", "Ultrium 3/16T" };
static struct density_info density_lto4 = {
	12725, 127, 896, 80000, medium_density_code_lto4,
			"LTO-CVE", "U-416", "Ultrium 4/16T" };
static struct density_info density_lto5 = {
	15142, 127, 1280, 80000, medium_density_code_lto5,
			"LTO-CVE", "U-516", "Ultrium 5/16T" };

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
	struct mode *smp;

	MHVTL_DBG(3, "+++ Trace +++");

	smp = lookup_pcode(m, MODE_VENDOR_SPECIFIC_24H, 0);
	if (smp) {
		if (value)
			smp->pcodePointer[5] |= ENCR_E;
		else
			smp->pcodePointer[5] &= ~ENCR_E;
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
	uint8_t worm = 1;	/* Supports WORM */
	uint8_t local_TapeAlert[8] =
			{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	pg = 0x86 & 0x7f;
	lu->lu_vpd[pg] = alloc_vpd(VPD_86_SZ);
	lu->lu_vpd[pg]->vpd_update = update_vpd_86;

	/* Sequential Access device capabilities - Ref: 8.4.2 */
	pg = 0xb0 & 0x7f;
	lu->lu_vpd[pg] = alloc_vpd(VPD_B0_SZ);
	lu->lu_vpd[pg]->vpd_update = update_vpd_b0;
	lu->lu_vpd[pg]->vpd_update(lu, &worm);

	/* Manufacture-assigned serial number - Ref: 8.4.3 */
	pg = 0xb1 & 0x7f;
	lu->lu_vpd[pg] = alloc_vpd(VPD_B1_SZ);
	lu->lu_vpd[pg]->vpd_update = update_vpd_b1;
	lu->lu_vpd[pg]->vpd_update(lu, lu->lu_serial_no);

	/* TapeAlert supported flags - Ref: 8.4.4 */
	pg = 0xb2 & 0x7f;
	lu->lu_vpd[pg] = alloc_vpd(VPD_B2_SZ);
	lu->lu_vpd[pg]->vpd_update = update_vpd_b2;
	lu->lu_vpd[pg]->vpd_update(lu, &local_TapeAlert);

	/* VPD page 0xC0 */
	pg = 0xc0 & 0x7f;
	lu->lu_vpd[pg] = alloc_vpd(VPD_C0_SZ);
	lu->lu_vpd[pg]->vpd_update = update_vpd_c0;
	lu->lu_vpd[pg]->vpd_update(lu, "10-03-2008 19:38:00");

	/* VPD page 0xC1 */
	pg = 0xc1 & 0x7f;
	lu->lu_vpd[pg] = alloc_vpd(strlen("Security"));
	lu->lu_vpd[pg]->vpd_update = update_vpd_c1;
	lu->lu_vpd[pg]->vpd_update(lu, "Security");
}

static int hp_lto_kad_validation(int encrypt_mode, int ukad, int akad)
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

 On cleaning media mount, hp_cleaning() is called which:
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

static uint8_t hp_media_load(struct lu_phy_attr *lu, int load)
{
	MHVTL_DBG(3, "+++ Trace +++ %s", (load) ? "load" : "unload");
	return 0;
}

static uint8_t hp_cleaning(void *ssc_priv)
{
	struct priv_lu_ssc *ssc;

	MHVTL_DBG(3, "+++ Trace +++");

	ssc = ssc_priv;

	ssc->cleaning_media_state = &cleaning_media_state;
	cleaning_media_state = CLEAN_MOUNT_STAGE1;

	set_cleaning_timer(30);

	return 0;
}

static char *pm_name_lto1 = "HP LTO-1";
static char *pm_name_lto2 = "HP LTO-2";
static char *pm_name_lto3 = "HP LTO-3";
static char *pm_name_lto4 = "HP LTO-4";
static char *pm_name_lto5 = "HP LTO-5";

static struct ssc_personality_template ssc_pm = {
	.valid_encryption_blk	= valid_encryption_blk, /* default in ssc.c */
	.check_restrictions	= check_restrictions, /* default in ssc.c */
	.clear_compression	= clear_ult_compression,
	.set_compression	= set_ult_compression,
	.media_load		= hp_media_load,
	.cleaning_media		= hp_cleaning,
};

void init_hp_ult_1(struct lu_phy_attr *lu)
{
	init_ult_inquiry(lu);
	ssc_pm.name = pm_name_lto1;
	ssc_pm.lu = lu;
	personality_module_register(&ssc_pm);
	ssc_pm.native_drive_density = &density_lto1;

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

void init_hp_ult_2(struct lu_phy_attr *lu)
{
	init_ult_inquiry(lu);
	ssc_pm.name = pm_name_lto2;
	ssc_pm.lu = lu;
	personality_module_register(&ssc_pm);

	ssc_pm.native_drive_density = &density_lto2;

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

void init_hp_ult_3(struct lu_phy_attr *lu)
{
	init_ult_inquiry(lu);
	ssc_pm.name = pm_name_lto3;
	ssc_pm.lu = lu;
	personality_module_register(&ssc_pm);
	ssc_pm.native_drive_density = &density_lto3;
	ssc_pm.clear_WORM = clear_ult_WORM;
	ssc_pm.set_WORM = set_ult_WORM;

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

void init_hp_ult_4(struct lu_phy_attr *lu)
{
	init_ult_inquiry(lu);
	ssc_pm.name = pm_name_lto4;
	ssc_pm.lu = lu;
	personality_module_register(&ssc_pm);

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

	ssc_pm.native_drive_density = &density_lto4;
	ssc_pm.update_encryption_mode = update_ult_encryption_mode,
	ssc_pm.encryption_capabilities = encr_capabilities_ult,
	ssc_pm.kad_validation = hp_lto_kad_validation,
	ssc_pm.clear_WORM = clear_ult_WORM,
	ssc_pm.set_WORM = set_ult_WORM,
	ssc_pm.drive_supports_early_warning = TRUE;

	/* Capacity units in MBytes */
	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 20;

	register_ops(lu, SECURITY_PROTOCOL_IN, ssc_spin);
	register_ops(lu, SECURITY_PROTOCOL_OUT, ssc_spout);
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

void init_hp_ult_5(struct lu_phy_attr *lu)
{
	init_ult_inquiry(lu);
	ssc_pm.name = pm_name_lto5;
	ssc_pm.lu = lu;
	personality_module_register(&ssc_pm);

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

	ssc_pm.native_drive_density = &density_lto5;
	ssc_pm.update_encryption_mode = update_ult_encryption_mode,
	ssc_pm.encryption_capabilities = encr_capabilities_ult,
	ssc_pm.kad_validation = hp_lto_kad_validation,
	ssc_pm.clear_WORM = clear_ult_WORM,
	ssc_pm.set_WORM = set_ult_WORM,
	ssc_pm.drive_supports_append_only_mode = TRUE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = TRUE;

	/* Capacity units in MBytes */
	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 20;

	register_ops(lu, SECURITY_PROTOCOL_IN, ssc_spin);
	register_ops(lu, SECURITY_PROTOCOL_OUT, ssc_spout);
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
