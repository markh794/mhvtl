/*
 * This daemon is the SCSI SMC target (Medium Changer) portion of the
 * vtl package.
 *
 * The vtl package consists of:
 *   a kernel module (vlt.ko) - Currently on 2.6.x Linux kernel support.
 *   SCSI target daemons for both SMC and SSC devices.
 *
 * Copyright (C) 2005 - 2009 Mark Harvey       markh794@gmail.com
 *                                          mark.harvey at veritas.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * See comments in vtltape.c for a more complete version release...
 *
 * 0.14 13 Feb 2008
 *	Since ability to define device serial number, increased ver from
 *	0.12 to 0.14
 *
 * v0.12 -> Forked into 'stable' (0.12) and 'devel' (0.13).
 *          My current thinking : This is a dead end anyway.
 *          An iSCSI target done in user-space is now my perferred solution.
 *          This means I don't have to do any kernel level drivers
 *          and leaverage the hosts native iSCSI initiator.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <syslog.h>
#include <ctype.h>
#include <inttypes.h>
#include <signal.h>
#include <pwd.h>
#include "vtl_common.h"
#include "scsi.h"
#include "list.h"
#include "q.h"
#include "logging.h"
#include "vtllib.h"
#include "spc.h"
#include "smc.h"
#include "mode.h"
#include "be_byteshift.h"
#include "log.h"

char vtl_driver_name[] = "vtllibrary";
long my_id = 0;

#define CAP_CLOSED	1
#define CAP_OPEN	0
#define OPERATOR	1
#define ROBOT_ARM	0

#define SMC_BUF_SIZE 1024 * 1024 /* Default size of buffer */

#define LIBCONTENTS "/library_contents."

int verbose = 0;
int debug = 0;
static uint8_t sam_status = 0;		/* Non-zero if Sense-data is valid */
long backoff;	/* Backoff value for polling char device */

extern int current_state;	/* scope, Global -> Last status sent to fifo */

struct lu_phy_attr lunit;

static struct smc_priv smc_slots;

struct s_info *add_new_slot(struct lu_phy_attr *lu);

static void usage(char *progname)
{
	printf("Usage: %s -q <Q number> [-d] [-v]\n", progname);
	printf("      Where\n");
	printf("             'q number' is the queue priority number\n");
	printf("             'd' == debug -> Don't run as daemon\n");
	printf("             'v' == verbose -> Extra info logged via syslog\n");
}

#ifndef Solaris
 int ioctl(int, int, void *);
#endif

struct device_type_template smc_template = {
	.ops	= {
		/* 0x00 -> 0x0f */
		{spc_tur,},
		{smc_rezero,},
		{spc_illegal_op,},
		{spc_request_sense,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{smc_initialize_element_status,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		/* 0x10 -> 0x1f */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_inquiry,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_mode_select,},
		{spc_reserve,},
		{spc_release,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_mode_sense,},
		{smc_open_close_import_export_element,},
		{spc_recv_diagnostics,},
		{spc_send_diagnostics,},
		{smc_allow_removal,},
		{spc_illegal_op,},

		[0x20 ... 0x3f] = {spc_illegal_op,},

		/* 0x40 -> 0x4f */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_log_select,},
		{smc_log_sense,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		/* 0x50 -> 0x5f */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_mode_select,},
		{spc_reserve,},
		{spc_release,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_mode_sense,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		[0x60 ... 0x9f] = {spc_illegal_op,},

		/* 0xa0 -> 0xaf */
		{spc_illegal_op,}, /* processed in the kernel module */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{smc_move_medium,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		/* 0xb0 -> 0xbf */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{smc_read_element_status,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		[0xc0 ... 0xdf] = {spc_illegal_op,},

		/* 0xe0 -> 0xef */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{smc_initialize_element_status_with_range,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		[0xf0 ... 0xff] = {spc_illegal_op,},
	}
};

__attribute__((constructor)) static void smc_init(void)
{
	device_type_register(&lunit, &smc_template);
}

/*
 * Update ops[xx] with new/updated/custom function 'f'
 */
void register_ops(struct lu_phy_attr *lu, int op,
			void *f, void *g, void *h)
{
	lu->scsi_ops->ops[op].cmd_perform = f;
	lu->scsi_ops->ops[op].pre_cmd_perform = g;
	lu->scsi_ops->ops[op].post_cmd_perform = h;
}

/*
 *
 * Process the SCSI command
 *
 * Called with:
 *	cdev          -> Char dev file handle,
 *	cdb           -> SCSI Command buffer pointer,
 *	struct vtl_ds -> general purpose data structure... Need better name
 *
 * Return:
 *	SAM status returned in struct vtl_ds.sam_stat
 */
static void processCommand(int cdev, uint8_t *cdb, struct vtl_ds *dbuf_p,
			useconds_t pollInterval)
{
	int err = 0;
	struct scsi_cmd _cmd;
	struct scsi_cmd *cmd;
	cmd = &_cmd;

	cmd->scb = cdb;
	cmd->scb_len = 16;	/* fixme */
	cmd->dbuf_p = dbuf_p;
	cmd->lu = &lunit;
	cmd->pollInterval = pollInterval;
	cmd->cdev = cdev;

	MHVTL_DBG_PRT_CDB(1, cmd);

	switch (cdb[0]) {
	case REPORT_LUNS:
	case REQUEST_SENSE:
	case MODE_SELECT:
	case INQUIRY:
		dbuf_p->sam_stat = SAM_STAT_GOOD;
		break;
	default:
		if (cmd->lu->online == 0) {
			sam_not_ready(E_OFFLINE, &dbuf_p->sam_stat);
			return;
		}
		if (check_reset(&dbuf_p->sam_stat))
			return;
	}

	/* Skip main op code processing if pre-cmd returns non-zero */
	if (cmd->lu->scsi_ops->ops[cdb[0]].pre_cmd_perform)
		err = cmd->lu->scsi_ops->ops[cdb[0]].pre_cmd_perform(cmd, NULL);

	if (!err)
		dbuf_p->sam_stat = cmd->lu->scsi_ops->ops[cdb[0]].cmd_perform(cmd);

	/* Post op code processing regardless */
	if (cmd->lu->scsi_ops->ops[cdb[0]].post_cmd_perform)
		cmd->lu->scsi_ops->ops[cdb[0]].post_cmd_perform(cmd, NULL);

	return;
}

/*
 * Respond to messageQ 'list map' by sending a list of PCLs to messageQ
 */
static void list_map(struct q_msg *msg)
{
	struct list_head *slot_head = &smc_slots.slot_list;
	struct s_info *sp;
	char buf[MAXOBN];
	char *c = buf;
	*c = '\0';

	list_for_each_entry(sp, slot_head, siblings) {
		if (slotOccupied(sp) && sp->element_type == MAP_ELEMENT) {
			strncat(c, (char *)sp->media->barcode, MAX_BARCODE_LEN);
			MHVTL_DBG(2, "MAP slot %d full", sp->slot_location);
		} else {
			MHVTL_DBG(2, "MAP slot %d empty", sp->slot_location);
		}
	}
	MHVTL_DBG(2, "map contents: %s", buf);
	send_msg(buf, msg->snd_id);
}

/* Check existing MAP & Storage slots for existing barcode */
int already_in_slot(char *barcode)
{
	struct list_head *slot_head = &smc_slots.slot_list;
	struct s_info *sp = NULL;
	int len;

	len = strlen(barcode);

	list_for_each_entry(sp, slot_head, siblings) {
		if (slotOccupied(sp)) {
			if (!strncmp((char *)sp->media->barcode, barcode, len)){
				MHVTL_DBG(3, "Match: %s %s",
					sp->media->barcode, barcode);
				return 1;
			} else
				MHVTL_DBG(3, "No match: %s %s",
					sp->media->barcode, barcode);
		}
	}
	return 0;
}

static struct s_info *locate_empty_map(void)
{
	struct s_info *sp = NULL;
	struct list_head *slot_head = &smc_slots.slot_list;

	list_for_each_entry(sp, slot_head, siblings) {
		if (!slotOccupied(sp) && sp->element_type == MAP_ELEMENT)
			return sp;
	}

	return NULL;
}

static struct m_info *lookup_barcode(struct lu_phy_attr *lu, char *barcode)
{
	struct list_head *media_list_head;
	struct m_info *m;
	int match;

	media_list_head = &((struct smc_priv *)lu->lu_private)->media_list;

	list_for_each_entry(m, media_list_head, siblings) {
		match = strncmp(m->barcode, barcode, MAX_BARCODE_LEN + 1);
		if (!match) {
			MHVTL_DBG(3, "Match barcodes: %s %s: %d",
				barcode, m->barcode, match);
			return m;
		}
	}

	return NULL;
}

static struct m_info *add_barcode(struct lu_phy_attr *lu, char *barcode)
{
	struct list_head *media_list_head;
	struct m_info *m;

	if (strlen(barcode) > MAX_BARCODE_LEN) {
		MHVTL_ERR("Barcode \'%s\' exceeds max barcode lenght: %d",
				barcode, MAX_BARCODE_LEN);
		exit(1);
	}
	if (lookup_barcode(lu, barcode)) {
		MHVTL_ERR("Duplicate barcode %s.. Exiting", barcode);
		exit(1);
	}

	m = zalloc(sizeof(struct m_info));
	if (!m) {
		MHVTL_ERR("Out of memory allocating memory for barcode %s",
			barcode);
		exit(-ENOMEM);
	}

	media_list_head = &((struct smc_priv *)lu->lu_private)->media_list;

	memset(m, 0, sizeof(struct m_info));

	snprintf((char *)m->barcode, MAX_BARCODE_LEN + 1, LEFT_JUST_16_STR,
					barcode);
	m->barcode[MAX_BARCODE_LEN] = '\0';
	m->cart_type = get_cart_type(barcode);
	if (!strncmp((char *)m->barcode, "NOBAR", 5))
		m->internal_status = INSTATUS_NO_BARCODE;
	else
		m->internal_status = 0;

	list_add_tail(&m->siblings, media_list_head);
	return m;
}


/* Return zero - failed, non-zero - success */
static int load_map(struct q_msg *msg)
{
	struct s_info *sp = NULL;
	struct m_info *mp = NULL;
	char *barcode;
	int i;
	int str_len;
	char *text = &msg->text[9];	/* skip past "load map " */

	MHVTL_DBG(2, "Loading %s into MAP", text);

	if (smc_slots.cap_closed) {
		send_msg("MAP not opened", msg->snd_id);
		return 0;
	}

	str_len = strlen(text);
	barcode = NULL;
	for (i = 0; i < str_len; i++)
		if (isalnum(text[i])) {
			barcode = &text[i];
			break;
		}

	/* No barcode - reject load */
	if (!barcode) {
		send_msg("Bad barcode", msg->snd_id);
		return 0;
	}

	if (already_in_slot(barcode)) {
		send_msg("barcode already in library", msg->snd_id);
		return 0;
	}

	if (strlen(barcode) > MAX_BARCODE_LEN) {
		send_msg("barcode length too long", msg->snd_id);
		return 0;
	}

	sp = locate_empty_map();
	if (sp) {
		mp = lookup_barcode(&lunit, barcode);
		if (!mp)
			mp = add_barcode(&lunit, barcode);

		snprintf((char *)mp->barcode, MAX_BARCODE_LEN, LEFT_JUST_16_STR,
						barcode);
		mp->barcode[MAX_BARCODE_LEN] = '\0';

		/* 1 = data, 2 = Clean */
		mp->cart_type = get_cart_type(barcode);
		sp->status = STATUS_InEnab | STATUS_ExEnab |
					STATUS_Access | STATUS_ImpExp |
					STATUS_Full;
		/* Media placed by operator */
		setImpExpStatus(sp, OPERATOR);
		sp->media = mp;
		sp->media->internal_status = 0;
		send_msg("OK", msg->snd_id);
		return 1;
	}
	send_msg("MAP Full", msg->snd_id);
	return 0;
}

static void open_map(struct q_msg *msg)
{
	MHVTL_DBG(1, "Called");

	current_state = MHVTL_STATE_OPENING_MAP;

	smc_slots.cap_closed = CAP_OPEN;
	send_msg("OK", msg->snd_id);
}

static void close_map(struct q_msg *msg)
{
	MHVTL_DBG(1, "Called");

	current_state = MHVTL_STATE_CLOSING_MAP;

	smc_slots.cap_closed = CAP_CLOSED;
	send_msg("OK", msg->snd_id);
}

/* add new slot && assignment && initialization memory */
static void add_storage_slot(struct q_msg *msg) {
	int buffer_size;
	struct s_info *sp1 = NULL;
	struct list_head *slot_head = &smc_slots.slot_list;
	struct smc_priv *smc_p = lunit.lu_private;
	struct list_head *p = slot_head;

	sp1 = add_new_slot(&lunit);

	sp1->element_type = STORAGE_ELEMENT;
	smc_p->num_storage++;
	sp1->status = STATUS_Access;
	sp1->slot_location = smc_p->num_storage + smc_p->pm->start_storage - 1;

	/* Slot status to Empty */
	setSlotEmpty(sp1);

	init_smc_log_pages(&lunit);
	init_smc_mode_pages(&lunit);
	MHVTL_LOG("add slot && init smc");

	/* malloc a big enough buffer to fit worst case read element status */
	buffer_size = (smc_slots.num_drives + smc_slots.num_picker
			+ smc_slots.num_map + smc_slots.num_storage) * 80;
	buffer_size = max(SMC_BUF_SIZE, buffer_size);
	smc_slots.bufsize = buffer_size;
	MHVTL_DBG(1, "Setting buffer size to %d", buffer_size);

	send_msg("append a storage slot to library", msg->snd_id);
	return;
}

/*
 * Respond to messageQ 'empty map' by clearing 'ocuplied' status in map slots.
 * Return 0 on failure, non-zero - success.
 */
static int empty_map(struct q_msg *msg)
{
	struct s_info *sp;
	struct list_head *slot_head = &smc_slots.slot_list;

	if (smc_slots.cap_closed) {
		MHVTL_DBG(1, "MAP slot empty failed - CAP Not open");
		send_msg("Can't empty map while MAP is closed", msg->snd_id);
		return 0;
	}

	list_for_each_entry(sp, slot_head, siblings) {
		if (slotOccupied(sp) && sp->element_type == MAP_ELEMENT) {
			setSlotEmpty(sp);
			MHVTL_DBG(2, "MAP slot %d emptied",
					sp->slot_location -
						smc_slots.pm->start_map);
		}
	}

	send_msg("OK", msg->snd_id);
	return 1;
}

/*
 * Return 1, exit program
 */
static int processMessageQ(struct q_msg *msg)
{

	MHVTL_DBG(1, "Sender id: %ld, msg : %s", msg->snd_id, msg->text);

	if (!strncmp(msg->text, "debug", 5)) {
		if (debug) {
			debug--;
		} else {
			debug++;
			verbose = 2;
		}
	}
	if (!strncmp(msg->text, "add slot", 8))
		add_storage_slot(msg);
	if (!strncmp(msg->text, "empty map", 9))
		empty_map(msg);
	if (!strncmp(msg->text, "exit", 4))
		return 1;
	if (!strncmp(msg->text, "open map", 8))
		open_map(msg);
	if (!strncmp(msg->text, "close map", 9))
		close_map(msg);
	if (!strncmp(msg->text, "list map", 8))
		list_map(msg);
	if (!strncmp(msg->text, "load map ", 9))
		load_map(msg);
	if (!strncmp(msg->text, "offline", 7)) {
		current_state = MHVTL_STATE_OFFLINE;
		lunit.online = 0;
	}
	if (!strncmp(msg->text, "online", 6)) {
		current_state = MHVTL_STATE_ONLINE;
		lunit.online = 1;
	}
	if (!strncmp(msg->text, "TapeAlert", 9)) {
		uint64_t flg = TA_NONE;
		sscanf(msg->text, "TapeAlert %" PRIx64, &flg);
		set_TapeAlert(&lunit, flg);
	}
	if (!strncmp(msg->text, "verbose", 7)) {
		if (verbose)
			verbose--;
		else
			verbose = 3;
		MHVTL_LOG("verbose: %s at level %d",
				 verbose ? "enabled" : "disabled", verbose);
	}

return 0;
}

static struct d_info *lookup_drive(struct lu_phy_attr *lu, int drive_no)
{
	struct list_head *drive_list_head;
	struct d_info *d;
	uint32_t slot_offset;

	drive_list_head = &((struct smc_priv *)lu->lu_private)->drive_list;
	slot_offset = ((struct smc_priv *)lu->lu_private)->pm->start_drive;

	/* Drive numbering starts from 1, decrement slot_offset */
	slot_offset--;

	list_for_each_entry(d, drive_list_head, siblings) {
		MHVTL_DBG(3, "Slot location: %d, offset + drive_no: %d",
				d->slot->slot_location, slot_offset + drive_no);
		if (d->slot->slot_location == slot_offset + drive_no)
			return d;
	}

return NULL;
}

struct s_info *add_new_slot(struct lu_phy_attr *lu)
{
	struct s_info *new;
	struct list_head *slot_list_head;

	slot_list_head = &((struct smc_priv *)lu->lu_private)->slot_list;

	new = zalloc(sizeof(struct s_info));
	if (!new) {
		MHVTL_ERR("Could not allocate memory for new slot struct");
		exit(-ENOMEM);
	}

	list_add_tail(&new->siblings, slot_list_head);
	return new;
}

/* Open device config file and update device information
 */
static void update_drive_details(struct lu_phy_attr *lu)
{
	char *config = MHVTL_CONFIG_PATH"/device.conf";
	FILE *conf;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	int slot;
	long drv_id, lib_id;
	struct d_info *dp;
	struct s_info *sp;
	struct smc_priv *smc_p = lu->lu_private;

	conf = fopen(config , "r");
	if (!conf) {
		MHVTL_DBG(1, "Can not open config file %s : %s", config,
					strerror(errno));
		perror("Can not open config file");
		exit(1);
	}
	s = zalloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = zalloc(MALLOC_SZ);
	if (!b) {
		perror("Could not allocate memory");
		exit(1);
	}

	drv_id = -1;
	dp = NULL;

	/* While read in a line */
	while (readline(b, MALLOC_SZ, conf) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		if (sscanf(b, "Drive: %ld", &drv_id) > 0)
			continue;
		if (sscanf(b, " Library ID: %ld Slot: %d", &lib_id, &slot) == 2
					&& lib_id == my_id
					&& drv_id >= 0) {
			MHVTL_DBG(2, "Found Drive %ld in slot %d",
					drv_id, slot);
			dp = lookup_drive(lu, slot);
			if (!dp) {
				MHVTL_LOG("WARNING: Creating new entry for %ld",
									drv_id);
				dp = zalloc(sizeof(struct d_info));
				if (!dp) {
					MHVTL_ERR("Couldn't malloc memory");
					exit(-ENOMEM);
				}
				sp = add_new_slot(lu);
				sp->element_type = DATA_TRANSFER;
				dp->slot = sp;
				sp->drive = dp;
				list_add_tail(&dp->siblings,
						&smc_p->drive_list);
			}
			MHVTL_DBG(3, "Updating drive id in slot %d to : %ld",
						dp->slot->slot_location,
						drv_id);
			dp->drv_id = drv_id;
			continue;
		}
		if (dp) {
			if (sscanf(b, " Unit serial number: %s", s) > 0) {
				strncpy(dp->inq_product_sno, s, 10);
				rmnl(dp->inq_product_sno, ' ', 10);
			}
			if (sscanf(b, " Product identification: %16c", s) > 0) {
				/* sscanf does not NULL terminate */
				/* 25 is len of ' Product identification: ' */
				s[strlen(b) - 25] = '\0';
				strncpy(dp->inq_product_id, s, 16);
				dp->inq_product_id[16] = 0;
				MHVTL_DBG(3, "id: \'%s\', inq_product_id: \'%s\'",
					s, dp->inq_product_id);
			}
			if (sscanf(b, " Product revision level: %s", s) > 0) {
				strncpy(dp->inq_product_rev, s, 4);
				rmnl(dp->inq_product_rev, ' ', 4);
			}
			if (sscanf(b, " Vendor identification: %s", s) > 0) {
				strncpy(dp->inq_vendor_id, s, 8);
				rmnl(dp->inq_vendor_id, ' ', 8);
			}
		}
		if (strlen(b) == 1) { /* Blank line => Reset device pointer */
			drv_id = -1;
			dp = NULL;
		}
	}

	free(b);
	free(s);
	fclose(conf);
}

/*
 * Return 0 - no address space conflict
 * Return 1 - overlap address with another slot type
 */
static int check_overflow(struct lu_phy_attr *lu, int slot, char type)
{
	struct smc_priv *smc_p;
	int co;

	smc_p = lu->lu_private;
	co = 0;

	switch (type) {
	case MAP_ELEMENT:
		co = slot + smc_p->pm->start_map;
		if (smc_p->pm->start_map < smc_p->pm->start_storage &&
					co > smc_p->pm->start_storage) {
			MHVTL_LOG("MAP: %d, overlaps with storage slot", slot);
			return 1;
		}
		if (smc_p->pm->start_map < smc_p->pm->start_picker &&
					co > smc_p->pm->start_picker) {
			MHVTL_LOG("MAP: %d, overlaps with Picker slot", slot);
			return 1;
		}
		if (smc_p->pm->start_map < smc_p->pm->start_drive &&
					co > smc_p->pm->start_drive) {
			MHVTL_LOG("MAP: %d, overlaps with Drives", slot);
			return 1;
		}
		break;
	case DATA_TRANSFER:
		co = slot + smc_p->pm->start_drive;
		if (smc_p->pm->start_drive < smc_p->pm->start_storage &&
					co > smc_p->pm->start_storage) {
			MHVTL_LOG("Drive: %d, overlaps with storage slot",
					slot);
			return 1;
		}
		if (smc_p->pm->start_drive < smc_p->pm->start_picker &&
					co > smc_p->pm->start_picker) {
			MHVTL_LOG("Drive: %d, overlaps with picker slot",
					slot);
			return 1;
		}
		if (smc_p->pm->start_drive < smc_p->pm->start_map &&
					co > smc_p->pm->start_map) {
			MHVTL_LOG("Drive: %d, overlaps with MAP slot",
					slot);
			return 1;
		}
		break;
	case MEDIUM_TRANSPORT:
		co = slot + smc_p->pm->start_picker;
		if (smc_p->pm->start_picker < smc_p->pm->start_map &&
					co > smc_p->pm->start_map) {
			MHVTL_LOG("Picker slot: %d overlaps with MAP", slot);
			return 1;
		}
		if (smc_p->pm->start_picker < smc_p->pm->start_drive &&
					co > smc_p->pm->start_drive) {
			MHVTL_LOG("Picker slot: %d overlaps with drives", slot);
			return 1;
		}
		if (smc_p->pm->start_picker < smc_p->pm->start_storage &&
					co > smc_p->pm->start_storage) {
			MHVTL_LOG("Picker slot: %d overlaps with Storage",
					slot);
			return 1;
		}
		break;
	case STORAGE_ELEMENT:
		co = slot + smc_p->pm->start_storage;
		if (smc_p->pm->start_storage < smc_p->pm->start_map &&
					co > smc_p->pm->start_map) {
			MHVTL_LOG("Storage slot: %d, overlaps with MAP", slot);
			return 1;
		}
		if (smc_p->pm->start_storage < smc_p->pm->start_picker &&
					co > smc_p->pm->start_picker) {
			MHVTL_LOG("Storage slot: %d, overlaps with picker",
					slot);
			return 1;
		}
		if (smc_p->pm->start_storage < smc_p->pm->start_drive &&
					co > smc_p->pm->start_drive) {
			MHVTL_LOG("Storage slot: %d, overlaps with drives",
					slot);
			return 1;
		}
		break;
	}
	return 0;
}

void init_drive_slot(struct lu_phy_attr *lu, int slt, char *s)
{
	struct s_info *sp = NULL;
	struct d_info *dp = NULL;
	struct smc_priv *smc_p = lu->lu_private;

	if (check_overflow(lu, slt, DATA_TRANSFER))
		return;

	dp = lookup_drive(lu, slt);
	if (!dp) {
		dp = zalloc(sizeof(struct d_info));
		if (!dp) {
			MHVTL_ERR("Couldn't malloc memory");
			exit(-ENOMEM);
		}
		sp = add_new_slot(lu);
		sp->element_type = DATA_TRANSFER;
		dp->slot = sp;
		sp->drive = dp;
		list_add_tail(&dp->siblings, &smc_p->drive_list);
	}
	dp->slot->slot_location = slt + smc_p->pm->start_drive - 1;
	dp->slot->status = STATUS_Access;
	smc_p->num_drives++;
	if (strlen(s)) {
		strncpy(dp->inq_product_sno, s, 10);
		MHVTL_DBG(2, "Drive s/no: %s", s);
	}
	MHVTL_DBG(3, "Slot: %d, start_drive: %d, slot_location: %d",
			slt, smc_p->pm->start_drive, dp->slot->slot_location);
}

void init_map_slot(struct lu_phy_attr *lu, int slt, char *barcode)
{
	struct s_info *sp = NULL;
	struct smc_priv *smc_p = lu->lu_private;

	if (check_overflow(lu, slt, MAP_ELEMENT))
		return;

	sp = add_new_slot(lu);
	sp->element_type = MAP_ELEMENT;
	smc_p->num_map++;

	sp->slot_location = slt + smc_p->pm->start_map - 1;
	sp->status = STATUS_InEnab | STATUS_ExEnab |
				STATUS_Access | STATUS_ImpExp;

	if (strlen(barcode)) {
		MHVTL_DBG(2, "Barcode %s in MAP %d", barcode, slt);
		sp->media = add_barcode(lu, barcode);
		sp->status |= STATUS_Full;
	}
}

void init_transport_slot(struct lu_phy_attr *lu, int slt, char *barcode)
{
	struct s_info *sp = NULL;
	struct smc_priv *smc_p = lu->lu_private;

	if (check_overflow(lu, slt, MEDIUM_TRANSPORT))
		return;

	sp = add_new_slot(lu);
	sp->element_type = MEDIUM_TRANSPORT;
	smc_p->num_picker++;
	sp->slot_location = slt + smc_p->pm->start_picker - 1;
	sp->status = 0;

	if (strlen(barcode)) {
		MHVTL_DBG(2, "Barcode %s in Picker %d", barcode, slt);
		sp->media = add_barcode(lu, barcode);
		sp->slot_location = slt + smc_p->pm->start_picker - 1;
		sp->status |= STATUS_Full;
	}
}

void init_storage_slot(struct lu_phy_attr *lu, int slt, char *barcode)
{
	struct s_info *sp = NULL;
	struct smc_priv *smc_p = lu->lu_private;

	if (check_overflow(lu, slt, STORAGE_ELEMENT))
		return;

	sp = add_new_slot(lu);
	sp->element_type = STORAGE_ELEMENT;
	smc_p->num_storage++;
	sp->status = STATUS_Access;
	sp->slot_location = slt + smc_p->pm->start_storage - 1;
	if (strlen(barcode)) {
		MHVTL_DBG(2, "Barcode %s in slot %d", barcode, slt);
		sp->media = add_barcode(lu, barcode);
		/* Slot full */
		sp->status |= STATUS_Full;
	}
}

static void __init_slot_info(struct lu_phy_attr *lu, int type)
{
	char conf[256];
	FILE *ctrl;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	int slt;
	struct stat configstat;
	struct stat persiststat;
	int filestat;

	filestat = -1;	/* Default to .persist file does not exist */

	/* Lets stat each (potential) file and identify the last one modified */
	snprintf(conf, ARRAY_SIZE(conf), MHVTL_CONFIG_PATH
					LIBCONTENTS "%ld" ".persist", my_id);

	if (lu->persist)	/* If enabled - stat .persist file */
		filestat = stat(conf, &persiststat);

	if (filestat < 0) {
		/* PERSIST is either disabled or .persist file does not exist
		 * - Update config filename to master 'library_contents.<id>
		*/
		snprintf(conf, ARRAY_SIZE(conf), MHVTL_CONFIG_PATH
					LIBCONTENTS "%ld", my_id);
	} else {
		/* stat original config file */
		snprintf(conf, ARRAY_SIZE(conf), MHVTL_CONFIG_PATH
					LIBCONTENTS "%ld", my_id);
		filestat = stat(conf, &configstat);
		if (filestat < 0) {	/* Does not exist !! */
			MHVTL_ERR("Can not stat config file %s: %s",
						conf, strerror(errno));
			exit(1);
		}
		if (configstat.st_mtime > persiststat.st_mtime) {
			/* Don't do anything - leave config filename alone */
			MHVTL_DBG(1, "%s is newer than %s.persist file. "
					"Using %s instead", conf, conf, conf);
		} else {
			/* Update the config file to
			   library_contents.<id>.persist
			*/
			snprintf(conf, ARRAY_SIZE(conf), MHVTL_CONFIG_PATH
					LIBCONTENTS "%ld" ".persist", my_id);
		}
	}

	/* By the time we get here -
	 * - If PERSIST is enabled
	 *   - We have stat'ed each file - so unless it's been removed within
	 *     last few millisecs we should be good.
	 * - Filename will be latest modify date
	*/
	ctrl = fopen(conf , "r");
	if (!ctrl) {
		MHVTL_ERR("Can not open config file %s : %s", conf,
					strerror(errno));
		exit(1);
	}

	/* Log which config file is being used to read in data */
	MHVTL_DBG(2, "Reading %s configuration information from %s",
						slot_type_str(type),
						conf);

	/* Grab a couple of generic MALLOC_SZ buffers.. */
	s = zalloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = zalloc(MALLOC_SZ);
	if (!b) {
		perror("Could not allocate memory");
		exit(1);
	}

	rewind(ctrl);
	while (readline(b, MALLOC_SZ, ctrl) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		s[0] = '\0';

		switch (type) {
		case DATA_TRANSFER:
			if (sscanf(b, "Drive %d: %s", &slt, s))
				init_drive_slot(lu, slt, s);
			break;

		case MAP_ELEMENT:
			if (sscanf(b, "MAP %d: %s", &slt, s))
				init_map_slot(lu, slt, s);
			break;

		case MEDIUM_TRANSPORT:
			if (sscanf(b, "Picker %d: %s", &slt, s))
				init_transport_slot(lu, slt, s);
			break;

		case STORAGE_ELEMENT:
			if (sscanf(b, "Slot %d: %s", &slt, s))
				init_storage_slot(lu, slt, s);
			break;
		}
	}
	fclose(ctrl);
	free(b);
	free(s);
}

/* Linked list data needs to be built in slot order */
void init_slot_info(struct lu_phy_attr *lu)
{
	int i;
	struct smc_type_slot arr[4];

	sort_library_slot_type(lu, &arr[0]);

	for (i = 0; i < 4; i++)
		__init_slot_info(lu, arr[i].type);
}

/* Return original slot location if empty
 */
static struct s_info *previous_storage_slot(struct s_info *s,
						struct list_head *slot_head)
{
	struct s_info *sp;	/* Slot Pointer */

	/* Find slot info for 'previous location' */
	list_for_each_entry(sp, slot_head, siblings) {
		if (sp->element_type == STORAGE_ELEMENT)
			if (sp->slot_location == s->last_location)
				if (!slotOccupied(sp))
					/* previous location is empty */
					return sp;
	}

	return NULL;
}

/* Return first empty storage slot.
 */
static struct s_info *find_empty_storage_slot(struct s_info *s,
						struct list_head *slot_head)
{
	struct s_info *sp;	/* Slot Pointer */

	/* If previous location is no good - lets find first empty slot */
	list_for_each_entry(sp, slot_head, siblings) {
		if (!slotOccupied(sp) && sp->element_type == STORAGE_ELEMENT)
			return sp;
	}

	return NULL;
}

/* Save config on shutdown - Not to be called at other times !! */
static void save_config(struct lu_phy_attr *lu)
{
	FILE *ctrl;
	char conf[256];
	struct smc_priv *lu_priv;
	struct list_head *slot_head;
	struct list_head *drive_head;
	struct s_info *sp;	/* Slot Pointer */
	struct d_info *dp;	/* Drive Pointer */
	int last_element_type = 0;

	if (strlen(MHVTL_CONFIG_PATH LIBCONTENTS) >=
				ARRAY_SIZE(conf) - sizeof(".persist")) {
		MHVTL_LOG("Filename length exceeds %d", (int)ARRAY_SIZE(conf));
	}

	snprintf(conf, ARRAY_SIZE(conf), MHVTL_CONFIG_PATH
					LIBCONTENTS "%ld" ".persist", my_id);
	ctrl = fopen(conf, "w");
	if (!ctrl) {
		MHVTL_ERR("Can not open file %s to save state : %s", conf,
					strerror(errno));
		return;
	}

	lu_priv = lu->lu_private;

	drive_head = &lu_priv->drive_list;
	slot_head = &lu_priv->slot_list;

	/* Walk each drive and force-unload into previous location
	 * - if possible */
	list_for_each_entry(dp, drive_head, siblings) {
		if (slotOccupied(dp->slot)) {
			/* Force a move of media from drive into
			 * empty storage slot on shutdown
			 */
			sp = dp->slot;
			MHVTL_DBG(1, "Found %s in drive %d from %d",
					sp->media->barcode,
					sp->slot_location -
						lu_priv->pm->start_drive + 1,
					sp->last_location);
			unload_drive_on_shutdown(sp,
					previous_storage_slot(sp, slot_head));
		}
	}

	/* Walk each drive and force-unload into first empty storage slot */
	list_for_each_entry(dp, drive_head, siblings) {
		if (slotOccupied(dp->slot)) {
			/* Force a move of media from drive into
			 * empty storage slot on shutdown
			 */
			sp = dp->slot;
			MHVTL_DBG(1, "Found %s in drive %d from %d",
					sp->media->barcode,
					sp->slot_location -
						lu_priv->pm->start_drive + 1,
					sp->last_location);
			unload_drive_on_shutdown(sp,
					find_empty_storage_slot(sp, slot_head));
		}
	}

	/* Walk the list of all slots and write data into .persist file */
	list_for_each_entry(sp, slot_head, siblings) {
		/* Pretty up conf file -
		 * Place a blank line between element types
		 */
		if (last_element_type != sp->element_type) {
			last_element_type = sp->element_type;
			fprintf(ctrl, "\n");
		}

		switch (sp->element_type) {
		case DATA_TRANSFER:
			fprintf(ctrl, "Drive %d:\n",
					sp->slot_location -
						lu_priv->pm->start_drive + 1);
			break;
		case MEDIUM_TRANSPORT:
			fprintf(ctrl, "Picker %d: %s\n",
				sp->slot_location -
						lu_priv->pm->start_picker + 1,
				slotOccupied(sp) ? sp->media->barcode : "");
			break;
		case MAP_ELEMENT:
			fprintf(ctrl, "MAP %d: %s\n",
				sp->slot_location -
						lu_priv->pm->start_map + 1,
				slotOccupied(sp) ? sp->media->barcode : "");
			break;
		case STORAGE_ELEMENT:
			fprintf(ctrl, "Slot %d: %s\n",
				sp->slot_location -
						lu_priv->pm->start_storage + 1,
				slotOccupied(sp) ? sp->media->barcode : "");
			break;
		}
	}
	fclose(ctrl);
}

static int init_lu(struct lu_phy_attr *lu, unsigned minor, struct vtl_ctl *ctl)
{

	struct vpd **lu_vpd = lu->lu_vpd;

	char *config = MHVTL_CONFIG_PATH"/device.conf";
	FILE *conf;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	int indx;
	struct vtl_ctl tmpctl;
	int found = 0;
	int linecount;

	backoff = DEFLT_BACKOFF_VALUE;
	lu->persist = FALSE;

	/* Set static 'home_directory' var - used for get_cart_type() function */
	update_home_dir(my_id);

	/* Configure default inquiry data */
	memset(&lu->inquiry, 0, MAX_INQUIRY_SZ);
	lu->inquiry[0] = TYPE_MEDIUM_CHANGER;	/* SMC device */
	lu->inquiry[1] = 0x80;	/* Removable bit set */
	lu->inquiry[2] = 0x05;	/* SCSI Version (v3) */
	lu->inquiry[3] = 0x02;	/* Response Data Format */
	lu->inquiry[4] = 59;	/* Additional Length */
	lu->inquiry[6] = 0x01;	/* Addr16 */
	lu->inquiry[7] = 0x20;	/* Wbus16 */

	put_unaligned_be16(0x0300, &lu->inquiry[58]); /* SPC-3 No ver claimed */
	put_unaligned_be16(0x0960, &lu->inquiry[60]); /* iSCSI */
	put_unaligned_be16(0x0200, &lu->inquiry[62]); /* SSC */

	lu->ptype = TYPE_MEDIUM_CHANGER;	/* SSC */

	lu->sense_p = &sense[0];	/* Save pointer to sense buffer */

	conf = fopen(config , "r");
	if (!conf) {
		MHVTL_ERR("Can not open config file %s : %s", config,
					strerror(errno));
		perror("Can not open config file");
		exit(1);
	}
	s = zalloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = zalloc(MALLOC_SZ);
	if (!b) {
		perror("Could not allocate memory");
		exit(1);
	}

	lu->fifoname = NULL;
	lu->fifo_fd = NULL;
	lu->fifo_flag = 0;

	smc_slots.movecommand = NULL;
	smc_slots.commandtimeout = 20;

	/* While read in a line */
	linecount = 0;	/* Line count */
	while (readline(b, MALLOC_SZ, conf) != NULL) {
		linecount++;
		if (b[0] == '#')	/* Ignore comments */
			continue;
		if (strlen(b) == 1)	/* Reset drive number of blank line */
			indx = 0xff;
		if (sscanf(b, "Library: %d CHANNEL: %d TARGET: %d LUN: %d",
					&indx, &tmpctl.channel,
					&tmpctl.id, &tmpctl.lun)) {
			MHVTL_DBG(2, "Found Library %d, looking for %u",
							indx, minor);
			if (indx == minor) {
				found = 1;
				memcpy(ctl, &tmpctl, sizeof(tmpctl));
			}
		}
		if (indx == minor) {
			unsigned int c, d, e, f, g, h, j, k;
			int i;

			if (sscanf(b, " Unit serial number: %s", s)) {
				checkstrlen(s, SCSI_SN_LEN, linecount);
				snprintf(lu->lu_serial_no, SCSI_SN_LEN, "%-14s", s);
			}
			if (sscanf(b, " Product identification: %16c", s) > 0) {
				/* sscanf does not NULL terminate */
				i = strlen(b) - 25; /* len of ' Product identification: ' */
				s[i] = '\0';
				snprintf(lu->product_id, PRODUCT_ID_LEN + 1, "%-16s", s);
				sprintf(&lu->inquiry[16], "%-16s", s);
			}
			if (sscanf(b, " Product revision level: %s", s)) {
				checkstrlen(s, PRODUCT_REV_LEN, linecount);
				sprintf(&lu->inquiry[32], "%-4s", s);
			}
			if (sscanf(b, " Vendor identification: %s", s)) {
				checkstrlen(s, VENDOR_ID_LEN, linecount);
				sprintf(lu->vendor_id, "%-8s", s);
				sprintf(&lu->inquiry[8], "%-8s", s);
			}
			if (sscanf(b, " fifo: %s", s))
				process_fifoname(lu, s, 0);
			if (sscanf(b, " PERSIST: %s", s)) {
				if (!strncasecmp(s, "yes", 3) ||
						 (!strncasecmp(s, "true", 4)))
					lu->persist = TRUE;
			}
			if (sscanf(b, " movecommand: %s", s))
				smc_slots.movecommand = strndup(s, MALLOC_SZ);
			if (sscanf(b, " commandtimeout: %d", &d))
				smc_slots.commandtimeout = d;
			if (sscanf(b, " Backoff: %d", &i)) {
				if ((i > 1) && (i < 10000)) {
					MHVTL_DBG(1, "Backoff value: %d", i);
					backoff = i;
				}
			}
			i = sscanf(b,
				" NAA: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
					&c, &d, &e, &f, &g, &h, &j, &k);
			if (i == 8) {
				free(lu->naa);
				lu->naa = zalloc(48);
				if (lu->naa)
					sprintf((char *)lu->naa,
				"%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
					c, d, e, f, g, h, j, k);
				MHVTL_DBG(2, "Setting NAA: to %s", lu->naa);
			} else if (i > 0) {
				free(lu->naa);
				lu->naa = NULL;
				/* Cleanup string (replace 'nl' with null)
				 * for logging */
				rmnl(b, '\0', MALLOC_SZ);
				MHVTL_DBG(1, "NAA: Incorrect params: %s"
						", Using defaults", b);
			}
		}
	}
	fclose(conf);
	free(b);
	free(s);

	if (found && !lu->inquiry[32]) {
		char *v;

		/* Default rev with mhvtl release info */
		v = get_version();
		MHVTL_DBG(1, "Adding default vers info: %s", v);
		sprintf(&lu->inquiry[32], "%-4s", v);
		free(v);
	}

	INIT_LIST_HEAD(&lu->den_list);
	INIT_LIST_HEAD(&lu->log_pg);
	INIT_LIST_HEAD(&lu->mode_pg);
	INIT_LIST_HEAD(&smc_slots.slot_list);
	INIT_LIST_HEAD(&smc_slots.drive_list);
	INIT_LIST_HEAD(&smc_slots.media_list);

	free(smc_slots.state_msg);

	smc_slots.num_drives = 0;
	smc_slots.num_picker = 0;
	smc_slots.num_map = 0;
	smc_slots.num_storage = 0;
	smc_slots.bufsize = SMC_BUF_SIZE;
	smc_slots.state_msg = NULL;

	/* Unit Serial Number */
	lu_vpd[PCODE_OFFSET(0x80)] = alloc_vpd(strlen(lu->lu_serial_no));
	update_vpd_80(lu, lu->lu_serial_no);

	lu->lu_private = &smc_slots;
	smc_slots.cap_closed = CAP_CLOSED;
	return found;
}

static void process_cmd(int cdev, uint8_t *buf, struct vtl_header *vtl_cmd,
			useconds_t pollInterval)
{
	struct vtl_ds dbuf;
	uint8_t *cdb;

	/* Get the SCSI cdb from vtl driver
	 * - Returns SCSI command S/No. */

	cdb = (uint8_t *)&vtl_cmd->cdb;

	/* Interpret the SCSI command & process
	-> Returns no. of bytes to send back to kernel
	 */
	dbuf.sz = 0;
	dbuf.serialNo = vtl_cmd->serialNo;
	dbuf.data = buf;
	dbuf.sam_stat = sam_status;
	dbuf.sense_buf = &sense;

	processCommand(cdev, cdb, &dbuf, pollInterval);

	/* Complete SCSI cmd processing */
	completeSCSICommand(cdev, &dbuf);

	/* dbuf.sam_stat was zeroed in completeSCSICommand */
	sam_status = dbuf.sam_stat;
}

/*
 * Be nice and free all malloc() on exit
 */
static void cleanup_lu(struct lu_phy_attr *lu)
{
	int i;
	struct smc_priv *lu_priv;
	struct list_head *slot_head;
	struct s_info *sp, *sn;	/* Slot */
	struct d_info *dp, *dn;	/* Drive */
	struct m_info *mp, *mn;	/* Media */

	lu_priv = lu->lu_private;

	/* Free all VPD pages */
	for (i = 0x80; i < 0x100; i++) {
		if (lu->lu_vpd[PCODE_OFFSET(i)]) {
			dealloc_vpd(lu->lu_vpd[PCODE_OFFSET(i)]);
			lu->lu_vpd[PCODE_OFFSET(i)] = NULL;
		}
	}
	free(lu->naa);
	lu->naa = NULL;

	dealloc_all_mode_pages(lu);
	dealloc_all_log_pages(lu);

	slot_head = &lu_priv->slot_list;
	list_for_each_entry_safe(sp, sn, slot_head, siblings) {
		list_del(&sp->siblings);
		free(sp);
	}

	slot_head = &lu_priv->drive_list;
	list_for_each_entry_safe(dp, dn, slot_head, siblings) {
		list_del(&dp->siblings);
		free(dp);
	}

	slot_head = &lu_priv->media_list;
	list_for_each_entry_safe(mp, mn, slot_head, siblings) {
		list_del(&mp->siblings);
		free(mp);
	}
	free(lu_priv->state_msg);
	lu_priv->state_msg = NULL;
}

static void customise_ibm_lu(struct lu_phy_attr *lu)
{
	if (!strncasecmp(lu->product_id, "3573-TL", 7))
		init_ibmts3100(lu);
	else if (!strncasecmp(lu->product_id, "03584", 5))
		init_ibm3584(lu);
	else
		init_default_smc(lu);
}

static void customise_stk_lu(struct lu_phy_attr *lu)
{
	if (!strncasecmp(lu->product_id, "SL500", 5))
		init_stkslxx(lu);	/* STK SL series */
	else if (!strncasecmp(lu->product_id, "L20", 3))
		init_stkl20(lu);	/* L20/40/80 */
	else if (!strncasecmp(lu->product_id, "L40", 3))
		init_stkl20(lu);	/* L20/40/80 */
	else if (!strncasecmp(lu->product_id, "L80", 3))
		init_stkl20(lu);	/* L20/40/80 */
	else
		init_stklxx(lu);	/* STK L series */
}

static void customise_hp_lu(struct lu_phy_attr *lu)
{
	if (!strncasecmp(lu->product_id, "MSL", 3))
		init_hp_msl_smc(lu);
	else
		init_hp_eml_smc(lu);
}

static void customise_lu(struct lu_phy_attr *lu)
{
	if (!strncasecmp(lu->vendor_id, "stk", 3))
		customise_stk_lu(lu);
	else if (!strncasecmp(lu->vendor_id, "IBM", 3))
		customise_ibm_lu(lu);
	else if (!strncasecmp(lu->vendor_id, "HP", 2))
		customise_hp_lu(lu);
	else if (!strncasecmp(lu->product_id, "OVERLAND", 8))
		init_overland_smc(lu);
	else if (!strncasecmp(lu->product_id, "scalar", 6))
		init_scalar_smc(lu);
	else if (!strncasecmp(lu->vendor_id, "SPECTRA ", 7))
		init_spectra_logic_smc(lu);
	else
		init_default_smc(lu);
}

void rereadconfig(int sig)
{
	struct vtl_ctl ctl;
	int buffer_size;

	lunit.online = 0;	/* Report library offline until finished */

	MHVTL_DBG(1, "Caught signal (%d): Re-initialising library %d",
			sig, (int)my_id);

	cleanup_lu(&lunit);

	if (lunit.fifo_fd) {
		fclose(lunit.fifo_fd);
		free(lunit.fifoname);
		lunit.fifo_fd = NULL;
	}

	if (!init_lu(&lunit, my_id, &ctl)) {
		printf("Can not find entry for '%ld' in config file\n", my_id);
		exit(1);
	}

	if (lunit.fifoname)
		open_fifo(&lunit.fifo_fd, lunit.fifoname);

	customise_lu(&lunit);

	update_drive_details(&lunit);

	/* malloc a big enough buffer to fit worst case read element status */
	buffer_size = (smc_slots.num_drives +
				smc_slots.num_picker +
				smc_slots.num_map +
				smc_slots.num_storage) * 80;
	reset_device();	/* Force a POWER-ON/RESET sense code */
	if (buffer_size > smc_slots.bufsize) {
		MHVTL_LOG("Too many slots configured"
			" - possible buffer overflow");
		MHVTL_LOG("Please shutdown this daemon and restart so"
			" correct buffer allocation can be performed");
		/* Leave library offline */
	} else {
		lunit.online = 1;	/* Should be good to go */
	}
}

void smc_personality_module_register(struct smc_personality_template *pm)
{
	MHVTL_LOG("%s", pm->name);
	smc_slots.pm = pm;
}

static void caught_signal(int signo)
{
	MHVTL_DBG(1, " %d", signo);
	printf("Please use 'vtlcmd <index> exit' to shutdown nicely\n");
	MHVTL_LOG("Please use 'vtlcmd <index> exit' to shutdown nicely,"
			" Received signal: %d", signo);
}

/*
 * main()
 *
 * e'nuf sed
 */
int main(int argc, char *argv[])
{
	int cdev;
	int ret;
	long pollInterval = 0L;
	uint8_t *buf;
	int buffer_size;
	int fifo_retval;

	int last_state = MHVTL_STATE_UNKNOWN;

	struct list_head *slot_head = &smc_slots.slot_list;
	struct s_info *sp;
	struct d_info *dp;

	struct vtl_header vtl_cmd;
	struct vtl_ctl ctl;
	char s[100];

	pid_t pid, sid, child_cleanup;
	struct sigaction new_action, old_action;

	char *progname = argv[0];
	char *name = "mhvtl";
	char *fifoname = NULL;
	struct passwd *pw;

	memset(&vtl_cmd, 0, sizeof(struct vtl_header));
	memset(&ctl, 0, sizeof(struct vtl_ctl));

	/* Message Q */
	int mlen, r_qid;
	struct q_entry r_entry;

	while (argc > 0) {
		if (argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				debug++;
				verbose = 9;	/* If debug, make verbose... */
				break;
			case 'v':
				verbose++;
				break;
			case 'q':
				if (argc > 1)
					my_id = atoi(argv[1]);
				break;
			case 'f':
				if (argc > 1)
					fifoname = argv[1];
				break;
			default:
				usage(progname);
				printf("    Unknown option %c\n", argv[0][1]);
				exit(1);
				break;
			}
		}
		argv++;
		argc--;
	}

	if (my_id <= 0 || my_id > MAXPRIOR) {
		usage(progname);
		if (my_id == 0)
			printf("    -q must be specified\n");
		else
			printf("    -q value out of range [1 - %d]\n",
				MAXPRIOR);
		exit(1);
	}

	openlog(progname, LOG_PID, LOG_DAEMON|LOG_WARNING);

	/* Clear Sense arr */
	memset(sense, 0, sizeof(sense));
	reset_device();	/* power-on reset */

	if (!init_lu(&lunit, my_id, &ctl)) {
		printf("Can not find entry for '%ld' in config file\n", my_id);
		exit(1);
	}

	/* Personality module init call */
	customise_lu(&lunit);

	update_drive_details(&lunit);

	lunit.online = 1; /* Mark unit online */

	if (chrdev_create(my_id)) {
		MHVTL_DBG(1, "Error creating device node mhvtl%d", (int)my_id);
		exit(1);
	}

	/* Check for correct user-account before creating lu */
	pw = getpwnam(USR);	/* Find UID for user 'vtl' */
	if (!pw) {
		printf("Unable to find user: %s\n", USR);
		exit(1);
	}

	new_action.sa_handler = caught_signal;
	new_action.sa_flags = 0;
	sigemptyset(&new_action.sa_mask);
	sigaction(SIGALRM, &new_action, &old_action);
	sigaction(SIGINT, &new_action, &old_action);
	sigaction(SIGPIPE, &new_action, &old_action);
	sigaction(SIGTERM, &new_action, &old_action);
	sigaction(SIGUSR1, &new_action, &old_action);
	sigaction(SIGUSR2, &new_action, &old_action);

	new_action.sa_handler = rereadconfig;
	sigaction(SIGHUP, &new_action, &old_action);

	child_cleanup = add_lu(my_id, &ctl);
	if (!child_cleanup) {
		printf("Could not create logical unit\n");
		exit(1);
	}

	chrdev_chown(my_id, pw->pw_uid, pw->pw_gid);

	/* Now that we have created the lu, drop root uid/gid */
	if (setgid(pw->pw_gid)) {
		perror("Unable to change gid");
		exit(1);
	}
	if (setuid(pw->pw_uid)) {
		perror("Unable to change uid");
		exit(1);
	}

	MHVTL_DBG(2, "Running as %s, uid: %d", pw->pw_name, getuid());

	/* Initialise message queue as necessary */
	r_qid = init_queue();
	if (r_qid == -1) {
		printf("Could not initialise message queue\n");
		exit(1);
	}

	if (check_for_running_daemons(my_id)) {
		MHVTL_LOG("%s: version %s, found another running daemon... exiting", progname, MHVTL_VERSION);
		exit(2);
	}

	/* Clear out message Q by reading anything there. */
	mlen = msgrcv(r_qid, &r_entry, MAXOBN, my_id, IPC_NOWAIT);
	while (mlen > 0) {
		MHVTL_DBG(2, "Found \"%s\" still in message Q", r_entry.msg.text);
		mlen = msgrcv(r_qid, &r_entry, MAXOBN, my_id, IPC_NOWAIT);
	}

	cdev = chrdev_open(name, my_id);
	if (cdev == -1) {
		MHVTL_ERR("Could not open /dev/%s%ld: %s",
					name, my_id, strerror(errno));
		fflush(NULL);
		exit(1);
	}

	/* malloc a big enough buffer to fit worst case read element status */
	buffer_size = (smc_slots.num_drives +
				smc_slots.num_picker +
				smc_slots.num_map +
				smc_slots.num_storage) * 80;
	buffer_size = max(SMC_BUF_SIZE, buffer_size);
	smc_slots.bufsize = buffer_size;
	MHVTL_DBG(1, "Setting buffer size to %d", buffer_size);
	buf = (uint8_t *)zalloc(buffer_size);
	if (NULL == buf) {
		perror("Problems allocating memory");
		exit(1);
	}

	/* Send a message to each tape drive so they know the
	 * controlling library's message Q id
	 */
	list_for_each_entry(sp, slot_head, siblings) {
		if (sp->element_type == DATA_TRANSFER) {
			dp = sp->drive;
			send_msg("Register", dp->drv_id);

			if (debug) {

				MHVTL_DBG(3, "\nDrive %d", sp->slot_location);

				strncpy(s, dp->inq_vendor_id, 8);
				rmnl(s, ' ', 8);
				s[8] = '\0';
				MHVTL_DBG(3, "Vendor ID     : \"%s\"", s);

				strncpy(s, dp->inq_product_id, 16);
				rmnl(s, ' ', 16);
				s[16] = '\0';
				MHVTL_DBG(3, "Product ID    : \"%s\"", s);

				strncpy(s, dp->inq_product_rev, 4);
				rmnl(s, ' ', 4);
				s[4] = '\0';
				MHVTL_DBG(3, "Revision Level: \"%s\"", s);

				strncpy(s, dp->inq_product_sno, 10);
				rmnl(s, ' ', 10);
				s[10] = '\0';
				MHVTL_DBG(3, "Product S/No  : \"%s\"", s);

				MHVTL_DBG(3, "Drive location: %d",
						dp->slot->slot_location);
				MHVTL_DBG(3, "Drive occupied: %s",
				(dp->slot->status & STATUS_Full) ? "No" : "Yes");
			}
		}
	}

	/* If debug, don't fork/run in background */
	if (!debug) {
		switch (pid = fork()) {
		case 0:         /* Child */
			break;
		case -1:
			printf("Failed to fork daemon\n");
			exit(-1);
			break;
		default:
			printf("%s process PID is %d\n", progname, (int)pid);
			exit(0);
			break;
		}

		umask(0);	/* Change the file mode mask */

		sid = setsid();
		if (sid < 0)
			exit(-1);

		if ((chdir(MHVTL_HOME_PATH)) < 0) {
			perror("Unable to change directory to " MHVTL_HOME_PATH);
			exit(-1);
		}

		close(STDIN_FILENO);
		close(STDERR_FILENO);
	}

	MHVTL_LOG("Started %s: version %s, verbose log lvl: %d, lu [%d:%d:%d]",
					progname, MHVTL_VERSION, verbose,
					ctl.channel, ctl.id, ctl.lun);

	oom_adjust();

	/* If fifoname passed as switch */
	if (fifoname)
		process_fifoname(&lunit, fifoname, 1);
	/* fifoname can be defined in device.conf */
	if (lunit.fifoname)
		open_fifo(&lunit.fifo_fd, lunit.fifoname);

	fifo_retval = inc_fifo_count();
	if (fifo_retval == -ENOMEM) {
		MHVTL_ERR("shared memory setup failed - exiting...");
		goto exit;
	} else if (fifo_retval < 0) {
		MHVTL_ERR("Failed to set fifo count()...");
	}

	for (;;) {
		/* Check for any messages */
		mlen = msgrcv(r_qid, &r_entry, MAXOBN, my_id, IPC_NOWAIT);
		if (mlen > 0) {
			if (processMessageQ(&r_entry.msg))
				goto exit;
		} else if (mlen < 0) {
			r_qid = init_queue();
			if (r_qid == -1)
				MHVTL_ERR("Can not open message queue: %s",
					strerror(errno));
		}

		ret = ioctl(cdev, VTL_POLL_AND_GET_HEADER, &vtl_cmd);
		if (ret < 0) {
			MHVTL_LOG("ret: %d : %s", ret, strerror(errno));
		} else {
			if (child_cleanup) {
				if (waitpid(child_cleanup, NULL, WNOHANG)) {
					MHVTL_DBG(2,
						"Cleaning up after add_lu "
						"child pid: %d",
							child_cleanup);
					child_cleanup = 0;
				}
			}
			fflush(NULL);	/* So I can pipe debug o/p thru tee */
			switch (ret) {
			case VTL_QUEUE_CMD:
				if (smc_slots.bufsize != buffer_size) {
					buffer_size = smc_slots.bufsize;
					buf = realloc(buf, buffer_size);
					if (!buf) {
						perror("Problems allocating memory");
						exit(1);
					}
				}
				process_cmd(cdev, buf, &vtl_cmd, pollInterval);
				pollInterval = MIN_SLEEP_TIME;
				break;

			case VTL_IDLE:
				usleep(pollInterval);

				if (pollInterval < 1000000)
					pollInterval += backoff;
				break;

			default:
				MHVTL_LOG("ioctl(0x%x) returned %d",
						VTL_POLL_AND_GET_HEADER, ret);
				sleep(1);
				break;
			}
			if (current_state != last_state) {
				status_change(lunit.fifo_fd,
							current_state,
							my_id,
							&smc_slots.state_msg);
				last_state = current_state;
			}
			if (pollInterval > 0x18000)
				if (current_state != MHVTL_STATE_OFFLINE)
					current_state = MHVTL_STATE_IDLE;
		}
	}
exit:
	ioctl(cdev, VTL_REMOVE_LU, &ctl);
	if (lunit.persist)
		save_config(&lunit);
	cleanup_lu(&lunit);
	close(cdev);
	free(buf);
	dec_fifo_count();
	if (lunit.fifo_fd) {
		fclose(lunit.fifo_fd);
		unlink(lunit.fifoname);
		free(lunit.fifoname);
	}

	exit(0);
}
