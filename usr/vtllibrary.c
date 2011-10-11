/*
 * This daemon is the SCSI SMC target (Medium Changer) portion of the
 * vtl package.
 *
 * The vtl package consists of:
 *   a kernel module (vlt.ko) - Currently on 2.6.x Linux kernel support.
 *   SCSI target daemons for both SMC and SSC devices.
 *
 * Copyright (C) 2005 - 2009 Mark Harvey       markh794@gmail.com
 *                                          mark_harvey@symantec.com
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
#include "vtllib.h"
#include "spc.h"
#include "smc.h"
#include "mode.h"
#include "be_byteshift.h"
#include "log.h"

char vtl_driver_name[] = "vtllibrary";
long my_id = 0;

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

#define SMC_BUF_SIZE 1024 * 1024 /* Default size of buffer */

int verbose = 0;
int debug = 0;
static uint8_t sam_status = 0;		/* Non-zero if Sense-data is valid */

extern int current_state;	/* scope, Global -> Last status sent to fifo */

struct lu_phy_attr lunit;

static struct smc_priv smc_slots;

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
		{spc_illegal_op,},
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
static void processCommand(int cdev, uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	struct scsi_cmd _cmd;
	struct scsi_cmd *cmd;
	cmd = &_cmd;

	cmd->scb = cdb;
	cmd->scb_len = 16;	/* fixme */
	cmd->dbuf_p = dbuf_p;
	cmd->lu = &lunit;

	MHVTL_DBG_PRT_CDB(1, dbuf_p->serialNo, cdb);

	switch (cdb[0]) {
	case REPORT_LUNS:
	case REQUEST_SENSE:
	case MODE_SELECT:
	case INQUIRY:
		dbuf_p->sam_stat = SAM_STAT_GOOD;
		break;
	default:
		if (cmd->lu->online == 0) {
			mkSenseBuf(NOT_READY, E_OFFLINE, &dbuf_p->sam_stat);
			return;
		}
		if (check_reset(&dbuf_p->sam_stat))
			return;
	}

	dbuf_p->sam_stat = cmd->lu->scsi_ops->ops[cdb[0]].cmd_perform(cmd);
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

/*
 * If barcode starts with string 'CLN' define it as a cleaning cart.
 * else its a data cartridge
 *
 * Return 1 = Data cartridge
 *        2 = Cleaning cartridge
 */
static uint8_t cart_type(char *barcode)
{
	uint8_t retval = 0;

	retval = (strncmp(barcode, "CLN", 3)) ? 1 : 2;
	MHVTL_DBG(2, "%s cart found: %s",
				(retval == 1) ? "Data" : "Cleaning", barcode);

return retval;
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

static struct s_info * locate_empty_map(void)
{
	struct s_info *sp = NULL;
	struct list_head *slot_head = &smc_slots.slot_list;

	list_for_each_entry(sp, slot_head, siblings) {
		if (!slotOccupied(sp) && sp->element_type == MAP_ELEMENT)
			return sp;
	}

	return NULL;
}

static struct m_info * lookup_barcode(struct lu_phy_attr *lu, char *barcode)
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

static struct m_info * add_barcode(struct lu_phy_attr *lu, char *barcode)
{
	struct list_head *media_list_head;
	struct m_info *m;

	if (strlen(barcode) > MAX_BARCODE_LEN) {
		MHVTL_LOG("Barcode \'%s\' exceeds max barcode lenght: %d",
				barcode, MAX_BARCODE_LEN);
		exit(1);
	}
	if (lookup_barcode(lu, barcode)) {
		MHVTL_LOG("Duplicate barcode %s.. Exiting", barcode);
		exit(1);
	}

	m = malloc(sizeof(struct m_info));
	if (!m) {
		MHVTL_DBG(1, "Out of memory allocating memory for barcode %s",
			barcode);
		exit(-ENOMEM);
	}

	media_list_head = &((struct smc_priv *)lu->lu_private)->media_list;

	memset(m, 0, sizeof(struct m_info));

	snprintf((char *)m->barcode, MAX_BARCODE_LEN + 1, LEFT_JUST_16_STR,
					barcode);
	m->barcode[MAX_BARCODE_LEN + 1] = '\0';
	m->cart_type = cart_type((char *)barcode);
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
		mp->barcode[MAX_BARCODE_LEN + 1] = '\0';

		/* 1 = data, 2 = Clean */
		mp->cart_type = cart_type(barcode);
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
					sp->slot_location - START_MAP);
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

	MHVTL_DBG(3, "Q snd_id %ld msg : %s", msg->snd_id, msg->text);

	if (!strncmp(msg->text, "debug", 5)) {
		if (debug) {
			debug--;
		} else {
			debug++;
			verbose = 2;
		}
	}
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
		uint64_t flg = 0L;
		sscanf(msg->text, "TapeAlert %" PRIx64, &flg);
		update_TapeAlert(&lunit, flg);
	}
	if (!strncmp(msg->text, "verbose", 7)) {
		if (verbose)
			verbose--;
		else
			verbose = 3;
		MHVTL_LOG("Verbose: %s at level %d",
				 verbose ? "enabled" : "disabled", verbose);
	}

return 0;
}

static struct d_info *lookup_drive(struct lu_phy_attr *lu, int drive_no)
{
	struct list_head *drive_list_head;
	struct d_info *d;

	drive_list_head = &((struct smc_priv *)lu->lu_private)->drive_list;

	list_for_each_entry(d, drive_list_head, siblings) {
		if (d->slot->slot_location == drive_no)
			return d;
	}

return NULL;
}

static struct s_info *lookup_slot(struct lu_phy_attr *lu, unsigned int slot)
{
	struct list_head *slot_list_head;
	struct s_info *s;

	slot_list_head = &((struct smc_priv *)lu->lu_private)->slot_list;

	list_for_each_entry(s, slot_list_head, siblings) {
		if (s->slot_location == slot)
			return s;
	}

return NULL;
}

struct s_info *add_new_slot(struct lu_phy_attr *lu)
{
	struct s_info *new;
	struct list_head *slot_list_head;

	slot_list_head = &((struct smc_priv *)lu->lu_private)->slot_list;

	new = malloc(sizeof(struct s_info));
	if (!new) {
		MHVTL_DBG(1, "Could not allocate memory for new slot struct");
		exit(-ENOMEM);
	}
	memset(new, 0, sizeof(struct s_info));

	list_add_tail(&new->siblings, slot_list_head);
	return new;
}

static void init_smc_log_pages(struct lu_phy_attr *lu)
{
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
}

static void init_smc_mode_pages(struct lu_phy_attr *lu)
{
	add_mode_disconnect_reconnect(lu);
	add_mode_control_extension(lu);
	add_mode_power_condition(lu);
	add_mode_information_exception(lu);
	add_mode_element_address_assignment(lu);
	add_mode_transport_geometry(lu);
	add_mode_device_capabilities(lu);
}

/* Open device config file and update device information
 */
static void update_drive_details(struct lu_phy_attr *lu)
{
	char *config=MHVTL_CONFIG_PATH"/device.conf";
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
	s = malloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = malloc(MALLOC_SZ);
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
		if (sscanf(b, "Drive: %ld", &drv_id) > 0) {
			continue;
		}
		if (sscanf(b, " Library ID: %ld Slot: %d", &lib_id, &slot) == 2
					&& lib_id == my_id
					&& drv_id >= 0) {
			MHVTL_DBG(2, "Found Drive %ld in slot %d",
					drv_id, slot);
			dp = lookup_drive(lu, slot);
			if (!dp) {
				dp = malloc(sizeof(struct d_info));
				if (!dp) {
					MHVTL_DBG(1, "Couldn't malloc memory");
					exit(-ENOMEM);
				}
				memset(dp, 0, sizeof(struct d_info));

				sp = add_new_slot(lu);
				sp->element_type = DATA_TRANSFER;
				dp->slot = sp;
				sp->drive = dp;
				list_add_tail(&dp->siblings,
						&smc_p->drive_list);
			}
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
 * Read config file and populate d_info struct with library's drives
 *
 * One very long and serial function...
 */
static void init_slot_info(struct lu_phy_attr *lu)
{
	char conf[1024];
	FILE *ctrl;
	struct d_info *dp = NULL;
	struct s_info *sp = NULL;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	char *barcode;
	int slt;
	int x;
	struct smc_priv *smc_p = lu->lu_private;

	sprintf(conf, MHVTL_CONFIG_PATH "/library_contents.%ld", my_id);
	ctrl = fopen(conf , "r");
	if (!ctrl) {
		MHVTL_DBG(1, "Can not open config file %s : %s", conf,
					strerror(errno));
		exit(1);
	}

	/* Grab a couple of generic MALLOC_SZ buffers.. */
	s = malloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = malloc(MALLOC_SZ);
	if (!b) {
		perror("Could not allocate memory");
		exit(1);
	}

	rewind(ctrl);
	barcode = s;
	while (readline(b, MALLOC_SZ, ctrl) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		barcode[0] = '\0';

		x = sscanf(b, "Drive %d: %s", &slt, s);

		if (x) {
			dp = lookup_drive(lu, slt);
			if (!dp) {
				dp = malloc(sizeof(struct d_info));
				if (!dp) {
					MHVTL_DBG(1, "Couldn't malloc memory");
					exit(-ENOMEM);
				}
				memset(dp, 0, sizeof(struct d_info));

				sp = add_new_slot(lu);
				sp->element_type = DATA_TRANSFER;
				dp->slot = sp;
				sp->drive = dp;
				list_add_tail(&dp->siblings,
						&smc_p->drive_list);
			}
		}

		switch (x) {
		case 2:
			/* Pull serial number out and fall thru to case 1*/
			strncpy(dp->inq_product_sno, s, 10);
			MHVTL_DBG(2, "Drive s/no: %s", s);
		case 1:
			dp->slot->slot_location = slt + START_DRIVE - 1;
			dp->slot->status = STATUS_Access;
			smc_p->num_drives++;
			break;
		}

		x = sscanf(b, "MAP %d: %s", &slt, barcode);
		if (x) {
			sp = add_new_slot(lu);
			sp->element_type = MAP_ELEMENT;
			smc_p->num_map++;
		}

		switch (x) {
		case 1:
			sp->slot_location = slt + START_MAP - 1;
			sp->status = STATUS_InEnab | STATUS_ExEnab |
					STATUS_Access | STATUS_ImpExp;
			break;
		case 2:
			MHVTL_DBG(2, "Barcode %s in MAP %d", barcode, slt);
			sp->media = add_barcode(lu, barcode);
			sp->status = STATUS_InEnab | STATUS_ExEnab |
					STATUS_Access | STATUS_ImpExp |
					STATUS_Full;
			sp->slot_location = slt + START_MAP - 1;
			break;
		}

		x = sscanf(b, "Picker %d: %s", &slt, barcode);
		if (x) {
			sp = add_new_slot(lu);
			sp->element_type = MEDIUM_TRANSPORT;
			smc_p->num_picker++;
		}

		switch (x) {
		case 1:
			sp->slot_location = slt + START_PICKER - 1;
			sp->status = 0;
			break;
		case 2:
			MHVTL_DBG(2, "Barcode %s in Picker %d", barcode, slt);
			sp->media = add_barcode(lu, barcode);
			sp->slot_location = slt + START_PICKER - 1;
			sp->status = STATUS_Full;
			break;
		}

		x = sscanf(b, "Slot %d: %s", &slt, barcode);
		if (x) {
			sp = add_new_slot(lu);
			sp->element_type = STORAGE_ELEMENT;
			smc_p->num_storage++;
		}

		switch (x) {
		case 1:
			sp->slot_location = slt + START_STORAGE - 1;
			sp->status = STATUS_Access;
			break;
		case 2:
			MHVTL_DBG(2, "Barcode %s in slot %d", barcode, slt);
			sp->media = add_barcode(lu, barcode);
			sp->slot_location = slt + START_STORAGE - 1;
			/* Slot full */
			sp->status = STATUS_Access | STATUS_Full;
			break;
		}
	}
	fclose(ctrl);
	free(b);
	free(s);
}

/* Set VPD data with device serial number */
static void update_vpd_80(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0x80)];

	memcpy(vpd_pg->data, p, strlen(p));
}

static void update_vpd_83(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0x83)];
	uint8_t *d;
	int num;
	char *ptr;
	int len, j;

	d = vpd_pg->data;

	d[0] = 2;
	d[1] = 1;
	d[2] = 0;
	num = VENDOR_ID_LEN + PRODUCT_ID_LEN + 10;
	d[3] = num;

	memcpy(&d[4], &lu->vendor_id, VENDOR_ID_LEN);
	memcpy(&d[12], &lu->product_id, PRODUCT_ID_LEN);
	memcpy(&d[28], &lu->lu_serial_no, 10);
	len = (int)strlen(lu->lu_serial_no);
	ptr = &lu->lu_serial_no[len];

	num += 4;
	/* NAA IEEE registered identifier (faked) */
	d[num] = 0x1;	/* Binary */
	d[num + 1] = 0x3;
	d[num + 2] = 0x0;
	d[num + 3] = 0x8;
	d[num + 4] = 0x51;
	d[num + 5] = 0x23;
	d[num + 6] = 0x45;
	d[num + 7] = 0x60;
	d[num + 8] = 0x3;
	d[num + 9] = 0x3;
	d[num + 10] = 0x3;
	d[num + 11] = 0x3;

	if (lu->naa) { /* If defined in config file */
		sscanf((const char *)lu->naa,
			"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			&d[num + 4],
			&d[num + 5],
			&d[num + 6],
			&d[num + 7],
			&d[num + 8],
			&d[num + 9],
			&d[num + 10],
			&d[num + 11]);
	} else { /* Else munge the serial number */
		ptr--;
		for (j = 11; j > 3; ptr--, j--)
			d[num + j] = *ptr;
	}
	/* Bug reported by Stefan Hauser.
	 * [num +4] is always 0x5x
	 */
	d[num + 4] &= 0x0f;
	d[num + 4] |= 0x50;
}

static int init_lu(struct lu_phy_attr *lu, int minor, struct vtl_ctl *ctl)
{

	struct vpd **lu_vpd = lu->lu_vpd;
	int pg;
	uint8_t local_TapeAlert[8] =
			{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	char *config=MHVTL_CONFIG_PATH"/device.conf";
	FILE *conf;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	int indx;
	struct vtl_ctl tmpctl;
	int found = 0;

	conf = fopen(config , "r");
	if (!conf) {
		MHVTL_LOG("Can not open config file %s : %s", config,
					strerror(errno));
		perror("Can not open config file");
		exit(1);
	}
	s = malloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = malloc(MALLOC_SZ);
	if (!b) {
		perror("Could not allocate memory");
		exit(1);
	}


	/* While read in a line */
	while (readline(b, MALLOC_SZ, conf) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		if (strlen(b) == 1)	/* Reset drive number of blank line */
			indx = 0xff;
		if (sscanf(b, "Library: %d CHANNEL: %d TARGET: %d LUN: %d",
					&indx, &tmpctl.channel,
					&tmpctl.id, &tmpctl.lun)) {
			MHVTL_DBG(2, "Found Library %d, looking for %d",
							indx, minor);
			if (indx == minor) {
				char *v;

				found = 1;
				memcpy(ctl, &tmpctl, sizeof(tmpctl));

				/* Default rev with mhvtl release info */
				v = get_version();
				sprintf(lu->product_rev, "%-4s", v);
				free(v);
			}
		}
		if (indx == minor) {
			unsigned int c, d, e, f, g, h, j, k;
			int i;

			if (sscanf(b, " Unit serial number: %s", s)) {
				checkstrlen(s, SCSI_SN_LEN);
				sprintf(lu->lu_serial_no, "%-10s", s);
			}
			if (sscanf(b, " Product identification: %16c", s) > 0) {
				/* sscanf does not NULL terminate */
				i = strlen(b) - 25; /* len of ' Product identification: ' */
				s[i] = '\0';
				snprintf(lu->product_id, PRODUCT_ID_LEN + 1, "%-16s", s);
			}
			if (sscanf(b, " Product revision level: %s", s)) {
				checkstrlen(s, PRODUCT_REV_LEN);
				sprintf(lu->product_rev, "%-4s", s);
			}
			if (sscanf(b, " Vendor identification: %s", s)) {
				checkstrlen(s, VENDOR_ID_LEN);
				sprintf(lu->vendor_id, "%-8s", s);
			}
			i = sscanf(b,
				" NAA: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
					&c, &d, &e, &f, &g, &h, &j, &k);
			if (i == 8) {
				if (lu->naa)
					free(lu->naa);
				lu->naa = malloc(48);
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

	INIT_LIST_HEAD(&lu->den_list);
	INIT_LIST_HEAD(&lu->log_pg);
	INIT_LIST_HEAD(&lu->mode_pg);
	INIT_LIST_HEAD(&smc_slots.slot_list);
	INIT_LIST_HEAD(&smc_slots.drive_list);
	INIT_LIST_HEAD(&smc_slots.media_list);

	if (smc_slots.state_msg)
		free(smc_slots.state_msg);

	smc_slots.num_drives = 0;
	smc_slots.num_picker = 0;
	smc_slots.num_map = 0;
	smc_slots.num_storage = 0;
	smc_slots.bufsize = SMC_BUF_SIZE;
	smc_slots.state_msg = NULL;

	lu->ptype = TYPE_MEDIUM_CHANGER;	/* SSC */
	lu->removable = 1;	/* Supports removable media */
	lu->version_desc[0] = 0x0300;	/* SPC-3 No version claimed */
	lu->version_desc[1] = 0x0960;	/* iSCSI */
	lu->version_desc[2] = 0x0200;	/* SSC */

	/* Unit Serial Number */
	pg = 0x80 & 0x7f;
	lu_vpd[pg] = alloc_vpd(strlen(lu->lu_serial_no));
	if (lu_vpd[pg]) {
		lu_vpd[pg]->vpd_update = update_vpd_80;
		lu_vpd[pg]->vpd_update(lu, lu->lu_serial_no);
	} else
		MHVTL_DBG(1, "Could not malloc(%d) line %d",
				(int)strlen(lu->lu_serial_no),
				__LINE__);

	/* Device Identification */
	pg = 0x83 & 0x7f;
	lu_vpd[pg] = alloc_vpd(VPD_83_SZ);
	if (lu_vpd[pg]) {
		lu_vpd[pg]->vpd_update = update_vpd_83;
		lu_vpd[pg]->vpd_update(lu, NULL);
	} else
		MHVTL_DBG(1, "Could not malloc(%d) line %d",
				VPD_83_SZ, __LINE__);

	/* Manufacture-assigned serial number - Ref: 8.4.3 */
	pg = 0xB1 & 0x7f;
	lu_vpd[pg] = alloc_vpd(VPD_B1_SZ);
	if (lu_vpd[pg]) {
		lu_vpd[pg]->vpd_update = update_vpd_b1;
		lu_vpd[pg]->vpd_update(lu, lu->lu_serial_no);
	} else
		MHVTL_DBG(1, "Could not malloc(%d) line %d",
				VPD_B1_SZ, __LINE__);

	/* TapeAlert supported flags - Ref: 8.4.4 */
	pg = 0xB2 & 0x7f;
	lu_vpd[pg] = alloc_vpd(VPD_B2_SZ);
	if (lu_vpd[pg]) {
		lu_vpd[pg]->vpd_update = update_vpd_b2;
		lu_vpd[pg]->vpd_update(lu, &local_TapeAlert);
	} else
		MHVTL_DBG(1, "Could not malloc(%d) line %d",
				VPD_B2_SZ, __LINE__);

	/* VPD page 0xC0 */
	pg = 0xC0 & 0x7f;
	lu_vpd[pg] = alloc_vpd(VPD_C0_SZ);
	if (lu_vpd[pg]) {
		lu_vpd[pg]->vpd_update = update_vpd_c0;
		lu_vpd[pg]->vpd_update(lu, "10-03-2008 19:38:00");
	} else
		MHVTL_DBG(1, "Could not malloc(%d) line %d",
				VPD_C0_SZ, __LINE__);

	/* VPD page 0xC1 */
	pg = 0xC1 & 0x7f;
	lu_vpd[pg] = alloc_vpd(strlen("Security"));
	if (lu_vpd[pg]) {
		lu_vpd[pg]->vpd_update = update_vpd_c1;
		lu_vpd[pg]->vpd_update(lu, "Security");
	} else
		MHVTL_DBG(1, "Could not malloc(%d) line %d",
				(int)strlen(lu->lu_serial_no),
				(int)__LINE__);

	lu->online = 1;
	lu->lu_private = &smc_slots;
	smc_slots.cap_closed = CAP_CLOSED;
	return found;
}

static void process_cmd(int cdev, uint8_t *buf, struct vtl_header *vtl_cmd)
{
	struct vtl_ds dbuf;
	uint8_t *cdb;

	/* Get the SCSI cdb from vtl driver
	 * - Returns SCSI command S/No. */

	cdb = (uint8_t *)&vtl_cmd->cdb;

	/* Interpret the SCSI command & process
	-> Returns no. of bytes to send back to kernel
	 */
	memset(&dbuf, 0, sizeof(struct vtl_ds));
	dbuf.serialNo = vtl_cmd->serialNo;
	dbuf.data = buf;
	dbuf.sam_stat = sam_status;
	dbuf.sense_buf = &sense;

	processCommand(cdev, cdb, &dbuf);

	/* Complete SCSI cmd processing */
	completeSCSICommand(cdev, &dbuf);

	/* dbuf.sam_stat was zeroed in completeSCSICommand */
	sam_status = dbuf.sam_stat;
}

void rereadconfig(int sig)
{
	struct list_head *slot_head;
	struct s_info *sp, *sn;	/* Slot */
	struct d_info *dp, *dn;	/* Drive */
	struct m_info *mp, *mn;	/* Media */
	struct mode *modep, *moden;	/* Mode page info */
	struct log_pg_list *logp, *logn;	/* Log page info */
	struct vtl_ctl ctl;
	int i;

	MHVTL_DBG(1, "Caught signal (%d)", sig);

	MHVTL_DBG(2, "Removing existing slots");
	slot_head = &smc_slots.slot_list;
	list_for_each_entry_safe(sp, sn, slot_head, siblings) {
		MHVTL_DBG(2, "slot %d", sp->slot_location);
		list_del(&sp->siblings);
		free(sp);
	}

	MHVTL_DBG(2, "Removing existing drives");
	i = 0;
	slot_head = &smc_slots.drive_list;
	list_for_each_entry_safe(dp, dn, slot_head, siblings) {
		MHVTL_DBG(2, "Drive: %d", i++);
		list_del(&dp->siblings);
		free(dp);
	}

	MHVTL_DBG(2, "Removing existing media");
	i = 0;
	slot_head = &smc_slots.media_list;
	list_for_each_entry_safe(mp, mn, slot_head, siblings) {
		MHVTL_DBG(2, "Media: %s", mp->barcode);
		list_del(&mp->siblings);
		free(mp);
	}

	MHVTL_DBG(2, "Removing existing mode pages");
	i = 0;
	slot_head = &lunit.mode_pg;
	list_for_each_entry_safe(modep, moden, slot_head, siblings) {
		MHVTL_DBG(2, "Mode Page: 0x%02x", modep->pcode);
		list_del(&modep->siblings);
		free(modep);
	}

	MHVTL_DBG(2, "Removing existing log pages");
	i = 0;
	slot_head = &lunit.log_pg;
	list_for_each_entry_safe(logp, logn, slot_head, siblings) {
		MHVTL_DBG(2, "Log Page: 0x%02x", logp->log_page_num);
		list_del(&logp->siblings);
		free(logp);
	}

	if (!init_lu(&lunit, my_id, &ctl)) {
		printf("Can not find entry for '%ld' in config file\n", my_id);
		exit(1);
	}
	init_slot_info(&lunit);
	update_drive_details(&lunit);
	init_smc_mode_pages(&lunit);
	init_smc_log_pages(&lunit);
}

static void caught_signal(int signo)
{
	MHVTL_DBG(1, " %d", signo);
	printf("Please use 'vtlcmd <index> exit' to shutdown nicely\n");
	MHVTL_LOG("Please use 'vtlcmd <index> exit' to shutdown nicely\n");
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

	int last_state = MHVTL_STATE_UNKNOWN;

	struct list_head *slot_head = &smc_slots.slot_list;
	struct s_info *sp;
	struct d_info *dp;

	struct vtl_header vtl_cmd;
	struct vtl_ctl ctl;
	char s[100];

	pid_t pid, sid, child_cleanup;
	struct sigaction new_action, old_action;

	FILE *fifo_fd = NULL;

	char *progname = argv[0];
	char *name = "mhvtl";
	char *fifoname = NULL;
	struct passwd *pw;

	int use_fifo = 0;

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
				if (argc > 1) {
					use_fifo = 1;
					fifoname = argv[1];
				}
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
	MHVTL_LOG("%s: version %s", progname, MHVTL_VERSION);

	/* Clear Sense arr */
	memset(sense, 0, sizeof(sense));
	reset_device();	/* power-on reset */

	if (!init_lu(&lunit, my_id, &ctl)) {
		printf("Can not find entry for '%ld' in config file\n", my_id);
		exit(1);
	}
	init_slot_info(&lunit);
	update_drive_details(&lunit);
	init_smc_mode_pages(&lunit);
	init_smc_log_pages(&lunit);

	if (chrdev_create(my_id)) {
		MHVTL_DBG(1, "Error creating device node mhvtl%d", (int)my_id);
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

	pw = getpwnam(USR);	/* Find UID for user 'vtl' */
	if (!pw) {
		printf("Unable to find user: %s\n", USR);
		exit(1);
	}

	chrdev_chown(my_id, pw->pw_uid, pw->pw_gid);

	/* Now that we have created the lu, drop root uid/gid */
	if (setgid(pw->pw_gid)) {
		perror("Unable to change gid");
		exit (1);
	}
	if (setuid(pw->pw_uid)) {
		perror("Unable to change uid");
		exit (1);
	}

	MHVTL_DBG(2, "Running as %s, uid: %d", pw->pw_name, getuid());

	/* Initialise message queue as necessary */
	if ((r_qid = init_queue()) == -1) {
		printf("Could not initialise message queue\n");
		exit(1);
	}

	if (check_for_running_daemons(my_id)) {
		MHVTL_LOG("%s: version %s, found another running daemon... exiting\n", progname, MHVTL_VERSION);
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
		MHVTL_LOG("Could not open /dev/%s%ld: %s",
					name, my_id, strerror(errno));
		fflush(NULL);
		exit(1);
	}

	buf = (uint8_t *)malloc(SMC_BUF_SIZE);
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
		switch(pid = fork()) {
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

	oom_adjust();

	/* Tweak as necessary to properly mimic the specified library type. */

	if (!strcmp(lunit.vendor_id, "SPECTRA ") &&
		!strcmp(lunit.product_id, "PYTHON          ")) {
		/* size of dvcid area in RES descriptor */
		smc_slots.dvcid_len = 10;
		/* dvcid area only contains a serial number */
		smc_slots.dvcid_serial_only = TRUE;
	} else {
		/* size of dvcid area in RES descriptor */
		smc_slots.dvcid_len = 34;
		/* dvcid area contains vendor, product, serial */
		smc_slots.dvcid_serial_only = FALSE;
	}

	if (use_fifo)
		open_fifo(&fifo_fd, fifoname);

	for (;;) {
		/* Check for any messages */
		mlen = msgrcv(r_qid, &r_entry, MAXOBN, my_id, IPC_NOWAIT);
		if (mlen > 0) {
			if (processMessageQ(&r_entry.msg))
				goto exit;
		} else if (mlen < 0) {
			r_qid = init_queue();
			if (r_qid == -1)
				MHVTL_LOG("Can not open message queue: %s",
					strerror(errno));
		}

		ret = ioctl(cdev, VTL_POLL_AND_GET_HEADER, &vtl_cmd);
		if (ret < 0) {
			MHVTL_LOG("ret: %d : %s", ret, strerror(errno));
		} else {
			if (child_cleanup) {
				if (waitpid(child_cleanup, NULL, WNOHANG)) {
					MHVTL_DBG(2,
						"Cleaning up after child %d",
							child_cleanup);
					child_cleanup = 0;
				}
			}
			fflush(NULL);	/* So I can pipe debug o/p thru tee */
			switch(ret) {
			case VTL_QUEUE_CMD:
				process_cmd(cdev, buf, &vtl_cmd);
				pollInterval = 10;
				break;

			case VTL_IDLE:
				if (pollInterval < 1000000)
					pollInterval += 4000;

				usleep(pollInterval);
				break;

			default:
				MHVTL_LOG("ioctl(0x%x) returned %d\n",
						VTL_POLL_AND_GET_HEADER, ret);
				sleep(1);
				break;
			}
			if (current_state != last_state) {
				status_change(fifo_fd, current_state, my_id,
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
	close(cdev);
	free(buf);
	if (fifoname) {
		fclose(fifo_fd);
		unlink(fifoname);
	}

	exit(0);
}

