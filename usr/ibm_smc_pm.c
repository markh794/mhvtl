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

/*
 * Undocumented page - raw dump from a real library..
 */
static void update_ibm_3100_vpd_d0(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd;
	uint8_t *d;
	int pg;
	uint8_t pg_d0[] = {
		0x08, 0xd0, 0x00, 0xc0, 0x04, 0x53, 0x43, 0x44,
		0x44, 0x00, 0x04, 0x04, 0xc0, 0x00, 0x00, 0x00,
		0x00, 0x06, 0x04, 0x22, 0x80, 0x00, 0x00, 0x00,
		0x08, 0x07, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00,
		0x0a, 0x00, 0x0b, 0x08, 0x80, 0x00, 0x00, 0x00,
		0x35, 0x73, 0x00, 0x40, 0x00, 0x0e, 0x02, 0x00,
		0xff, 0x00, 0x0f, 0x01, 0xff, 0x00, 0x10, 0x02,
		0x00, 0xff, 0x00, 0x11, 0x10, 0x04, 0x8e, 0x82,
		0x72, 0x04, 0x83, 0x82, 0x75, 0x3b, 0x12, 0x82,
		0x75, 0x04, 0x12, 0x82, 0x76, 0x00, 0x14, 0x3a,
		0x00, 0x01, 0x03, 0x01, 0x07, 0x0b, 0x12, 0x01,
		0x15, 0x01, 0x16, 0x01, 0x17, 0x01, 0x1a, 0x01,
		0x1b, 0x0a, 0x1d, 0x01, 0x1e, 0x01, 0x2b, 0x0a,
		0x37, 0x0b, 0x3b, 0x03, 0x3c, 0x01, 0x4c, 0x01,
		0x4d, 0x01, 0x55, 0x01, 0x56, 0x01, 0x57, 0x01,
		0x5a, 0x01, 0x5e, 0x01, 0x5f, 0x01, 0xa3, 0x01,
		0xa4, 0x01, 0xa5, 0x19, 0xb5, 0x01, 0xb6, 0x01,
		0xb8, 0x01, 0x00, 0x16, 0x03, 0x80, 0x24, 0x02,
		0x00, 0x17, 0x11, 0x00, 0x00, 0x10, 0x20, 0x06,
		0x01, 0x00, 0x10, 0x20, 0x06, 0x01, 0x01, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x1b, 0x01, 0x60,
		0x00, 0x1c, 0x05, 0x00, 0x00, 0x06, 0x06, 0x02,
		0x00, 0x24, 0x0c, 0x83, 0x00, 0x83, 0x03, 0x83,
		0x30, 0x83, 0x11, 0x3b, 0x12, 0x83, 0x02, 0x00,
		0x26, 0x02, 0x00, 0x05
	};

	lu_vpd = lu->lu_vpd;

	pg = PCODE_OFFSET(0xd0);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0xc8);
	if (!lu_vpd[pg]) {
		MHVTL_ERR("Could not malloc(0xc8) bytes, line %d", __LINE__);
		return;
	}

	d = lu_vpd[pg]->data;
	memcpy(d, &pg_d0[0], sizeof(pg_d0));
}

/*
 * Undocumented page - raw dump from a real library..
 */
static void update_ibm_3100_vpd_ff(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd;
	uint8_t *d;
	int pg;
	uint8_t pg_ff[] = {
		0x08, 0xff, 0x00, 0x20, 0xb5, 0x8e, 0xb0, 0x0e,
		0x1c, 0x0e, 0x98, 0x0e, 0x30, 0x0b, 0x90, 0x35,
		0x1c, 0x0b, 0x35, 0x0b, 0x98, 0x40, 0x78, 0xc0,
		0x07, 0x71, 0xd4, 0x0b, 0x98, 0x80, 0x78, 0x00,
		0x28, 0x0f, 0xd0, 0x34
	};

	lu_vpd = lu->lu_vpd;

	pg = PCODE_OFFSET(0xff);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x26);
	if (!lu_vpd[pg]) {
		MHVTL_ERR("Could not malloc(0x26) bytes, line %d", __LINE__);
		return;
	}

	d = lu_vpd[pg]->data;
	memcpy(d, &pg_ff[0], sizeof(pg_ff));
}

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
	if (!lu_vpd[pg]) {
		MHVTL_ERR("Could not malloc(0x16) bytes, line %d", __LINE__);
		return;
	}

	d = lu_vpd[pg]->data;
	/* d[4 - 15] Serial number of device */
	snprintf((char *)&d[0], 10, "%-10s", lu->lu_serial_no);
	/* First Storage Element Address */
	snprintf((char *)&d[12], 4, "%04x", smc_p->pm->start_storage);
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
	lu_vpd[pg] = alloc_vpd(0x2c);
	if (!lu_vpd[pg]) {
		MHVTL_ERR("Could not malloc(0x2c) bytes, line %d", __LINE__);
		return;
	}

	d = lu_vpd[pg]->data;
	d[0] = 2;	/* Code set */
	d[1] = 1;	/* Identifier Type */
	d[3] = 0x28;	/* Identifier length */
	/* Vendor ID */
	memcpy(&d[4], &lu->inquiry[8], 8);
	/* Device type and Model Number */
	memcpy(&d[12], &lu->inquiry[16], 16);
	/* Serial Number of device */
	memcpy(&d[28], &lu->inquiry[38], 12);
	/* First Storage Element Address */
	snprintf((char *)&d[40], 4, "%04x", smc_p->pm->start_storage);
}

static void update_ibm_3584_inquiry(struct lu_phy_attr *lu)
{
	lu->inquiry[2] = 3;	/* SNSI Approved Version */
	lu->inquiry[3] = 2;	/* Response data format */
	lu->inquiry[4] = 0x35;	/* Additional length */

	memcpy(&lu->inquiry[38], &lu->lu_serial_no, 12);
	lu->inquiry[50] = 0x30;
	lu->inquiry[51] = 0x30;
}

static void update_ibm_3100_inquiry(struct lu_phy_attr *lu)
{
	struct smc_priv *smc_p;
	smc_p = lu->lu_private;

	lu->inquiry[2] = 5;	/* SNSI Approved Version */
	lu->inquiry[3] = 2;	/* Response data format */
	lu->inquiry[4] = 0x43;	/* Additional length */

	memcpy(&lu->inquiry[38], &lu->lu_serial_no, 12);
	lu->inquiry[55] |= smc_p->pm->library_has_barcode_reader ? 1 : 0;
	put_unaligned_be16(0x005c, &lu->inquiry[58]); /* SAM-2 */
	put_unaligned_be16(0x0b56, &lu->inquiry[60]); /* SPI-4 */
	put_unaligned_be16(0x02fe, &lu->inquiry[62]); /* SMC-2 */
	put_unaligned_be16(0x030f, &lu->inquiry[64]); /* SPC-3 */
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
	/* d[4 - 15] Serial number of device */
	snprintf((char *)&d[0], 13, "%-12s", lu->lu_serial_no);
	/* Unique Logical Library Identifier */
	memset(&d[12], 0x20, 4);	/* Space chars */
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
	if (!lu_vpd[pg]) {
		MHVTL_ERR("Could not malloc(0x32) bytes, line %d", __LINE__);
		return;
	}

	d = lu_vpd[pg]->data;
	d[0] = 2;	/* Code set */
	d[1] = 1;	/* Identifier Type */
	d[3] = 0x28;	/* Identifier length */
	/* Vendor ID */
	memcpy(&d[4], &lu->inquiry[8], 8);
	/* Device type and Model Number */
	memcpy(&d[12], &lu->inquiry[16], 16);
	/* Serial Number of device */
	memcpy(&d[28], &lu->inquiry[38], 12);
	/* Unique Logical Library Identifier */
	memset(&d[40], 0x20, 4);	/* Space chars */
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
	if (!lu_vpd[pg]) {
		MHVTL_ERR("Could not malloc(0x40) bytes, line %d", __LINE__);
		return;
	}

	d = lu_vpd[pg]->data;
	sprintf((char *)&d[4], "%s", "DEAD");	/* Media f/w checksum */
	/* Media changer firmware build date (mm-dd-yyyy) */
	snprintf((char *)&d[8], 22, "%02d-%02d-%04d", m, dd, y);
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

	/* Update vendor specific info in main INQUIRY page */
	update_ibm_3100_inquiry(lu);

	/* Need slot info before we can fill out VPD data */
	update_ibm_3100_vpd_80(lu);
	update_ibm_3100_vpd_83(lu);
	/* IBM Doco hints at VPD page 0xd0 & 0xff - but does not document it */
/*
 * lsscsi -g
 * [2:0:1:0]    tape    IBM      ULT3580-TD4      8192  /dev/st0  /dev/sg2
 * [2:0:1:1]    mediumx IBM      3573-TL          6.50  -         /dev/sg3
 *
 * # sg_inq -p 0 /dev/sg3
 * Only hex output supported
 * VPD INQUIRY, page code=0x00:
 *   [PQual=0  Peripheral device type: medium changer]
 *    Supported VPD pages:
 *        0x0        Supported VPD pages
 *        0x80       Unit serial number
 *        0x83       Device identification
 *        0xc0       vendor: Firmware numbers (seagate); Unit path report (EMC)
 *        0xd0
 *        0xff

 * # sg_inq -H -p 0xc0 /dev/sg3
 * VPD INQUIRY, page code=0xc0:
 *  00     08 c0 00 3c 00 00 00 00  34 38 34 37 30 34 2d 30    ...<....484704-0
 *  10     33 2d 32 30 30 38 20 20  20 20 20 20 20 20 20 20    3-2008
 *  20     20 20 20 20 00 00 00 00  00 00 00 00 00 00 00 00        ............
 *  30     00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00    ................
 */
	update_ibm_3100_vpd_c0(lu);

/*
 * # sg_inq -p 0xd0 /dev/sg3
 * VPD INQUIRY, page code=0xd0:
 *  00     08 d0 00 c0 04 53 43 44  44 00 04 04 c0 00 00 00    .....SCDD.......
 *  10     00 06 04 22 80 00 00 00  08 07 80 00 00 05 00 00    ..."............
 *  20     0a 00 0b 08 80 00 00 00  35 73 00 40 00 0e 02 00    ........5s.@....
 *  30     ff 00 0f 01 ff 00 10 02  00 ff 00 11 10 04 8e 82    ................
 *  40     72 04 83 82 75 3b 12 82  75 04 12 82 76 00 14 3a    r...u;..u...v..:
 *  50     00 01 03 01 07 0b 12 01  15 01 16 01 17 01 1a 01    ................
 *  60     1b 0a 1d 01 1e 01 2b 0a  37 0b 3b 03 3c 01 4c 01    ......+.7.;.<.L.
 *  70     4d 01 55 01 56 01 57 01  5a 01 5e 01 5f 01 a3 01    M.U.V.W.Z.^._...
 *  80     a4 01 a5 19 b5 01 b6 01  b8 01 00 16 03 80 24 02    ..............$.
 *  90     00 17 11 00 00 10 20 06  01 00 10 20 06 01 01 00    ...... .... ....
 *  a0     00 00 00 00 00 1b 01 60  00 1c 05 00 00 06 06 02    .......`........
 *  b0     00 24 0c 83 00 83 03 83  30 83 11 3b 12 83 02 00    .$......0..;....
 *  c0     26 02 00 05                                         &...
 */
	update_ibm_3100_vpd_d0(lu);

/*
 * # sg_inq -p 0xff /dev/sg3
 * VPD INQUIRY, page code=0xff:
 *  00     08 ff 00 20 b5 8e b0 0e  1c 0e 98 0e 30 0b 90 35    ... ........0..5
 *  10     1c 0b 35 0b 98 40 78 c0  07 71 d4 0b 98 80 78 00    ..5..@x..q....x.
 *  20     28 0f d0 34                                         (..4
 *
 */
	update_ibm_3100_vpd_ff(lu);

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

	/* Update vendor specific info in main INQUIRY page */
	update_ibm_3584_inquiry(lu);

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
