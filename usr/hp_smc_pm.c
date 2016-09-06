/*
 * Personality module for HP E-Series
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

static void update_eml_vpd_80(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd = lu->lu_vpd;
	uint8_t *d;
	int pg;

	/* Unit Serial Number */
	pg = PCODE_OFFSET(0x80);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x12);
	if (lu_vpd[pg]) {
		d = lu_vpd[pg]->data;
		snprintf((char *)&d[0], 10, "%-10s", lu->lu_serial_no);
		/* Unique Logical Library Identifier */
	} else {
		MHVTL_ERR("Could not malloc(0x12) bytes, line %d", __LINE__);
	}
}

static void update_eml_vpd_83(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd = lu->lu_vpd;
	uint8_t *d;
	int num;
	char *ptr;
	int pg;
	int len, j;

	num = VENDOR_ID_LEN + PRODUCT_ID_LEN + 10;

	pg = PCODE_OFFSET(0x83);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(num + 12);
	if (!lu_vpd[pg]) {
		MHVTL_ERR("Can't malloc() to setup for vpd_83");
		return;
	}

	d = lu_vpd[pg]->data;

	d[0] = 2;
	d[1] = 1;
	d[2] = 0;
	d[3] = num;

	memcpy(&d[4], &lu->vendor_id, VENDOR_ID_LEN);
	memcpy(&d[12], &lu->product_id, PRODUCT_ID_LEN);
	memcpy(&d[28], &lu->lu_serial_no, 10);
	len = (int)strlen(lu->lu_serial_no);
	ptr = &lu->lu_serial_no[len];

	num += 4;
	/* NAA IEEE registered identifier (faked) */
	d[num] = 0x1;	/* Binary */
	d[num + 1] = 0x3;
	d[num + 2] = 0x0;
	d[num + 3] = 0x8;
	d[num + 4] = 0x51;
	d[num + 5] = 0x23;
	d[num + 6] = 0x45;
	d[num + 7] = 0x60;
	d[num + 8] = 0x3;
	d[num + 9] = 0x3;
	d[num + 10] = 0x3;
	d[num + 11] = 0x3;

	if (lu->naa) { /* If defined in config file */
		sscanf((const char *)lu->naa,
			"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			&d[num + 4],
			&d[num + 5],
			&d[num + 6],
			&d[num + 7],
			&d[num + 8],
			&d[num + 9],
			&d[num + 10],
			&d[num + 11]);
	} else { /* Else munge the serial number */
		ptr--;
		for (j = 11; j > 3; ptr--, j--)
			d[num + j] = *ptr;
	}
	d[num + 4] &= 0x0f;
	d[num + 4] |= 0x50;
}

static struct smc_personality_template smc_pm = {
	.library_has_map		= TRUE,
	.library_has_barcode_reader	= TRUE,
	.library_has_playground		= FALSE,
	.start_picker			= 0x0001,
	.start_map			= 0x000a,
	.start_drive			= 0x01f4,
	.start_storage			= 0x03e8,

	.dvcid_len			= 34,
};

void init_hp_eml_smc(struct  lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - HP EML E-Series emulation";

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_eml_vpd_80(lu);
	update_eml_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}

void init_hp_msl_smc(struct lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - HP MSL Series emulation";

	smc_pm.lu = lu;
	smc_pm.start_picker	= 0x0001;
	smc_pm.start_storage	= 0x0020;
	smc_pm.start_drive	= 0x01e0;
	smc_pm.start_map	= 0x01c0;
	smc_pm.dvcid_len	= 20,
	smc_pm.dvcid_serial_only = TRUE,
	smc_pm.no_dvcid_flag	 = TRUE,

	lu->inquiry[2] = 2;	/* Set SCSI-2 Approved Version */

	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_eml_vpd_80(lu);
	update_eml_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
