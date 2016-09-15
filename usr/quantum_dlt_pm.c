/*
 * This handles any SCSI OP codes defined in the standards as 'STREAM'
 *
 * Copyright (C) 2005 - 2012 Mark Harvey markh794@gmail.com
 *                                mark.harvey at veritas.com
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

/* FIXME: This data needs to be updated to suit SDLT range of media */
static struct density_info density_dlt2 = {
	1000, 640, 64, 1000, medium_density_code_dlt2,
			"DLT-CVE", "DLT II", "DLTtape II" };
static struct density_info density_dlt3 = {
	62500, 640, 64, 20000, medium_density_code_dlt3,
			"DLT-CVE", "DLT III", "DLTtape III" };
static struct density_info density_dlt4 = {
	85937, 640, 52, 35000, medium_density_code_dlt4,
			"DLT-CVE", "DLT IV", "DLTtape IV" };
static struct density_info density_sdlt = {
	15142, 640, 1502, 80000, medium_density_code_sdlt,
			"DLT-CVE", "U-216", "SDLT" };
static struct density_info density_sdlt220 = {
	15142, 640, 1502, 80000, medium_density_code_220,
			"DLT-CVE", "U-316", "SDLT 220" };
static struct density_info density_sdlt320 = {
	15142, 640, 1502, 80000, medium_density_code_320,
			"DLT-CVE", "U-416", "SDLT 320" };
static struct density_info density_sdlt600 = {
	15142, 640, 1502, 80000, medium_density_code_600,
			"DLT-CVE", "U-516", "SDLT 600" };

static struct name_to_media_info media_info[] = {
	{"DLT2", Media_DLT2,
			media_type_unknown, medium_density_code_dlt2},
	{"DLT3", Media_DLT3,
			media_type_unknown, medium_density_code_dlt3},
	{"DLT4", Media_DLT4,
			media_type_unknown, medium_density_code_dlt4},
	{"SDLT", Media_SDLT,
			media_type_unknown, medium_density_code_sdlt},
	{"SDLT 220", Media_SDLT220,
			media_type_unknown, medium_density_code_220},
	{"SDLT 320", Media_SDLT320,
			media_type_unknown, medium_density_code_320},
	{"SDLT 320 Clean", Media_SDLT320_CLEAN,
			media_type_unknown, medium_density_code_320},
	{"SDLT 600", Media_SDLT600,
			media_type_unknown, medium_density_code_600},
	{"SDLT 600 Clean", Media_SDLT600_CLEAN,
			media_type_unknown, medium_density_code_600},
	{"SDLT 600 WORM", Media_SDLT600_WORM,
			media_type_unknown, medium_density_code_600},
	{"", 0, 0, 0},
};

static uint8_t clear_dlt_compression(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	/* default clear_compression is in libvtlscsi */
	return clear_compression_mode_pg(m);
}

static uint8_t set_dlt_compression(struct list_head *m, int lvl)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	/* default set_compression is in libvtlscsi */
	return set_compression_mode_pg(m, lvl);
}

/* As per IBM LTO5 SCSI Programmers Guide..
 * Filling in compile time/date & dummy 'platform' string
 */
static void update_vpd_dlt_c0(struct lu_phy_attr *lu)
{
	uint8_t *data;
	struct vpd *vpd_p;
	int h, m, s;
	int day, month, year;

	vpd_p = lu->lu_vpd[PCODE_OFFSET(0xc0)];
	data = vpd_p->data;
	month = 0;

	ymd(&year, &month, &day, &h, &m, &s);

	data[1] = 0xc0;
	data[3] = 0x28;

	/* Controller firmware build date */
	sprintf((char *)&data[20], "%02d-%02d-%04d %02d:%02d:%02d",
			day, month, year, h, m, s);
}

static int get_product_family(struct lu_phy_attr *lu)
{
	int ret;
	if (!strncmp(lu->product_id, "SDLT600", 7))
		ret = 0xc0;	/* Product Family - (300/600 GB)  */
	else  if (!strncmp(lu->product_id, "SDLT 320", 8))
		ret = 0xb0;	/* Product Family - (160/320 GB)  */
	else
		ret = 0xa0;	/* Product Family - (110/220 GB)  */

	return ret;
}

static void update_vpd_dlt_c1(struct lu_phy_attr *lu, char *sn)
{
	uint8_t *data;
	struct vpd *vpd_p;

	vpd_p = lu->lu_vpd[PCODE_OFFSET(0xc1)];
	data = vpd_p->data;

	data[1] = 0xc1;
	data[3] = 0x39;
	data[4] = get_product_family(lu);
	snprintf((char *)&data[4], 12, "%-12s", sn);
	snprintf((char *)&data[24], 12, "%-12s", sn);
}

static uint8_t set_dlt_WORM(struct list_head *lst)
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

static uint8_t clear_dlt_WORM(struct list_head *m)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", m);
	return clear_WORM(m);
}

/* DLT7000 & DLT8000 */
static void init_dlt_inquiry(struct lu_phy_attr *lu)
{
	int pg;
	char b[32];
	int x, y, z;

	lu->inquiry[2] =
		((struct priv_lu_ssc *)lu->lu_private)->pm->drive_ANSI_VERSION;

	lu->inquiry[36] = get_product_family(lu);

	sprintf(b, "%s", MHVTL_VERSION);
	sscanf(b, "%d.%d.%d", &x, &y, &z);
	if (x) {
		lu->inquiry[37] = x;
		lu->inquiry[38] = y;
	} else {
		lu->inquiry[37] = y;
		lu->inquiry[38] = z;
	}

	/* VPD page 0xC0 */
	pg = PCODE_OFFSET(0xc0);
	lu->lu_vpd[pg] = alloc_vpd(44);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_dlt_c0(lu);

	/* VPD page 0xC1 */
	pg = PCODE_OFFSET(0xc1);
	lu->lu_vpd[pg] = alloc_vpd(44);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_dlt_c1(lu, lu->lu_serial_no);
}

/* SuperDLT range */
static void init_sdlt_inquiry(struct lu_phy_attr *lu)
{
	int pg;
	uint8_t worm;
	uint8_t ta[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	char b[32];
	int x, y, z;

	worm = ((struct priv_lu_ssc *)lu->lu_private)->pm->drive_supports_WORM;
	lu->inquiry[2] =
		((struct priv_lu_ssc *)lu->lu_private)->pm->drive_ANSI_VERSION;


	lu->inquiry[36] = get_product_family(lu);

	sprintf(b, "%s", MHVTL_VERSION);
	sscanf(b, "%d.%d.%d", &x, &y, &z);
	if (x) {
		lu->inquiry[37] = x;
		lu->inquiry[38] = y;
	} else {
		lu->inquiry[37] = y;
		lu->inquiry[38] = z;
	}

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
	update_vpd_b2(lu, &ta);

	/* VPD page 0xC0 */
	pg = PCODE_OFFSET(0xc0);
	lu->lu_vpd[pg] = alloc_vpd(44);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_dlt_c0(lu);

	/* VPD page 0xC1 */
	pg = PCODE_OFFSET(0xc1);
	lu->lu_vpd[pg] = alloc_vpd(44);
	if (!lu->lu_vpd[pg]) {
		MHVTL_ERR("Failed to malloc(): Line %d", __LINE__);
		exit(-ENOMEM);
	}
	update_vpd_dlt_c1(lu, lu->lu_serial_no);
}

/* Some comments before I forget how this is supose to work..
 - cleaning_media_state is either
   0 - Not mounted
   1 - Cleaning media mounted -> return Cleaning cartridge installed
   2 - Cleaning media mounted -> return Cause not reportable
   3 - Cleaning media mounted -> return Initializing command required

 On cleaning media mount, dlt_cleaning() is called which:
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

static uint8_t dlt_media_load(struct lu_phy_attr *lu, int load)
{
	MHVTL_DBG(3, "+++ Trace +++ %s", (load) ? "load" : "unload");
	return 0;
}

static uint8_t dlt_cleaning(void *ssc_priv)
{
	struct priv_lu_ssc *ssc;

	MHVTL_DBG(3, "+++ Trace +++");

	ssc = ssc_priv;

	ssc->cleaning_media_state = &cleaning_media_state;
	cleaning_media_state = CLEAN_MOUNT_STAGE1;

	set_cleaning_timer(30);

	return 0;
}

static char *pm_name_dlt7000 = "DLT7000";
static char *pm_name_dlt8000 = "DLT8000";
static char *pm_name_sdlt320 = "SDLT320";
static char *pm_name_sdlt600 = "SDLT600";

static struct ssc_personality_template ssc_pm = {
	.valid_encryption_blk	= valid_encryption_blk,
	.check_restrictions	= check_restrictions, /* default in ssc.c */
	.clear_compression	= clear_dlt_compression,
	.set_compression	= set_dlt_compression,
	.media_load		= dlt_media_load,
	.cleaning_media		= dlt_cleaning,
	.media_handling		= media_info,
};

void init_dlt7000_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_dlt7000;
	ssc_pm.lu = lu;

	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = FALSE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = FALSE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 2;

	ssc_personality_module_register(&ssc_pm);

	init_dlt_inquiry(lu);

	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_control(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_medium_partition(lu);
	add_mode_information_exception(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	ssc_pm.native_drive_density = &density_dlt4;

	/* Capacity units in MBytes */
	((struct priv_lu_ssc *)lu->lu_private)->capacity_unit = 1L << 20;

	add_density_support(&lu->den_list, &density_dlt2, 0);
	add_density_support(&lu->den_list, &density_dlt3, 1);
	add_density_support(&lu->den_list, &density_dlt4, 1);

	add_drive_media_list(lu, LOAD_RO, "DLT3");
	add_drive_media_list(lu, LOAD_RW, "DLT4");
	add_drive_media_list(lu, LOAD_RO, "DLT4 Clean");
}

void init_dlt8000_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_dlt8000;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_dlt4;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = FALSE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = FALSE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 2;

	ssc_personality_module_register(&ssc_pm);

	init_dlt_inquiry(lu);

	add_mode_page_rw_err_recovery(lu);
	add_mode_disconnect_reconnect(lu);
	add_mode_control(lu);
	add_mode_data_compression(lu);
	add_mode_device_configuration(lu);
	add_mode_medium_partition(lu);
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

	/* Don't support PERSISTENT RESERVATION */
	register_ops(lu, PERSISTENT_RESERVE_IN, spc_illegal_op, NULL, NULL);
	register_ops(lu, PERSISTENT_RESERVE_OUT, spc_illegal_op, NULL, NULL);

	add_density_support(&lu->den_list, &density_dlt2, 0);
	add_density_support(&lu->den_list, &density_dlt3, 1);
	add_density_support(&lu->den_list, &density_dlt4, 1);
	add_drive_media_list(lu, LOAD_RO, "DLT3");
	add_drive_media_list(lu, LOAD_RW, "DLT4");
	add_drive_media_list(lu, LOAD_RO, "DLT4 Clean");
}

void init_sdlt320_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_sdlt320;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_sdlt320;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_sdlt_inquiry(lu);

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

	add_density_support(&lu->den_list, &density_sdlt, 0);
	add_density_support(&lu->den_list, &density_sdlt220, 1);
	add_density_support(&lu->den_list, &density_sdlt320, 1);
	add_drive_media_list(lu, LOAD_RO, "SDLT");
	add_drive_media_list(lu, LOAD_RW, "SDLT 220");
	add_drive_media_list(lu, LOAD_RW, "SDLT 320");
	add_drive_media_list(lu, LOAD_RO, "SDLT 320 Clean");
}

void init_sdlt600_ssc(struct lu_phy_attr *lu)
{
	ssc_pm.name = pm_name_sdlt600;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_sdlt600;
	ssc_pm.clear_WORM = clear_dlt_WORM;
	ssc_pm.set_WORM = set_dlt_WORM;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = TRUE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = TRUE;
	ssc_pm.drive_supports_SPR = TRUE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 5;

	ssc_personality_module_register(&ssc_pm);

	init_sdlt_inquiry(lu);

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

	add_density_support(&lu->den_list, &density_sdlt220, 0);
	add_density_support(&lu->den_list, &density_sdlt320, 1);
	add_density_support(&lu->den_list, &density_sdlt600, 1);
	add_drive_media_list(lu, LOAD_RO, "SDLT 220");
	add_drive_media_list(lu, LOAD_RW, "SDLT 320");
	add_drive_media_list(lu, LOAD_RO, "SDLT 320 Clean");
	add_drive_media_list(lu, LOAD_RW, "SDLT 600");
	add_drive_media_list(lu, LOAD_RO, "SDLT 600 Clean");
	add_drive_media_list(lu, LOAD_RW, "SDLT 600 WORM");
}
