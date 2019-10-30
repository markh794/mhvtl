/*
 * Personality module for default emulation
 */

#include <stdio.h>
#include <string.h>
#include "mhvtl_list.h"
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

	.dvcid_len			= 32,
};

static void update_default_inquiry(struct lu_phy_attr *lu)
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

	/* Device Identification */
	lu->lu_vpd[PCODE_OFFSET(0x83)] = alloc_vpd(VPD_83_SZ);
	if (lu->lu_vpd[PCODE_OFFSET(0x83)])
		update_vpd_83(lu, NULL);

}

void init_default_smc(struct  lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - Default emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = TRUE;
	smc_pm.dvcid_serial_only = FALSE;

	smc_pm.start_drive	= 0x001;
	smc_pm.start_picker	= 0x2f0;
	smc_pm.start_map	= 0x300;
	smc_pm.start_storage	= 0x400;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);
	update_default_inquiry(lu);

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
