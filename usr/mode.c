/*
 * This handles any SCSI OP 'mode sense / mode select'
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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <syslog.h>
#include <inttypes.h>
#include <errno.h>
#include "scsi.h"
#include "list.h"
#include "logging.h"
#include "vtllib.h"
#include "q.h"
#include "ssc.h"
#include "smc.h"
#include "be_byteshift.h"

static char *mode_rw_error_recover = "Read/Write Error Recovery";
static char *mode_disconnect_reconnect = "Disconnect/Reconnect";
static char *mode_control = "Control";
static char *mode_control_extension = "Control Extension";
static char *mode_data_compression = "Data Compression";
static char *mode_device_configuration = "Device Configuration";
static char *mode_device_configuration_extension =
					"Device Configuration Extension";
static char *mode_medium_partition = "Medium Partition";
static char *mode_power_condition = "Power Condition";
static char *mode_information_exception = "Information Exception";
static char *mode_medium_configuration = "Medium Configuration";
static char *mode_vendor_24h = "Vendor (IBM) unique page 24h"
				" - Advise ENCRYPTION Capable device";
static char *mode_vendor_25h = "Vendor (IBM) unique page 25h"
				" - Early Warning";
static char *mode_encryption_mode = "Encryption Mode";
static char *mode_behaviour_configuration = "Behaviour Configuration";
static char *mode_ait_device_configuration = "AIT Device Configuration";
static char *mode_element_address = "Element Address";
static char *mode_transport_geometry = "Transport Geometry";
static char *mode_device_capabilities = "Device Capabilities";
static char *drive_configuration_page = "STK Vendor-Unique Drive Configuration";

struct mode *lookup_pcode(struct list_head *m, uint8_t pcode, uint8_t subpcode)
{
	struct mode *mp;

	MHVTL_DBG(3, "Looking for: Page/subpage (%02x/%02x)",
					pcode, subpcode);

	list_for_each_entry(mp, m, siblings) {
		if (mp->pcode == pcode && mp->subpcode == subpcode) {
			MHVTL_DBG(3, "Found \"%s\" -> "
				"Page/subpage (%02x/%02x)",
					mp->description,
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
 *  byte[1] = size - sizeof(byte[0])
 *
 * Return pointer to mode structure being init. or NULL if alloc failed
 */
static struct mode *alloc_mode_page(struct list_head *m,
				uint8_t pcode, uint8_t subpcode, int size)
{
	struct mode *mp;

	MHVTL_DBG(3, "Allocating %d bytes for (%02x/%02x)",
					size, pcode, subpcode);

	mp = lookup_pcode(m, pcode, subpcode);
	if (!mp) {	/* Create a new entry */
		mp = (struct mode *)zalloc(sizeof(struct mode));
	}
	if (mp) {
		mp->pcodePointer = (uint8_t *)zalloc(size);
		if (mp->pcodePointer) {	/* If ! null, set size of data */
			mp->pcode = pcode;
			mp->subpcode = subpcode;
			mp->pcodeSize = size;

			/* Allocate a 'changeable bitmap' mode page info */
			mp->pcodePointerBitMap = zalloc(size);
			if (!mp->pcodePointerBitMap) {
				free(mp);
				MHVTL_ERR("Unable to malloc(%d)", size);
				return NULL;
			}

			list_add_tail(&mp->siblings, m);
			return mp;
		} else {
			MHVTL_ERR("Unable to malloc(%d)", size);
			free(mp);
		}
	}
	return NULL;
}

void dealloc_all_mode_pages(struct lu_phy_attr *lu)
{
	struct mode *mp, *mn;

	list_for_each_entry_safe(mp, mn, &lu->mode_pg, siblings) {
		MHVTL_DBG(2, "Removing %s", mp->description);
		free(mp->pcodePointer);
		free(mp->pcodePointerBitMap);
		list_del(&mp->siblings);
		free(mp);
	}
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
	uint8_t subpcode;
	uint8_t size;

	pcode = MODE_RW_ERROR_RECOVER;
	subpcode = 0;
	size = 12;

	mode_pg = &lu->mode_pg;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_rw_error_recover, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->description = mode_rw_error_recover;

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
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_DISCONNECT_RECONNECT;
	subpcode = 0;
	size = 0x0e + 2;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_disconnect_reconnect, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[2] = 50;	/* Buffer full ratio */
	mp->pcodePointer[3] = 50;	/* Buffer empty ratio */
	mp->pcodePointer[10] = 4;

	mp->description = mode_disconnect_reconnect;

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
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_CONTROL;
	subpcode = 0;
	size = 12;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_control, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->description = mode_control;

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
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_CONTROL;
	subpcode = 1;
	size = 0x1c;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_control_extension, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->description = mode_control_extension;

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
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_DATA_COMPRESSION;
	subpcode = 0;
	size = 16;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_data_compression, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[2] = 0xc0;	/* Set data compression enable */
	mp->pcodePointer[3] = 0x80;	/* Set data decompression enable */
	put_unaligned_be32(COMPRESSION_TYPE, &mp->pcodePointer[4]);
	put_unaligned_be32(COMPRESSION_TYPE, &mp->pcodePointer[8]);

	/* Changeable fields */
	mp->pcodePointerBitMap[2] = 0xc0; /* DCE & DCC */
	mp->pcodePointerBitMap[3] = 0x80; /* DDE bit */
	put_unaligned_be32(0xffffffff, &mp->pcodePointer[4]); /* Comp alg */
	put_unaligned_be32(0xffffffff, &mp->pcodePointer[8]); /* De-comp alg */

	mp->description = mode_data_compression;

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
	uint8_t subpcode;
	uint8_t size;

	ssc = (struct priv_lu_ssc *)lu->lu_private;
	mode_pg = &lu->mode_pg;
	pcode = MODE_DEVICE_CONFIGURATION;
	subpcode = 0;
	size = 16;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_device_configuration, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[7] = 0x64;	/* Write delay (100mS intervals) */
	mp->pcodePointer[8] = 0x40;	/* Block Identifiers supported */
	mp->pcodePointer[10] = 0x18;	/* Enable EOD & Sync at early warning */
	mp->pcodePointer[14] = ssc->configCompressionFactor;
	mp->pcodePointer[15] = 0x80;	/* WTRE (WORM handling) */

	mp->pcodePointerBitMap[14] = 0xff;	/* Compression is changeable */

	/* Set pointer for compressionFactor to correct location in
	 * mode page struct
	 */
	ssc->compressionFactor = &mp->pcodePointer[14];

	mp->description = mode_device_configuration;

	return 0;
}

int add_mode_device_configuration_extention(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct priv_lu_ssc *ssc;
	struct ssc_personality_template *pm;
	struct mode *mp;
	uint8_t pcode;
	uint8_t subpcode;
	uint8_t size;

	/* Only for TAPE (SSC) devices */
	if (lu->ptype != TYPE_TAPE)
		return -ENOTTY;

	ssc = lu->lu_private;
	pm = ssc->pm;

	mode_pg = &lu->mode_pg;
	pcode = MODE_DEVICE_CONFIGURATION;
	subpcode = 0x01;
	size = 32;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			mode_device_configuration_extension, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[5] = 0x02;	/* Short erase mode  - write EOD */

	/* default size of early warning */
	put_unaligned_be16(0, &mp->pcodePointer[6]);

	/* Update mode page bitmap to reflect changeable fields */
	if (pm->drive_supports_append_only_mode)
		mp->pcodePointerBitMap[5] |= 0xf0;
	if (pm->drive_supports_prog_early_warning) {
		mp->pcodePointerBitMap[6] |= 0xff;
		mp->pcodePointerBitMap[7] |= 0xff;
	}

	mp->description = mode_device_configuration_extension;

	return 0;
}

int add_mode_medium_partition(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_MEDIUM_PARTITION;
	subpcode = 0;
	size = 16;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_medium_partition, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* FDP (Fixed Data Partitions) |
	 *	PSUM (partition size unit of measure) |
	 *	POFM (partition on Format Medium) */
	mp->pcodePointerBitMap[4] = 0x9c;
	/* Medium Format Recognition */
	mp->pcodePointerBitMap[5] = 0x03;
	mp->pcodePointerBitMap[6] = 0x09;
	mp->pcodePointerBitMap[8] = 0x03;
	mp->pcodePointerBitMap[9] = 0x5a;

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];
	mp->pcodePointerBitMap[4] = mp->pcodePointer[4];
	mp->pcodePointerBitMap[5] = mp->pcodePointer[5];
	mp->pcodePointerBitMap[6] = mp->pcodePointer[6];
	mp->pcodePointerBitMap[8] = mp->pcodePointer[8];
	mp->pcodePointerBitMap[9] = mp->pcodePointer[9];

	mp->description = mode_medium_partition;

	return 0;
}

int add_mode_power_condition(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_POWER_CONDITION;
	subpcode = 0;
	size = 0x26;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_power_condition, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->description = mode_power_condition;

	return 0;
}

int add_mode_information_exception(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_INFORMATION_EXCEPTION;
	subpcode = 0;
	size = 12;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_information_exception, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[2] = 0x08;
	mp->pcodePointer[3] = 0x03;

	mp->description = mode_information_exception;

	return 0;
}

int add_mode_medium_configuration(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_MEDIUM_CONFIGURATION;
	subpcode = 0;
	size = 32;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_medium_configuration, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[4] = 0x01;	/* WORM mode label restrictions */
	mp->pcodePointer[5] = 0x01;	/* WORM mode filemark restrictions */

	mp->description = mode_medium_configuration;

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
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_VENDOR_SPECIFIC_24H;
	subpcode = 0;
	size = 8;

	/* Vendor Unique (IBM Ultrium)
	 * Page 151, table 118
	 * Advise ENCRYPTION Capable device
	 */
	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_vendor_24h, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[7] = ENCR_C;

	mp->description = mode_vendor_24h;

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
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_VENDOR_SPECIFIC_25H;
	subpcode = 0;
	size = 32;

	/* Vendor Unique (IBM Ultrium)
	 * Page 151, table 118
	 * Advise ENCRYPTION Capable device
	 */
	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_vendor_25h, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[5] = 1;	/* LEOP to maximize medium capacity */
	mp->pcodePointer[6] = 1;	/* Early Warning */

	mp->description = mode_vendor_25h;

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

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_encryption_mode, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	/* Application Managed Encryption */
	mp->pcodePointer[5] = 0x03;	/* Encryption Solution Method */
	mp->pcodePointer[6] = 0x01;	/* Key Path */
	mp->pcodePointer[7] = 0x01;	/* Default Encruption State */
	mp->pcodePointer[8] = 0x00;	/* Desnity Reporting */

	mp->description = mode_encryption_mode;

	return 0;
}

int add_mode_ait_device_configuration(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_AIT_DEVICE_CONFIGURATION;
	subpcode = 0;
	size = 8;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_ait_device_configuration, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[2] = 0xf0;
	mp->pcodePointer[3] = 0x0a;
	mp->pcodePointer[4] = 0x40;

	mp->description = mode_ait_device_configuration;

	return 0;
}

int add_mode_element_address_assignment(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	static struct smc_priv *smc_p;
	uint8_t pcode;
	uint8_t subpcode;
	uint8_t size;
	uint8_t *p;

	mode_pg = &lu->mode_pg;
	smc_p = (struct smc_priv *)lu->lu_private;
	pcode = MODE_ELEMENT_ADDRESS;
	subpcode = 0;
	size = 20;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_element_address, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	p = mp->pcodePointer;

	p[0] = pcode;
	p[1] = size - sizeof(p[0]) - sizeof(p[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	put_unaligned_be16(smc_p->pm->start_picker, &p[2]);
	put_unaligned_be16(smc_p->num_picker, &p[4]);
	put_unaligned_be16(smc_p->pm->start_storage, &p[6]);
	put_unaligned_be16(smc_p->num_storage, &p[8]);
	put_unaligned_be16(smc_p->pm->start_map, &p[10]);
	put_unaligned_be16(smc_p->num_map, &p[12]);
	put_unaligned_be16(smc_p->pm->start_drive, &p[14]);
	put_unaligned_be16(smc_p->num_drives, &p[16]);

	mp->description = mode_element_address;

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
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_TRANSPORT_GEOMETRY;
	subpcode = 0;
	size = 4;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_transport_geometry, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->description = mode_transport_geometry;

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
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_DEVICE_CAPABILITIES;
	subpcode = 0;
	size = 20;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_device_capabilities, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

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

	mp->description = mode_device_capabilities;

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
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	pcode = MODE_BEHAVIOR_CONFIGURATION;
	subpcode = 0;
	size = 10;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				mode_behaviour_configuration, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[3] = 0; /* Clean Behavior */
	mp->pcodePointer[4] = 0; /* WORM Behavior */

	mp->description = mode_behaviour_configuration;

	return 0;
}

int update_prog_early_warning(struct lu_phy_attr *lu)
{
	uint8_t *mp;
	struct mode *m;
	struct list_head *mode_pg;
	struct priv_lu_ssc *lu_priv;

	mode_pg = &lu->mode_pg;
	lu_priv = (struct priv_lu_ssc *)lu->lu_private;

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

int add_smc_mode_page_drive_configuration(struct lu_phy_attr *lu)
{
	struct list_head *mode_pg;
	struct mode *mp;
	uint8_t pcode;
	uint8_t subpcode;
	uint8_t size;

	mode_pg = &lu->mode_pg;
	/* A Vendor-specific page for the StorageTek L20, L40 and L80 libraries */
	pcode = 0x2d;
	subpcode = 0;
	size = 0x26;
/*
 * FIXME: Need to fill in details from Table 4-21 L20 SCSI Reference Manual
 */

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
				drive_configuration_page, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size
				 - sizeof(mp->pcodePointer[0])
				 - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->description = drive_configuration_page;

	return 0;
}

