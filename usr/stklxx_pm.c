/*
 * Personality module for STK L series of robots
 * e.g. L180, L700, L20/40/80
 */

#include <stdio.h>
#include <errno.h>
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

static void update_stk_l_vpd_80(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd;
	uint8_t *d;

	lu_vpd = &lu->lu_vpd[PCODE_OFFSET(0x80)];

	/* Unit Serial Number */
	if (*lu_vpd) {	/* Free any earlier allocation */
		dealloc_vpd(*lu_vpd);
		*lu_vpd = NULL;
	}

	*lu_vpd = alloc_vpd(0x12);
	if (*lu_vpd) {
		d = (*lu_vpd)->data;
		/* d[4 - 15] Serial number of device */
		snprintf((char *)&d[0], 13, "%-12.12s", lu->lu_serial_no);
		/* Unique Logical Library Identifier */
	} else {
		MHVTL_ERR("Could not malloc(0x12) bytes, line %d", __LINE__);
	}
}

static void update_stk_l_vpd_83(struct lu_phy_attr *lu)
{
	struct vpd *lu_vpd;

	lu_vpd = lu->lu_vpd[PCODE_OFFSET(0x83)];

	/* STK L series do not have this VPD page - remove */
	if (lu_vpd) {	/* Free any earlier allocation */
		dealloc_vpd(lu_vpd);
		lu->lu_vpd[PCODE_OFFSET(0x83)] = NULL;
	}
}

void init_stkl20(struct lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - STK L20/40/80 series emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = TRUE;

	/* Follow L20 SCSI Reference Manual  */
	smc_pm.start_picker	= 0x0001;
	smc_pm.start_map	= 0x000a;	/*   10d -   55d */
	smc_pm.start_drive	= 0x01f4;	/*  500d -  519d */
	smc_pm.start_storage	= 0x03e8;	/* 1000d - 1677d */

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_stk_l_vpd_80(lu);
	update_stk_l_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
	/* FIXME: Need to add page 0x2d - Drive Configuration Page */
	add_smc_mode_page_drive_configuration(lu);
}

void init_stklxx(struct lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - STK L series emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = TRUE;

	/* Follow L700e/L180 SCSI Reference Manual - 8th Edition */
	smc_pm.start_picker	= 0x0001;
	smc_pm.start_map	= 0x000a;	/*   10d -   55d */
	smc_pm.start_drive	= 0x01f4;	/*  500d -  519d */
	smc_pm.start_storage	= 0x03e8;	/* 1000d - 1677d */

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_stk_l_vpd_80(lu);
	update_stk_l_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}

void init_stkslxx(struct lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - STK SL series emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = TRUE;

	/* Follow Streamline SL500 Interface Reference Manual - 2th Edition */
	smc_pm.start_picker	= 0x0001;
	smc_pm.start_map	= 0x000a;	/*   10d -   55d */
	smc_pm.start_drive	= 0x01f4;	/*  500d -  518d */
	smc_pm.start_storage	= 0x03e8;	/* 1000d - 1628d */

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_stk_l_vpd_80(lu);
	update_stk_l_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
