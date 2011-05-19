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

static struct media_handling j1a_media_handling[] = {
	{ "j1a", "RW", medium_density_code_j1a, },
	{ "j1a", "WORM", medium_density_code_j1a, },
	{ "j1a", "ENCR", medium_density_code_j1a_ENCR, },
	};

static struct media_handling e05_media_handling[] = {
	{ "j1a", "RW", medium_density_code_j1a, },
	{ "j1a", "WORM", medium_density_code_j1a, },
	{ "j1a", "ENCR", medium_density_code_j1a_ENCR, },
	{ "e05", "RW", medium_density_code_e05, },
	{ "e05", "WORM", medium_density_code_e05, },
	{ "e05", "ENCR", medium_density_code_e05_ENCR, },
	};

static struct media_handling e06_media_handling[] = {
	{ "j1a", "RW", medium_density_code_j1a, },
	{ "j1a", "WORM", medium_density_code_j1a, },
	{ "j1a", "ENCR", medium_density_code_j1a_ENCR, },
	{ "e05", "RW", medium_density_code_e05, },
	{ "e05", "WORM", medium_density_code_e05, },
	{ "e05", "ENCR", medium_density_code_e05_ENCR, },
	{ "e06", "RW", medium_density_code_e06, },
	{ "e06", "WORM", medium_density_code_e06, },
	{ "e06", "ENCR", medium_density_code_e06_ENCR, },
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
		blockDescriptorBlock[0] = lu_priv->pm->drive_native_density;
		mam.MediumDensityCode = blockDescriptorBlock[0];
		mam.FormattedDensityCode = blockDescriptorBlock[0];
		rewriteMAM(sam_stat);
	} else {
		/* Extra check for 3592 to be sure the cartridge is
		 * formatted for encryption
		 */
		if ((lu_priv->pm->drive_type == drive_3592_E06) &&
				lu_priv->ENCRYPT_MODE &&
				!(mam.Flags & MAM_FLAGS_ENCRYPTION_FORMAT)) {
			mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT, sam_stat);
			return 0;
		}
		if ((!lu_priv->pm->drive_native_density) &&
			(mam.MediumDensityCode != lu_priv->pm->drive_native_density)) {
			mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		if (mam.MediumDensityCode != lu_priv->pm->drive_native_density) {
			switch (lu_priv->pm->drive_type) {
			case drive_3592_E05:
				if (mam.MediumDensityCode ==
						medium_density_code_j1a)
					break;
				mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT,
						sam_stat);
				return SAM_STAT_CHECK_CONDITION;
				break;
			case drive_3592_E06:
				if (mam.MediumDensityCode ==
						medium_density_code_e05)
					break;
				mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT,
						sam_stat);
				return SAM_STAT_CHECK_CONDITION;
				break;
			default:
				mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT,
						sam_stat);
				return SAM_STAT_CHECK_CONDITION;
				break;
			}
		}
	}

	return SAM_STAT_GOOD;
}

static uint8_t clear_3592_comp(void)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default clear_compression is in libvtlscsi */
	return clear_compression_mode_pg(sm);
}

static uint8_t set_3592_comp(int lvl)
{
	MHVTL_DBG(3, "+++ Trace +++");
	/* default set_compression is in libvtlscsi */
	return set_compression_mode_pg(sm, lvl);
}

static uint8_t update_3592_encryption_mode(void *p, int value)
{
	MHVTL_DBG(3, "+++ Trace +++");

	return SAM_STAT_GOOD;
}

static uint8_t set_3592_WORM(void)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", sm);
	return set_WORM(sm);
}

static uint8_t clear_3592_WORM(void)
{
	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", sm);
	return clear_WORM(sm);
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
	uint8_t worm = 1;	/* Supports WORM */
	uint8_t local_TapeAlert[8] =
			{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

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

static char *pm_name_j1a = "03592J1A";
static char *pm_name_e05 = "03592E05";
static char *pm_name_e06 = "03592E06";

static struct ssc_personality_template ssc_pm = {
	.drive_native_density	= medium_density_code_e06,
	.valid_encryption_blk	= valid_encryption_blk,
	.valid_encryption_media	= valid_encryption_media_E06,
	.update_encryption_mode	= update_3592_encryption_mode,
	.kad_validation		= e06_kad_validation,
	.check_restrictions	= check_restrictions,
	.clear_compression	= clear_3592_comp,
	.set_compression	= set_3592_comp,
	.clear_WORM		= clear_3592_WORM,
	.set_WORM		= set_3592_WORM,
	.cleaning_media		= ibm_cleaning,
};

void init_3592_j1a(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace +++");

	init_3592_inquiry(lu);
	ssc_pm.name = pm_name_j1a;
	ssc_pm.drive_type = drive_3592_J1A;
	ssc_pm.media_capabilities = j1a_media_handling;
	ssc_pm.drive_native_density = medium_density_code_j1a;
	personality_module_register(&ssc_pm);
	init_default_ssc_mode_pages(sm);
}

void init_3592_E05(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace +++");

	init_3592_inquiry(lu);
	ssc_pm.name = pm_name_e05;
	ssc_pm.drive_type = drive_3592_E05;
	ssc_pm.media_capabilities = e05_media_handling;
	ssc_pm.drive_native_density = medium_density_code_e05;
	personality_module_register(&ssc_pm);
	init_default_ssc_mode_pages(sm);
}

void init_3592_E06(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "+++ Trace +++");

	init_3592_inquiry(lu);
	ssc_pm.name = pm_name_e06;
	ssc_pm.drive_type = drive_3592_E06;
	ssc_pm.media_capabilities = e06_media_handling;
	ssc_pm.drive_native_density = medium_density_code_e06;
	personality_module_register(&ssc_pm);
	init_default_ssc_mode_pages(sm);
	lu->mode_pages = sm;
	ssc_pm.encryption_capabilities = encr_capabilities_3592;
	register_ops(lu, SECURITY_PROTOCOL_IN, ssc_spin);
	register_ops(lu, SECURITY_PROTOCOL_OUT, ssc_spout);
}

