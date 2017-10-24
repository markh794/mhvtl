/*
 * Personality module for Spectra Logic
 */

#include <stdio.h>
#include "list.h"
#include "vtllib.h"
#include "smc.h"
#include "logging.h"
#include "be_byteshift.h"
#include "log.h"
#include "mode.h"
#include "scsi.h"

static void update_spectra_215_device_capabilities(struct lu_phy_attr *lu)
{
	struct mode *mp;

	mp = lookup_pcode(&lu->mode_pg, MODE_DEVICE_CAPABILITIES, 0);
	if (!mp) {	/* Can't find page ??? */
		MHVTL_ERR("Can't find MODE_DEVICE_CAPABILITIES page");
		return;
	}

	mp->pcodePointer[2] = 0x0b;
	mp->pcodePointer[3] = 0x00;
	mp->pcodePointer[4] = 0x0a;
	mp->pcodePointer[5] = 0x0b;
	mp->pcodePointer[6] = 0x00;
	mp->pcodePointer[7] = 0x0b;
}

static void update_spectra_gator_device_capabilities(struct lu_phy_attr *lu)
{
	struct mode *mp;

	mp = lookup_pcode(&lu->mode_pg, MODE_DEVICE_CAPABILITIES, 0);
	if (!mp) {	/* Can't find page ??? */
		MHVTL_ERR("Can't find MODE_DEVICE_CAPABILITIES page");
		return;
	}

	mp->pcodePointer[2] = 0x0e;
	mp->pcodePointer[3] = 0x00;
	mp->pcodePointer[4] = 0x0e;
	mp->pcodePointer[5] = 0x0e;
	mp->pcodePointer[6] = 0x0e;
	mp->pcodePointer[7] = 0x0e;
}

static void update_spectra_t_series_device_capabilities(struct lu_phy_attr *lu)
{
	struct mode *mp;

	mp = lookup_pcode(&lu->mode_pg, MODE_DEVICE_CAPABILITIES, 0);
	if (!mp) {	/* Can't find page ??? */
		MHVTL_ERR("Can't find MODE_DEVICE_CAPABILITIES page");
		return;
	}

	mp->pcodePointer[2] = 0x0e;
	mp->pcodePointer[3] = 0x00;
	mp->pcodePointer[4] = 0x0e;
	mp->pcodePointer[5] = 0x0e;
	mp->pcodePointer[6] = 0x0e;
	mp->pcodePointer[7] = 0x0e;
}

static struct smc_personality_template smc_pm = {
	.library_has_map		= TRUE,
	.library_has_barcode_reader	= TRUE,
	.library_has_playground		= FALSE,

	/* Rev G of SpectraLogic Tseries states this is now 1Eh
	 * Yet...
	 * DVCID=1 'Identifier Length (1Eh) but goes on to describe
	 * fields 52 through 83 are for the Device Identifier -> and that
	 * adds up to 32 by my calculations. And in table 10-5 specifies
	 * Identifier Length as 1Gh... (hmmm forgot to carry the one in
	 * base 16 addition when they got to 1Fh) - which is of course
	 * 20h (32 decimal).
	 */
	.dvcid_len			= 0x20,
};

void init_spectra_215_smc(struct lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - Spectra Treefrog emulation";
	smc_pm.library_has_map = FALSE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.dvcid_serial_only = TRUE;
	smc_pm.no_dvcid_flag = TRUE;
	smc_pm.dvcid_len	= 0;

	/* Extracted from Spectra Treefrog-Series SCSI Developers guide */
	smc_pm.start_picker	= 86;
	smc_pm.start_map	= 99;	/* fake */
	smc_pm.start_drive	= 31;
	smc_pm.start_storage	= 1;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
	/* Now that 'init_smc_mode_pages()' has allocated device capabilities
	 * page, update to valid default values for Spectra-Logic treefrog
	 */
	update_spectra_215_device_capabilities(lu);
}

void init_spectra_gator_smc(struct lu_phy_attr *lu)
{
	smc_pm.name = "mhVTL - Spectra Gator emulation";
	smc_pm.library_has_map = TRUE;
	smc_pm.library_has_barcode_reader = TRUE;
	smc_pm.dvcid_serial_only = TRUE;
	smc_pm.dvcid_len	= 10;

	/* Extracted from Spectra Gator SCSI Developers guide */
	smc_pm.start_picker	= 0x02c3;
	smc_pm.start_map	= 0x0001;
	smc_pm.start_drive	= 0x02a3;
	smc_pm.start_storage	= 0x001e;

	smc_pm.lu = lu;
	smc_personality_module_register(&smc_pm);

	init_slot_info(lu);

	init_smc_log_pages(lu);
	init_smc_mode_pages(lu);
	/* Now that 'init_smc_mode_pages()' has allocated device capabilities
	 * page, update to valid default values for Spectra-Logic Gator Series
	 */
	update_spectra_gator_device_capabilities(lu);
}

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
