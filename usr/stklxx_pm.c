/*
 * Personality module for STK L series of robots
 * e.g. L180, L700, L20/40/80
 */

#include <stdio.h>
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
	struct vpd **lu_vpd = lu->lu_vpd;
	struct smc_priv *smc_p = lu->lu_private;
	uint8_t *d;
	int pg;

	smc_p = lu->lu_private;

	/* Unit Serial Number */
	pg = PCODE_OFFSET(0x80);
	if (lu_vpd[pg])		/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
	lu_vpd[pg] = alloc_vpd(0x12);
	if (lu_vpd[pg]) {
		d = lu_vpd[pg]->data;
		d[0] = lu->ptype;
		d[1] = 0x80;	/* Page code */
		d[3] = 0x0b;	/* Page length */
		/* d[4 - 15] Serial number of device */
		snprintf((char *)&d[4], 10, "%-10s", lu->lu_serial_no);
		/* Unique Logical Library Identifier */
	} else {
		MHVTL_DBG(1, "Could not malloc(0x12) bytes, line %d", __LINE__);
	}
}

static void update_stk_l_vpd_83(struct lu_phy_attr *lu)
{
	struct vpd **lu_vpd = lu->lu_vpd;
	int pg;

	/* STK L series do not have this VPD page - remove */
	pg = PCODE_OFFSET(0x83);
	if (lu_vpd[pg]) {	/* Free any earlier allocation */
		dealloc_vpd(lu_vpd[pg]);
		lu_vpd[pg] = NULL;
	}
}

void init_stklxx(struct  lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - STK L series emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = TRUE;

	/* Follow L700e/L180 SCSI Reference Manual - 8th Edition */
	smc_pm.start_picker	= 0x0001;
	smc_pm.start_map	= 0x0010;
	smc_pm.start_drive	= 0x01f0;
	smc_pm.start_storage	= 0x0400;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_stk_l_vpd_80(lu);
	update_stk_l_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}

void init_stkslxx(struct  lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - STK SL series emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = TRUE;

	/* Follow L700e/L180 SCSI Reference Manual - 8th Edition */
	smc_pm.start_picker	= 0x0001;
	smc_pm.start_map	= 0x000a;	/*   10d */
	smc_pm.start_drive	= 0x01f4;	/*  500d */
	smc_pm.start_storage	= 0x03e8;	/* 1000d */

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	update_stk_l_vpd_80(lu);
	update_stk_l_vpd_83(lu);
	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
