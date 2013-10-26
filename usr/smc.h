
/* Element type codes */
#define ANY			0
#define MEDIUM_TRANSPORT	1
#define STORAGE_ELEMENT		2
#define MAP_ELEMENT		3
#define DATA_TRANSFER		4

#define CAP_CLOSED	1
#define CAP_OPEN	0
#define OPERATOR	1
#define ROBOT_ARM	0

struct smc_personality_template {
	char *name;
	uint32_t library_has_map:1;
	uint32_t library_has_barcode_reader:1;
	uint32_t library_has_playground:1;
	uint32_t dvcid_serial_only:1;

	uint32_t start_drive;
	uint32_t start_picker;
	uint32_t start_map;
	uint32_t start_storage;

	struct lu_phy_attr *lu;
};

uint8_t smc_allow_removal(struct scsi_cmd *cmd);
uint8_t smc_initialize_element_status(struct scsi_cmd *cmd);
uint8_t smc_initialize_element_status_with_range(struct scsi_cmd *cmd);
uint8_t smc_log_sense(struct scsi_cmd *cmd);
uint8_t smc_move_medium(struct scsi_cmd *cmd);
uint8_t smc_read_element_status(struct scsi_cmd *cmd);
uint8_t smc_rezero(struct scsi_cmd *cmd);
uint8_t smc_open_close_import_export_element(struct scsi_cmd *cmd);

int slotOccupied(struct s_info *s);
void setImpExpStatus(struct s_info *s, int flg);
void setSlotEmpty(struct s_info *s);
void unload_drive_on_shutdown(struct s_info *src, struct s_info *dest);

void init_slot_info(struct lu_phy_attr *lu);
void init_stklxx(struct lu_phy_attr *lu);
void init_default_smc(struct lu_phy_attr *lu);
void init_scalar_smc(struct lu_phy_attr *lu);
void init_spectra_logic_smc(struct  lu_phy_attr *lu);
void init_ibmts3100(struct  lu_phy_attr *lu);
void init_ibmts3500(struct  lu_phy_attr *lu);
void init_hp_eml_smc(struct  lu_phy_attr *lu);
void smc_personality_module_register(struct smc_personality_template *pm);
