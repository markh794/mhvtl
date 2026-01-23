/*
 * This handles any SCSI OP 'mode sense / mode select'
 *
 * Copyright (C) 2005 - 2025 Mark Harvey markh794 at gmail dot com
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
#include <inttypes.h>
#include <errno.h>
#include "mhvtl_scsi.h"
#include "mhvtl_list.h"
#include "logging.h"
#include "vtllib.h"
#include "ssc.h"
#include "smc.h"
#include "be_byteshift.h"
#include "mode.h"
#include "mhvtl_log.h"

static char *mode_rw_error_recover		  = "Read/Write Error Recovery";
static char *mode_disconnect_reconnect	  = "Disconnect/Reconnect";
static char *mode_control				  = "Control";
static char *mode_control_extension		  = "Control Extension";
static char *mode_control_data_protection = "Control Data Protection";
static char *mode_data_compression		  = "Data Compression";
static char *mode_device_configuration	  = "Device Configuration";
static char *mode_device_configuration_extension =
	"Device Configuration Extension";
static char *mode_medium_partition		   = "Medium Partition";
static char *mode_power_condition		   = "Power Condition";
static char *mode_information_exception	   = "Information Exception";
static char *mode_medium_configuration	   = "Medium Configuration";
static char *mode_vendor_24h			   = "Vendor (IBM) unique page 24h"
											 " - Advise ENCRYPTION Capable device";
static char *mode_vendor_25h			   = "Vendor (IBM) unique page 25h"
											 " - Early Warning";
static char *mode_encryption_mode		   = "Encryption Mode";
static char *mode_behaviour_configuration  = "Behaviour Configuration";
static char *mode_ait_device_configuration = "AIT Device Configuration";
static char *mode_element_address		   = "Element Address";
static char *mode_transport_geometry	   = "Transport Geometry";
static char *mode_device_capabilities	   = "Device Capabilities";
static char *drive_configuration_page	   = "STK Vendor-Unique Drive Configuration";

struct mode *lookup_mode_pg(struct list_head *m, uint8_t pcode, uint8_t subpcode) {
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
									uint8_t pcode, uint8_t subpcode, int size) {
	struct mode *mp;

	MHVTL_DBG(3, "Allocating %d bytes for (%02x/%02x)",
			  size, pcode, subpcode);

	mp = lookup_mode_pg(m, pcode, subpcode);
	if (!mp) { /* Create a new entry */
		mp = (struct mode *)zalloc(sizeof(struct mode));
	}
	if (mp) {
		mp->pcodePointer = (uint8_t *)zalloc(size);
		if (mp->pcodePointer) { /* If ! null, set size of data */
			mp->pcode	  = pcode;
			mp->subpcode  = subpcode;
			mp->pcodeSize = size;

			/* Allocate a 'changeable bitmap' mode page info */
			mp->pcodePointerBitMap = zalloc(size);
			if (!mp->pcodePointerBitMap) {
				free(mp);
				MHVTL_ERR("Mode Pointer bitmap: Unable to malloc(%d)", size);
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

void dealloc_all_mode_pages(struct lu_phy_attr *lu) {
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
int add_mode_page_rw_err_recovery(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	pcode	 = MODE_RW_ERROR_RECOVER;
	subpcode = 0;
	size	 = 12;

	mode_pg = &lu->mode_pg;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_rw_error_recover, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

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
int add_mode_disconnect_reconnect(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_DISCONNECT_RECONNECT;
	subpcode = 0;
	size	 = 0x0e + 2;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_disconnect_reconnect, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[2]	 = 50; /* Buffer full ratio */
	mp->pcodePointer[3]	 = 50; /* Buffer empty ratio */
	mp->pcodePointer[10] = 4;

	mp->description = mode_disconnect_reconnect;

	return 0;
}

/*
 * Control
 * SPC3
 */
int add_mode_control(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_CONTROL;
	subpcode = 0;
	size	 = 12;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_control, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->description = mode_control;

	return 0;
}

/*
 * Control Extension 0x0A/0x01
 * SPC3
 */
int add_mode_control_extension(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_CONTROL;
	subpcode = 1;
	size	 = 0x1e; /* 0x1c + 2 for page/subpage code */

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_control_extension, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = subpcode;
	put_unaligned_be16(size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]),
					   &mp->pcodePointer[2]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[1];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[2];

	mp->description = mode_control_extension;

	return 0;
}

/*
 * Control Data Protection Mode page 0x0A/0xF0
 * IBM Ultrium Logical Block Protection
 */
int add_mode_control_data_protection(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_CONTROL;
	subpcode = 0xf0;
	size	 = 0x1e; /* 0x1c + two bytes for page/subpage */

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_control_extension, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	MHVTL_DBG(3, "Added mode page %s (%02x/%02x)",
			  mode_control_extension, pcode, subpcode);

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = subpcode;
	put_unaligned_be16(size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]),
					   &mp->pcodePointer[2]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[1];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[2];

	mp->description = mode_control_data_protection;

	/* Default to off */
	mp->pcodePointer[4] = 0x00; /* LBP Method: 0 off, 1 Reed-Solomon CRC, 2 CRC32C */
	mp->pcodePointer[5] = 0x04; /* LBP length - 32bit CRC */
	mp->pcodePointer[6] = 0;	/* LBP on write and LBP on read */

	/* And define changeable bits */
	mp->pcodePointerBitMap[4] = 0x03;
	mp->pcodePointerBitMap[5] = 0x07;
	mp->pcodePointerBitMap[6] = 0xc0;

	return 0;
}

/*
 * Data Compression
 * SSC3-8.3.2
 */
#define COMPRESSION_TYPE 0x10

int add_mode_data_compression(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_DATA_COMPRESSION;
	subpcode = 0;
	size	 = 16;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_data_compression, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[2] = 0xc0; /* Set data compression enable */
	mp->pcodePointer[3] = 0x80; /* Set data decompression enable */
	put_unaligned_be32(COMPRESSION_TYPE, &mp->pcodePointer[4]);
	put_unaligned_be32(COMPRESSION_TYPE, &mp->pcodePointer[8]);

	/* Changeable fields */
	mp->pcodePointerBitMap[2] = 0xc0;					  /* DCE & DCC */
	mp->pcodePointerBitMap[3] = 0x80;					  /* DDE bit */
	put_unaligned_be32(0xffffffff, &mp->pcodePointer[4]); /* Comp alg */
	put_unaligned_be32(0xffffffff, &mp->pcodePointer[8]); /* De-comp alg */

	mp->description = mode_data_compression;

	return 0;
}

void set_mode_compression(struct scsi_cmd *cmd, uint8_t *p) {
	struct lu_phy_attr				*lu		 = cmd->lu;
	struct priv_lu_ssc				*lu_priv = lu->lu_private;
	struct ssc_personality_template *pm		 = lu_priv->pm;

	int dce = p[2] & 0x80;

	MHVTL_DBG(2, " Data Compression Enable   : %s (0x%02x)",
			  (p[2] & 0x80) ? "Yes" : "No", p[2]);
	MHVTL_DBG(2, " Data Compression Capable  : %s",
			  (p[2] & 0x40) ? "Yes" : "No");
	MHVTL_DBG(2, " Data DeCompression Enable : %s (0x%02x)",
			  (p[3] & 0x80) ? "Yes" : "No", p[3]);
	MHVTL_DBG(2, " Compression Algorithm     : 0x%04x",
			  get_unaligned_be32(&p[4]));
	MHVTL_DBG(2, " DeCompression Algorithm   : 0x%04x",
			  get_unaligned_be32(&p[8]));
	MHVTL_DBG(2, " Report Exception on Decompression: 0x%02x",
			  (p[3] & 0x6) >> 5);

	if (dce) { /* Data Compression Enable bit set */
		MHVTL_DBG(1, " Setting compression");
		if (pm->set_compression) {
			pm->set_compression(&lu->mode_pg,
								lu_priv->configCompressionFactor);
			set_lp11_compression(1); /* Update LogPage 11 compression bit */
		}
	} else {
		MHVTL_DBG(1, " Clearing compression");
		if (pm->clear_compression) {
			pm->clear_compression(&lu->mode_pg);
			set_lp11_compression(0); /* Update LogPage 11 compression bit */
		}
	}
}

/*
 * Device Configuration
 * SSC3-8.3.3
 */

int add_mode_device_configuration(struct lu_phy_attr *lu) {
	struct list_head   *mode_pg;
	struct mode		   *mp;
	struct priv_lu_ssc *ssc;
	uint8_t				pcode;
	uint8_t				subpcode;
	uint8_t				size;

	ssc		 = (struct priv_lu_ssc *)lu->lu_private;
	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_DEVICE_CONFIGURATION;
	subpcode = 0;
	size	 = 16;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_device_configuration, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	mp->pcodePointer[3] = 0; /* Active partition, default = 0 */

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[7]	 = 0x64; /* Write delay (100mS intervals) */
	mp->pcodePointer[8]	 = 0x40; /* Block Identifiers supported */
	mp->pcodePointer[10] = 0x18; /* Enable EOD & Sync at early warning */
	mp->pcodePointer[14] = ssc->configCompressionFactor;
	mp->pcodePointer[15] = 0x80; /* WTRE (WORM handling) */

	mp->pcodePointerBitMap[14] = 0xff; /* Compression is changeable */

	/* Set pointer for compressionFactor to correct location in
	 * mode page struct
	 */
	ssc->compressionFactor = &mp->pcodePointer[14];

	mp->description = mode_device_configuration;

	return 0;
}

void set_device_configuration(struct scsi_cmd *cmd, uint8_t *p) {
	struct lu_phy_attr				*lu		 = cmd->lu;
	struct priv_lu_ssc				*lu_priv = cmd->lu->lu_private;
	struct ssc_personality_template *pm		 = lu_priv->pm;

	MHVTL_DBG(2, " Report Early Warning   : %s",
			  (p[8] & 0x01) ? "Yes" : "No");
	MHVTL_DBG(2, " Software Write Protect : %s",
			  (p[10] & 0x04) ? "Yes" : "No");
	MHVTL_DBG(2, " WORM Tamper Read Enable: %s",
			  (p[15] & 0x80) ? "Yes" : "No");

	MHVTL_DBG(2, " Setting device compression Algorithm");
	if (p[14]) { /* Select Data Compression Alg */
		MHVTL_DBG(2, "  Mode Select->Setting compression: %d", p[14]);
		if (pm->set_compression) {
			pm->set_compression(&lu->mode_pg,
								lu_priv->configCompressionFactor);
			set_lp11_compression(1); /* Update LogPage 11 compression bit */
		}
	} else {
		MHVTL_DBG(2, "  Mode Select->Clearing compression");
		if (pm->clear_compression) {
			pm->clear_compression(&lu->mode_pg);
			set_lp11_compression(0); /* Update LogPage 11 compression bit */
		}
	}
}

int add_mode_device_configuration_extension(struct lu_phy_attr *lu) {
	struct list_head				*mode_pg;
	struct priv_lu_ssc				*ssc;
	struct ssc_personality_template *pm;
	struct mode						*mp;
	uint8_t							 pcode;
	uint8_t							 subpcode;
	uint8_t							 size;

	/* Only for TAPE (SSC) devices */
	if (lu->ptype != TYPE_TAPE)
		return -ENOTTY;

	ssc = lu->lu_private;
	pm	= ssc->pm;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_DEVICE_CONFIGURATION;
	subpcode = 0x01;
	size	 = 32;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_device_configuration_extension, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[5] = 0x02; /* Short erase mode  - write EOD */

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

uint8_t set_device_configuration_extension(struct scsi_cmd *cmd, uint8_t *p) {
	struct lu_phy_attr				*lu		  = cmd->lu;
	struct priv_lu_ssc				*lu_priv  = cmd->lu->lu_private;
	struct ssc_personality_template *pm		  = lu_priv->pm;
	uint8_t							*sam_stat = &cmd->dbuf_p->sam_stat;

	struct mode *mp;
	struct s_sd	 sd;
	int			 page_code_len;
	int			 write_mode;
	int			 pews; /* Programable Early Warning Size */

	mp = lookup_mode_pg(&lu->mode_pg, MODE_DEVICE_CONFIGURATION, 1);

	/* Code error
	 * Any device supporting this should have this mode page defined */
	if (!mp) {
		sam_hardware_error(E_INTERNAL_TARGET_FAILURE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	page_code_len = get_unaligned_be16(&p[2]);

	if (page_code_len != 0x1c) {
		sd.byte0		 = SKSV;
		sd.field_pointer = 2;
		MHVTL_LOG("Unexpected page code length.. Unexpected results");
		sam_illegal_request(E_INVALID_FIELD_IN_PARMS, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	write_mode = (p[5] & 0xf0) >> 4;
	if (write_mode > 1) {
		MHVTL_LOG("Unsupported write mode: 0x%x", write_mode);
		sd.byte0		 = SKSV | BPV | 7; /* bit 7 */
		sd.field_pointer = 5;
		sam_illegal_request(E_INVALID_FIELD_IN_PARMS, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	MHVTL_DBG(2, "%s mode", write_mode ? "Append-only" : "Overwrite-allowed");

	pews = get_unaligned_be16(&p[6]);
	if (pm->drive_supports_prog_early_warning) {
		MHVTL_DBG(2, "Set Programable Early Warning Size: %d", pews);
		lu_priv->prog_early_warning_sz = pews;
		update_prog_early_warning(lu);
	} else {
		MHVTL_DBG(2, "Programable Early Warning Size not supported"
					 " by this device");
	}

	MHVTL_DBG(2, "Volume containing encrypted logical blocks "
				 "requires encryption: %d",
			  p[8] & 0x01);

	if (pm->drive_supports_append_only_mode) {
		/* Can't reset append-only mode via mode page ssc4 8.3.8 */
		if (lu_priv->append_only_mode && write_mode == 0) {
			MHVTL_LOG("Can't reset append only mode via mode page");
			sam_illegal_request(E_INVALID_FIELD_IN_PARMS,
								NULL, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		if (write_mode) {
			lu_priv->append_only_mode = write_mode;
			lu_priv->allow_overwrite  = FALSE;
		}
	}

	/* Now update our copy of this mode page */
	mp->pcodePointer[5] &= 0x0f;
	mp->pcodePointer[5] |= write_mode << 4;

	return SAM_STAT_GOOD;
}

int add_mode_medium_partition(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_MEDIUM_PARTITION;
	subpcode = 0;
	size	 = 16;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_medium_partition, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[2] = MAX_PARTITIONS - 1; /* Maximum Additional Partitions */
	mp->pcodePointer[3] = MAX_PARTITIONS - 1; /* Additional Partitions Defined
												 should be dynamically set to mam.num_partitions - 1 later */

	mp->pcodePointer[4] = 0x9c; /* FDP (Fixed Data Partitions) |
								 *	PSUM (partition size unit of measure) |
								 *	POFM (partition on Format Medium) */

	mp->pcodePointer[5] = 0x03; /* Medium Format Recognition */
	mp->pcodePointer[6] = 0x09; /* Partitioning Type |
								 * Partition Units	 */

	/* Changeable fields */
	mp->pcodePointerBitMap[3] = 0xff;
	mp->pcodePointerBitMap[4] = 0xf8;
	mp->pcodePointerBitMap[6] = 0xff;
	put_unaligned_be16(0xffff, &mp->pcodePointerBitMap[8]);
	put_unaligned_be16(0xffff, &mp->pcodePointerBitMap[10]);
	put_unaligned_be16(0xffff, &mp->pcodePointerBitMap[12]);
	put_unaligned_be16(0xffff, &mp->pcodePointerBitMap[14]);

	mp->description = mode_medium_partition;

	return 0;
}

void set_medium_partition(struct scsi_cmd *cmd, uint8_t *p) {
	struct lu_phy_attr *lu = cmd->lu;
	struct mode		   *mp = lookup_mode_pg(&lu->mode_pg, MODE_MEDIUM_PARTITION, 0);

	/* ADDITIONAL PARTITIONS DEFINED */
	mp->pcodePointer[3] = p[3];
	mam.num_partitions	= mp->pcodePointer[3] + 1;
	MHVTL_DBG(3, "New total number of partitions : %d", mam.num_partitions);

	mp->pcodePointer[4] = p[4]; /* flags */
	mp->pcodePointer[5] = p[5]; /* MEDIUM FORMAT RECOGNITION */
	mp->pcodePointer[6] = p[6]; /* PARTITIONING TYPE | PARTITION UNITS */

	/* PARTITION SIZES */
	for (int k = 0; k < mam.num_partitions; k++) {
		put_unaligned_be16(p[8 + 2 * k], &mp->pcodePointer[8 + 2 * k]);
	}
}

int add_mode_power_condition(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_POWER_CONDITION;
	subpcode = 0;
	size	 = 0x26;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_power_condition, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->description = mode_power_condition;

	return 0;
}

int add_mode_information_exception(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_INFORMATION_EXCEPTION;
	subpcode = 0;
	size	 = 12;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_information_exception, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[2] = 0x08;
	mp->pcodePointer[3] = 0x03;

	mp->description = mode_information_exception;

	return 0;
}

int add_mode_medium_configuration(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_MEDIUM_CONFIGURATION;
	subpcode = 0;
	size	 = 32;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_medium_configuration, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[4] = 0x01; /* WORM mode label restrictions */
	mp->pcodePointer[5] = 0x01; /* WORM mode filemark restrictions */

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
int add_mode_ult_encr_mode_pages(struct lu_phy_attr *lu) {
	struct list_head *mode_pg; /* Mode Page list */
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_VENDOR_SPECIFIC_24H;
	subpcode = 0;
	size	 = 8;

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
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

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
int add_mode_vendor_25h_mode_pages(struct lu_phy_attr *lu) {
	struct list_head *mode_pg; /* Mode Page list */
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_VENDOR_SPECIFIC_25H;
	subpcode = 0;
	size	 = 32;

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
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[5] = 1; /* LEOP to maximize medium capacity */
	mp->pcodePointer[6] = 1; /* Early Warning */

	mp->description = mode_vendor_25h;

	return 0;
}

int add_mode_encryption_mode_attribute(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_ENCRYPTION_MODE;
	subpcode = 0x20;
	size	 = 9;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_encryption_mode, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	/* Application Managed Encryption */
	mp->pcodePointer[5] = 0x03; /* Encryption Solution Method */
	mp->pcodePointer[6] = 0x01; /* Key Path */
	mp->pcodePointer[7] = 0x01; /* Default Encruption State */
	mp->pcodePointer[8] = 0x00; /* Desnity Reporting */

	mp->description = mode_encryption_mode;

	return 0;
}

int add_mode_ait_device_configuration(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_AIT_DEVICE_CONFIGURATION;
	subpcode = 0;
	size	 = 8;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_ait_device_configuration, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[2] = 0xf0;
	mp->pcodePointer[3] = 0x0a;
	mp->pcodePointer[4] = 0x40;

	mp->description = mode_ait_device_configuration;

	return 0;
}

int add_mode_element_address_assignment(struct lu_phy_attr *lu) {
	struct list_head	   *mode_pg;
	struct mode			   *mp;
	static struct smc_priv *smc_p;
	uint8_t					pcode;
	uint8_t					subpcode;
	uint8_t					size;
	uint8_t				   *p;

	mode_pg	 = &lu->mode_pg;
	smc_p	 = (struct smc_priv *)lu->lu_private;
	pcode	 = MODE_ELEMENT_ADDRESS;
	subpcode = 0;
	size	 = 20;

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
int add_mode_transport_geometry(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_TRANSPORT_GEOMETRY;
	subpcode = 0;
	size	 = 4;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_transport_geometry, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

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
int add_mode_device_capabilities(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_DEVICE_CAPABILITIES;
	subpcode = 0;
	size	 = 20;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_device_capabilities, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

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
int add_mode_behavior_configuration(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg	 = &lu->mode_pg;
	pcode	 = MODE_BEHAVIOR_CONFIGURATION;
	subpcode = 0;
	size	 = 10;

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  mode_behaviour_configuration, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->pcodePointer[3] = 0; /* Clean Behavior */
	mp->pcodePointer[4] = 0; /* WORM Behavior */

	mp->description = mode_behaviour_configuration;

	return 0;
}

int update_prog_early_warning(struct lu_phy_attr *lu) {
	uint8_t			   *mp;
	struct mode		   *m;
	struct list_head   *mode_pg;
	struct priv_lu_ssc *lu_priv;

	mode_pg = &lu->mode_pg;
	lu_priv = (struct priv_lu_ssc *)lu->lu_private;

	m = lookup_mode_pg(mode_pg, MODE_DEVICE_CONFIGURATION, 1);
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

int update_logical_block_protection(struct lu_phy_attr *lu, uint8_t *buf) {
	uint8_t			   *mp;
	struct mode		   *m;
	struct list_head   *mode_pg;
	struct priv_lu_ssc *lu_priv;

	mode_pg = &lu->mode_pg;
	lu_priv = (struct priv_lu_ssc *)lu->lu_private;

	MHVTL_DBG(3, "+++ entry +++");

	m = lookup_mode_pg(mode_pg, MODE_CONTROL, 0xf0);
	MHVTL_DBG(3, "l: %p, m: %p, m->pcodePointer: %p",
			  mode_pg, m, m->pcodePointer);
	if (m) {
		mp = m->pcodePointer;
		if (!mp) {
			MHVTL_ERR("Could not find mode page");
			return SAM_STAT_GOOD;
		}
		mp[4]				= buf[4]; /* Logical Block Protection Method */
		mp[5]				= buf[5]; /* Logical Block Protection Information Length */
		mp[6]				= buf[6]; /* LBP_W & LBP_R */
		lu_priv->LBP_method = buf[4] & 0x03;
		lu_priv->LBP_R		= (buf[6] & 0x40) ? 1 : 0;
		lu_priv->LBP_W		= (buf[6] & 0x80) ? 1 : 0;
		MHVTL_DBG(1, "Updating Logical Block Protection: Method: 0x%02x, LBP_R: %s, LPB_W: %s",
				  lu_priv->LBP_method,
				  lu_priv->LBP_R ? "True" : "False",
				  lu_priv->LBP_W ? "True" : "False");
	}
	return SAM_STAT_GOOD;
}

uint8_t set_lbp(struct scsi_cmd *cmd, uint8_t *buf, int len) {
	struct priv_lu_ssc				*lu_priv  = cmd->lu->lu_private;
	struct ssc_personality_template *pm		  = lu_priv->pm;
	uint8_t							*sam_stat = &cmd->dbuf_p->sam_stat;
	struct s_sd						 sd;

	/* OK, the drive supports Logical Block Protection - good to go */
	if (pm->drive_supports_LBP) {
		return update_logical_block_protection(cmd->lu, buf);
	}
	sd.byte0		 = SKSV | CD;
	sd.field_pointer = 1;
	sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}

int add_smc_mode_page_drive_configuration(struct lu_phy_attr *lu) {
	struct list_head *mode_pg;
	struct mode		 *mp;
	uint8_t			  pcode;
	uint8_t			  subpcode;
	uint8_t			  size;

	mode_pg = &lu->mode_pg;
	/* A Vendor-specific page for the StorageTek L20, L40 and L80 libraries */
	pcode	 = 0x2d;
	subpcode = 0;
	size	 = 0x26;
	/*
	 * FIXME: Need to fill in details from Table 4-21 L20 SCSI Reference Manual
	 */

	MHVTL_DBG(3, "Adding mode page %s (%02x/%02x)",
			  drive_configuration_page, pcode, subpcode);

	mp = alloc_mode_page(mode_pg, pcode, subpcode, size);
	if (!mp)
		return -ENOMEM;

	mp->pcodePointer[0] = pcode;
	mp->pcodePointer[1] = size - sizeof(mp->pcodePointer[0]) - sizeof(mp->pcodePointer[1]);

	/* And copy pcode/size into bitmap structure */
	mp->pcodePointerBitMap[0] = mp->pcodePointer[0];
	mp->pcodePointerBitMap[1] = mp->pcodePointer[1];

	mp->description = drive_configuration_page;

	return 0;
}
