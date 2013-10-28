/*
 * Personality module for IBM TotalStorage(c) 3584 series of robots
 * e.g. TS3500
 */

#include <stdio.h>
#include <string.h>
#include "list.h"
#include "vtllib.h"
#include "smc.h"
#include "logging.h"
#include "be_byteshift.h"
#include "log.h"
#include "mode.h"

static struct smc_personality_template smc_pm = {
	.library_has_map		= TRUE,
	.library_has_barcode_reader	= TRUE,
	.library_has_playground		= TRUE,

	.dvcid_len			= 34,
};

static void update_ibm_3100_vpd_80(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd = lu->lu_vpd;
	uint8_t *d;
	int pg;

	/* Unit Serial Number */
	pg = PCODE_OFFSET(0x80);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x16);
	if (lu_vpd[pg]) {
		d = lu_vpd[pg]->data;
		d[0] = lu->ptype;
		d[1] = 0x80;	/* Page code */
		d[3] = 0x10;	/* Page length */
		/* d[4 - 15] Serial number of device */
		snprintf((char *)&d[4], 10, "%-10s", lu->lu_serial_no);
		/* Unique Logical Library Identifier */
		memset(&d[16], 0x20, 4);	/* Space chars */
	} else {
		MHVTL_DBG(1, "Could not malloc(0x16) bytes, line %d", __LINE__);
	}
}

static void update_ibm_3100_vpd_83(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd = lu->lu_vpd;
	int pg;
	uint8_t *d;

	/* Device Identification */
	pg = PCODE_OFFSET(0x83);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x32);	/* Allocate 2 more bytes than needed */
	if (lu_vpd[pg]) {
		d = lu_vpd[pg]->data;
		d[0] = lu->ptype;
		d[1] = 0x83;	/* Page Code */
		d[3] = 0x2c;	/* Page length */
		d[4] = 2;	/* Code set */
		d[5] = 1;	/* Identifier Type */
		d[7] = 0x28;	/* Identifier length */
		/* Vendor ID */
		memcpy(&d[8], &lu->inquiry[8], 8);
		/* Device type and Model Number */
		memcpy(&d[16], &lu->inquiry[16], 16);
		/* Serial Number of device */
		memcpy(&d[32], &lu->inquiry[38], 12);
		/* Unique Logical Library Identifier */
		memset(&d[44], 0x20, 4);	/* Space chars */
	} else {
		MHVTL_DBG(1, "Could not malloc(0x32) bytes, line %d", __LINE__);
	}
}

static void update_ibm_3100_vpd_c0(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd = lu->lu_vpd;
	int pg;
	uint8_t *d;
	int y, m, dd, hh, mm, ss;

	ymd(&y, &m, &dd, &hh, &mm, &ss);

	/* Device Identification */
	pg = PCODE_OFFSET(0xc0);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x40);
	if (lu_vpd[pg]) {
		d = lu_vpd[pg]->data;
		d[0] = lu->ptype;
		d[1] = 0xc0;	/* Page Code */
		d[3] = 0x3c;	/* Page length */
		sprintf((char *)&d[8], "%s", "DEAD");	/* Media f/w checksum */
		/* Media changer firmware build date (mm-dd-yyyy) */
		snprintf((char *)&d[12], 22, "%02d-%02d-%04d", m, dd, y);
	} else {
		MHVTL_DBG(1, "Could not malloc(0x40) bytes, line %d", __LINE__);
	}
}

static void update_ibm_3500_vpd_80(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd = lu->lu_vpd;
	struct smc_priv *smc_p = lu->lu_private;
	uint8_t *d;
	int pg;

	/* Unit Serial Number */
	pg = PCODE_OFFSET(0x80);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x16);
	if (lu_vpd[pg]) {
		d = lu_vpd[pg]->data;
		d[0] = lu->ptype;
		d[1] = 0x80;	/* Page code */
		d[3] = 0x10;	/* Page length */
		/* d[4 - 15] Serial number of device */
		snprintf((char *)&d[4], 10, "%-10s", lu->lu_serial_no);
		/* d[16 - 19] First storage element address */
		snprintf((char *)&d[16], 4, "%04d", smc_p->pm->start_storage);
	} else {
		MHVTL_DBG(1, "Could not malloc(0x16) bytes, line %d", __LINE__);
	}
}

static void update_ibm_3500_vpd_83(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd = lu->lu_vpd;
	struct smc_priv *smc_p = lu->lu_private;
	int pg;
	uint8_t *d;

	/* Device Identification */
	pg = PCODE_OFFSET(0x83);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x32);	/* Allocate 2 more bytes than needed */
	if (lu_vpd[pg]) {
		d = lu_vpd[pg]->data;
		d[0] = lu->ptype;
		d[1] = 0x83;	/* Page Code */
		d[3] = 0x2c;	/* Page length */
		d[4] = 2;	/* Code set */
		d[5] = 1;	/* Identifier Type */
		d[7] = 0x28;	/* Identifier length */
		/* Vendor ID */
		memcpy(&d[8], &lu->inquiry[8], 8);
		/* Device type and Model Number */
		memcpy(&d[16], &lu->inquiry[16], 16);
		/* Serial Number of device */
		memcpy(&d[32], &lu->inquiry[38], 12);
		/* First Storage Element Address */
		snprintf((char *)&d[44], 4, "%04d", smc_p->pm->start_storage);
	} else {
		MHVTL_DBG(1, "Could not malloc(0x32) bytes, line %d", __LINE__);
	}
}

void init_ibmts3100(struct lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - IBM TS3100 series emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = TRUE;

	/* Follow IBM TS3100 & TS3200 SCSI Reference Manual */
	smc_pm.start_picker	= 0x0001;
	smc_pm.start_map	= 0x0010;
	smc_pm.start_drive	= 0x0100;
	smc_pm.start_storage	= 0x1000;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	/* Need slot info before we can fill out VPD data */
	update_ibm_3100_vpd_80(lu);
	update_ibm_3100_vpd_83(lu);
	update_ibm_3100_vpd_c0(lu);
	/* IBM Doco hints at VPD page 0xd0 - but does not document it */

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}

void init_ibmts3500(struct lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - IBM TS3500 series emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = TRUE;

	/* Follow IBM 3584 SCSI Reference Manual */
	smc_pm.start_picker	= 0x0001;
	smc_pm.start_drive	= 0x0100;
	smc_pm.start_map	= 0x0300;
	smc_pm.start_storage	= 0x0400;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	/* Initialise order 'Picker, Drives, MAP, Storage */
	init_slot_info(lu);

	/* Need slot info before we can fill out VPD data */
	update_ibm_3500_vpd_80(lu);
	update_ibm_3500_vpd_83(lu);
	/* IBM Doco hints at VPD page 0xd0 - but does not document it */

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
