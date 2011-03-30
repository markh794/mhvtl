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
 * Mode Pages defined for IBM Ultrium-TD4
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
	{0x24, 0x00, 0x00, NULL, }, /* Vendor Specific (IBM Ultrium) */
	{0x00, 0x00, 0x00, NULL, }, /* NULL terminator */
	};

static struct media_handling ult1_media_handling[] = {
	{ "LTO-1", "RW", medium_density_code_lto1, },
	};

static struct media_handling ult2_media_handling[] = {
	{ "LTO-1", "RW", medium_density_code_lto1, },
	{ "LTO-2", "RW", medium_density_code_lto2, },
	};

static struct media_handling ult3_media_handling[] = {
	{ "LTO-1", "RO", medium_density_code_lto1, },
	{ "LTO-2", "RW", medium_density_code_lto2, },
	{ "LTO-3", "RW", medium_density_code_lto3, },
	{ "LTO-3", "WORM", medium_density_code_lto3_WORM, },
	};

static struct media_handling ult4_media_handling[] = {
	{ "LTO-2", "RO", medium_density_code_lto2, },
	{ "LTO-3", "RW", medium_density_code_lto3, },
	{ "LTO-3", "WORM", medium_density_code_lto3_WORM, },
	{ "LTO-4", "RW", medium_density_code_lto4, },
	{ "LTO-4", "ENCR", medium_density_code_lto4, },
	{ "LTO-4", "WORM", medium_density_code_lto4_WORM, },
	};

static struct media_handling ult5_media_handling[] = {
	{ "LTO-3", "RO", medium_density_code_lto3, },
	{ "LTO-3", "WORM", medium_density_code_lto3_WORM, },
	{ "LTO-4", "RW", medium_density_code_lto4, },
	{ "LTO-4", "ENCR", medium_density_code_lto4, },
	{ "LTO-4", "WORM", medium_density_code_lto4_WORM, },
	{ "LTO-5", "RW", medium_density_code_lto5, },
	{ "LTO-5", "ENCR", medium_density_code_lto5, },
	{ "LTO-5", "WORM", medium_density_code_lto5_WORM, },
	};

/*
 * Initialise structure data for mode pages.
 * - Allocate memory for each mode page & init to 0
 * - Set up size of mode page
 * - Set initial values of mode pages
 *
 * Return void  - Nothing
 */
static void init_ult_mode_pages(struct lu_phy_attr *lu, struct mode *m)
{
	struct mode *mp;

	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);

	mp = alloc_mode_page(0x24, m, 6);
	MHVTL_DBG(3, "smp: %p", mp);
}

/*
 * Initialise structure data for mode pages.
 * - Allocate memory for each mode page & init to 0
 * - Set up size of mode page
 * - Set initial values of mode pages
 *
 * Return void  - Nothing
 */
static void init_ult_encr_mode_pages(struct lu_phy_attr *lu, struct mode *m)
{
	struct mode *mp;

	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);

	/* Vendor Unique (IBM Ultrium)
	 * Page 151, table 118
	 * Advise ENCRYPTION Capable device
	 */
	mp = alloc_mode_page(0x24, m, 6);
	if (mp)
		mp->pcodePointer[5] = ENCR_C;
}

static uint8_t clear_ult_compression(void)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);
	/* default clear_compression is in libvtlscsi */
	return clear_compression_mode_pg(sm);
}

static uint8_t set_ult_compression(int lvl)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);
	/* default set_compression is in libvtlscsi */
	return set_compression_mode_pg(sm, lvl);
}

static uint8_t set_ult_WORM(void)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);
	return set_WORM(sm);
}

static uint8_t clear_ult_WORM(void)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);
	return clear_WORM(sm);
}

static uint8_t update_ult_encryption_mode(void *p, int value)
{
	struct mode *smp;

	MHVTL_DBG(3, "*** Trace ***");

	smp = find_pcode(0x24, sm);
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

static char *pm_name_lto1 = "LTO-1";
static char *pm_name_lto2 = "LTO-2";
static char *pm_name_lto3 = "LTO-3";
static char *pm_name_lto4 = "LTO-4";
static char *pm_name_lto5 = "LTO-5";

static struct ssc_personality_template ssc_pm = {
	.valid_encryption_blk	= valid_encryption_blk, /* default in ssc.c */
	.check_restrictions	= check_restrictions, /* default in ssc.c */
	.clear_compression	= clear_ult_compression,
	.set_compression	= set_ult_compression,
};

static struct ssc_personality_template ult4_ssc_pm = {
	.valid_encryption_blk	= valid_encryption_blk, /* default in ssc.c */
	.update_encryption_mode	= update_ult_encryption_mode,
	.encryption_capabilities = encr_capabilities_ult,
	.check_restrictions	= check_restrictions, /* default in ssc.c */
	.clear_compression	= clear_ult_compression,
	.set_compression	= set_ult_compression,
	.clear_WORM		= clear_ult_WORM,
	.set_WORM		= set_ult_WORM,
};

void init_ult3580_td1(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);

	init_ult_inquiry(lu);
	ssc_pm.name = pm_name_lto1;
	ssc_pm.drive_native_density = medium_density_code_lto1;
	ssc_pm.media_capabilities = ult1_media_handling;
	personality_module_register(&ssc_pm);
	init_default_ssc_mode_pages(sm);
	init_ult_mode_pages(lu, sm);
	lu->mode_pages = sm;
}

void init_ult3580_td2(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);

	init_ult_inquiry(lu);
	ssc_pm.name = pm_name_lto2;
	ssc_pm.drive_native_density = medium_density_code_lto2;
	ssc_pm.media_capabilities = ult2_media_handling;
	personality_module_register(&ssc_pm);
	init_default_ssc_mode_pages(sm);
	init_ult_mode_pages(lu, sm);
	lu->mode_pages = sm;
}

void init_ult3580_td3(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);

	init_ult_inquiry(lu);
	ssc_pm.name = pm_name_lto3;
	ssc_pm.drive_native_density = medium_density_code_lto2;
	ssc_pm.media_capabilities = ult3_media_handling;
	ssc_pm.clear_WORM	= clear_ult_WORM;
	ssc_pm.set_WORM		= set_ult_WORM;
	personality_module_register(&ssc_pm);
	init_default_ssc_mode_pages(sm);
	init_ult_mode_pages(lu, sm);
	lu->mode_pages = sm;
}

void init_ult3580_td4(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);

	init_ult_inquiry(lu);
	ult4_ssc_pm.name = pm_name_lto4;
	ult4_ssc_pm.drive_native_density = medium_density_code_lto4;
	ult4_ssc_pm.media_capabilities = ult4_media_handling;
	personality_module_register(&ult4_ssc_pm);
	init_default_ssc_mode_pages(sm);
	init_ult_encr_mode_pages(lu, sm);
	lu->mode_pages = sm;
	register_ops(lu, SECURITY_PROTOCOL_IN, ssc_spin);
	register_ops(lu, SECURITY_PROTOCOL_OUT, ssc_spout);
}

void init_ult3580_td5(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);

	init_ult_inquiry(lu);
	ult4_ssc_pm.name = pm_name_lto5;
	ult4_ssc_pm.drive_native_density = medium_density_code_lto5;
	ult4_ssc_pm.media_capabilities = ult5_media_handling;
	personality_module_register(&ult4_ssc_pm);
	init_default_ssc_mode_pages(sm);
	init_ult_encr_mode_pages(lu, sm);
	lu->mode_pages = sm;
	register_ops(lu, SECURITY_PROTOCOL_IN, ssc_spin);
	register_ops(lu, SECURITY_PROTOCOL_OUT, ssc_spout);
}

