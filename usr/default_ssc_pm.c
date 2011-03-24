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

static struct media_handling default_media_handling[] = {
	};

static uint8_t clear_deflt_comp(void)
{
	MHVTL_DBG(3, "*** Trace ***");
	/* default clear_compression is in libvtlscsi */
	return clear_compression(sm);
}

static uint8_t set_deflt_comp(int lvl)
{
	MHVTL_DBG(3, "*** Trace ***");
	/* default set_compression is in libvtlscsi */
	return set_compression(sm, lvl);
}

static uint8_t update_default_encryption_mode(void *p, int value)
{
	MHVTL_DBG(3, "*** Trace ***");

	return SAM_STAT_GOOD;
}

static uint8_t set_deflt_WORM(void)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);
	/* default set_compression is in libvtlscsi */
	return set_WORM(sm);
}

static uint8_t clear_deflt_WORM(void)
{
	MHVTL_DBG(3, "*** Trace mode pages at %p ***", sm);
	/* default set_compression is in libvtlscsi */
	return clear_WORM(sm);
}

static void init_default_inquiry(struct lu_phy_attr *lu)
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

static char *pm_name = "default emulation";

static struct ssc_personality_template ssc_pm = {
	.drive_native_density	= medium_density_code_unknown,
	.media_capabilities	= default_media_handling,
	.valid_encryption_blk	= valid_encryption_blk,
	.update_encryption_mode	= update_default_encryption_mode,
	.check_restrictions	= check_restrictions,
	.clear_compression	= clear_deflt_comp,
	.set_compression	= set_deflt_comp,
	.clear_WORM		= clear_deflt_WORM,
	.set_WORM		= set_deflt_WORM,
};

void init_default_ssc(struct lu_phy_attr *lu)
{
	MHVTL_DBG(3, "*** Trace ***");

	init_default_inquiry(lu);
	ssc_pm.name = pm_name;
	ssc_pm.drive_native_density = 0x40;
	ssc_pm.media_capabilities = NULL;
	personality_module_register(&ssc_pm);
	init_default_ssc_mode_pages(sm);
	lu->mode_pages = sm;
}

