/*
 * Personality module for HP E-Series
 */

#include <stdio.h>
#include <string.h>
#include "vtllib.h"
#include "smc.h"
#include "logging.h"
#include "be_byteshift.h"

/*
 * Standard INQUIRY data as documented in the HPE Storage Tape Library
 * SCSI Reference for the MSL6480, MSL3040, MSL2024 G4 and 1/8 G3
 * Autoloader. The same table applies to all four.
 */
static void update_hp_msl_inquiry(struct lu_phy_attr *lu) {
	struct smc_priv *smc_p = lu->lu_private;

	lu->inquiry[2] = 5;	   /* SPC-3 */
	lu->inquiry[3] = 2;	   /* Response data format */
	lu->inquiry[4] = 0x45; /* Additional length */

	lu->inquiry[55] |= smc_p->pm->library_has_barcode_reader ? 1 : 0;

	put_unaligned_be16(0x005c, &lu->inquiry[58]); /* SAM-2 */
	put_unaligned_be16(0x0000, &lu->inquiry[60]);
	put_unaligned_be16(0x030f, &lu->inquiry[62]); /* SPC-3 */
	put_unaligned_be16(0x02fe, &lu->inquiry[64]); /* SMC-2 */
}

static void update_eml_vpd_80(struct lu_phy_attr *lu) {
	struct vpd **lu_vpd = lu->lu_vpd;
	uint8_t		*d;
	int			 pg;

	/* Unit Serial Number */
	pg = PCODE_OFFSET(0x80);
	if (lu_vpd[pg]) /* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x12);
	if (lu_vpd[pg]) {
		d = lu_vpd[pg]->data;
		snprintf((char *)&d[0], 11, "%-10.10s", lu->lu_serial_no);
		/* Unique Logical Library Identifier */
	} else {
		MHVTL_ERR("Could not malloc(0x12) bytes, line %d", __LINE__);
	}
}

static void update_eml_vpd_83(struct lu_phy_attr *lu) {
	struct vpd **lu_vpd = lu->lu_vpd;
	uint8_t		*d;
	int			 num;
	char		*ptr;
	int			 pg;
	int			 len, j;

	num = VENDOR_ID_LEN + PRODUCT_ID_LEN + 10;

	pg = PCODE_OFFSET(0x83);
	if (lu_vpd[pg]) /* Free any earlier allocation */
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
	d[num]		= 0x1; /* Binary */
	d[num + 1]	= 0x3;
	d[num + 2]	= 0x0;
	d[num + 3]	= 0x8;
	d[num + 4]	= 0x51;
	d[num + 5]	= 0x23;
	d[num + 6]	= 0x45;
	d[num + 7]	= 0x60;
	d[num + 8]	= 0x3;
	d[num + 9]	= 0x3;
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

/* The element addresses below are inherited from stklxx_pm.c, where they
 * come from the StorageTek L20 reference. They are not taken from any HP
 * document and are unverified: the HPE Storage Tape Library SCSI
 * Reference documents mode page 1Dh as a format only and publishes no
 * default addresses for any model. init_hp_msl_smc() overrides all four.
 */
static struct smc_personality_template smc_pm = {
	.library_has_map			= TRUE,
	.library_has_barcode_reader = TRUE,
	.library_has_playground		= FALSE,
	.start_picker				= 0x0001,
	.start_map					= 0x000a,
	.start_drive				= 0x01f4,
	.start_storage				= 0x03e8,

	.dvcid_len = 34,
};

void init_hp_eml_smc(struct lu_phy_attr *lu) {
	smc_pm.name = "mhVTL - HP EML E-Series emulation";

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_eml_vpd_80(lu);
	update_eml_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}

/* The MSL identity below is documented for the MSL6480, MSL3040, MSL2024 G4
 * and the 1/8 G3 Autoloader. The earlier MSL2024, MSL4048, MSL6000 and
 * MSL8096 are not covered by that reference and keep the serial only
 * identifier they have always reported. The G4 reports "MSL2024 G4", which
 * tells it from the earlier MSL2024.
 */
static int hp_msl_documented(struct lu_phy_attr *lu) {
	return !strncasecmp(lu->product_id, "MSL6480", 7) ||
		   !strncasecmp(lu->product_id, "MSL3040", 7) ||
		   !strncasecmp(lu->product_id, "MSL2024 G4", 10) ||
		   !strncasecmp(lu->product_id, "1x8", 3);
}

void init_hp_msl_smc(struct lu_phy_attr *lu) {
	int documented = hp_msl_documented(lu);

	smc_pm.name = "mhVTL - HP MSL Series emulation";

	smc_pm.lu			 = lu;
	smc_pm.start_picker	 = 0x0001;
	smc_pm.start_storage = 0x0020;
	smc_pm.start_drive	 = 0x01e0;
	smc_pm.start_map	 = 0x01c0;

	if (documented) {
		/* HPE returns a 34 byte identifier holding the drive's
		 * vendor id, product id and serial number, with identifier
		 * type 1, and honours the DVCID bit of the command.
		 */
		smc_pm.dvcid_len		 = 34;
		smc_pm.dvcid_serial_only = FALSE;
		smc_pm.no_dvcid_flag	 = FALSE;
	} else {
		smc_pm.dvcid_len		 = 20;
		smc_pm.dvcid_serial_only = TRUE;
		smc_pm.no_dvcid_flag	 = TRUE;

		lu->inquiry[2] = 2; /* Set SCSI-2 Approved Version */
	}

	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	if (documented)
		update_hp_msl_inquiry(lu);

	update_eml_vpd_80(lu);
	update_eml_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
