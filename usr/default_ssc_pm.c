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

static struct density_info density_default = {
	1024, 127, 1, 500, medium_density_code_unknown,
			"mhVTL", "DEFAULT", "linuxVTL" };

static struct name_to_media_info media_info[] = {
	{"Undefined", Media_undefined,
			media_type_unknown, medium_density_code_unknown},

	/* Ultrium media */
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

	/* IBM 03592 media */
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

	/* AIT media */
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

	/* STK 9x40 media */
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

	/* STK T10000 media */
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

	/* Quantum DLT / SDLT media */
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

	/* 4MM DAT media */
	{"DDS1", Media_DDS1,
			media_type_unknown, medium_density_code_DDS1},
	{"DDS2", Media_DDS2,
			media_type_unknown, medium_density_code_DDS2},
	{"DDS3", Media_DDS3,
			media_type_unknown, medium_density_code_DDS3},
	{"DDS4", Media_DDS4,
			media_type_unknown, medium_density_code_DDS4},
	{"DDS5", Media_DDS5,
			media_type_unknown, medium_density_code_DDS5},
	{"", 0, 0, 0},
};

static uint8_t clear_default_comp(struct list_head *l)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default clear_compression is in libvtlscsi */
	return clear_compression_mode_pg(l);
}

static uint8_t set_default_comp(struct list_head *l, int lvl)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default set_compression is in libvtlscsi */
	return set_compression_mode_pg(l, lvl);
}

static uint8_t update_default_encryption_mode(struct list_head *m, void *p, int value)
{
	MHVTL_DBG(3, "+++ Trace +++");

	return SAM_STAT_GOOD;
}

static uint8_t set_default_WORM(struct list_head *l)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", l);
	return set_WORM(l);
}

static uint8_t clear_default_WORM(struct list_head *l)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", l);
	return clear_WORM(l);
}

static void init_default_inquiry(struct lu_phy_attr *lu)
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

/* Dummy routine. Always return false */
static int default_kad_validation(int mode, int ukad, int akad)
{
	return FALSE;
}

/* Some comments before I forget how this is supose to work..
 - cleaning_media_state is either
   0 - Not mounted
   1 - Cleaning media mounted -> return Cleaning cartridge installed
   2 - Cleaning media mounted -> return Cause not reportable
   3 - Cleaning media mounted -> return Initializing command required

 On cleaning media mount, default_cleaning() is called which:
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

static uint8_t default_media_load(struct lu_phy_attr *lu, int load)
{
	MHVTL_DBG(3, "+++ Trace +++ %s", (load) ? "load" : "unload");
	return 0;
}

static uint8_t default_cleaning(void *ssc_priv)
{
	struct priv_lu_ssc *ssc;

	MHVTL_DBG(3, "+++ Trace +++");

	ssc = ssc_priv;

	ssc->cleaning_media_state = &cleaning_media_state;
	cleaning_media_state = CLEAN_MOUNT_STAGE1;

	set_cleaning_timer(30);

	return 0;
}

static void init_default_mode_pages(struct lu_phy_attr *lu)
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

static char *pm_name = "default emulation";

static struct ssc_personality_template ssc_pm = {
	.valid_encryption_blk	= valid_encryption_blk,
	.update_encryption_mode	= update_default_encryption_mode,
	.kad_validation		= default_kad_validation,
	.check_restrictions	= check_restrictions,
	.clear_compression	= clear_default_comp,
	.set_compression	= set_default_comp,
	.clear_WORM		= clear_default_WORM,
	.set_WORM		= set_default_WORM,
	.media_load		= default_media_load,
	.cleaning_media		= default_cleaning,
	.media_handling		= media_info,
};

void init_default_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace +++");

	ssc_pm.name = pm_name;
	ssc_pm.lu = lu;
	ssc_pm.native_drive_density = &density_default;
	ssc_pm.drive_supports_append_only_mode = FALSE;
	ssc_pm.drive_supports_early_warning = FALSE;
	ssc_pm.drive_supports_prog_early_warning = FALSE;
	ssc_pm.drive_supports_WORM = FALSE;
	ssc_pm.drive_supports_SPR = FALSE;
	ssc_pm.drive_supports_SP = FALSE;
	ssc_pm.drive_ANSI_VERSION = 2;

	ssc_personality_module_register(&ssc_pm);

	init_default_inquiry(lu);

	init_default_mode_pages(lu);

	add_log_write_err_counter(lu);
	add_log_read_err_counter(lu);
	add_log_sequential_access(lu);
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
	add_log_tape_usage(lu);
	add_log_tape_capacity(lu);
	add_log_data_compression(lu);

	add_density_support(&lu->den_list, &density_default, 1);

	/* LTO media */
	add_drive_media_list(lu, LOAD_RW, "LTO1");
	add_drive_media_list(lu, LOAD_RW, "LTO2");
	add_drive_media_list(lu, LOAD_RW, "LTO3");
	add_drive_media_list(lu, LOAD_RW, "LTO4");
	add_drive_media_list(lu, LOAD_RW, "LTO5");

	/* DDS media */
	add_drive_media_list(lu, LOAD_RW, "DDS1");
	add_drive_media_list(lu, LOAD_RW, "DDS2");
	add_drive_media_list(lu, LOAD_RW, "DDS3");
	add_drive_media_list(lu, LOAD_RW, "DDS4");
	add_drive_media_list(lu, LOAD_RW, "DDS5");

	/* DLT media */
	add_drive_media_list(lu, LOAD_RW, "DLT2");
	add_drive_media_list(lu, LOAD_RW, "DLT3");
	add_drive_media_list(lu, LOAD_RW, "DLT4");
	add_drive_media_list(lu, LOAD_RW, "SDLT");
	add_drive_media_list(lu, LOAD_RW, "SDLT 220");
	add_drive_media_list(lu, LOAD_RW, "SDLT 320");
	add_drive_media_list(lu, LOAD_RW, "SDLT 600");

	/* STK T10000 */
	add_drive_media_list(lu, LOAD_RW, "T10KA");
	add_drive_media_list(lu, LOAD_RW, "T10KB");
	add_drive_media_list(lu, LOAD_RW, "T10KC");

	/* STK 9x40 */
	add_drive_media_list(lu, LOAD_RW, "9840A");
	add_drive_media_list(lu, LOAD_RW, "9840B");
	add_drive_media_list(lu, LOAD_RW, "9840C");
	add_drive_media_list(lu, LOAD_RW, "9840D");
	add_drive_media_list(lu, LOAD_RW, "9940A");
	add_drive_media_list(lu, LOAD_RW, "9940B");

	/* AIT media */
	add_drive_media_list(lu, LOAD_RW, "AIT1");
	add_drive_media_list(lu, LOAD_RW, "AIT2");
	add_drive_media_list(lu, LOAD_RW, "AIT3");
	add_drive_media_list(lu, LOAD_RW, "AIT4");

	/* IBM 03592 series */
	add_drive_media_list(lu, LOAD_RW, "03592 JA");
	add_drive_media_list(lu, LOAD_RW, "03592 JB");
	add_drive_media_list(lu, LOAD_RW, "03592 JC");

	/* Don't support PERSISTENT RESERVATION */
	register_ops(lu, PERSISTENT_RESERVE_IN, spc_illegal_op, NULL, NULL);
	register_ops(lu, PERSISTENT_RESERVE_OUT, spc_illegal_op, NULL, NULL);
}
