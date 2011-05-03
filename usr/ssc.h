
#define ENCR_C	1	/* Device supports Encryption */
#define ENCR_E	4	/* Encryption is enabled */

#define ENCR_IN_SUPPORT_PAGES		0
#define ENCR_OUT_SUPPORT_PAGES		1
#define ENCR_CAPABILITIES		0x10
#define ENCR_KEY_FORMATS		0x11
#define ENCR_KEY_MGT_CAPABILITIES	0x12
#define ENCR_DATA_ENCR_STATUS		0x20
#define ENCR_NEXT_BLK_ENCR_STATUS	0x21

#define ENCR_SET_DATA_ENCRYPTION	0x10

struct media_handling {
	char media_type[16];
	char op[8];
	unsigned char density;
};

struct ssc_personality_template {
	char *name;
	int drive_native_density;
	int drive_type;
	struct media_handling *media_capabilities;
	uint8_t (*valid_encryption_blk)(struct scsi_cmd *cmd);
	uint8_t (*valid_encryption_media)(struct scsi_cmd *cmd);
	int (*encryption_capabilities)(struct scsi_cmd *cmd);
	int (*kad_validation)(int encrypt_mode, int akad, int ukad);
	uint8_t (*update_encryption_mode)(void *p, int mode);
	uint8_t (*check_restrictions)(struct scsi_cmd *cmd);
	uint8_t (*clear_compression)(void);
	uint8_t (*set_compression)(int level);
	uint8_t (*clear_WORM)(void);
	uint8_t (*set_WORM)(void);
	uint8_t (*mode_sense)(struct scsi_cmd *cmd);
	uint8_t (*mode_select)(struct scsi_cmd *cmd);
};

int readBlock(uint8_t *buf, uint32_t request_sz, int sili, uint8_t *sam_stat);
int writeBlock(struct scsi_cmd *cmd, uint32_t request_sz);

uint8_t ssc_a3_service_action(struct scsi_cmd *cmd);
uint8_t ssc_a4_service_action(struct scsi_cmd *cmd);
uint8_t ssc_allow_overwrite(struct scsi_cmd *cmd);
uint8_t ssc_allow_prevent_removal(struct scsi_cmd *cmd);
uint8_t ssc_erase(struct scsi_cmd *cmd);
uint8_t ssc_format_media(struct scsi_cmd *cmd);
uint8_t ssc_load_display(struct scsi_cmd *cmd);
uint8_t ssc_mode_select(struct scsi_cmd *cmd);
uint8_t ssc_pr_in(struct scsi_cmd *cmd);
uint8_t ssc_pr_out(struct scsi_cmd *cmd);
uint8_t ssc_read_6(struct scsi_cmd *cmd);
uint8_t ssc_read_attributes(struct scsi_cmd *cmd);
uint8_t ssc_read_block_limits(struct scsi_cmd *cmd);
uint8_t ssc_read_display(struct scsi_cmd *cmd);
uint8_t ssc_read_media_sn(struct scsi_cmd *cmd);
uint8_t ssc_read_position(struct scsi_cmd *cmd);
uint8_t ssc_release(struct scsi_cmd *cmd);
uint8_t ssc_report_density(struct scsi_cmd *cmd);
uint8_t ssc_report_luns(struct scsi_cmd *cmd);
uint8_t ssc_reserve(struct scsi_cmd *cmd);
uint8_t ssc_rewind(struct scsi_cmd *cmd);
uint8_t ssc_seek_10(struct scsi_cmd *cmd);
uint8_t ssc_space(struct scsi_cmd *cmd);
uint8_t ssc_spin(struct scsi_cmd *cmd);
uint8_t ssc_spout(struct scsi_cmd *cmd);
uint8_t ssc_load_unload(struct scsi_cmd *cmd);
uint8_t ssc_tur(struct scsi_cmd *cmd);
uint8_t ssc_write_6(struct scsi_cmd *cmd);
uint8_t ssc_write_attributes(struct scsi_cmd *cmd);
uint8_t ssc_write_filemarks(struct scsi_cmd *cmd);

void init_ait1_ssc(struct lu_phy_attr *lu);
void init_ait2_ssc(struct lu_phy_attr *lu);
void init_ait3_ssc(struct lu_phy_attr *lu);
void init_ait4_ssc(struct lu_phy_attr *lu);
void init_default_ssc(struct lu_phy_attr *lu);
void init_t10kA_ssc(struct lu_phy_attr *lu);
void init_t10kB_ssc(struct lu_phy_attr *lu);
void init_t10kC_ssc(struct lu_phy_attr *lu);
void init_ult3580_td1(struct lu_phy_attr *lu);
void init_ult3580_td2(struct lu_phy_attr *lu);
void init_ult3580_td3(struct lu_phy_attr *lu);
void init_ult3580_td4(struct lu_phy_attr *lu);
void init_ult3580_td5(struct lu_phy_attr *lu);
void init_hp_ult_1(struct lu_phy_attr *lu);
void init_hp_ult_2(struct lu_phy_attr *lu);
void init_hp_ult_3(struct lu_phy_attr *lu);
void init_hp_ult_4(struct lu_phy_attr *lu);
void init_hp_ult_5(struct lu_phy_attr *lu);
void init_3592_j1a(struct lu_phy_attr *lu);
void init_3592_E05(struct lu_phy_attr *lu);
void init_3592_E06(struct lu_phy_attr *lu);

void register_ops(struct lu_phy_attr *lu, int op, void *f);

uint8_t valid_encryption_blk(struct scsi_cmd *cmd);
uint8_t check_restrictions(struct scsi_cmd *cmd);
void init_default_ssc_mode_pages(struct mode *m);
uint8_t resp_spin(struct scsi_cmd *cmd);
uint8_t resp_spout(struct scsi_cmd *cmd);
int resp_write_attribute(struct scsi_cmd *cmd);
int resp_read_attribute(struct scsi_cmd *cmd);
int resp_report_density(uint8_t media, struct vtl_ds *dbuf_p);
void resp_space(int32_t count, int code, uint8_t *sam_stat);
void unloadTape(uint8_t *sam_stat);
