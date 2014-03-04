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

static void update_spectra_t_series_device_capabilities(struct lu_phy_attr *lu)
{
	struct mode *mp;

	mp = lookup_pcode(&lu->mode_pg, MODE_DEVICE_CAPABILITIES, 0);
	if (!mp) {	/* Can't find page ??? */
		MHVTL_ERR("Can't find MODE_DEVICE_CAPABILITIES page");
		return;
	}

	mp->pcodePointer[2] = 0x07;
	mp->pcodePointer[3] = 0x00;
	mp->pcodePointer[4] = 0x07;
	mp->pcodePointer[5] = 0x07;
	mp->pcodePointer[6] = 0x07;
	mp->pcodePointer[7] = 0x07;
}

static struct smc_personality_template smc_pm = {
	.library_has_map		= TRUE,
	.library_has_barcode_reader	= TRUE,
	.library_has_playground		= FALSE,

	.dvcid_len			= 10,
};

void init_spectra_logic_smc(struct lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - Spectra T-Series emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.dvcid_serial_only = TRUE;

	/* Extracted from Spectra T-Series SCSI Developers guide */
	smc_pm.start_picker	= 0x0001;
	smc_pm.start_map	= 0x0010;
	smc_pm.start_drive	= 0x0100;
	smc_pm.start_storage	= 0x1000;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
	/* Now that 'init_smc_mode_pages()' has allocated device capabilities
	 * page, update to valid default values for Spectra-Logic T Series
	 */
	update_spectra_t_series_device_capabilities(lu);
}
