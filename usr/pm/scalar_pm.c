/*
 * Personality module for Scalar series of robots
 */

#include <stdio.h>
#include <string.h>
#include "vtllib.h"
#include "smc.h"
#include "logging.h"
#include "be_byteshift.h"

/* Page lengths as the Scalar reference documents them: the serial number
 * page carries the vendor id and the serial, the identification page a
 * header, the vendor id and a 24 byte serial.
 */
#define VPD_80_LEN 24
#define VPD_83_LEN 36

static struct smc_personality_template smc_pm = {
	.library_has_map			= TRUE,
	.library_has_barcode_reader = TRUE,
	.library_has_playground		= FALSE,

	.dvcid_len = 34,
};

static void init_scalar_inquiry(struct lu_phy_attr *lu) {
	struct smc_priv *smc_p;
	smc_p = lu->lu_private;

	lu->inquiry[2] = 3;	   /* SNSI Approved Version */
	lu->inquiry[3] = 2;	   /* Response data format */
	lu->inquiry[4] = 0x1f; /* Additional length */

	lu->inquiry[6] |= smc_p->pm->library_has_barcode_reader ? 0x20 : 0;
	lu->inquiry[55] |= smc_p->pm->library_has_barcode_reader ? 1 : 0;

	put_unaligned_be16(0x005c, &lu->inquiry[58]); /* SAM-2 */
	put_unaligned_be16(0x008d, &lu->inquiry[60]); /* SAM-4 */
	put_unaligned_be16(0x0120, &lu->inquiry[62]); /* SPC-3 */
	put_unaligned_be16(0x02fe, &lu->inquiry[64]); /* SMC-2 */
}

static void update_scalar_vpd_80(struct lu_phy_attr *lu) {
	struct vpd **lu_vpd = &lu->lu_vpd[PCODE_OFFSET(0x80)];
	char		 serial[VPD_80_LEN + 1];

	/* Unit Serial Number */
	if (*lu_vpd) { /* Free any earlier allocation */
		dealloc_vpd(*lu_vpd);
		*lu_vpd = NULL;
	}

	*lu_vpd = alloc_vpd(VPD_80_LEN);
	if (!*lu_vpd) {
		MHVTL_ERR("Could not malloc(%d) bytes, line %d", VPD_80_LEN, __LINE__);
		return;
	}

	/* d[4 - 27] Serial number prefixed by Vendor ID.
	 * Built in a local buffer because snprintf() terminates what it
	 * writes, and the page has no room for the terminator.
	 */
	snprintf(serial, sizeof(serial), "%-8.8s%-16.16s",
			 lu->vendor_id, lu->lu_serial_no);
	memcpy((*lu_vpd)->data, serial, VPD_80_LEN);
}

static void update_scalar_vpd_83(struct lu_phy_attr *lu) {
	struct vpd **lu_vpd = &lu->lu_vpd[PCODE_OFFSET(0x83)];
	uint8_t		*d;
	char		 field[25];

	/* Device Identification */
	if (*lu_vpd) { /* Free any earlier allocation */
		dealloc_vpd(*lu_vpd);
		*lu_vpd = NULL;
	}

	*lu_vpd = alloc_vpd(VPD_83_LEN);
	if (!*lu_vpd) {
		MHVTL_ERR("Could not malloc(%d) bytes, line %d", VPD_83_LEN, __LINE__);
		return;
	}

	d	 = (*lu_vpd)->data;
	d[0] = 0xf2;
	d[1] = 0x01;
	d[3] = 0x20;
	/* d[4 - 11] Vendor ID, d[12 - 35] serial number: written through a
	 * local buffer so the terminating NUL does not land past the page.
	 */
	snprintf(field, sizeof(field), "%-8.8s", lu->vendor_id);
	memcpy(&d[4], field, 8);
	snprintf(field, sizeof(field), "%-24.24s", lu->lu_serial_no);
	memcpy(&d[12], field, 24);
}

void init_scalar_smc(struct lu_phy_attr *lu) {
	int h, m, sec;
	int day, month, year;

	smc_pm.name						  = "mhVTL - Scalar emulation";
	smc_pm.library_has_map			  = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground	  = FALSE;
	smc_pm.dvcid_serial_only		  = FALSE;

	smc_pm.start_picker	 = 0x0001;
	smc_pm.start_map	 = 0x0010;
	smc_pm.start_drive	 = 0x0100;
	smc_pm.start_storage = 0x1000;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	/* Reference Quantum 6-00423013 SCSI Reference - Rev A */
	ymd(&year, &month, &day, &h, &m, &sec);

	/* Controller firmware build date */
	sprintf((char *)&lu->inquiry[36], "%04d-%02d-%02d %02d:%02d:%02d",
			year, month, day, h, m, sec);

	init_scalar_inquiry(lu);
	update_scalar_vpd_80(lu);
	update_scalar_vpd_83(lu);

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
