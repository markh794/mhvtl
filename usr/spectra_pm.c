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
	.library_has_playground		= FALSE,
};

void init_spectra_logic_smc(struct  lu_phy_attr *lu)
{
	struct smc_priv *lu_priv = lu->lu_private;

	smc_pm.name = "mhVTL - Spectra Python emulation";
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

	/* Initialise order 'Drive, Picker, MAP, Storage */
	init_slot_info(lu);

	/* size of dvcid area in RES descriptor */
	lu_priv->dvcid_len = 10;

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
