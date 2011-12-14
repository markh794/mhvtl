/*
 * This handles any SCSI OP 'mode sense / mode select'
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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <syslog.h>
#include <inttypes.h>
#include <errno.h>
#include "scsi.h"
#include "list.h"
#include "vtllib.h"
#include "ssc.h"
#include "smc.h"
#include "be_byteshift.h"

struct mode *lookup_pcode(struct list_head *m, uint8_t pcode, uint8_t subpcode)
{
	struct mode *mp;

	MHVTL_DBG(3, "Looking for: pcode 0x%02x, subpcode 0x%02x",
					pcode, subpcode);

	list_for_each_entry(mp, m, siblings) {
		if (mp->pcode == pcode && mp->subpcode == subpcode) {
			MHVTL_DBG(3, "Matched list entry -> "
				"pcode 0x%02x, subpcode 0x%02x",
					mp->pcode, mp->subpcode);
			return mp;
		}
	}

	MHVTL_DBG(3, "Page/subpage code 0x%02x/0x%02x not found",
					pcode, subpcode);

	return NULL;
}

/*
 * Used by mode sense/mode select struct.
 *
 * Allocate 'size' bytes & init to 0
 * set first 2 bytes:
 *  byte[0] = pcode
 *  byte[1] = size - sizeof(byte[0]
 *
 * Return pointer to mode structure being init. or NULL if alloc failed
 */
struct mode *alloc_mode_page(struct list_head *m,
				uint8_t pcode, uint8_t subpcode, int size)
{
	struct mode *mp;

	MHVTL_DBG(3, "%p : Allocate mode page 0x%02x, size %d", m, pcode, size);

	mp = lookup_pcode(m, pcode, subpcode);
	if (!mp) {	/* Create a new entry */
		mp = malloc(sizeof(struct mode));
	}
	if (mp) {
		mp->pcodePointer = malloc(size);
		MHVTL_DBG(3, "pcodePointer: %p for mode page 0x%02x",
			mp->pcodePointer, pcode);
		if (mp->pcodePointer) {	/* If ! null, set size of data */
			memset(mp->pcodePointer, 0, size);
			mp->pcode = pcode;
			mp->subpcode = subpcode;
			mp->pcodeSize = size;
			list_add_tail(&mp->siblings, m);
			return mp;
		} else {
			MHVTL_ERR("Unable to malloc(%d)", size);
			free(mp);
		}
	}
	return NULL;
}

/*
 * READ/WRITE Error Recovery
 * SSC3-8.3.5
 */
int add_mode_page_rw_err_recovery(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	pcode = MODE_RW_ERROR_RECOVER;
	size = 12;

	mode_pg = &lu->mode_pg;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	return 0;
}

/*
 * Disconnect / Reconnect
 * SPC3-7.4.8
 */
int add_mode_disconnect_reconnect(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_DISCONNECT_RECONNECT;
	size = 12;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);
	mp->pcodePointer[2] = 50;	/* Buffer full ratio */
	mp->pcodePointer[3] = 50;	/* Buffer empty ratio */
	mp->pcodePointer[10] = 4;

	return 0;
}

/*
 * Control
 * SPC3
 */
int add_mode_control(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_CONTROL;
	size = 12;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	return 0;
}

/*
 * Control Extension
 * SPC3
 */
int add_mode_control_extension(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_CONTROL;
	size = 0x1c;

	mp = alloc_mode_page(mode_pg, pcode, 1, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	return 0;
}

/*
 * Data Compression
 * SSC3-8.3.2
 */
#define COMPRESSION_TYPE 0x10

int add_mode_data_compression(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_DATA_COMPRESSION;
	size = 16;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);
	mp->pcodePointer[2] = 0xc0;	/* Set data compression enable */
	mp->pcodePointer[3] = 0x80;	/* Set data decompression enable */
	put_unaligned_be32(COMPRESSION_TYPE, &mp->pcodePointer[4]);
	put_unaligned_be32(COMPRESSION_TYPE, &mp->pcodePointer[8]);

	return 0;
}

/*
 * Device Configuration
 * SSC3-8.3.3
 */

int add_mode_device_configuration(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	struct priv_lu_ssc *ssc;
	uint8_t pcode;
	uint8_t size;

	ssc = lu->lu_private;
	mode_pg = &lu->mode_pg;
	pcode = MODE_DEVICE_CONFIGURATION;
	size = 16;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);
	mp->pcodePointer[7] = 0x64;	/* Write delay (100mS intervals) */
	mp->pcodePointer[8] = 0x40;	/* Block Identifiers supported */
	mp->pcodePointer[10] = 0x18;	/* Enable EOD & Sync at early warning */
	mp->pcodePointer[14] = ssc->configCompressionFactor;
	mp->pcodePointer[15] = 0x80;	/* WTRE (WORM handling) */

	/* Set pointer for compressionFactor to correct location in
	 * mode page struct
	 */
	ssc->compressionFactor = &mp->pcodePointer[14];

	return 0;
}

int add_mode_device_configuration_extention(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_DEVICE_CONFIGURATION;
	subpcode = 0x01;
	size = 32;

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	mp->pcodePointer[5] = 0x02;	/* Short erase mode  - write EOD */

	/* default size of early warning */
	put_unaligned_be16(0, &mp->pcodePointer[6]);

	return 0;
}

int add_mode_medium_partition(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_MEDIUM_PARTITION;
	size = 16;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	return 0;
}

int add_mode_power_condition(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_POWER_CONDITION;
	size = 0x26;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	return 0;
}

int add_mode_information_exception(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_INFORMATION_EXCEPTION;
	size = 12;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);
	mp->pcodePointer[2] = 0x08;
	mp->pcodePointer[3] = 0x03;

	return 0;
}

int add_mode_medium_configuration(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_MEDIUM_CONFIGURATION;
	size = 32;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	mp->pcodePointer[4] = 0x01;	/* WORM mode label restrictions */
	mp->pcodePointer[5] = 0x01;	/* WORM mode filemark restrictions */
	return 0;
}

/*
 * Initialise structure data for mode pages.
 * - Allocate memory for each mode page & init to 0
 * - Set up size of mode page
 * - Set initial values of mode pages
 *
 * Return void  - Nothing
 */
int add_mode_ult_encr_mode_pages(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;	/* Mode Page list */
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_VENDOR_SPECIFIC_24H;
	size = 8;

	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", mode_pg);

	/* Vendor Unique (IBM Ultrium)
	 * Page 151, table 118
	 * Advise ENCRYPTION Capable device
	 */
	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);
	mp->pcodePointer[7] = ENCR_C;
	return 0;
}

/*
 * Initialise structure data for mode pages.
 * - Allocate memory for each mode page & init to 0
 * - Set up size of mode page
 * - Set initial values of mode pages
 *
 * Return void  - Nothing
 */
int add_mode_vendor_25h_mode_pages(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;	/* Mode Page list */
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_VENDOR_SPECIFIC_25H;
	size = 32;

	MHVTL_DBG(3, "+++ Trace mode pages at %p +++", mode_pg);

	/* Vendor Unique (IBM Ultrium)
	 * Page 151, table 118
	 * Advise ENCRYPTION Capable device
	 */
	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);
	mp->pcodePointer[5] = 1;	/* LEOP to maximize medium capacity */
	mp->pcodePointer[6] = 1;	/* Early Warning */

	return 0;
}

int add_mode_encryption_mode_attribute(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_ENCRYPTION_MODE;
	subpcode = 0x20;
	size = 9;

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* Application Managed Encryption */
	mp->pcodePointer[5] = 0x03;	/* Encryption Solution Method */
	mp->pcodePointer[6] = 0x01;	/* Key Path */
	mp->pcodePointer[7] = 0x01;	/* Default Encruption State */
	mp->pcodePointer[8] = 0x00;	/* Desnity Reporting */

	return 0;
}

int add_mode_ait_device_configuration(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_AIT_DEVICE_CONFIGURATION;
	size = 8;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);
	mp->pcodePointer[2] = 0xf0;
	mp->pcodePointer[3] = 0x0a;
	mp->pcodePointer[4] = 0x40;

	return 0;
}

int add_mode_element_address_assignment(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	static struct smc_priv *smc_slots;
	uint8_t pcode;
	uint8_t size;
	uint8_t *p;

	mode_pg = &lu->mode_pg;
	smc_slots = lu->lu_private;
	pcode = MODE_ELEMENT_ADDRESS;
	size = 20;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	p = mp->pcodePointer;

	p[0] = pcode;
	p[1] = size - sizeof(p[0]) - sizeof(p[1]);

	put_unaligned_be16(START_PICKER, &p[2]); /* First transport. */
	put_unaligned_be16(smc_slots->num_picker, &p[4]);
	put_unaligned_be16(START_STORAGE, &p[6]); /* First storage */
	put_unaligned_be16(smc_slots->num_storage, &p[8]);
	put_unaligned_be16(START_MAP, &p[10]); /* First i/e address */
	put_unaligned_be16(smc_slots->num_map, &p[12]);
	put_unaligned_be16(START_DRIVE, &p[14]); /* First Drives */
	put_unaligned_be16(smc_slots->num_drives, &p[16]);

	return 0;
}


/*
 * Transport Geometry Parameters mode page
 * SMC-3 7.3.4
 */
int add_mode_transport_geometry(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_TRANSPORT_GEOMETRY;
	size = 4;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	return 0;
}

/*
 * Device Capabilities mode page:
 * SMC-3 7.3.2
 */
int add_mode_device_capabilities(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_DEVICE_CAPABILITIES;
	size = 20;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	mp->pcodePointer[2] = 0x0f;
	mp->pcodePointer[3] = 0x07;
	mp->pcodePointer[4] = 0x0f;
	mp->pcodePointer[5] = 0x0f;
	mp->pcodePointer[6] = 0x0f;
	mp->pcodePointer[7] = 0x0f;
	/* [8-11] -> reserved */
	mp->pcodePointer[12] = 0x00;
	mp->pcodePointer[13] = 0x00;
	mp->pcodePointer[14] = 0x00;
	mp->pcodePointer[15] = 0x00;
	/* [16-19] -> reserved */

	return 0;
}

/*
 * Behavior Configuration Mode Page
 * IBM Ultrium SCSI Reference - 9th Edition
 */
int add_mode_behavior_configuration(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_BEHAVIOR_CONFIGURATION;
	size = 10;

	mp = alloc_mode_page(mode_pg, pcode, 0, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	mp->pcodePointer[3] = 0; /* Clean Behavior */
	mp->pcodePointer[4] = 0; /* WORM Behavior */

	return 0;
}

int update_prog_early_warning(struct lu_phy_attr *lu)
{
	uint8_t *mp;
	struct mode *m;
	struct list_head *mode_pg;
	struct priv_lu_ssc *lu_priv;

	mode_pg = &lu->mode_pg;
	lu_priv = lu->lu_private;

	m = lookup_pcode(mode_pg, MODE_DEVICE_CONFIGURATION, 1);
	MHVTL_DBG(3, "l: %p, m: %p, m->pcodePointer: %p",
			mode_pg, m, m->pcodePointer);
	if (m) {
		mp = m->pcodePointer;
		if (!mp)
			return SAM_STAT_GOOD;

		put_unaligned_be16(lu_priv->prog_early_warning_sz, &mp[6]);
	}
	return SAM_STAT_GOOD;
}
