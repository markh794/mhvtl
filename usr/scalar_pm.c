/*
 * Personality module for Scalar series of robots
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

	.dvcid_len			= 34,
};

void init_scalar_smc(struct  lu_phy_attr *lu)
{
	int h, m, sec;
	int day, month, year;

	smc_pm.name = "mhVTL - Scalar emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.library_has_playground = FALSE;
	smc_pm.dvcid_serial_only = FALSE;

	smc_pm.start_picker	= 0x0001;
	smc_pm.start_map	= 0x0010;
	smc_pm.start_drive	= 0x0100;
	smc_pm.start_storage	= 0x1000;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	/* Reference Quantum 6-00423013 SCSI Reference - Rev A */
	ymd(&year, &month, &day, &h, &m, &sec);

	/* Controller firmware build date */
	sprintf((char *)&lu->inquiry[36], "%04d-%02d-%02d %02d:%02d:%02d",
				year, month, day, h, m, sec);

	lu->inquiry[55] = 0x01;	/* Contains barcode scanner : BarC */

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
}
