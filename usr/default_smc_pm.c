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

	.dvcid_len			= 32,
};

void init_default_smc(struct  lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - Default emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = TRUE;
	smc_pm.dvcid_serial_only = FALSE;

	smc_pm.start_drive	= 0x001;
	smc_pm.start_picker	= 0x02c;
	smc_pm.start_map	= 0x300;
	smc_pm.start_storage	= 0x400;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
