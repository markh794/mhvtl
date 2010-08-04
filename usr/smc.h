
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

/*
 * FIXME: The following should be dynamic (read from config file)
 *
 **** Danger Will Robinson!! ****:
 *   START_DRIVE HAS TO start at slot 1
 *	The Order of Drives with lowest start, followed by Picker, followed
 *	by MAP, finally Storage slots is IMPORTANT. - You have been warned.
 *   Some of the logic in this source depends on it.
 */
#define START_DRIVE	0x0001
#define START_PICKER	0x0100
#define START_MAP	0x0200
#define START_STORAGE	0x0400

int smc_allow_removal(struct scsi_cmd *cmd);
int smc_initialize_element(struct scsi_cmd *cmd);
int smc_initialize_element_range(struct scsi_cmd *cmd);
int smc_move_medium(struct scsi_cmd *cmd);
int smc_read_element_status(struct scsi_cmd *cmd);
int smc_rezero(struct scsi_cmd *cmd);
int smc_start_stop(struct scsi_cmd *cmd);

int slotOccupied(struct s_info *s);
void setImpExpStatus(struct s_info *s, int flg);
void setSlotEmpty(struct s_info *s);
