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
#include <pwd.h>
#include "vtl_common.h"
#include "scsi.h"
#include "q.h"
#include "vtllib.h"
#include "spc.h"
#include "be_byteshift.h"

char vtl_driver_name[] = "vtllibrary";

/*
 * The following should be dynamic (read from config file)
 *
 **** Danger Will Robinson!! ****:
 *   START_DRIVE HAS TO start at slot 1
 *	The Order of Drives with lowest start, followed by Picker, followed
 *	by MAP, finally Storage slots is IMPORTANT. - You have been warned.
 *   Some of the logic in this source depends on it.
 */
#define START_DRIVE	0x0001
static int num_drives = 4;

#define START_PICKER	0x0100
static int num_picker = 1;

#define START_MAP	0x0200
static int num_map = 0x0020;

#define START_STORAGE	0x0400
static int num_storage = 0x0800;

uint32_t SPR_Reservation_Generation;
uint8_t SPR_Reservation_Type;
uint64_t SPR_Reservation_Key;

// Element type codes
#define ANY			0
#define MEDIUM_TRANSPORT	1
#define STORAGE_ELEMENT		2
#define MAP_ELEMENT		3
#define DATA_TRANSFER		4

#define CAP_CLOSED	1
#define CAP_OPEN	0
#define OPERATOR 	1
#define ROBOT_ARM	0

#define	VOLTAG_LEN	36	/* size of voltag area in RES descriptor */

static int dvcid_len;
static int dvcid_serial_only;

static int bufsize = 1024 * 1024;
int verbose = 0;
int debug = 0;
static int libraryOnline = 1;		/* Default to Off-line */
static int cap_closed = CAP_CLOSED;	/* CAP open/closed status */
int reset = 1;				/* Poweron reset */
static uint8_t sam_status = 0;		/* Non-zero if Sense-data is valid */

uint8_t sense[SENSE_BUF_SIZE]; /* Request sense buffer */

struct lu_phy_attr lunit;

struct s_info { /* Slot Info */
	uint8_t cart_type; // 0 = Unknown, 1 = Data medium, 2 = Cleaning
	uint8_t barcode[11];
	uint32_t slot_location;
	uint32_t last_location;
	uint8_t	status;	// Used for MAP status.
	uint8_t	asc;	// Additional Sense Code
	uint8_t	ascq;	// Additional Sense Code Qualifier
	uint8_t internal_status; // internal states
};
// status definitions (byte[2] in the element descriptor)
#define STATUS_Full      0x01
#define STATUS_ImpExp    0x02
#define STATUS_Except    0x04
#define STATUS_Access    0x08
#define STATUS_ExEnab    0x10
#define STATUS_InEnab    0x20
#define STATUS_Reserved6 0x40
#define STATUS_Reserved7 0x80
// internal_status definitions:
#define INSTATUS_NO_BARCODE 0x01

/* Drive Info */
struct d_info {
	char inq_vendor_id[8];
	char inq_product_id[16];
	char inq_product_rev[4];
	char inq_product_sno[10];
	char online;		/* Physical status of drive */
	int SCSI_BUS;
	int SCSI_ID;
	int SCSI_LUN;
	char tapeLoaded;	/* Tape is 'loaded' by drive */
	struct s_info *slot;
};

static struct d_info *drive_info;
static struct s_info *storage_info;
static struct s_info *map_info;
static struct s_info *picker_info;

/* Log pages */
static struct Temperature_page Temperature_pg = {
	{ TEMPERATURE_PAGE, 0x00, 0x06, },
	{ 0x00, 0x00, 0x60, 0x02, }, 0x00,	/* Temperature */
	};

static struct TapeAlert_page TapeAlert;

/*
 * Mode Pages defined for SMC-3 devices..
 */

static struct mode sm[] = {
//	Page,  subpage, len, 'pointer to data struct'
	{0x02, 0x00, 0x00, NULL, }, // Disconnect Reconnect - SPC3
	{0x0a, 0x00, 0x00, NULL, }, // Control Extension - SPC3
	{0x1a, 0x00, 0x00, NULL, }, // Power condition - SPC3
	{0x1c, 0x00, 0x00, NULL, }, // Informational Exception Ctrl SPC3-8.3.6
	{0x1d, 0x00, 0x00, NULL, }, // Element Address Assignment - SMC3-7.3.3
	{0x1e, 0x00, 0x00, NULL, }, // Transport Geometry - SMC3-7.3.4
	{0x1f, 0x00, 0x00, NULL, }, // Device Capabilities - SMC3-7.3.2
	{0x00, 0x00, 0x00, NULL, }, // NULL terminator
	};

uint8_t blockDescriptorBlock[8] = {0, 0, 0, 0, 0, 0, 0, 0, };

static void usage(char *progname)
{
	printf("Usage: %s [-d] [-v]\n", progname);
	printf("      Where file == data file\n");
	printf("             'd' == debug -> Don't run as daemon\n");
	printf("             'v' == verbose -> Extra info logged via syslog\n");
}

#ifndef Solaris
 int ioctl(int, int, void *);
#endif

/* Copy bytes from 'src' to 'dest, blank-filling to length 'len'.  There will
 * not be a NULL byte at the end.
*/

static void blank_fill(uint8_t *dest, uint8_t *src, int len)
{
	int	i;

	for (i = 0; i < len; i++) {
		if (*src != '\0') {
			*dest++ = *src++;
		} else {
			*dest++ = ' ';
		}
	}
}

/* Return the element type of a particular element address */

static int slot_type(int addr)
{
	if ((addr >= START_DRIVE) && (addr < START_DRIVE + num_drives))
		return DATA_TRANSFER;
	if ((addr >= START_PICKER) && (addr < START_PICKER + num_picker))
		return MEDIUM_TRANSPORT;
	if ((addr >= START_MAP) && (addr < START_MAP + num_map))
		return MAP_ELEMENT;
	if ((addr >= START_STORAGE) && (addr < START_STORAGE + num_storage))
		return STORAGE_ELEMENT;
	return 0;
}

static void dump_element_desc(uint8_t *p, int voltag, int num_elem, int len)
{
	int i, j, idlen;

	i = 0;
	for (j = 0; j < num_elem; j++) {
		MHVTL_DBG(3, " Debug.... i = %d, len = %d", i, len);
		MHVTL_DBG(3, "  Element Address             : %d",
					get_unaligned_be16(&p[i]));
		MHVTL_DBG(3, "  Status                      : 0x%02x",
					p[i + 2]);
		MHVTL_DBG(3, "  Medium type                 : %d",
					p[i + 9] & 0x7);
		if (p[i + 9] & 0x80)
			MHVTL_DBG(3, "  Source Address              : %d",
					get_unaligned_be16(&p[i + 10]));
		i += 12;
		if (voltag) {
			i += VOLTAG_LEN;
			MHVTL_DBG(3, " Voltag info...");
		}

		MHVTL_DBG(3, " Identification Descriptor");
		MHVTL_DBG(3, "  Code Set                     : 0x%02x", p[i] & 0xf);
		MHVTL_DBG(3, "  Identifier type              : 0x%02x",
					p[i + 1] & 0xf);
		idlen = p[i + 3];
		MHVTL_DBG(3, "  Identifier length            : %d", idlen);
		if (idlen) {
			if (dvcid_serial_only) {
				MHVTL_DBG(3, "  ASCII data                   : %10s", &p[i + 4]);
			} else {
				MHVTL_DBG(3, "  ASCII data                   : %8s", &p[i + 4]);
				MHVTL_DBG(3, "  ASCII data                   : %16s", &p[i + 12]);
				MHVTL_DBG(3, "  ASCII data                   : %10s", &p[i + 28]);
			}
		}
		i = (j + 1) * len;
	}
}

static void decode_element_status(uint8_t *p)
{
	int voltag;
	int elem_len;
	int total_elements, total_bytes;
	int page_elements, page_bytes;

	MHVTL_DBG(3, "Element Status Data");
	MHVTL_DBG(3, "  First element reported       : %d",
					get_unaligned_be16(&p[0]));
	total_elements = get_unaligned_be16(&p[2]);
	MHVTL_DBG(3, "  Number of elements available : %d", total_elements);
	total_bytes = get_unaligned_be24(&p[5]);
	MHVTL_DBG(3, "  Byte count of report         : %d", total_bytes);
	p += 8;

	while (total_elements && total_bytes) {

		MHVTL_DBG(3, "Element Status Page");
		MHVTL_DBG(3, "  Element Type code            : %d", p[0]);
		voltag = (p[1] & 0x80) ? 1 : 0;
		MHVTL_DBG(3, "  Primary Vol Tag              : %s",
					voltag ? "Yes" : "No");
		MHVTL_DBG(3, "  Alt Vol Tag                  : %s",
					(p[1] & 0x40) ? "Yes" : "No");
		elem_len = get_unaligned_be16(&p[2]);
		MHVTL_DBG(3, "  Element descriptor length    : %d", elem_len);
		page_bytes = get_unaligned_be24(&p[5]);
		MHVTL_DBG(3, "  Byte count of descriptor data: %d", page_bytes);
		page_elements = page_bytes / elem_len;
		p += 8;

		MHVTL_DBG(3, "Element Descriptor(s) : Num of Elements %d",
			page_elements);

		dump_element_desc(p, voltag, page_elements, elem_len);

		total_elements -= page_elements;
		total_bytes -= page_bytes;
		p += page_bytes;
	}

	fflush(NULL);
}


/*
 * Process the MODE_SELECT command
 */
static int resp_mode_select(int cdev, struct vtl_ds *dbuf_p)
{

	return retrieve_CDB_data(cdev, dbuf_p);
}

/*
 * Takes a slot number and returns a struct pointer to the slot
 */
static struct s_info *slot2struct(int addr)
{
	switch (slot_type(addr)) {
	case MAP_ELEMENT:
		MHVTL_DBG(2, "slot2struct: MAP %d", addr);
		return &map_info[addr - START_MAP];
	case STORAGE_ELEMENT:
		MHVTL_DBG(2, "slot2struct: Storage %d", addr);
		return &storage_info[addr - START_STORAGE];
	case MEDIUM_TRANSPORT:
		MHVTL_DBG(2, "slot2struct: Picker %d", addr);
		return &picker_info[addr - START_PICKER];
	case DATA_TRANSFER:
		MHVTL_DBG(2, "slot2struct: Drive %d", addr);
		return drive_info[addr - START_DRIVE].slot;
	}

// Should NEVER get here as we have performed bounds checking b4
	MHVTL_DBG(1, "Arrr... slot2struct returning NULL");

return NULL;
}

/*
 * Takes a Drive number and returns a struct pointer to the drive
 */
static struct d_info *drive2struct(int addr)
{
	return &drive_info[addr - START_DRIVE];
}

/* Returns true if slot has media in it */
static int slotOccupied(struct s_info *s)
{
	return(s->status & STATUS_Full);
}

/* Returns true if drive has media in it */
static int driveOccupied(struct d_info *d)
{
	return(slotOccupied(d->slot));
}

/*
 * A value of 0 indicates that media movement from the I/O port
 * to the handler is denied; a value of 1 indicates that the movement
 * is permitted.
 */
/*
static void setInEnableStatus(struct s_info *s, int flg)
{
	if (flg)
		s->status |= STATUS_InEnab;
	else
		s->status &= ~STATUS_InEnab;
}
*/
/*
 * A value of 0 in the Export Enable field indicates that media movement
 * from the handler to the I/O port is denied. A value of 1 indicates that
 * movement is permitted.
 */
/*
static void setExEnableStatus(struct s_info *s, int flg)
{
	if (flg)
		s->status |= STATUS_ExEnab;
	else
		s->status &= ~STATUS_ExEnab;
}
*/

/*
 * A value of 1 indicates that a cartridge may be moved to/from
 * the drive (but not both).
 */
/*
static void setAccessStatus(struct s_info *s, int flg)
{
	if (flg)
		s->status |= STATUS_Access;
	else
		s->status &= ~STATUS_Access;
}
*/

/*
 * Reset to 0 indicates it is in normal state, set to 1 indicates an Exception
 * condition exists. An exception indicates the libary is uncertain of an
 * elements status.
 */
/*
static void setExceptStatus(struct s_info *s, int flg)
{
	if (flg)
		s->status |= STATUS_Except;
	else
		s->status &= ~STATUS_Except;
}
*/

/*
 * If set(1) then cartridge placed by operator
 * If clear(0), placed there by handler.
 */
static void setImpExpStatus(struct s_info *s, int flg)
{
	if (flg)
		s->status |= STATUS_ImpExp;
	else
		s->status &= ~STATUS_ImpExp;
}

/*
 * Sets the 'Full' bit true/false in the status field
 */
static void setFullStatus(struct s_info *s, int flg)
{
	if (flg)
		s->status |= STATUS_Full;
	else
		s->status &= ~STATUS_Full;
}

static void setSlotEmpty(struct s_info *s)
{
	setFullStatus(s, 0);
}

static void setDriveEmpty(struct d_info *d)
{
	setFullStatus(d->slot, 0);
}

static void setSlotFull(struct s_info *s)
{
	setFullStatus(s, 1);
}

static void setDriveFull(struct d_info *d)
{
	setFullStatus(d->slot, 1);
}

/* Returns 1 (true) if slot is MAP slot */
static int is_map_slot(struct s_info *s)
{
	if (slot_type(s->slot_location) == MAP_ELEMENT)
		return 1;

	return 0;
}

/*
 * Logically move information from 'src' address to 'dest' address
 */
static void move_cart(struct s_info *src, struct s_info *dest)
{

	dest->cart_type = src->cart_type;
	memcpy(dest->barcode, src->barcode, 10);
	dest->last_location = src->slot_location;
	setSlotFull(dest);
	if (is_map_slot(dest))
		setImpExpStatus(dest, ROBOT_ARM); /* Placed by robot arm */

	src->cart_type = 0;		/* Src slot no longer occupied */
	memset(src->barcode, 0, 10);	/* Zero out barcode */
	src->last_location = 0;		/* Forget where the old media was */
	setSlotEmpty(src);		/* Clear Full bit */
}

/* Move media in drive 'src_addr' to drive 'dest_addr' */
static int move_drive2drive(int src_addr, int dest_addr, uint8_t *sam_stat)
{
	struct d_info *src;
	struct d_info *dest;
	char cmd[128];
	int x;

	src  = drive2struct(src_addr);
	dest = drive2struct(dest_addr);

	if (!driveOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sam_stat);
		return 1;
	}
	if (driveOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sam_stat);
		return 1;
	}

	move_cart(src->slot, dest->slot);

	// Send 'unload' message to drive b4 the move..
	send_msg("unload", src->slot->slot_location);

	sprintf(cmd, "lload %s", dest->slot->barcode);

	/* Remove traling spaces */
	for (x = 6; x < 16; x++)
		if (cmd[x] == ' ') {
			cmd[x] = '\0';
			break;
		}
	MHVTL_DBG(2, "Sending cmd: \'%s\' to drive %d",
					cmd, dest->slot->slot_location);

	send_msg(cmd, dest->slot->slot_location);

return 0;
}

static int move_drive2slot(int src_addr, int dest_addr, uint8_t *sam_stat)
{
	struct d_info *src;
	struct s_info *dest;

	src  = drive2struct(src_addr);
	dest = slot2struct(dest_addr);

	if ( ! driveOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sam_stat);
		return 1;
	}
	if ( slotOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sam_stat);
		return 1;
	}

	if (is_map_slot(dest)) {
		if (! cap_closed) {
			mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_REMOVAL_PREVENTED,
						sam_stat);
			return 1;
		}
	}

	// Send 'unload' message to drive b4 the move..
	send_msg("unload", src->slot->slot_location);

	move_cart(src->slot, dest);
	setDriveEmpty(src);

return 0;
}

static int move_slot2drive(int src_addr, int dest_addr, uint8_t *sam_stat)
{
	struct s_info *src;
	struct d_info *dest;
	char cmd[128];
	int x;

	src  = slot2struct(src_addr);
	dest = drive2struct(dest_addr);

	if ( ! slotOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sam_stat);
		return 1;
	}
	if ( driveOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sam_stat);
		return 1;
	}

	move_cart(src, dest->slot);
	setDriveFull(dest);

	sprintf(cmd, "lload %s", dest->slot->barcode);
	/* Remove traling spaces */
	for (x = 6; x < 16; x++)
		if (cmd[x] == ' ') {
			cmd[x] = '\0';
			break;
		}
	MHVTL_DBG(1, "About to send cmd: \'%s\' to drive %d",
					cmd, dest->slot->slot_location);

	send_msg(cmd, dest->slot->slot_location);

return 0;
}

static int move_slot2slot(int src_addr, int dest_addr, uint8_t *sam_stat)
{
	struct s_info *src;
	struct s_info *dest;

	src  = slot2struct(src_addr);
	dest = slot2struct(dest_addr);

	MHVTL_DBG(1, "Moving from slot %d to slot %d",
				src->slot_location, dest->slot_location);

	if (! slotOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sam_stat);
		return 1;
	}
	if (slotOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sam_stat);
		return 1;
	}

	if (is_map_slot(dest)) {
		if (! cap_closed) {
			mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_REMOVAL_PREVENTED,
						sam_stat);
			return 1;
		}
	}

	move_cart(src, dest);

	return 0;
}

/* Return OK if 'addr' is within either a MAP, Drive or Storage slot */
static int valid_slot(int addr)
{
	switch (slot_type(addr)) {
	case STORAGE_ELEMENT:
	case MAP_ELEMENT:
	case DATA_TRANSFER:
		return 1;
	}
	return 0;
}

/* Move a piece of medium from one slot to another */
static int resp_move_medium(uint8_t *cmd, uint8_t *buf, uint8_t *sam_stat)
{
	int transport_addr;
	int src_addr, src_type;
	int dest_addr, dest_type;
	int retVal = 0;	// Return a success status

	transport_addr = get_unaligned_be16(&cmd[2]);
	src_addr  = get_unaligned_be16(&cmd[4]);
	dest_addr = get_unaligned_be16(&cmd[6]);
	src_type = slot_type(src_addr);
	dest_type = slot_type(dest_addr);

	if (verbose) {
		if (cmd[11] && 0xc0)
			syslog(LOG_DAEMON|LOG_INFO, "%s",
				(cmd[11] & 0x80) ? "  Retract I/O port" :
						   "  Extend I/O port");
		else
			syslog(LOG_DAEMON|LOG_INFO,
	 "Moving from slot %d to Slot %d using transport %d, Invert media: %s",
					src_addr, dest_addr, transport_addr,
					(cmd[10]) ? "yes" : "no");
	}

	if (cmd[10] != 0) {	/* Can not Invert media */
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sam_stat);
		return -1;
	}
	if (cmd[11] == 0xc0) {	// Invalid combo of Extend/retract I/O port
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sam_stat);
		return -1;
	}
	if (cmd[11]) // Must be an Extend/Retract I/O port cmd.. NO-OP
		return 0;

	if (transport_addr == 0)
		transport_addr = START_PICKER;
	if (slot_type(transport_addr) != MEDIUM_TRANSPORT) {
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sam_stat);
		retVal = -1;
	}
	if (! valid_slot(src_addr)) {
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sam_stat);
		retVal = -1;
	}
	if (! valid_slot(dest_addr)) {
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sam_stat);
		retVal = -1;
	}

	if (retVal == 0) {
		if (src_type == DATA_TRANSFER && dest_type == DATA_TRANSFER) {
			/* Move between drives */
			if (move_drive2drive(src_addr, dest_addr, sam_stat))
				retVal = -1;
		} else if (src_type == DATA_TRANSFER) {
			if (move_drive2slot(src_addr, dest_addr, sam_stat))
				retVal = -1;
		} else if (dest_type == DATA_TRANSFER) {
			if (move_slot2drive(src_addr, dest_addr, sam_stat))
				retVal = -1;
		} else {   // Move between (non-drive) slots
			if (move_slot2slot(src_addr, dest_addr, sam_stat))
				retVal = -1;
		}
	}

return(retVal);
}

/*
 * Calculate length of one element
 */
static int determine_element_sz(uint8_t dvcid, uint8_t voltag, int type)
{
	return 16 + (voltag ? VOLTAG_LEN : 0) +
		(dvcid && (type == DATA_TRANSFER) ? dvcid_len : 0);
}

/*
 * Fill in a single element descriptor
 *
 * Returns number of bytes in element data.
 */
static int fill_element_descriptor(uint8_t *p, int addr, int voltag, int dvcid)
{
	struct d_info *d = NULL;
	struct s_info *s = NULL;
	int type = slot_type(addr);
	int j = 0;

	switch (type) {
	case DATA_TRANSFER:
		d = drive2struct(addr);
		s = slot2struct(addr);
		break;
	case MEDIUM_TRANSPORT:
		s = slot2struct(addr);
		break;
	case MAP_ELEMENT:
		s = slot2struct(addr);
		break;
	case STORAGE_ELEMENT:
		s = slot2struct(addr);
		break;
	}

	/* Should never occur, but better to trap then core */
	if (!s) {
		MHVTL_DBG(1, "Slot out of range");
		return 0;
	}

	MHVTL_DBG(2, "Slot location: %d", s->slot_location);

	put_unaligned_be16(s->slot_location, &p[j]);
	j += 2;

	p[j] = s->status;
	if (type == MAP_ELEMENT) {
		if (cap_closed)
			p[j] |= STATUS_Access;
		else
			p[j] &= ~STATUS_Access;
	}
	j++;

	p[j++] = 0;	/* Reserved */

/* Possible values for ASC/ASCQ for data transfer elements
 * 0x30/0x03 Cleaner cartridge present
 * 0x83/0x00 Barcode not scanned
 * 0x83/0x02 No magazine installed
 * 0x83/0x04 Tape drive not installed
 * 0x83/0x09 Unable to read bar code
 * 0x80/0x5d Drive operating in overheated state
 * 0x80/0x5e Drive being shutdown due to overheat condition
 * 0x80/0x63 Drive operating with low module fan speed
 * 0x80/0x5f Drive being shutdown due to low module fan speed
 */
	p[j++] = s->asc;  /* Additional Sense Code */
	p[j++] = s->ascq; /* Additional Sense Code Qualifer */

	j++;		/* Reserved */
	if (type == DATA_TRANSFER) {
		p[j++] = d->SCSI_ID;
	} else {
		j++;	/* Reserved */
	}
	j++;		/* Reserved */

	/* bit 8 set if Source Storage Element is valid | s->occupied */
	p[j] = (s->last_location > 0) ? 0x80 : 0;
	/* 0 - empty, 1 - data, 2 cleaning tape */
	p[j++] |= (s->cart_type & 0x0f);

	/* Source Storage Element Address */
	put_unaligned_be16(s->last_location, &p[j]);
	j += 2;

	MHVTL_DBG(2, "DVCID: %d, VOLTAG: %d, Index: %d", dvcid, voltag, j);

	if (voltag) {
		/* Barcode with trailing space(s) */
		if ((s->status & STATUS_Full) &&
		    !(s->internal_status & INSTATUS_NO_BARCODE))
			blank_fill(&p[j], s->barcode, VOLTAG_LEN);
		else
			memset(&p[j], 0, VOLTAG_LEN);

		j += VOLTAG_LEN;	/* Account for barcode */
	}

	if (dvcid && type == DATA_TRANSFER) {
		p[j++] = 2;	/* Code set 2 = ASCII */
		p[j++] = 1;	/* Identifier type */
		j++;		/* Reserved */
		p[j++] = dvcid_len;	/* Identifier Length */
		if (dvcid_serial_only) {
			blank_fill(&p[j], (uint8_t *)d->inq_product_sno, dvcid_len);
			j += dvcid_len;
		} else {
			blank_fill(&p[j], (uint8_t *)d->inq_vendor_id, 8);
			j += 8;
			blank_fill(&p[j], (uint8_t *)d->inq_product_id, 16);
			j += 16;
			blank_fill(&p[j], (uint8_t *)d->inq_product_sno, 10);
			j += 10;
		}
	} else {
		j += 4;		/* Reserved */
	}
	MHVTL_DBG(3, "Returning %d bytes", j);

return j;
}

/*
 * Fill in element status page Header (8 bytes)
 */
static int fill_element_status_page_hdr(uint8_t *p,
					uint16_t element_count, uint8_t dvcid,
					uint8_t voltag, uint8_t typeCode)
{
	int	element_sz;
	uint32_t	element_len;

	element_sz = determine_element_sz(dvcid, voltag, typeCode);

	p[0] = typeCode;	/* Element type Code */

	/* Primary Volume Tag set - Returning Barcode info */
	p[1] = (voltag == 0) ? 0 : 0x80;

	/* Number of bytes per element */
	put_unaligned_be16(element_sz, &p[2]);

	element_len = element_sz * element_count;

	/* Total number of bytes in all element descriptors */
	put_unaligned_be32(element_len & 0xffffff, &p[4]);

	/* Reserved */
	p[4] = 0;	/* Above mask should have already set this to 0... */

	MHVTL_DBG(2, "Element Status Page Header: "
			"%02x %02x %02x %02x %02x %02x %02x %02x",
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);


return(8);	/* Always 8 bytes in header */
}

/*
 * Build the initial ELEMENT STATUS HEADER
 *
 */
static int fill_element_status_data_hdr(uint8_t *p, int start, int count,
					uint32_t byte_count)
{
	MHVTL_DBG(2, "Building READ ELEMENT STATUS Header struct");
	MHVTL_DBG(2, " Starting slot: %d, number of configured slots: %d",
					start, count);

	/* Start of ELEMENT STATUS DATA */
	put_unaligned_be16(start, &p[0]);
	put_unaligned_be16(count, &p[2]);

	/* The byte_count should be the length required to return all of
	 * valid data.
	 * The 'allocated length' indicates how much data can be returned.
	 */
	put_unaligned_be32(byte_count & 0xffffff, &p[4]);

	MHVTL_DBG(2, " Element Status Data HEADER: "
			"%02x %02x %02x %02x %02x %02x %02x %02x",
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
	MHVTL_DBG(3, " Decoded:");
	MHVTL_DBG(3, "  First element Address    : %d",
					get_unaligned_be16(&p[0]));
	MHVTL_DBG(3, "  Number elements reported : %d",
					get_unaligned_be16(&p[2]));
	MHVTL_DBG(3, "  Total byte count         : %d",
					get_unaligned_be32(&p[4]));

return 8;	// Header is 8 bytes in size..
}

/*
 * Read Element Status command will pass 'start element address' & type of slot
 *
 * We return the first valid slot number which matches.
 * or zero on no matching slots..
 */
static int find_first_matching_element(uint16_t start, uint8_t typeCode)
{
	switch(typeCode) {
	case ANY:	// Don't care what 'type'
		/* Logic here depends on Storage slots being
		 * higher (numerically) than MAP which is higher than
		 * Picker, which is higher than the drive slot number..
		 * See DWR: near top of this file !!
		 */

		/* Special case - 'All types'
		 * If Start is undefined/defined as '0', then return
		 * Beginning slot
		 */
		if (start == 0)
			return(START_DRIVE);

		/* If we are above Storage range, return nothing. */
		if (start >= START_STORAGE + num_storage)
			return 0;
		if (start >= START_STORAGE)
			return(start);
		/* If we are above I/O Range -> return START_STORAGE */
		if (start >= START_MAP + num_map)
			return(START_STORAGE);
		if (start >= START_MAP)
			return(start);
		/* If we are above the Picker range -> Return I/O Range.. */
		if (start >= START_PICKER + num_picker)
			return START_MAP;
		if (start >= START_PICKER)
			return (start);
		/* If we are above the Drive range, return Picker.. */
		if (start >= START_DRIVE + num_drives)
			return(START_PICKER);
		if (start >= START_DRIVE)
			return(start);
		break;
	case MEDIUM_TRANSPORT:	/* Medium Transport. */
		if ((start >= START_PICKER) &&
		   (start < (START_PICKER + num_picker)))
			return start;
		if (start < START_PICKER)
			return START_PICKER;
		break;
	case STORAGE_ELEMENT:	/* Storage Slots */
		if ((start >= START_STORAGE) &&
		   (start < (START_STORAGE + num_storage)))
			return start;
		if (start < START_STORAGE)
			return START_STORAGE;
		break;
	case MAP_ELEMENT:	/* Import/Export */
		if ((start >= START_MAP) &&
		   (start < (START_MAP + num_map)))
			return start;
		if (start < START_MAP)
			return START_MAP;
		break;
	case DATA_TRANSFER:	/* Data transfer */
		if ((start >= START_DRIVE) &&
		   (start < (START_DRIVE + num_drives)))
			return start;
		if (start < START_DRIVE)
			return START_DRIVE;
		break;
	}
return 0;
}

/*
 * Fill in Element status page header + each Element descriptor
 *
 * Returns zero on success, or error code if illegal request.
 */
static uint32_t fill_element_page(uint8_t *p, int type, uint16_t start,
		uint16_t max_count, uint32_t max_bytes, uint8_t voltag,
		uint8_t dvcid, uint16_t *cur_count, uint32_t *cur_offset)
{
	uint16_t	begin;
	uint16_t	count, avail, space;
	int	min_addr, num_addr;
	int j;

	switch (type) {
	case MEDIUM_TRANSPORT:
		min_addr = START_PICKER;
		num_addr = num_picker;
		dvcid = 0;
		break;
	case STORAGE_ELEMENT:
		min_addr = START_STORAGE;
		num_addr = num_storage;
		dvcid = 0;
		break;
	case MAP_ELEMENT:
		min_addr = START_MAP;
		num_addr = num_map;
		dvcid = 0;
		break;
	case DATA_TRANSFER:
		min_addr = START_DRIVE;
		num_addr = num_drives;
		break;
	default:
		return E_INVALID_FIELD_IN_CDB;
	}

	// Find first valid slot.
	begin = find_first_matching_element(start, type);
	if (begin == 0)
		return E_INVALID_FIELD_IN_CDB;

	// The number of elements to report is the minimum of:
	// 1. the number the caller asked for (max_count - *cur_count).
	// 2. the number that remain starting at address begin, and
	// 3. the number that will fit in the remaining
	//    (max_bytes - *cur_offset) bytes, allowing for an 8-byte header.

	avail = min_addr + num_addr - begin;
	count = avail < max_count - *cur_count ? avail : max_count - *cur_count;
	space = (max_bytes - *cur_offset - 8) /
				determine_element_sz(dvcid, voltag, type);
	count = space < count ? space : count;
	if (count == 0) {
		if (*cur_count == 0) {
			return E_PARAMETER_LIST_LENGTH_ERR;
		} else {
			return 0;
		}
	}

	// Create Element Status Page Header.
	*cur_offset += fill_element_status_page_hdr(&p[*cur_offset], count,
		dvcid, voltag, type);

	// Now loop over each slot and fill in details.

	for (j = 0; j < count; j++, begin++) {
		MHVTL_DBG(2, "Slot: %d", begin);
		*cur_offset += fill_element_descriptor(&p[*cur_offset],
			begin, voltag, dvcid);
	}
	*cur_count += count;

return 0;
}

/*
 * Build READ ELEMENT STATUS data.
 *
 * Returns number of bytes to xfer back to host.
 */
static int resp_read_element_status(uint8_t *cdb, uint8_t *buf,
							uint8_t *sam_stat)
{
	uint8_t	*p;
	uint8_t	typeCode = cdb[1] & 0x0f;
	uint8_t	voltag = (cdb[1] & 0x10) >> 4;
	uint16_t req_start_elem;
	uint16_t number;
	uint8_t	dvcid = cdb[6] & 0x01;	/* Device ID */
	uint32_t alloc_len;
	uint16_t start;	// First valid slot location
	uint32_t cur_offset;
	uint16_t cur_count;
	uint32_t ec;

	req_start_elem = get_unaligned_be16(&cdb[2]);
	number = get_unaligned_be16(&cdb[4]);
	alloc_len = 0xffffff & get_unaligned_be32(&cdb[6]);

	switch(typeCode) {
	case ANY:
		MHVTL_DBG(3, " Element type(%d) => All Elements", typeCode);
		break;
	case MEDIUM_TRANSPORT:
		MHVTL_DBG(3, " Element type(%d) => Medium Transport", typeCode);
		break;
	case STORAGE_ELEMENT:
		MHVTL_DBG(3, " Element type(%d) => Storage Elements", typeCode);
		break;
	case MAP_ELEMENT:
		MHVTL_DBG(3, " Element type(%d) => Import/Export", typeCode);
		break;
	case DATA_TRANSFER:
		MHVTL_DBG(3,
			" Element type(%d) => Data Transfer Elements",typeCode);
		break;
	default:
		MHVTL_DBG(3,
			" Element type(%d) => Invalid type requested",typeCode);
		break;
	}
	MHVTL_DBG(3, "  Starting Element Address: %d",req_start_elem);
	MHVTL_DBG(3, "  Number of Elements      : %d",number);
	MHVTL_DBG(3, "  Allocation length       : %d",alloc_len);
	MHVTL_DBG(3, "  Device ID: %s, voltag: %s",
					(dvcid == 0) ? "No" :  "Yes",
					(voltag == 0) ? "No" :  "Yes" );

	/* Set alloc_len to smallest value */
	if (alloc_len > bufsize)
		alloc_len = bufsize;

	/* Init buffer */
	memset(buf, 0, alloc_len);

	if (cdb[11] != 0x0) {	/* Reserved byte.. */
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,sam_stat);
		return(0);
	}

	/* Find first matching slot number which matches the typeCode. */
	start = find_first_matching_element(req_start_elem, typeCode);
	if (start == 0) {	// Nothing found..
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,sam_stat);
		return(0);
	}

	/* Leave room for 'master' header which is filled in at the end... */
	p = buf;
	cur_offset = 8;
	cur_count = 0;
	ec = 0;

	switch(typeCode) {
	case MEDIUM_TRANSPORT:
	case STORAGE_ELEMENT:
	case MAP_ELEMENT:
	case DATA_TRANSFER:
		ec = fill_element_page(p, typeCode, start, number,
			alloc_len, voltag, dvcid, &cur_count, &cur_offset);
		break;
	case ANY:
		/* Logic here depends on Storage slots being
		 * higher (numerically) than MAP which is higher than
		 * Picker, which is higher than the drive slot number..
		 * See DWR: near top of this file !!
		 */
		if (slot_type(start) == DATA_TRANSFER) {
			ec = fill_element_page(p, DATA_TRANSFER, start, number,
				alloc_len, voltag, dvcid, &cur_count, &cur_offset);
			if (ec)
				break;
			start = START_PICKER;
		}
		if (slot_type(start) == MEDIUM_TRANSPORT) {
			ec = fill_element_page(p, MEDIUM_TRANSPORT, start, number,
				alloc_len, voltag, dvcid, &cur_count, &cur_offset);
			if (ec)
				break;
			start = START_MAP;
		}
		if (slot_type(start) == MAP_ELEMENT) {
			ec = fill_element_page(p, MAP_ELEMENT, start, number,
				alloc_len, voltag, dvcid, &cur_count, &cur_offset);
			if (ec)
				break;
			start = START_STORAGE;
		}
		if (slot_type(start) == STORAGE_ELEMENT) {
			ec = fill_element_page(p, STORAGE_ELEMENT, start, number,
				alloc_len, voltag, dvcid, &cur_count, &cur_offset);
			if (ec)
				break;
		}
		break;
	default:	/* Illegal descriptor type. */
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,sam_stat);
		return(0);
		break;
	}
	if (ec != 0) {
		mkSenseBuf(ILLEGAL_REQUEST, ec, sam_stat);
		return(0);
	}

	/* Now populate the 'main' header structure with byte count.. */
	fill_element_status_data_hdr(&buf[0], start, cur_count, cur_offset);

	MHVTL_DBG(3, "Returning %d bytes", cur_offset);

	if (debug)
		hex_dump(buf, cur_offset);

	decode_element_status(buf);

	/* Return the smallest number */
	return cur_offset;
}

/*
 * Process the LOG_SENSE command
 *
 * Temperature page & tape alert pages only...
 */
#define TAPE_ALERT 0x2e
static int resp_log_sense(uint8_t *cdb, uint8_t *buf)
{
	uint8_t	*b = buf;
	int retval = 0;

	uint8_t supported_pages[] = {	0x00, 0x00, 0x00, 0x04,
					0x00,
					TEMPERATURE_PAGE,
					TAPE_ALERT
					};

	switch (cdb[2] & 0x3f) {
	case 0:	/* Send supported pages */
		MHVTL_DBG(2, "%s", "Sending supported pages");
		put_unaligned_be16(sizeof(supported_pages) - 4,
					&supported_pages[2]);
		b = memcpy(b, supported_pages, sizeof(supported_pages));
		retval = sizeof(supported_pages);
		break;
	case TEMPERATURE_PAGE:	/* Temperature page */
		MHVTL_DBG(2, "LOG SENSE: Temperature page");
		put_unaligned_be16(sizeof(Temperature_pg) - sizeof(Temperature_pg.pcode_head), &Temperature_pg.pcode_head.len);
		put_unaligned_be16(35, &Temperature_pg.temperature);
		b = memcpy(b, &Temperature_pg, sizeof(Temperature_pg));
		retval += sizeof(Temperature_pg);
		break;
	case TAPE_ALERT:	/* TapeAlert page */
		MHVTL_DBG(2, "LOG SENSE: TapeAlert page");
		put_unaligned_be16(sizeof(TapeAlert) - sizeof(TapeAlert.pcode_head), &TapeAlert.pcode_head.len);
		b = memcpy(b, &TapeAlert, sizeof(TapeAlert));
		retval += sizeof(TapeAlert);
		setTapeAlert(&TapeAlert, 0); /* Clear flags after value read. */
		break;
	default:
		MHVTL_DBG(1, "Unknown log sense code: 0x%x", cdb[2] & 0x3f);
		retval = 2;
		break;
	}
	return retval;
}

/*
 *
 * Process the SCSI command
 *
 * Called with:
 *	cdev     -> Char dev file handle,
 *	cdb    -> SCSI Command buffer pointer,
 *	struct vtl_ds -> general purpose data structure... Need better name
 *
 * Return:
 *	success/failure
 */
static int processCommand(int cdev, uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	uint32_t block_size = 0;
	uint32_t ret = 0;
	int k = 0;
	struct mode *smp = sm;
	uint32_t count;

	/* Quick fix for POC */
	uint8_t *buf = dbuf_p->data;
	uint8_t *sam_stat = &dbuf_p->sam_stat;

	MHVTL_DBG_PRT_CDB(1, dbuf_p->serialNo, cdb);

	switch (cdb[0]) {
	case INITIALIZE_ELEMENT_STATUS_WITH_RANGE:
	case INITIALIZE_ELEMENT_STATUS:
		MHVTL_DBG(1, "%s", "INITIALIZE ELEMENT **");
		if (check_reset(sam_stat))
			break;
		sleep(1);
		break;

	case INQUIRY:
		MHVTL_DBG(1, "%s", "INQUIRY **");
		ret += spc_inquiry(cdb, dbuf_p, &lunit);
		break;

	case LOG_SELECT:	// Set or reset LOG stats.
		MHVTL_DBG(1, "%s", "LOG SELECT **");
		if (check_reset(sam_stat))
			break;
		resp_log_select(cdb, sam_stat);
		break;
	case LOG_SENSE:
		MHVTL_DBG(1, "%s", "LOG SENSE **");
		ret += resp_log_sense(cdb, buf);
		break;

	case MODE_SELECT:
	case MODE_SELECT_10:
		MHVTL_DBG(1, "%s", "MODE SELECT **");
		dbuf_p->sz = (MODE_SELECT == cdb[0]) ? cdb[4] :
						((cdb[7] << 8) | cdb[8]);
		ret += resp_mode_select(cdev, dbuf_p);
		break;

	case MODE_SENSE:
	case MODE_SENSE_10:
		MHVTL_DBG(1, "%s", "MODE SENSE **");
		ret += resp_mode_sense(cdb, buf, smp, 0, sam_stat);
		break;

	case MOVE_MEDIUM:
		MHVTL_DBG(1, "%s", "MOVE MEDIUM **");
		if (check_reset(sam_stat))
			break;
		k = resp_move_medium(cdb, buf, sam_stat);
		break;
	case ALLOW_MEDIUM_REMOVAL:
		if (check_reset(sam_stat))
			break;
		resp_allow_prevent_removal(cdb, sam_stat);
		break;
	case READ_ELEMENT_STATUS:
		MHVTL_DBG(1, "%s", "READ ELEMENT STATUS **");
		if (check_reset(sam_stat))
			break;
		ret += resp_read_element_status(cdb, buf, sam_stat);
		break;

	case REQUEST_SENSE:
		MHVTL_DBG(1, "%s", "SCSI REQUEST SENSE **");
		MHVTL_DBG(1, "Sense key/ASC/ASCQ [0x%02x 0x%02x 0x%02x]",
					sense[2], sense[12], sense[13]);
		block_size = (cdb[4] < sizeof(sense)) ? cdb[4] : sizeof(sense);
		memcpy(buf, sense, block_size);
		/* Clear out the request sense flag */
		*sam_stat = 0;
		memset(sense, 0, sizeof(sense));
		ret += block_size;
		break;

	case RESERVE:
	case RESERVE_10:
	case RELEASE:
	case RELEASE_10:
		if (check_reset(sam_stat))
			break;
		break;

	case REZERO_UNIT:	/* Rewind */
		MHVTL_DBG(1, "%s", "Rewinding **");
		if (check_reset(sam_stat))
			break;
		sleep(1);
		break;

	case START_STOP:	// Load/Unload cmd
		if (check_reset(sam_stat))
			break;
		if (cdb[4] && 0x1) {
			libraryOnline = 1;
			MHVTL_DBG(1, "%s", "Library online **");
		} else {
			libraryOnline = 0;
			MHVTL_DBG(1, "%s", "Library offline **");
		}
		break;
	case TEST_UNIT_READY:	// Return OK by default
		MHVTL_DBG(1, "%s %s", "Test Unit Ready :",
					(libraryOnline == 0) ? "No" : "Yes");
		if (check_reset(sam_stat))
			break;
		if ( ! libraryOnline)
			mkSenseBuf(NOT_READY, NO_ADDITIONAL_SENSE, sam_stat);
		break;

	case RECEIVE_DIAGNOSTIC:
		MHVTL_DBG(1, "Receive Diagnostic (%ld) **",
						(long)dbuf_p->serialNo);
		ret += ProcessReceiveDiagnostic(cdb, dbuf_p);
		break;

	case SEND_DIAGNOSTIC:
		MHVTL_DBG(1, "Send Diagnostic **");
		count = get_unaligned_be16(&cdb[3]);
		if (count) {
			dbuf_p->sz = count;
			block_size = retrieve_CDB_data(cdev, dbuf_p);
			ProcessSendDiagnostic(cdb, 16, dbuf_p);
		}
		break;

	default:
		MHVTL_DBG(1,  "%s", "******* Unsupported command **********");
		MHVTL_DBG_PRT_CDB(1, dbuf_p->serialNo, cdb);
		if (check_reset(sam_stat))
			break;
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		break;
	}

	dbuf_p->sz = ret;

	return 0;
}

/*
 * Respond to messageQ 'list map' by sending a list of PCLs to messageQ
 */
static void list_map(void)
{
	struct s_info *sp;
	int	a;
	char msg[MAXOBN];
	char *c = msg;
	*c = '\0';

	for (a = START_MAP; a < START_MAP + num_map; a++) {
		sp = slot2struct(a);
		if (slotOccupied(sp)) {
			strncat(c, (char *)sp->barcode, 10);
			MHVTL_DBG(2, "MAP slot %d full", a - START_MAP);
		} else {
			MHVTL_DBG(2, "MAP slot %d empty", a - START_MAP);
		}
	}
	MHVTL_DBG(2, "map contents: %s", msg);
	send_msg(msg, LIBRARY_Q + 1);
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
	struct s_info *sp = NULL;
	int slt;
	int len;

	len = strlen(barcode);

	for (slt = 0; slt < num_map; slt++) {
		sp = &map_info[slt];
		if (slotOccupied(sp)) {
			if (! strncmp((char *)sp->barcode, barcode, len)) {
				MHVTL_DBG(3, "Match: %s %s",
					sp->barcode, barcode);
				return 1;
			} else 
				MHVTL_DBG(3, "No match: %s %s",
					sp->barcode, barcode);
		}
	}
	for (slt = 0; slt < num_storage; slt++) {
		sp = &storage_info[slt];
		if (slotOccupied(sp)) {
			if (! strcmp((char *)sp->barcode, barcode))
				return 1;
		}
	}
	return 0;
}

#define MALLOC_SZ 1024

/* Return zero - failed, non-zero - success */
static int load_map(char *msg)
{
	struct s_info *sp = NULL;
	char *barcode;
	int slt;
	int i;
	int str_len;

	MHVTL_DBG(2, "Loading %s into MAP", msg);

	if (cap_closed) {
		send_msg("MAP not opened", LIBRARY_Q + 1);
		return 0;
	}

	str_len = strlen(msg);
	barcode = NULL;
	for (i = 0; i < str_len; i++)
		if (isalnum(msg[i])) {
			barcode = &msg[i];
			break;
		}

	/* No barcode - reject load */
	if (! barcode) {
		send_msg("Bad barcode", LIBRARY_Q + 1);
		return 0;
	}

	if (already_in_slot(barcode)) {
		send_msg("barcode already in library", LIBRARY_Q + 1);
		return 0;
	}

	if (strlen(barcode) > 10) {
		send_msg("barcode length too long", LIBRARY_Q + 1);
		return 0;
	}

	for (slt = 0; slt < num_map; slt++) {
		sp = &map_info[slt];
		if (slotOccupied(sp))
			continue;
		snprintf((char *)sp->barcode, 10, "%-10s", barcode);
		sp->barcode[10] = '\0';
		/* 1 = data, 2 = Clean */
		sp->cart_type = cart_type(barcode);
		sp->status = STATUS_InEnab | STATUS_ExEnab |
					STATUS_Access | STATUS_ImpExp |
					STATUS_Full;
		/* Media placed by operator */
		setImpExpStatus(sp, OPERATOR);
		sp->slot_location = slt + START_MAP - 1;
		sp->internal_status = 0;
		send_msg("OK", LIBRARY_Q + 1);
		return 1;
	}
	send_msg("MAP Full", LIBRARY_Q + 1);
	return 0;
}

static void open_map(void)
{
	MHVTL_DBG(1, "Called");

	cap_closed = CAP_OPEN;
	send_msg("OK", LIBRARY_Q + 1);
}

static void close_map(void)
{
	MHVTL_DBG(1, "Called");

	cap_closed = CAP_CLOSED;
	send_msg("OK", LIBRARY_Q + 1);
}

/*
 * Respond to messageQ 'empty map' by clearing 'ocuplied' status in map slots.
 * Return 0 on failure, non-zero - success.
 */
static int empty_map(void)
{
	struct s_info *sp;
	int	a;

	if (cap_closed) {
		MHVTL_DBG(1, "MAP slot empty failed - CAP Not open");
		send_msg("Can't empty map while MAP is closed", LIBRARY_Q + 1);
		return 0;
	}

	for (a = START_MAP; a < START_MAP + num_map; a++) {
		sp = slot2struct(a);
		if (slotOccupied(sp)) {
			setSlotEmpty(sp);
			MHVTL_DBG(2, "MAP slot %d emptied", a - START_MAP);
		}
	}
	send_msg("OK", LIBRARY_Q + 1);
	return 1;
}

/*
 * Return 1, exit program
 */
static int processMessageQ(char *mtext)
{

	MHVTL_DBG(3, "Q msg : %s", mtext);

	if (! strncmp(mtext, "debug", 5)) {
		if (debug) {
			debug--;
		} else {
			debug++;
			verbose = 2;
		}
	}
	if (! strncmp(mtext, "empty map", 9))
		empty_map();
	if (! strncmp(mtext, "exit", 4))
		return 1;
	if (! strncmp(mtext, "open map", 8))
		open_map();
	if (! strncmp(mtext, "close map", 9))
		close_map();
	if (! strncmp(mtext, "list map", 8))
		list_map();
	if (! strncmp(mtext, "load map ", 9))
		load_map(&mtext[9]);
	if (! strncmp(mtext, "offline", 7))
		libraryOnline = 0;
	if (! strncmp(mtext, "online", 6))
		libraryOnline = 1;
	if (! strncmp(mtext, "TapeAlert", 9)) {
		uint64_t flg = 0L;
		sscanf(mtext, "TapeAlert %" PRIx64, &flg);
		setTapeAlert(&TapeAlert, flg);
	}
	if (! strncmp(mtext, "verbose", 7)) {
		if (verbose)
			verbose--;
		else
			verbose = 3;
		syslog(LOG_DAEMON|LOG_NOTICE, "Verbose: %s at level %d",
				 verbose ? "enabled" : "disabled", verbose);
	}

return 0;
}

int init_queue(void)
{
	int	queue_id;

	/* Attempt to create or open message queue */
	if ( (queue_id = msgget(QKEY, IPC_CREAT | QPERM)) == -1)
		syslog(LOG_DAEMON|LOG_ERR, "%s %m", "msgget failed");

return (queue_id);
}

static void init_mode_pages(struct mode *m)
{

	struct mode *mp;

	// Disconnect-Reconnect: SPC-3 7.4.8
	mp = alloc_mode_page(2, m, 16);
	if (mp) {
		mp->pcodePointer[2] = 50;	// Buffer full ratio
		mp->pcodePointer[3] = 50;	// Buffer empty ratio
	}

	// Control: SPC-3 7.4.6
	mp = alloc_mode_page(0x0a, m, 12);

	// Power condition: SPC-3 7.4.12
	mp = alloc_mode_page(0x1a, m, 12);

	// Informational Exception Control: SPC-3 7.4.11 (TapeAlert)
	mp = alloc_mode_page(0x1c, m, 12);
	if (mp)
		mp->pcodePointer[2] = 0x08;

	// Device Capabilities mode page: SMC-3 7.3.2
	mp = alloc_mode_page(0x1f, m, 20);
	if (mp) {
		mp->pcodePointer[2] = 0x0f;
		mp->pcodePointer[3] = 0x07;
		mp->pcodePointer[4] = 0x0f;
		mp->pcodePointer[5] = 0x0f;
		mp->pcodePointer[6] = 0x0f;
		mp->pcodePointer[7] = 0x0f;
		/* [8-11] -> reserved */
		mp->pcodePointer[12] = 0x0f;
		mp->pcodePointer[13] = 0x0f;
		mp->pcodePointer[14] = 0x0f;
		mp->pcodePointer[15] = 0x0f;
		/* [16-19] -> reserved */
	}

	// Element Address Assignment mode page: SMC-3 7.3.3
	mp = alloc_mode_page(0x1d, m, 20);
	if (mp) {
		uint8_t *p = mp->pcodePointer;

		put_unaligned_be16(START_PICKER, &p[2]); // First transport.
		put_unaligned_be16(num_picker, &p[4]); // No. transport elem.
		put_unaligned_be16(START_STORAGE, &p[6]); // First storage slot
		put_unaligned_be16(num_storage, &p[8]);	// No. of storage slots
		put_unaligned_be16(START_MAP, &p[10]); // First i/e address
		put_unaligned_be16(num_map, &p[12]); // No. of i/e slots
		put_unaligned_be16(START_DRIVE, &p[14]); // First Drives
		put_unaligned_be16(num_drives, &p[16]); // No. of dives
	}

	// Transport Geometry Parameters mode page: SMC-3 7.3.4
	mp = alloc_mode_page(0x1e, m, 4);
}

/*
 * Allocate enough storage for (size) drives & init to zero
 * - Returns pointer or NULL on error
 */
static struct d_info *init_d_struct(int size)
{
	struct d_info d_info;
	struct d_info *dp;

	dp = (struct d_info *)malloc(size * sizeof(d_info));
	if (dp)
		memset(dp, 0, sizeof(size * sizeof(d_info)));
	else
		syslog(LOG_DAEMON|LOG_ERR, "d_struct() Malloc failed: %m");
return dp;
}

/*
 * Allocate enough storage for (size) elements & init to zero
 * - Returns pointer or NULL on error
 */
static struct s_info *init_s_struct(int size)
{
	struct s_info s_info;
	struct s_info *sp;

	sp = (struct s_info *)malloc(size * sizeof(s_info));
	if (sp)
		memset(sp, 0, sizeof(size * sizeof(s_info)));
	else
		syslog(LOG_DAEMON|LOG_ERR, "s_struct() Malloc failed: %m");

return sp;
}

/* Open device config file and update device information
 */
static void update_drive_details(struct d_info *drv, int drive_count)
{
	char *config=MHVTL_CONFIG_PATH"/device.conf";
	FILE *conf;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	int indx;
	int found;
	struct d_info *dp = NULL;

	conf = fopen(config , "r");
	if (!conf) {
		MHVTL_DBG(1, "Can not open config file %s : %m", config);
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

	found = 0;
	/* While read in a line */
	while (fgets(b, MALLOC_SZ, conf) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		MHVTL_DBG(3, "strlen: %ld", (long)strlen(b));
		if (sscanf(b, "Drive: %d", &indx)) {
			MHVTL_DBG(2, "Found Drive %d", indx);
			if (indx > 0) {
				indx--;
				dp = &drv[indx];
			}

			if (indx > drive_count)
				goto done;

		}
		if (dp) {
			if (sscanf(b, " Unit serial number: %s", s) > 0)
				strncpy(dp->inq_product_sno, s, 10);
			if (sscanf(b, " Product identification: %s", s) > 0)
				strncpy(dp->inq_product_id, s, 16);
			if (sscanf(b, " Product revision level: %s", s) > 0)
				strncpy(dp->inq_product_rev, s, 4);
			if (sscanf(b, " Vendor identification: %s", s) > 0)
				strncpy(dp->inq_vendor_id, s, 8);
		}
		if (strlen(b) == 1)	/* Blank line => Reset device pointer */
			dp = NULL;
	}

done:
	free(b);
	free(s);
	fclose(conf);
}

/*
 * Read config file and populate d_info struct with library's drives
 *
 * One very long and serial function...
 */
static void init_slot_info(void)
{
	char *conf=MHVTL_CONFIG_PATH"/library_contents";
	FILE *ctrl;
	struct d_info *dp = NULL;
	struct s_info *sp = NULL;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	char *barcode;
	int slt;
	int x;

	ctrl = fopen(conf , "r");
	if (!ctrl) {
		MHVTL_DBG(1, "Can not open config file %s : %m", conf);
		exit(1);
	}

	// Grab a couple of generic MALLOC_SZ buffers..
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

	/* First time thru the config file, determine the number of slots
	 * so we know how much memory to alloc */
	num_drives = 0;
	num_storage = 0;
	num_map = 0;
	num_picker = 0;
	/* While read in a line */
	while (fgets(b, MALLOC_SZ, ctrl) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		if (sscanf(b, "Drive %d", &slt) > 0)
			num_drives++;
		if (sscanf(b, "Slot %d", &slt) > 0)
			num_storage++;
		if (sscanf(b, "MAP %d", &slt) > 0)
			num_map++;
		if (sscanf(b, "Picker %d", &slt) > 0)
			num_picker++;
	}

	MHVTL_DBG(1, "%d Drives, %d Storage slots", num_drives, num_storage);

	/* Allocate enough memory for drives */
	drive_info = init_d_struct(num_drives + 1);
	if (drive_info) {
		for (x = 0; x < num_drives; x++) {
			dp = &drive_info[x];
			dp->slot = init_s_struct(1);
			if (!dp->slot)
				exit(1);
		}
	} else
		exit(1);

	/* Allocate enough memory for storage slots */
	storage_info = init_s_struct(num_storage + 1);
	if (!storage_info)
		exit(1);

	/* Allocate enough memory for MAP slots */
	map_info = init_s_struct(num_map + 1);
	if (!map_info)
		exit(1);

	/* Allocate enough memory for picker slots */
	picker_info = init_s_struct(num_picker + 1);
	if (!picker_info)
		exit(1);

	/* Rewind and parse config file again... */
	rewind(ctrl);
	barcode = s;
	while (fgets(b, MALLOC_SZ, ctrl) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		barcode[0] = '\0';

		x = sscanf(b, "Drive %d: %s", &slt, s);
		if (x && slt > num_drives) {
			MHVTL_DBG(1, "Too many drives");
			continue;
		}
		dp = &drive_info[slt - 1];
		switch (x) {
		case 2:
			/* Pull serial number out and fall thru to case 1*/
			strncpy(dp->inq_product_sno, s, 10);
			MHVTL_DBG(2, "Drive s/no: %s", s);
		case 1:
			dp->slot->slot_location = slt + START_DRIVE - 1;
			dp->slot->status = STATUS_Access;
			dp->slot->cart_type = 0;
			dp->slot->internal_status = 0;
			break;
		}

		x = sscanf(b, "MAP %d: %s", &slt, barcode);
		if (x && slt > num_map) {
			MHVTL_DBG(1, "Too many MAPs");
			continue;
		}
		sp = &map_info[slt - 1];
		switch (x) {
		case 1:
			sp->slot_location = slt + START_MAP - 1;
			sp->status = STATUS_InEnab | STATUS_ExEnab |
					STATUS_Access | STATUS_ImpExp;
			sp->cart_type = 0;
			sp->internal_status = 0;
			break;
		case 2:
			MHVTL_DBG(2, "Barcode %s in MAP %d", barcode, slt);
			snprintf((char *)sp->barcode, 10, "%-10s", barcode);
			sp->barcode[10] = '\0';
			/* 1 = data, 2 = Clean */
			sp->cart_type = cart_type(barcode);
			sp->status = STATUS_InEnab | STATUS_ExEnab |
					STATUS_Access | STATUS_ImpExp |
					STATUS_Full;
			sp->slot_location = slt + START_MAP - 1;
			/* look for special media that should be reported
			   as not having a barcode */
			if (!strncmp((char *)sp->barcode, "NOBAR", 5))
				sp->internal_status = INSTATUS_NO_BARCODE;
			else
				sp->internal_status = 0;
			break;
		}

		x = sscanf(b, "Picker %d: %s", &slt, barcode);
		if (x && slt > num_picker) {
			MHVTL_DBG(1, "Too many pickers");
			continue;
		}
		sp = &picker_info[slt - 1];
		switch (x) {
		case 1:
			sp->slot_location = slt + START_PICKER - 1;
			sp->cart_type = 0;
			sp->status = 0;
			sp->internal_status = 0;
			break;
		case 2:
			MHVTL_DBG(2, "Barcode %s in Picker %d", barcode, slt);
			snprintf((char *)sp->barcode, 10, "%-10s", barcode);
			sp->barcode[10] = '\0';
			/* 1 = data, 2 = Clean */
			sp->cart_type = cart_type(barcode);
			sp->slot_location = slt + START_PICKER - 1;
			sp->status = STATUS_Full;
			/* look for special media that should be reported
			 * as not having a barcode */
			if (!strncmp((char *)sp->barcode, "NOBAR", 5))
				sp->internal_status = INSTATUS_NO_BARCODE;
			else
				sp->internal_status = 0;
		}

		x = sscanf(b, "Slot %d: %s", &slt, barcode);
		if (x && (slt > num_storage)) {
			MHVTL_DBG(1, "Storage slot %d out of range", slt);
			continue;
		}
		sp = &storage_info[slt - 1];
		switch (x) {
		case 1:
			sp->slot_location = slt + START_STORAGE - 1;
			sp->status = STATUS_Access;
			sp->cart_type = 0x08;
			sp->internal_status = 0;
			break;
		case 2:
			MHVTL_DBG(2, "Barcode %s in slot %d", barcode, slt);
			snprintf((char *)sp->barcode, 10, "%-10s", barcode);
			sp->barcode[10] = '\0';
			sp->slot_location = slt + START_STORAGE - 1;
			/* 1 = data, 2 = Clean */
			sp->cart_type = cart_type(barcode);
			/* Slot full */
			sp->status = STATUS_Access | STATUS_Full;
			/* look for special media that should be reported
			 * as not having a barcode */
			if (!strncmp((char *)sp->barcode, "NOBAR", 5))
				sp->internal_status = INSTATUS_NO_BARCODE;
			else
				sp->internal_status = 0;
			break;
		}
	}
	fclose(ctrl);
	free(b);
	free(s);

	/* Now update the details of each drive
	 * Details contained in MHVTL_CONFIG_PATH/device.conf
	 * Data keyed by device s/no
	 */
	update_drive_details(&drive_info[0], num_drives);
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
}

static void update_vpd_b1(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xb1)];

	memcpy(vpd_pg->data, p, vpd_pg->sz);
}

static void update_vpd_b2(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xb2)];

	memcpy(vpd_pg->data, p, vpd_pg->sz);
}

static void update_vpd_c0(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xc0)];

	memcpy(&vpd_pg->data[20], p, strlen(p));
}

static void update_vpd_c1(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xc1)];

	memcpy(vpd_pg->data, p, vpd_pg->sz);
}

#define VPD_83_SZ 52
#define VPD_B0_SZ 4
#define VPD_B1_SZ SCSI_SN_LEN
#define VPD_B2_SZ 8
#define VPD_C0_SZ 0x28

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
	int indx, n = 0;
	struct vtl_ctl tmpctl;
	int found = 0;

	conf = fopen(config , "r");
	if (!conf) {
		MHVTL_DBG(1, "Can not open config file %s : %m", config);
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
	while (fgets(b, MALLOC_SZ, conf) != NULL) {
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
				found = 1;
				memcpy(ctl, &tmpctl, sizeof(tmpctl));
			}
		}
		if (indx == minor) {
			unsigned int c, d, e, f, g, h, j, k;
			int i;

			if (sscanf(b, " Unit serial number: %s", s))
				sprintf(lu->lu_serial_no, "%-10s", s);
			if (sscanf(b, " Product identification: %s", s))
				sprintf(lu->product_id, "%-16s", s);
			if (sscanf(b, " Product revision level: %s", s))
				sprintf(lu->product_rev, "%-4s", s);
			if (sscanf(b, " Vendor identification: %s", s))
				sprintf(lu->vendor_id, "%-8s", s);
			if (sscanf(b, " Density : %s", s)) {
				lu->supported_density[n] =
					(uint8_t)strtol(s, NULL, 16);
				MHVTL_DBG(2, "Supported density: 0x%x (%d)",
						lu->supported_density[n],
						lu->supported_density[n]);
				n++;
			}
			i = sscanf(b,
				" NAA: %x:%x:%x:%x:%x:%x:%x:%x",
					&c, &d, &e, &f, &g, &h, &j, &k);
			if (i == 8) {
				if (lu->naa)
					free(lu->naa);
				lu->naa = malloc(24);
				if (lu->naa)
					sprintf((char *)lu->naa,
				"%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
					c, d, e, f, g, h, j, k);
				MHVTL_DBG(2, "Setting NAA: to %s", lu->naa);
			} else if (i > 0) {
				MHVTL_DBG(1, "NAA: Incorrect no params: %s", b);
			}
		}
	}
	fclose(conf);
	free(b);
	free(s);

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
				(int)__LINE__);

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
		syslog(LOG_DAEMON|LOG_WARNING,
			"%s: could not malloc(%d) line %d",
				__func__, VPD_B1_SZ, __LINE__);

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

/*
 * main()
 *
 * e'nuf sed
 */
int main(int argc, char *argv[])
{
	int cdev;
	int ret;
	int q_priority = 0;
	int exit_status = 0;
	long pollInterval = 0L;
	uint8_t *buf;

	struct d_info *dp;
	struct vtl_header vtl_cmd;
	struct vtl_ctl ctl;
	char s[100];
	int a;

	pid_t pid, sid, child_cleanup;

	char *progname = argv[0];
	char *name = "mhvtl";
	uint8_t	minor = 0;
	struct passwd *pw;

	/* Message Q */
	int	mlen, r_qid;
	struct q_entry r_entry;

	while (argc > 0) {
		if (argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				debug++;
				verbose = 9;	// If debug, make verbose...
				break;
			case 'v':
				verbose++;
				break;
			case 'q':
				if (argc > 1)
					q_priority = atoi(argv[1]);
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

	if (q_priority != 0) {
		usage(progname);
		printf("    queue prority must be 0\n");
		exit(1);
	} else {
		q_priority = LIBRARY_Q;
	}

	openlog(progname, LOG_PID, LOG_DAEMON|LOG_WARNING);
	MHVTL_DBG(1, "%s: version %s", progname, MHVTL_VERSION);
	if (verbose) {
		printf("%s: version %s\n", progname, MHVTL_VERSION);
		syslog(LOG_DAEMON|LOG_INFO, "verbose: %d\n", verbose);
	}

	/* Clear Sense arr */
	memset(sense, 0, sizeof(sense));
	reset = 1;

	init_slot_info();
	init_mode_pages(sm);
	initTapeAlert(&TapeAlert);
	/* One of these days, we will support multiple libraries */
	if (!init_lu(&lunit, q_priority - LIBRARY_Q, &ctl)) {
		printf("Can not find entry for '%d' in config file\n", minor);
		exit(1);
	}

	child_cleanup = add_lu((q_priority == LIBRARY_Q) ? 0 : q_priority, &ctl);
	if (! child_cleanup) {
		printf("Could not create logical unit\n");
		exit(1);
	}

	pw = getpwnam(USR);	/* Find UID for user 'vtl' */
	if (!pw) {
		printf("Unable to find user: %s\n", USR);
		exit(1);
	}

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

	if (check_for_running_daemons(minor)) {
		syslog(LOG_DAEMON|LOG_INFO, "%s: version %s, found another running daemon... exiting\n", progname, MHVTL_VERSION);
		exit(2);
	}

	/* Clear out message Q by reading anthing there.. */
	mlen = msgrcv(r_qid, &r_entry, MAXOBN, q_priority, IPC_NOWAIT);
	while (mlen > 0) {
		r_entry.mtext[mlen] = '\0';
		MHVTL_DBG(2, "Found \"%s\" still in message Q", r_entry.mtext);
		mlen = msgrcv(r_qid, &r_entry, MAXOBN, q_priority, IPC_NOWAIT);
	}

	if ((cdev = chrdev_open(name, minor)) == -1) {
		syslog(LOG_DAEMON|LOG_ERR,
				"Could not open /dev/%s%d: %m", name, minor);
		printf("Could not open /dev/%s%d: %m", name, minor);
		fflush(NULL);
		exit(1);
	}

	buf = (uint8_t *)malloc(bufsize);
	if (NULL == buf) {
		perror("Problems allocating memory");
		exit(1);
	}

	/* Send a message to each tape drive so they know the
	 * controlling library's message Q id
	 */
	for (a = 0; a < num_drives; a++) {
		send_msg("Register", a + 1);

		if (debug) {
			dp = &drive_info[a];

			MHVTL_DBG(3, "\nDrive %d", a);

			strncpy(s, dp->inq_vendor_id, 8);
			s[8] = '\0';
			MHVTL_DBG(3, "Vendor ID     : \"%s\"", s);

			strncpy(s, dp->inq_product_id, 16);
			s[16] = '\0';
			MHVTL_DBG(3, "Product ID    : \"%s\"", s);

			strncpy(s, dp->inq_product_rev, 4);
			s[4] = '\0';
			MHVTL_DBG(3, "Revision Level: \"%s\"", s);

			strncpy(s, dp->inq_product_sno, 10);
			s[10] = '\0';
			MHVTL_DBG(3, "Product S/No  : \"%s\"", s);

			MHVTL_DBG(3, "Drive location: %d",
						dp->slot->slot_location);
			MHVTL_DBG(3, "Drive occupied: %s",
				(dp->slot->status & STATUS_Full) ? "No" : "Yes");
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
		dvcid_len = 10;
		/* dvcid area only contains a serial number */
		dvcid_serial_only = 1;
	} else {
		/* size of dvcid area in RES descriptor */
		dvcid_len = 34;
		/* dvcid area contains vendor, product, serial */
		dvcid_serial_only = 0;
	}

	for (;;) {
		/* Check for any messages */
		mlen = msgrcv(r_qid, &r_entry, MAXOBN, q_priority, IPC_NOWAIT);
		if (mlen > 0) {
			r_entry.mtext[mlen] = '\0';
			exit_status = processMessageQ(r_entry.mtext);
		} else if (mlen < 0) {
			r_qid = init_queue();
			if (r_qid == -1)
				syslog(LOG_DAEMON|LOG_ERR,
					"Can not open message queue: %m");
		}
		if (exit_status)	// Process a 'exit' messageQ
			goto exit;

		ret = ioctl(cdev, VTL_POLL_AND_GET_HEADER, &vtl_cmd);
		if (ret < 0) {
			syslog(LOG_DAEMON|LOG_WARNING, "ret: %d : %m", ret);
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
				syslog(LOG_DAEMON|LOG_NOTICE,
					"ioctl(0x%x) returned %d\n",
						VTL_POLL_AND_GET_HEADER, ret);
				sleep(1);
				break;
			}
		}
	}
exit:
	ioctl(cdev, VTL_REMOVE_LU, &ctl);
	close(cdev);
	free(buf);
	free(drive_info);

	exit(0);
}

