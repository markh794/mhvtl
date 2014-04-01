/*
 * Personality module for IBM TotalStorage(c) 3584 series of robots
 * e.g. TS3500
 */

#include <stdio.h>
#include <string.h>
#include "list.h"
#include "vtllib.h"
#include "scsi.h"
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

static void update_3573_device_capabilities(struct lu_phy_attr *lu)
{
	struct mode *mp;

	mp = lookup_pcode(&lu->mode_pg, MODE_DEVICE_CAPABILITIES, 0);
	if (!mp) {	/* Can't find page ??? */
		MHVTL_ERR("Can't find MODE_DEVICE_CAPABILITIES page");
		return;
	}

	mp->pcodePointer[2] = 0x0e;
	mp->pcodePointer[3] = 0x00;

	mp->pcodePointer[4] = 0x0e;	/* Medium Transport Capabilities */
	mp->pcodePointer[5] = 0x0e;	/* Storage Element Capabilities */
	mp->pcodePointer[6] = 0x0e;	/* MAP Element Capabilities */
	mp->pcodePointer[7] = 0x0e;	/* Data Trans. Element Capabilities */

	mp->pcodePointer[12] = 0x0e;	/* Medium Transport Capabilities */
	mp->pcodePointer[13] = 0x0e;	/* Storage Element Capabilities */
	mp->pcodePointer[14] = 0x0e;	/* MAP Element Capabilities */
	mp->pcodePointer[15] = 0x0e;	/* Data Trans. Element Capabilities */
}

static void update_3584_device_capabilities(struct lu_phy_attr *lu)
{
	struct mode *mp;

	mp = lookup_pcode(&lu->mode_pg, MODE_DEVICE_CAPABILITIES, 0);
	if (!mp) {	/* Can't find page ??? */
		MHVTL_ERR("Can't find MODE_DEVICE_CAPABILITIES page");
		return;
	}

	mp->pcodePointer[2] = 0x0e;
	mp->pcodePointer[3] = 0x00;

	mp->pcodePointer[4] = 0x0e;	/* Medium Transport Capabilities */
	mp->pcodePointer[5] = 0x0e;	/* Storage Element Capabilities */
	mp->pcodePointer[6] = 0x0e;	/* MAP Element Capabilities */
	mp->pcodePointer[7] = 0x0e;	/* Data Trans. Element Capabilities */

	mp->pcodePointer[12] = 0x0e;	/* Medium Transport Capabilities */
	mp->pcodePointer[13] = 0x0e;	/* Storage Element Capabilities */
	mp->pcodePointer[14] = 0x0e;	/* MAP Element Capabilities */
	mp->pcodePointer[15] = 0x0e;	/* Data Trans. Element Capabilities */
}

static void update_ibm_3584_vpd_80(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd;
	struct smc_priv *smc_p;
	uint8_t *d;
	int pg;

	lu_vpd = lu->lu_vpd;
	smc_p = lu->lu_private;

	/* Unit Serial Number */
	pg = PCODE_OFFSET(0x80);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x16);
	if (lu_vpd[pg]) {
		MHVTL_ERR("Could not malloc(0x16) bytes, line %d", __LINE__);
		return;
	}

	d = lu_vpd[pg]->data;
	d[0] = lu->ptype;
	d[1] = 0x80;	/* Page code */
	d[3] = 0x10;	/* Page length */
	/* d[4 - 15] Serial number of device */
	snprintf((char *)&d[4], 10, "%-10s", lu->lu_serial_no);
	/* First Storage Element Address */
	snprintf((char *)&d[16], 4, "%04x", smc_p->pm->start_storage);
}

static void update_ibm_3584_vpd_83(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd;
	struct smc_priv *smc_p;
	int pg;
	uint8_t *d;

	lu_vpd = lu->lu_vpd;
	smc_p = lu->lu_private;

	/* Device Identification */
	pg = PCODE_OFFSET(0x83);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x32);	/* Allocate 2 more bytes than needed */
	if (!lu_vpd[pg]) {
		MHVTL_ERR("Could not malloc(0x32) bytes, line %d", __LINE__);
		return;
	}

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
	snprintf((char *)&d[44], 4, "%04x", smc_p->pm->start_storage);
}

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
	if (!lu_vpd[pg]) {
		MHVTL_ERR("Could not malloc(0x16) bytes, line %d", __LINE__);
		return;
	}

	d = lu_vpd[pg]->data;
	d[0] = lu->ptype;
	d[1] = 0x80;	/* Page code */
	d[3] = 0x10;	/* Page length */
	/* d[4 - 15] Serial number of device */
	snprintf((char *)&d[4], 10, "%-10s", lu->lu_serial_no);
	/* Unique Logical Library Identifier */
	memset(&d[16], 0x20, 4);	/* Space chars */
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
		MHVTL_ERR("Could not malloc(0x32) bytes, line %d", __LINE__);
		return;
	}

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
		MHVTL_DBG(1, "Could not malloc(0x40) bytes, line %d", __LINE__);
		return;
	}

	d = lu_vpd[pg]->data;
	d[0] = lu->ptype;
	d[1] = 0xc0;	/* Page Code */
	d[3] = 0x3c;	/* Page length */
	sprintf((char *)&d[8], "%s", "DEAD");	/* Media f/w checksum */
	/* Media changer firmware build date (mm-dd-yyyy) */
	snprintf((char *)&d[12], 22, "%02d-%02d-%04d", m, dd, y);
}

/* 3573TL */
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
	update_3573_device_capabilities(lu);
}

/*
 * This should be fun to keep in sync..
 *
 * 03584L22 => 03592 drives
 * 03584L32 => LTO drives
 * 03584L42 => DLT drives
 */
void init_ibm3584(struct lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - IBM 03584 series emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = TRUE;

	/* Follow IBM 3584 SCSI Reference Manual */
	smc_pm.start_picker	= 0x0001;
	smc_pm.start_drive	= 0x0101;
	smc_pm.start_map	= 0x0300;
	smc_pm.start_storage	= 0x0400;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	/* Initialise order 'Picker, Drives, MAP, Storage */
	init_slot_info(lu);

	/* Need slot info before we can fill out VPD data */
	update_ibm_3584_vpd_80(lu);
	update_ibm_3584_vpd_83(lu);
	/* IBM Doco hints at VPD page 0xd0 - but does not document it */

	/* At least log what we should contain
	 * Need to identify a sanity check on incorrect
	* drive config later
	*
	* Forth edition of the IBM System Storage TS3500 Tape Library SCSI
	* Reference (August 2011)
	*
	*/
	if (!strncasecmp(lu->product_id, "03584L22", 8)
			|| !strncasecmp(lu->product_id, "03584L23", 8)
			|| !strncasecmp(lu->product_id, "03584D22", 8)
			|| !strncasecmp(lu->product_id, "03584D23", 8)) {
		MHVTL_LOG("%s library should contain 03592 drives",
				lu->product_id);
	} else if (!strncasecmp(lu->product_id, "03584L32", 8)
			|| !strncasecmp(lu->product_id, "03584D32", 8)
			|| !strncasecmp(lu->product_id, "03584L52", 8)
			|| !strncasecmp(lu->product_id, "03584D52", 8)
			|| !strncasecmp(lu->product_id, "03584L53", 8)
			|| !strncasecmp(lu->product_id, "03584D53", 8)) {
		MHVTL_LOG("%s library should contain LTO drives",
				lu->product_id);
	} else if (!strncasecmp(lu->product_id, "03584L42", 8)
			|| !strncasecmp(lu->product_id, "03584L42", 8)) {
		MHVTL_LOG("%s library should contain DLT drives",
				lu->product_id);
	} else {
		MHVTL_ERR("%s library model not known",
				lu->product_id);
	}

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
	update_3584_device_capabilities(lu);
}
