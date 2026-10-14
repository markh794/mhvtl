#ifndef SMC_H
#define SMC_H

#include <stdint.h>

struct scsi_cmd;
struct s_info;

/* Element type codes */
#define ANY				 0
#define MEDIUM_TRANSPORT 1
#define STORAGE_ELEMENT	 2
#define MAP_ELEMENT		 3
#define DATA_TRANSFER	 4

#define CAP_CLOSED 1
#define CAP_OPEN   0
#define OPERATOR   1
#define ROBOT_ARM  0

/* Capacity reported by READ BUFFER descriptor mode. The emulated buffer
 * holds nothing, so this only has to be a value a host can work with.
 */
#define SMC_READ_BUFFER_SZ 65536

struct smc_personality_template {
	char	*name;
	uint32_t library_has_map : 1;
	uint32_t library_has_barcode_reader : 1;
	uint32_t library_has_playground : 1;
	uint32_t dvcid_serial_only : 1;
	uint32_t no_dvcid_flag : 1;
	/* Serial-only identifier is a NUL terminated string, NUL filled to
	 * dvcid_len, so it holds at most dvcid_len - 1 characters (Spectra
	 * T-Series).
	 */
	uint32_t dvcid_nul_terminated : 1;
	/* Report ID VALID and the drive's SCSI bus address in the data
	 * transfer element descriptor. SMC-2 marks both fields obsolete,
	 * but libraries with parallel SCSI drives still populate them
	 * (HPE, IBM, Quantum); ones without report zero (StorageTek).
	 */
	uint32_t report_scsi_bus_address : 1;
	/* With DVCID=1 a data transfer element descriptor reports only the
	 * drive's identity: Except set with ASC/ASCQ 81/00 (status is
	 * questionable, drive is present), and Access, Full, ID Valid, the bus
	 * address, SValid and the source address cleared (IBM TS3500/3584 SCSI
	 * Reference, READ ELEMENT STATUS, Element Type 4 descriptor with
	 * DVCID=B'1', table 7).
	 */
	uint32_t dvcid_identity_only : 1;

	uint32_t start_drive;
	uint32_t start_picker;
	uint32_t start_map;
	uint32_t start_storage;
	uint32_t dvcid_len;

	struct lu_phy_attr *lu;
};

uint8_t smc_allow_removal(struct scsi_cmd *cmd);
uint8_t smc_initialize_element_status(struct scsi_cmd *cmd);
uint8_t smc_initialize_element_status_with_range(struct scsi_cmd *cmd);
uint8_t smc_log_sense(struct scsi_cmd *cmd);
uint8_t smc_move_medium(struct scsi_cmd *cmd);
uint8_t smc_position_to_element(struct scsi_cmd *cmd);
uint8_t smc_read_buffer(struct scsi_cmd *cmd);
uint8_t smc_read_element_status(struct scsi_cmd *cmd);
uint8_t smc_request_volume_element_address(struct scsi_cmd *cmd);
uint8_t smc_send_volume_tag(struct scsi_cmd *cmd);
uint8_t smc_rezero(struct scsi_cmd *cmd);
uint8_t smc_open_close_import_export_element(struct scsi_cmd *cmd);

int	 get_cart_type(char *barcode);
void update_home_dir(long my_id); /* for the 'get_cart_type()' function only */

int	 slotOccupied(struct s_info *s);
void setImpExpStatus(struct s_info *s, int flg);
void setSlotEmpty(struct s_info *s);
void unload_drive_on_shutdown(struct s_info *src, struct s_info *dest);

void init_slot_info(struct lu_phy_attr *lu);
void init_stkl20(struct lu_phy_attr *lu);
void init_stklxx(struct lu_phy_attr *lu);
void init_stkslxx(struct lu_phy_attr *lu);
void init_default_smc(struct lu_phy_attr *lu);
void init_scalar_smc(struct lu_phy_attr *lu);
void init_spectra_logic_smc(struct lu_phy_attr *lu);
void init_spectra_215_smc(struct lu_phy_attr *lu);
void init_spectra_gator_smc(struct lu_phy_attr *lu);
void init_ibmts3100(struct lu_phy_attr *lu);
void init_ibm3584(struct lu_phy_attr *lu);
void init_hp_eml_smc(struct lu_phy_attr *lu);
void init_hp_msl_smc(struct lu_phy_attr *lu);
void init_overland_smc(struct lu_phy_attr *lu);
void smc_personality_module_register(struct smc_personality_template *pm);
void setAccessStatus(struct s_info *s, int flg);

#endif /* SMC_H */