/*
 * Personality module for STK L series of robots
 * e.g. L180, L700, L20/40/80
 */

#include <stdio.h>
#include <string.h>
#include "vtllib.h"
#include "smc.h"
#include "logging.h"
#include "mode.h"

static struct smc_personality_template smc_pm = {
	.library_has_map			= TRUE,
	.library_has_barcode_reader = TRUE,
	.library_has_playground		= TRUE,
	.stk_dte_layout				= TRUE,

	.dvcid_len = 32,
};

/* Unit Serial Number page (80h). The serial is ASCII: a shorter one is
 * padded with blanks, a longer one is cut.
 * - L20/40/80: 11 characters, page length 0Bh. SCSI Reference Manual,
 *   doc 95971, 4th ed. (2003), table 4-11, p. 4-14:
 *   https://docs.oracle.com/cd/E19827-01/CRCM2231/CRCM2231.pdf
 * - L180/L700: 11 characters, page length 0Bh. SCSI and Fibre Channel
 *   Interface Reference Manual, doc 95869, 10th ed. Rev K (2007),
 *   tables 6-11 and 6-15, pp. 6-15 and 6-23:
 *   https://docs.oracle.com/cd/E19828-01/95869K/95869K.pdf
 * - SL500: 12 characters, page length 0Ch. Interface Reference Manual,
 *   PN 96122 Rev E (2007), table 6-11, p. 96:
 *   https://docs.oracle.com/cd/E19957-01/96122E/96122E.pdf
 */
static void update_stk_l_vpd_80(struct lu_phy_attr *lu, uint8_t sn_len) {
	struct vpd **lu_vpd;

	lu_vpd = &lu->lu_vpd[PCODE_OFFSET(0x80)];

	if (*lu_vpd) { /* Free any earlier allocation */
		dealloc_vpd(*lu_vpd);
		*lu_vpd = NULL;
	}

	*lu_vpd = alloc_vpd(sn_len);
	if (*lu_vpd) {
		memset((*lu_vpd)->data, ' ', sn_len);
		memcpy((*lu_vpd)->data, lu->lu_serial_no,
			   strnlen(lu->lu_serial_no, sn_len));
	} else {
		MHVTL_ERR("Could not malloc(%u) bytes, line %d", sn_len, __LINE__);
	}
}

static void update_stk_l_vpd_83(struct lu_phy_attr *lu) {
	struct vpd *lu_vpd;

	lu_vpd = lu->lu_vpd[PCODE_OFFSET(0x83)];

	/* STK L series do not have this VPD page - remove */
	if (lu_vpd) { /* Free any earlier allocation */
		dealloc_vpd(lu_vpd);
		lu->lu_vpd[PCODE_OFFSET(0x83)] = NULL;
	}
}

void init_stkl20(struct lu_phy_attr *lu) {
	smc_pm.name						  = "mhVTL - STK L20/40/80 series emulation";
	smc_pm.library_has_map			  = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground	  = TRUE;

	/* The L20/40/80 reports each drive's SCSI bus address (doc 95971
	 * table 4-50, bytes 6-7).
	 */
	smc_pm.report_scsi_bus_address = TRUE;

	/* Follow L20 SCSI Reference Manual  */
	smc_pm.start_picker	 = 0x0001;
	smc_pm.start_map	 = 0x000a; /*   10d -   55d */
	smc_pm.start_drive	 = 0x01f4; /*  500d -  519d */
	smc_pm.start_storage = 0x03e8; /* 1000d - 1677d */

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_stk_l_vpd_80(lu, 11);
	update_stk_l_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
	/* FIXME: Need to add page 0x2d - Drive Configuration Page */
	add_smc_mode_page_drive_configuration(lu);
}

void init_stklxx(struct lu_phy_attr *lu) {
	smc_pm.name						  = "mhVTL - STK L series emulation";
	smc_pm.library_has_map			  = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground	  = TRUE;

	/* The L700/L180 reports each drive's SCSI bus address (doc 95869
	 * tables 6-67/6-68, bytes 6-7).
	 */
	smc_pm.report_scsi_bus_address = TRUE;

	/* Follow L700e/L180 SCSI Reference Manual - 8th Edition */
	smc_pm.start_picker	 = 0x0001;
	smc_pm.start_map	 = 0x000a; /*   10d -   55d */
	smc_pm.start_drive	 = 0x01f4; /*  500d -  519d */
	smc_pm.start_storage = 0x03e8; /* 1000d - 1677d */

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_stk_l_vpd_80(lu, 11);
	update_stk_l_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}

void init_stkslxx(struct lu_phy_attr *lu) {
	smc_pm.name						  = "mhVTL - STK SL series emulation";
	smc_pm.library_has_map			  = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground	  = TRUE;

	/* SL500 bytes 6-8 are reserved (PN 96122 tables 6-76/6-77) */
	smc_pm.report_scsi_bus_address = FALSE;

	/* Follow Streamline SL500 Interface Reference Manual - 2th Edition */
	smc_pm.start_picker	 = 0x0001;
	smc_pm.start_map	 = 0x000a; /*   10d -   55d */
	smc_pm.start_drive	 = 0x01f4; /*  500d -  518d */
	smc_pm.start_storage = 0x03e8; /* 1000d - 1628d */

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_stk_l_vpd_80(lu, 12);
	update_stk_l_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
