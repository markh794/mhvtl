/*
 * This daemon is the SCSI SMC target (Medium Changer) portion of the
 * vtl package.
 *
 * The vtl package consists of:
 *   a kernel module (vlt.ko) - Currently on 2.6.x Linux kernel support.
 *   SCSI target daemons for both SMC and SSC devices.
 *
 * Copyright (C) 2005 - 2008 Mark Harvey markh794 at gmail dot com
 *                                mark_harvey at symantec dot com
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
 * 	Since ability to define device serial number, increased ver from
 * 	0.12 to 0.14
 *
 * v0.12 -> Forked into 'stable' (0.12) and 'devel' (0.13).
 *          My current thinking : This is a dead end anyway.
 *          An iSCSI target done in user-space is now my perferred solution.
 *          This means I don't have to do any kernel level drivers
 *          and leaverage the hosts native iSCSI initiator.
 */

static const char *Version = "$Id: vtllibrary.c 2008-02-14 19:35:01 markh Exp $";

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <syslog.h>
#include <ctype.h>
#include <inttypes.h>
#include "scsi.h"
#include "q.h"
#include "vx.h"
#include "vxshared.h"

/*
 * Define DEBUG to 0 and recompile to remove most debug messages.
 * or DEFINE TO 1 to make the -d (debug operation) mode more chatty
 */

#define DEBUG 1

#if DEBUG

#define DEB(a) a
#define DEBC(a) if (debug) { a ; }

#else

#define DEB(a)
#define DEBC(a)

#endif

/*
 * The following should be dynamic (read from config file)
 *
 **** Warning Will Robertson (or is that Robinson ??) ****:
 *   START_DRIVE HAS TO start at slot 1
 *	The Order of Drives with lowest start, followed by Picker, followed
 *	by MAP, finally Storage slots is IMPORTANT. - You have been warned.
 *   Some of the logic in this source depends on it.
 */
#define START_DRIVE	0x0001
static int NUM_DRIVES = 4;

#define START_PICKER	0x0100
static int NUM_PICKER = 1;

#define START_MAP	0x0200
static int NUM_MAP = 0x0020;

#define START_STORAGE	0x0400
static int NUM_STORAGE = 0x0800;

// Element type codes
#define ANY			0
#define MEDIUM_TRANSPORT	1
#define STORAGE_ELEMENT		2
#define MAP_ELEMENT		3
#define DATA_TRANSFER		4

static int bufsize = 0;
int verbose = 0;
int debug = 0;
static int libraryOnline = 1;	/* Default to Off-line */
int reset = 1;		/* Poweron reset */
static uint8_t request_sense = 0; /* Non-zero if Sense-data is valid */

uint8_t sense[SENSE_BUF_SIZE]; /* Request sense buffer */

// If I leave this as 'static struct', the I get a gcc warning
// " warning: useless storage class specifier in empty declaration"
// static struct s_info { /* Slot Info */
struct s_info { /* Slot Info */
	uint8_t cart_type; // 0 = Unknown, 1 = Data medium, 2 = Cleaning
	uint8_t barcode[11];
	uint32_t slot_location;
	uint32_t last_location;
	uint8_t	status;	// Used for MAP status.
	uint8_t	asc;	// Additional Sense Code
	uint8_t	ascq;	// Additional Sense Code Qualifier
};

// If I leave this as 'static struct', the I get a gcc warning
// " warning: useless storage class specifier in empty declaration"
// static struct d_info {	/* Drive Info */
struct d_info {	/* Drive Info */
	char inq_vendor_id[8];
	char inq_product_id[16];
	char inq_product_rev[4];
	char inq_product_sno[10];
	char online;		// Physical status of drive
	int SCSI_BUS;
	int SCSI_ID;
	int SCSI_LUN;
	char tapeLoaded;	// Tape is 'loaded' by drive
	struct s_info *slot;
};

static struct d_info *drive_info;
static struct s_info *storage_info;
static struct s_info *map_info;
static struct s_info *picker_info;

/* Log pages */
static struct Temperature_page Temperature_pg = {
	{ TEMPERATURE_PAGE, 0x00, 0x06, },
	{ 0x00, 0x00, 0x60, 0x02, }, 0x00, 	// Temperature
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

static void usage(char *progname) {
	printf("Usage: %s [-d] [-v]\n", progname);
	printf("      Where file == data file\n");
	printf("             'd' == debug -> Don't run as daemon\n");
	printf("             'v' == verbose -> Extra info logged via syslog\n");
}

#ifndef Solaris
 int ioctl(int, int, void *);
#endif


static void dump_element_desc(uint8_t *p, int voltag, int len)
{
	int i;

	printf("  Element Address             : %d\n", (p[0] << 8) | p[1]);
	printf("  Status                      : 0x%02x\n", p[2]);
	printf("  Medium type                 : %d\n", p[9] & 0x7);
	if (p[9] & 0x80)
		printf("  Source Address              : %d\n", (p[10] << 8) | p[11]);
	i = 12;
	if (voltag) {
		i += 36;
		printf(" Need to print Voltag info\n");
	}
	printf(" Identification Descriptor\n");
	printf("  Code Set                     : 0x%02x\n", p[i] & 0xf);
	printf("  Identifier type              : 0x%02x\n", p[i + 1] & 0xf);
	printf("  Identifier length            : %d\n", p[i + 3]);
	printf("  ASCII data                   : %s\n", &p[i + 4]);
	printf("  ASCII data                   : %s\n", &p[i + 12]);
	printf("  ASCII data                   : %s\n", &p[i + 28]);
}

static void decode_element_status(uint8_t *p)
{
	int voltag;
	int len;

	printf("Element Status Data\n");
	printf("  First element reported       : %d\n", (p[0] << 8) | p[1]);
	printf("  Number of elements available : %d\n", (p[2] << 8) | p[3]);
	printf("  Byte count of report         : %d\n",
				(p[5] << 16) | (p[6] << 8) | p[7]);
	printf("Element Status Page\n");
	printf("  Element Type code            : %d\n", p[8]);
	printf("  Primary Vol Tag              : %s\n", (p[9] & 0x80) ? "Yes" : "No");
	voltag = (p[9] & 0x80) ? 1 : 0;
	printf("  Alt Vol Tag                  : %s\n", (p[9] & 0x40) ? "Yes" : "No");
	printf("  Element descriptor length    : %d\n", (p[10] << 8) | p[11]);
	printf("  Byte count of descriptor data: %d\n",
				(p[13] << 16) | (p[14] << 8) | p[15]);
	len = (p[13] << 16) | (p[14] << 8) | p[15];

	printf("Element Descriptor(s)\n");
	dump_element_desc(&p[16], voltag, len);

	fflush(NULL);
}


/*
 * Simple function to read 'count' bytes from the chardev into 'buf'.
 */
static int retreive_CDB_data(int cdev, uint8_t *buf, uint32_t count) {

	return (read(cdev, buf, bufsize));
}

/*
 * Process the MODE_SELECT command
 */
static int resp_mode_select(int cdev, uint8_t *cmd, uint8_t *buf) {
	int alloc_len;
	DEB( int k; )

	alloc_len = (MODE_SELECT == cmd[0]) ? cmd[4] : ((cmd[7] << 8) | cmd[8]);

	retreive_CDB_data(cdev, buf, alloc_len);

	DEBC(	for (k = 0; k < alloc_len; k++)
			printf("%02x ", (uint32_t)buf[k]);
		printf("\n");
	)

	return 0;
}

/*
 * Takes a slot number and returns a struct pointer to the slot
 */
static struct s_info *slot2struct(int addr) {
	if ((addr >= START_MAP) && (addr <= (START_MAP + NUM_MAP))) {
		addr -= START_MAP;
		DEBC(	printf("slot2struct: MAP %d\n", addr); )
		return &map_info[addr];
	}
	if ((addr >= START_STORAGE) && (addr <= (START_STORAGE + NUM_STORAGE))) {
		addr -= START_STORAGE;
		DEBC(	printf("slot2struct: Storage %d\n", addr); )
		return &storage_info[addr];
	}
	if ((addr >= START_PICKER) && (addr <= (START_PICKER + NUM_PICKER))) {
		addr -= START_PICKER;
		DEBC(	printf("slot2struct: Picker %d\n", addr); )
		return &picker_info[addr];
	}

// Should NEVER get here as we have performed bounds checking b4
	if (verbose)
		syslog(LOG_DAEMON|LOG_ERR, "Arrr... slot2struct returning NULL\n");

	syslog(LOG_DAEMON|LOG_ERR, "%s",  "Fatal: slot2struct() returned NULL");

return NULL;
}

/*
 * Takes a Drive number and returns a struct pointer to the drive
 */
static struct d_info *drive2struct(int addr) {
	addr -= START_DRIVE;
	return &drive_info[addr];
}

/* Returns true if slot has media in it */
static int slotOccupied(struct s_info *s) {
	return(s->status & 0x01);
}

/* Returns true if drive has media in it */
static int driveOccupied(struct d_info *d) {
	return(slotOccupied(d->slot));
}

/*
 * A value of 0 indicates that media movement from the I/O port
 * to the handler is denied; a value of 1 indicates that the movement
 * is permitted.
 */
/*
static void setInEnableStatus(struct s_info *s, int flg) {
	if (flg)	// Set Full bit
		s->status |= 0x20;
	else		// Set Full bit to 0
		s->status &= 0xdf;
}
*/

/*
 * A value of 0 in the Export Enable field indicates that media movement
 * from the handler to the I/O port is denied. A value of 1 indicates that
 * movement is permitted.
 */
/*
static void setExEnableStatus(struct s_info *s, int flg) {
	if (flg)	// Set Full bit
		s->status |= 0x10;
	else		// Set Full bit to 0
		s->status &= 0xef;
}
*/

/*
 * A value of 1 indicates that a cartridge may be moved to/from
 * the drive (but not both).
 */
/*
static void setAccessStatus(struct s_info *s, int flg) {
	if (flg)	// Set Full bit
		s->status |= 0x08;
	else		// Set Full bit to 0
		s->status &= 0xf7;
}
*/

/*
 * Reset to 0 indicates it is in normal state, set to 1 indicates an Exception
 * condition exists. An exception indicates the libary is uncertain of an
 * elements status.
 */
/*
static void setExceptStatus(struct s_info *s, int flg) {
	if (flg)	// Set Full bit
		s->status |= 0x04;
	else		// Set Full bit to 0
		s->status &= 0xfb;
}
*/

/*
 * If set(1) then cartridge placed by operator
 * If clear(0), placed there by handler.
 */
/*
static void setImpExpStatus(struct s_info *s, int flg) {
	if (flg)	// Set Full bit
		s->status |= 0x02;
	else		// Set Full bit to 0
		s->status &= 0xfd;
}
*/

/*
 * Sets the 'Full' bit true/false in the status field
 */
static void setFullStatus(struct s_info *s, int flg) {
	if (flg)	// Set Full bit
		s->status |= 0x01;
	else		// Set Full bit to 0
		s->status &= 0xfe;
}

static void setSlotEmpty(struct s_info *s) {
	setFullStatus(s, 0);
}

static void setDriveEmpty(struct d_info *d) {
	setFullStatus(d->slot, 0);
}

static void setSlotFull(struct s_info *s) {
	setFullStatus(s, 1);
}

static void setDriveFull(struct d_info *d) {
	setFullStatus(d->slot, 1);
}

/*
 * Logically move information from 'src' address to 'dest' address
 */
static void move_cart(struct s_info *src, struct s_info *dest) {

	dest->cart_type = src->cart_type;
	memcpy(dest->barcode, src->barcode, 10);
	dest->last_location = src->slot_location;
	setSlotFull(dest);

	src->cart_type = 0;		/* Src slot no longer occupied */
	memset(src->barcode, 0, 10);	/* Zero out barcode */
	src->last_location = 0;		/* Forget where the old media was */
	setSlotEmpty(src);		/* Clear Full bit */
}

/* Move media in drive 'src_addr' to drive 'dest_addr' */
static int move_drive2drive(int src_addr, int dest_addr, uint8_t *sense_flg) {
	struct d_info *src;
	struct d_info *dest;
	char   cmd[128];
	int    x;

	src  = drive2struct(src_addr);
	dest = drive2struct(dest_addr);

	if ( ! driveOccupied(src) ) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sense_flg);
		return 1;
	}
	if ( driveOccupied(dest) ) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sense_flg);
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
	DEBC(	printf("Sending cmd: \'%s\' to drive %d\n",
					cmd, dest->slot->slot_location);
	)

	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO,
				"Sending cmd: \'%s\' to drive %d\n",
					cmd, dest->slot->slot_location);

	send_msg(cmd, dest->slot->slot_location);

return 0;
}

static int move_drive2slot(int src_addr, int dest_addr, uint8_t *sense_flg) {
	struct d_info *src;
	struct s_info *dest;

	src  = drive2struct(src_addr);
	dest = slot2struct(dest_addr);

	if ( ! driveOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sense_flg);
		return 1;
	}
	if ( slotOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sense_flg);
		return 1;
	}

	// Send 'unload' message to drive b4 the move..
	send_msg("unload", src->slot->slot_location);

	move_cart(src->slot, dest);
	setDriveEmpty(src);

return 0;
}

static int move_slot2drive(int src_addr, int dest_addr, uint8_t *sense_flg) {
	struct s_info *src;
	struct d_info *dest;
	char   cmd[128];
	int    x;

	src  = slot2struct(src_addr);
	dest = drive2struct(dest_addr);

	if ( ! slotOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sense_flg);
		return 1;
	}
	if ( driveOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sense_flg);
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
	syslog(LOG_DAEMON|LOG_INFO, "About to send cmd: \'%s\' to drive %d\n",
					cmd, dest->slot->slot_location);

	send_msg(cmd, dest->slot->slot_location);

return 0;
}

static int move_slot2slot(int src_addr, int dest_addr, uint8_t *sense_flg) {
	struct s_info *src;
	struct s_info *dest;

	src  = slot2struct(src_addr);
	dest = slot2struct(dest_addr);

	if (debug)
		printf("Moving from slot %d to slot %d\n",
				src->slot_location, dest->slot_location);

	if (! slotOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sense_flg);
		return 1;
	}
	if (slotOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sense_flg);
		return 1;
	}

	move_cart(src, dest);

	return 0;
}

/* Return OK if 'addr' is within either a MAP, Drive or Storage slot */
static int valid_slot(int addr) {

	int maxDrive = START_DRIVE + NUM_DRIVES;
	int maxStorage = START_STORAGE + NUM_STORAGE;
	int maxMAP   = START_MAP + NUM_MAP;

	if (((addr >= START_DRIVE)   && (addr <= maxDrive)) ||
	   ((addr >= START_MAP)     && (addr <= maxMAP))   ||
	   ((addr >= START_STORAGE) && (addr <= maxStorage)))
		return 1;
	else
		return 0;
}

/* Move a piece of medium from one slot to another */
static int resp_move_medium(uint8_t *cmd, uint8_t *buf, uint8_t *sense_flg) {
	int transport_addr;
	int src_addr;
	int dest_addr;
	int maxDrive = START_DRIVE + NUM_DRIVES;
	int retVal = 0;	// Return a success status
	uint16_t *sp;

	sp = (uint16_t *)&cmd[2];
	transport_addr = ntohs(*sp);
	sp = (uint16_t *)&cmd[4];
	src_addr  = ntohs(*sp);
	sp = (uint16_t *)&cmd[6];
	dest_addr = ntohs(*sp);

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
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sense_flg);
		return -1;
	}
	if (cmd[11] == 0xc0) {	// Invalid combo of Extend/retract I/O port
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sense_flg);
		return -1;
	}
	if (cmd[11]) // Must be an Extend/Retract I/O port cmd.. NO-OP
		return 0;

	if (transport_addr == 0)
		transport_addr = START_PICKER;
	if (transport_addr > (START_PICKER + NUM_PICKER)) {
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sense_flg);
		retVal = -1;
	}
	if (! valid_slot(src_addr)) {
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sense_flg);
		retVal = -1;
	}
	if (! valid_slot(dest_addr)) {
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sense_flg);
		retVal = -1;
	}

	if (retVal == 0) {

	/* WWR - The following depends on Drive 1 being in the lowest slot */
		if ((src_addr < maxDrive) && (dest_addr < maxDrive)) {
			/* Move between drives */
			if (move_drive2drive(src_addr, dest_addr, sense_flg))
				retVal = -1;
		} else if (src_addr < maxDrive) {
			if (move_drive2slot(src_addr, dest_addr, sense_flg))
				retVal = -1;
		} else if (dest_addr < maxDrive) {
			if (move_slot2drive(src_addr, dest_addr, sense_flg))
				retVal = -1;
		} else {   // Move between (non-drive) slots
			if (move_slot2slot(src_addr, dest_addr, sense_flg))
				retVal = -1;
		}
	}

return(retVal);
}

/*
 * Fill in skeleton of element descriptor data
 *
 * Returns number of bytes in element data.
 */
static int skel_element_descriptor(uint8_t *p, struct s_info *s, int voltag) {
	int j = 0;
	uint16_t *sp;

	if (debug)
		printf("Slot location: %d\n", s->slot_location);
	sp = (uint16_t *)&p[j];
	*sp = htons(s->slot_location);
	j += 2;
	p[j++] = s->status;
	p[j++] = 0;	/* Reserved */
	p[j++] = s->asc;  /* Additional Sense Code */
	p[j++] = s->ascq; /* Additional Sense Code Qualifer */

	j++;		/* Reserved */
	j++;		/* Reserved */
	j++;		/* Reserved */

	/* bit 8 set if Source Storage Element is valid | s->occupied */
	p[j] = (s->last_location > 0) ? 0x80 : 0;
	/* 0 - empty, 1 - data, 2 cleaning tape */
	p[j++] |= (s->cart_type & 0x0f);

	/* Source Storage Element Address */
	sp = (uint16_t *)&p[j];
	*sp = htons(s->last_location);
	j += 2;

	if (voltag) {
		DEBC( printf("voltag set\n"); )

		if (s->status & 0x01) /* Barcode with trailing space(s) */
			snprintf((char *)&p[j], 32, "%-32s", s->barcode);
		else
			memset(&p[j], 0, 32);

		j += 32;	/* Account for barcode */
		j += 8;		/* Reserved */
	} else {
		j += 4;		/* Reserved */
		DEBC( printf("voltag cleared => no barcode\n"); )
	}

return j;
}

/*
 * Starts at slot 's' and works thru 'slot_count' number of slots
 * filling in details of contents of slot 's'
 *
 * Returns number of bytes used to fill in all details.
 */
static int fill_element_detail(uint8_t *p, struct s_info *slot, int slot_count, int voltag) {
	int k;
	int j;

	/**** For each Storage Element ****/
	for (k = 0, j = 0; j < slot_count; j++, slot++) {
		DEBC( printf("Slot: %d, k = %d\n", slot->slot_location, k); )
		k += skel_element_descriptor( (uint8_t *)&p[k], slot, voltag);
	}
	DEBC( printf("%s() returning %d bytes\n", __FUNCTION__, k); ) ;

return (k);
}

/*
 * Fill in details of Drive slot at *d into struct *p
 *
 * Return number of bytes in element.
 */
static int fill_data_transfer_element(uint8_t *p, struct d_info *d, uint8_t dvcid, uint8_t voltag ) {
	int j = 0;
	int m;
	char s[128];
	uint16_t *sp;

	sp = (uint16_t *)&p[j];
	*sp = htons(d->slot->slot_location);
	j+=2;
	p[j++] = d->slot->status;
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
	p[j++] = 0;	/* Additional Sense Code */
	p[j++] = 0;	/* Additional Sense Code Qualifer */

//	p[j++] = 0xa0 | d->SCSI_LUN;	/* SCSI ID bit valid */
	p[j++] = 0;	/* SCSI ID bit not-valid */
	p[j++] = d->SCSI_ID;
	j++;		/* Reserved */
	if (d->slot->last_location > 0) {
		/* bit 8 set if Source Storage Element is valid */
		p[j++] = 0x80;

		/* Source Storage Element Address */
		sp = (uint16_t *)&p[j];
		*sp = htons(d->slot->last_location);
		j += 2;
	} else {
		j += 3;
	}

/*	syslog(LOG_DAEMON|LOG_WARNING, "DVCID: %d, VOLTAG: %d, Index: %d\n",
			dvcid, voltag, j);
*/

	// Tidy up messy if/then/else into a 'case'
	m = dvcid + dvcid + voltag;
	switch(m) {
	case 0:
		DEBC( printf("voltag & DVCID both not set\n"); )
		j += 4;
		break;
	case 1:
		DEBC( printf("voltag set, DVCID not set\n"); )
		if (d->slot->status & 0x01) {
			strncpy(s, (char *)d->slot->barcode, 10);
			s[10] = '\0';
			/* Barcode with trailing space(s) */
			snprintf((char *)&p[j], 32, "%-32s", s);
		} else {
			memset(&p[j], 0, 32);
		}
		j += 32;	/* Account for barcode */
		j += 8;		/* Reserved */
		break;
	case 2:
		DEBC( printf("voltag not set, DVCID set\n"); )
		p[j++] = 2;	/* Code set 2 = ASCII */
		p[j++] = 1;	/* Identifier type */
		j++;		/* Reserved */
		p[j++] = 34;	/* Identifier Length */

		strncpy(s, d->inq_vendor_id, 8);
		s[8] = '\0';
		snprintf((char *)&p[j], 8, "%-8s", s);
		DEBC( printf("Vendor ID: \"%-8s\"\n", s); )
		j += 8;

		strncpy(s, d->inq_product_id, 16);
		s[16] = '\0';
		snprintf((char *)&p[j], 16, "%-16s", s);
		DEBC( printf("Product ID: \"%-16s\"\n", s); )
		j += 16;

		strncpy(s, d->inq_product_sno, 10);
		s[10] = '\0';
		snprintf((char *)&p[j], 10, "%-10s", s);
		DEBC( printf("Product S/No: \"%-10s\"\n", s); )
		j += 10;
		break;
	case 3:
		DEBC( printf("voltag set, DVCID set\n"); )
		if (d->slot->status & 0x01) {
			strncpy(s, (char *)d->slot->barcode, 10);
			s[10] = '\0';
			/* Barcode with trailing space(s) */
			snprintf((char *)&p[j], 32, "%-32s", s);
		} else {
			memset(&p[j], 0, 32);
		}
		j += 32;	/* Account for barcode */
		j += 4;		/* Reserved */

		p[j++] = 2;	/* Code set 2 = ASCII */
		p[j++] = 1;	/* Identifier type */
		j++;		/* Reserved */
		p[j++] = 34;	/* Identifier Length */

		strncpy(s, d->inq_vendor_id, 8);
		s[8] = '\0';
		snprintf((char *)&p[j], 8, "%-8s", s);
		DEBC( printf("Vendor ID: \"%-8s\"\n", s); )
		j += 8;

		strncpy(s, d->inq_product_id, 16);
		s[16] = '\0';
		snprintf((char *)&p[j], 16, "%-16s", s);
		DEBC( printf("Product ID: \"%-16s\"\n", s); )
		j += 16;

		strncpy(s, d->inq_product_sno, 10);
		s[10] = '\0';
		snprintf((char *)&p[j], 10, "%-10s", s);
		DEBC( printf("Product S/No: \"%-10s\"\n", s); )
		j += 10;
		break;
	}
/*
	syslog(LOG_DAEMON|LOG_WARNING, "Index: %d\n", j);
*/
	DEBC( printf("%s() returning %d bytes\n", __FUNCTION__, j); )

return j;
}

/*
 * Calculate length of one element
 */
static int determine_element_sz(uint8_t dvcid, uint8_t voltag) {

	if (voltag) /* If voltag bit is set */
		return  (dvcid == 0) ? 52 : 86;
	else
		return  (dvcid == 0) ? 16 : 50;
}

/*
 * Fill in element status page Header (8 bytes)
 */
static int fill_element_status_page(uint8_t *p, uint16_t start,
					uint16_t element_count, uint8_t dvcid,
					uint8_t voltag, uint8_t typeCode)
{
	int	element_sz;
	uint32_t	element_len;
	uint16_t	*sp;
	uint32_t	*sl;

	element_sz = determine_element_sz(dvcid, voltag);

	p[0] = typeCode;		/* Element type Code */

	/* Primary Volume Tag set - Returning Barcode info */
	p[1] = (voltag == 0) ? 0 : 0x80;

	/* Number of bytes per element */
	sp = (uint16_t *)&p[2];
	*sp = htons(element_sz);

	element_len = element_sz *element_count;

	/* Total number of bytes in all element descriptors */
	sl = (uint32_t *)&p[4];
	*sl = htonl(element_len & 0xffffff);
	/* Reserved */
	p[4] = 0;	// Above mask should have already set this to 0...

	DEBC(	printf("Element Status Page Header: "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

	) ;

	if (verbose > 2)
		syslog(LOG_DAEMON|LOG_INFO, "Element Status Page: "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

return(8);	// Always 8 bytes in header
}

/*
 * Build the initial ELEMENT STATUS HEADER
 * 
 */
static int
element_status_hdr(uint8_t *p, uint8_t dvcid, uint8_t voltag, int start, int count) {
	uint32_t	byte_count;
	int	element_sz;
	uint32_t	*lp;
	uint16_t	*sp;

	element_sz = determine_element_sz(dvcid, voltag);

	DEBC(	printf("Building READ ELEMENT STATUS Header struct\n");
		printf(" Starting slot: %d, number of configured slots: %d\n",
					start, count);
	)

	/* Start of ELEMENT STATUS DATA */
	sp = (uint16_t *)&p[0];
	*sp = htons(start);

	sp = (uint16_t *)&p[2];
	*sp = htons(count);

	/* The byte_count should be the length required to return all of
	 * valid data.
	 * The 'allocated length' indicates how much data can be returned.
	 */
	byte_count = 8 + (count * element_sz);
	lp = (uint32_t *)&p[4];
	*lp = htonl(byte_count & 0xffffff);

	DEBC(	printf(" Element Status Data HEADER: "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
		sp = (uint16_t *)&p[0];
		printf(" Decoded:\n");
		printf("  First element Address    : %d\n", ntohs(*sp));
		sp = (uint16_t *)&p[2];
		printf("  Number elements reported : %d\n", ntohs(*sp));
		lp = (uint32_t *)&p[4];
		printf("  Total byte count         : %d\n", ntohl(*lp));
	) ;

	if (verbose > 2)
		syslog(LOG_DAEMON|LOG_INFO, "Element Status Data Header: "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
return 8;	// Header is 8 bytes in size..
}

/*
 * Read Element Status command will pass 'start element address' & type of slot
 *
 * We return the first valid slot number which matches.
 * or zero on no matching slots..
 */
static int find_first_matching_element(uint16_t start, uint8_t typeCode) {

	switch(typeCode) {
	case ANY:	// Don't care what 'type'
		/* Logic here depends on Storage slots being
		 * higher (numerically) than MAP which is higher than
		 * Picker, which is higher than the drive slot number..
		 * See WWR: near top of this file !!
		 */
		// Check we are within a storage slot range.
		if ((start >= START_STORAGE) &&
		   (start <= (START_STORAGE + NUM_STORAGE)))
			return(start);
		// If we are above I/O Range -> return START_STORAGE
		if (start > (START_MAP + NUM_MAP))
			return(START_STORAGE);
		// Must be within the I/O Range..
		if (start >= START_MAP)
			return(start);
		// If we are above the Picker range -> Return I/O Range..
		if (start > (START_PICKER + NUM_PICKER))
			return START_MAP;
		// Must be a valid picker slot..
		if (start >= START_PICKER)
			return (start);
		// If we are starting above the valid drives, return Picker..
		if (start > (START_DRIVE + NUM_DRIVES))
			return(START_PICKER);
		// Must be a valid drive
		if (start >= START_DRIVE)
			return(start);
		break;
	case MEDIUM_TRANSPORT:	// Medium Transport.
		if ((start >= START_PICKER) &&
		   (start <= (START_PICKER + NUM_PICKER)))
			return start;
		if (start < START_PICKER)
			return START_PICKER;
		break;
	case STORAGE_ELEMENT:	// Storage Slots
		if ((start >= START_STORAGE) &&
		   (start <= (START_STORAGE + NUM_STORAGE)))
			return start;
		if (start < START_STORAGE)
			return START_STORAGE;
		break;
	case MAP_ELEMENT:	// Import/Export
		if ((start >= START_MAP) &&
		   (start <= (START_MAP + NUM_MAP)))
			return start;
		if (start < START_MAP)
			return START_MAP;
		break;
	case DATA_TRANSFER:	// Data transfer
		if ((start >= START_DRIVE) &&
		   (start <= (START_DRIVE + NUM_DRIVES)))
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
 * Returns number of bytes used to fill in data
 */
static int medium_transport_descriptor(uint8_t *p, uint16_t start,
				uint16_t count, uint8_t dvcid, uint8_t voltag)
{
	uint16_t	len;
	uint16_t	begin;

	// Find first valid slot.
	begin = find_first_matching_element(start, MEDIUM_TRANSPORT);

	// Create Element Status Page Header.
	len = 
	  fill_element_status_page(p,begin,count,dvcid,voltag,MEDIUM_TRANSPORT);

	begin -= START_PICKER;	// Array starts at [0]
	count = NUM_PICKER - begin;

	// Now loop over each picker slot and fill in details.
	len += fill_element_detail(&p[len], &picker_info[begin], count, voltag);

return len;
}

/*
 * Fill in Element status page header + each Element descriptor
 *
 * Returns number of bytes used to fill in data
 */
static int storage_element_descriptor(uint8_t *p, uint16_t start,
				uint16_t count, uint8_t dvcid, uint8_t voltag)
{
	uint16_t	len;
	uint16_t	begin;

	begin = find_first_matching_element(start, STORAGE_ELEMENT);
	len =
	  fill_element_status_page(p,begin,count,dvcid,voltag,STORAGE_ELEMENT);

	begin -= START_STORAGE;	// Array starts at [0]
	count = NUM_STORAGE - begin;

	// Now loop over each picker slot and fill in details.
	len += fill_element_detail(&p[len],&storage_info[begin],count,voltag);

return len;
}

/*
 * Fill in Element status page header + each Element descriptor
 *
 * Returns number of bytes used to fill in data
 */
static int map_element_descriptor(uint8_t *p, uint16_t start, uint16_t count,
						uint8_t dvcid, uint8_t voltag)
{
	uint16_t	len;
	uint16_t	begin;

	begin = find_first_matching_element(start, MAP_ELEMENT);
	len =
	    fill_element_status_page(p,begin,count,dvcid,voltag,MAP_ELEMENT);

	begin -= START_MAP;	// Array starts at [0]
	count = NUM_MAP - begin;

	// Now loop over each picker slot and fill in details.
	len += fill_element_detail(&p[len], &map_info[begin], count, voltag);

return len;
}

/*
 * Fill in Element status page header + each Element descriptor
 *
 * Returns number of bytes used to fill in data
 */
static int data_transfer_descriptor(uint8_t *p, uint16_t start,
				uint16_t count, uint8_t dvcid, uint8_t voltag)
{
	uint32_t	len;
	uint16_t	drive;

	drive = find_first_matching_element(start, DATA_TRANSFER);
	len =
	    fill_element_status_page(p,drive,count,dvcid,voltag,DATA_TRANSFER);

	drive -= START_DRIVE;	// Array starts at [0]

	DEBC(	printf("Starting at drive: %d, count %d\n", drive + 1, count);
		printf("Element Length: %d for drive: %d\n",
				determine_element_sz(dvcid, voltag),
				drive); )

	if (verbose > 2)
		syslog(LOG_DAEMON|LOG_INFO, "Len: %d\n", len);
	/**** For each Data Transfer Element ****/
	count += drive; // We want 'count' number of drive entries returned
	for (; drive < count; drive++) {
		DEBC(	printf("Processing drive %d, stopping after %d\n",
					drive + 1, count);	) ;
		len += fill_data_transfer_element( &p[len],
					&drive_info[drive], dvcid, voltag);
		if (verbose > 2)
			syslog(LOG_DAEMON|LOG_INFO, "Len: %d\n", len);
	}

	DEBC( printf("%s() returning %d bytes\n", __FUNCTION__, len); )

return len;
}

/*
 * Build READ ELEMENT STATUS data.
 *
 * Returns number of bytes to xfer back to host.
 */
static int resp_read_element_status(uint8_t *cdb, uint8_t *buf,
							uint8_t *sense_flg)
{
	uint16_t *sp;
	uint32_t *sl;
	uint8_t	*p;
	uint8_t	typeCode = cdb[1] & 0x0f;
	uint8_t	voltag = (cdb[1] & 0x10) >> 4;
	uint16_t req_start_elem;
	uint16_t number;
	uint8_t	dvcid = cdb[6] & 0x01;	/* Device ID */
	uint32_t alloc_len;
	uint16_t start;	// First valid slot location
	uint16_t len = 0;

	sp = (uint16_t *)&cdb[2];
	req_start_elem = ntohs(*sp);
	sp = (uint16_t *)&cdb[4];
	number = ntohs(*sp);
	sl = (uint32_t *)&cdb[6];
	alloc_len = 0xffffff & ntohl(*sl);

	DEBC(	printf("Element type (%d) => ", typeCode);
		switch(typeCode) {
		case ANY:
			printf("All Elements\n");
			break;
		case MEDIUM_TRANSPORT:
			printf("Medium Transport\n");
			break;
		case STORAGE_ELEMENT:
			printf("Storage Elements\n");
			break;
		case MAP_ELEMENT:
			printf("Import/Export\n");
			break;
		case DATA_TRANSFER:
			printf("Data Transfer Elements\n");
			break;
		default:
			printf("Invalid type\n");
			break;
		}
		printf("  Starting Element Address: %d\n", req_start_elem);
		printf("  Number of Elements      : %d\n", number);
		printf("  Allocation length       : %d\n", alloc_len);
		printf("  Device ID: %s, voltag: %s\n",
					(dvcid == 0) ? "No" :  "Yes",
					(voltag == 0) ? "No" :  "Yes" );
	)
	if (verbose) {
		switch(typeCode) {
		case ANY: syslog(LOG_DAEMON|LOG_INFO,
			" Element type(%d) => All Elements", typeCode);
			break;
		case MEDIUM_TRANSPORT: syslog(LOG_DAEMON|LOG_INFO,
			" Element type(%d) => Medium Transport", typeCode);
			break;
		case STORAGE_ELEMENT: syslog(LOG_DAEMON|LOG_INFO,
			" Element type(%d) => Storage Elements", typeCode);
			break;
		case MAP_ELEMENT: syslog(LOG_DAEMON|LOG_INFO,
			" Element type(%d) => Import/Export", typeCode);
			break;
		case DATA_TRANSFER: syslog(LOG_DAEMON|LOG_INFO,
			" Element type(%d) => Data Transfer Elements",typeCode);
			break;
		default:
			syslog(LOG_DAEMON|LOG_INFO,
			" Element type(%d) => Invalid type requested",typeCode);
			break;
		}
		syslog(LOG_DAEMON|LOG_INFO,
			"  Starting Element Address: %d\n",req_start_elem);
		syslog(LOG_DAEMON|LOG_INFO,
			"  Number of Elements      : %d\n",number);
		syslog(LOG_DAEMON|LOG_INFO,
			"  Allocation length       : %d\n",alloc_len);
		syslog(LOG_DAEMON|LOG_INFO, "  Device ID: %s, voltag: %s\n",
					(dvcid == 0) ? "No" :  "Yes",
					(voltag == 0) ? "No" :  "Yes" );
	}

	/* Set alloc_len to smallest value */
	if (alloc_len > bufsize)
		alloc_len = bufsize;

	/* Init buffer */
	memset(buf, 0, alloc_len);

	if (cdb[11] != 0x0) {	// Reserved byte..
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,sense_flg);
		return(0);
	}

	// Find first matching slot number which matches the typeCode.
	start = find_first_matching_element(req_start_elem, typeCode);
	if (start == 0) {	// Nothing found..
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,sense_flg);
		return(0);
	}

	// Leave room for 'master' header which is filled in at the end...
	p = &buf[8];

	switch(typeCode) {
	case MEDIUM_TRANSPORT:
		len =
		  medium_transport_descriptor(p, start, number, dvcid, voltag);
		break;
	case STORAGE_ELEMENT:
		len =
		  storage_element_descriptor(p, start, number, dvcid, voltag);
		break;
	case MAP_ELEMENT:
		len =
		  map_element_descriptor(p, start, number, dvcid, voltag);
		break;
	case DATA_TRANSFER:
		len =
		  data_transfer_descriptor(p, start, number, dvcid, voltag);
		break;
	case ANY:
		if (start >= START_STORAGE) {
			len = storage_element_descriptor(p, start, number,
								dvcid, voltag);
//			number = start - START_STORAGE;
		} else if (start >= START_MAP) {
			len = map_element_descriptor(p, start, number,
								dvcid, voltag);
		} else if (start >= START_PICKER) {
			len = medium_transport_descriptor(p, start, number,
								dvcid, voltag);
		} else {	// Must start reading with drives.
			len = data_transfer_descriptor(p, start, number,
								dvcid, voltag);
		}
		break;
	default:	// Illegal descriptor type.
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,sense_flg);
		return(0);
		break;
	}

	// Now populate the 'main' header structure with byte count..
	len += element_status_hdr(&buf[0], dvcid, voltag, start, number);

	DEBC( printf("%s() returning %d bytes\n", __FUNCTION__,
		((len < alloc_len) ? len : alloc_len));

		hex_dump(buf, (len < alloc_len) ? len : alloc_len);

		decode_element_status(buf);
	)

	/* Return the smallest number */
	return ((len < alloc_len) ? len : alloc_len);
}

/*
 * Process the LOG_SENSE command
 *
 * Temperature page & tape alert pages only...
 */
#define TAPE_ALERT 0x2e
static int resp_log_sense(uint8_t *SCpnt, uint8_t *buf) {
	uint8_t	*b = buf;
	int	retval = 0;
	uint16_t	*sp;

	uint8_t supported_pages[] = {	0x00, 0x00, 0x00, 0x04,
					0x00,
					TEMPERATURE_PAGE,
					TAPE_ALERT
					};

	switch (SCpnt[2] & 0x3f) {
	case 0:	/* Send supported pages */
		if (verbose)
			syslog(LOG_DAEMON|LOG_WARNING, "%s",
						"Sending supported pages");
		sp = (uint16_t *)&supported_pages[2];
		*sp = htons(sizeof(supported_pages) - 4);
		b = memcpy(b, supported_pages, sizeof(supported_pages));
		retval = sizeof(supported_pages);
		break;
	case TEMPERATURE_PAGE:	/* Temperature page */
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
						"LOG SENSE: Temperature page");
		Temperature_pg.pcode_head.len = htons(sizeof(Temperature_pg) -
					sizeof(Temperature_pg.pcode_head));
		Temperature_pg.temperature = htons(35);
		b = memcpy(b, &Temperature_pg, sizeof(Temperature_pg));
		retval += sizeof(Temperature_pg);
		break;
	case TAPE_ALERT:	/* TapeAlert page */
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,"LOG SENSE: TapeAlert page");
		TapeAlert.pcode_head.len = htons(sizeof(TapeAlert) -
					sizeof(TapeAlert.pcode_head));
		b = memcpy(b, &TapeAlert, sizeof(TapeAlert));
		retval += sizeof(TapeAlert);
		setTapeAlert(&TapeAlert, 0);	// Clear flags after value read.
		break;
	default:
		if (debug)
			printf(
				"Unknown log sense code: 0x%x\n",
							SCpnt[2] & 0x3f);
		else if (verbose)
			syslog(LOG_DAEMON|LOG_INFO,
				"Unknown log sense code: 0x%x\n",
							SCpnt[2] & 0x3f);
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
 * 	cdev     -> Char dev file handle,
 * 	SCpnt    -> SCSI Command buffer pointer,
 *	buf      -> General purpose data buffer pointer
 * 	sense_flg    -> int buffer -> 1 = request_sense, 0 = no sense
 *
 * Return:
 *	total number of bytes to send back to vtl device
 */
static uint32_t processCommand(int cdev, uint8_t *SCpnt,
					uint8_t *buf, uint8_t *sense_flg)
{
	uint32_t	block_size = 0L;
	uint32_t	ret = 5L;	// At least 4 bytes for Sense & 4 for S/No.
	int	k = 0;
	struct	mode *smp = sm;

	switch(SCpnt[0]) {
	case INITIALIZE_ELEMENT_STATUS_WITH_RANGE:
	case INITIALIZE_ELEMENT_STATUS:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "%s",
						"INITIALIZE ELEMENT **");
		if (check_reset(sense_flg))
			break;
		sleep(1);
		break;

	case LOG_SELECT:	// Set or reset LOG stats.
		if (check_reset(sense_flg))
			break;
		resp_log_select(SCpnt, sense_flg);
		break;
	case LOG_SENSE:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "%s", "LOG SENSE **");
		ret += resp_log_sense(SCpnt, buf);
		break;

	case MODE_SELECT:
	case MODE_SELECT_10:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "%s", "MODE SELECT **");
		ret += resp_mode_select(cdev, SCpnt, buf);
		break;

	case MODE_SENSE:
	case MODE_SENSE_10:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "%s", "MODE SENSE **");
		DEBC( printf("MODE SENSE\n"); )
		ret += resp_mode_sense(SCpnt, buf, smp, sense_flg);
		break;

	case MOVE_MEDIUM:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "%s", "MOVE MEDIUM **");
		DEBC( printf("MOVE MEDIUM\n"); )
		if (check_reset(sense_flg))
			break;
		k = resp_move_medium(SCpnt, buf, sense_flg);
		break;
	case ALLOW_MEDIUM_REMOVAL:
		if (check_reset(sense_flg))
			break;
		resp_allow_prevent_removal(SCpnt, sense_flg);
		break;
	case READ_ELEMENT_STATUS:
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "%s",
						 "READ ELEMENT STATUS **");
		DEBC(	printf("READ ELEMENT STATUS\n"); )
		if (check_reset(sense_flg))
			break;
		ret += resp_read_element_status(SCpnt, buf, sense_flg);
		break;

	case REQUEST_SENSE:
		if (verbose) {
			syslog(LOG_DAEMON|LOG_INFO, "%s",
						"SCSI REQUEST SENSE **");
			syslog(LOG_DAEMON|LOG_INFO,
				"Sense key/ASC/ASCQ [0x%02x 0x%02x 0x%02x]",
					sense[2], sense[12], sense[13]);
		}
		DEBC( printf("Request Sense: key 0x%02x, ASC 0x%02x, ASCQ 0x%02x\n",
					sense[2], sense[12], sense[13]);
		)
		block_size =
		 (SCpnt[4] < sizeof(sense)) ? SCpnt[4] : sizeof(sense);
		memcpy(buf, sense, block_size);
		/* Clear out the request sense flag */
		*sense_flg = 0;
		memset(sense, 0, sizeof(sense));
		ret += block_size;
		break;

	case RESERVE:
	case RESERVE_10:
	case RELEASE:
	case RELEASE_10:
		if (check_reset(sense_flg))
			break;
		break;

	case REZERO_UNIT:	/* Rewind */
		if (verbose) 
			syslog(LOG_DAEMON|LOG_INFO, "%s", "Rewinding **");
		if (check_reset(sense_flg))
			break;
		sleep(1);
		break;

	case START_STOP:	// Load/Unload cmd
		if (check_reset(sense_flg))
			break;
		if (SCpnt[4] && 0x1) {
			libraryOnline = 1;
			if (verbose) 
				syslog(LOG_DAEMON|LOG_INFO, "%s",
							"Library online **");
		} else {
			libraryOnline = 0;
			if (verbose)
				syslog(LOG_DAEMON|LOG_INFO, "%s",
							"Library offline **");
		}
		break;
	case TEST_UNIT_READY:	// Return OK by default
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "%s %s",
					"Test Unit Ready :",
					(libraryOnline == 0) ? "No" : "Yes");
		if (check_reset(sense_flg))
			break;
		if ( ! libraryOnline)
			mkSenseBuf(NOT_READY, NO_ADDITIONAL_SENSE, sense_flg);
		break;

	default:
		syslog(LOG_DAEMON|LOG_ERR, "%s",
				"******* Unsupported command **********");
		DEBC( printf("0x%02x : Unsupported command ************\n", SCpnt[0]); )

		logSCSICommand(SCpnt);
		if (check_reset(sense_flg))
			break;
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sense_flg);
		break;
	}
	DEBC(
		printf("%s returning %d bytes\n\n", __FUNCTION__, ret);
	)
	return ret;
}

/*
 * Respond to messageQ 'list map' by sending a list of PCLs to messageQ
 */
static void listMap(void) {
	struct s_info *sp;
	int	a;
	char msg[MAXOBN];
	char *c = msg;
	*c = '\0';

	for (a = START_MAP; a < START_MAP + NUM_MAP; a++) {
		sp = slot2struct(a);
		if (slotOccupied(sp)) {
			strncat(c, (char *)sp->barcode, 10);
			syslog(LOG_DAEMON|LOG_NOTICE, "MAP slot %d full",
					a - START_MAP);
		} else {
			syslog(LOG_DAEMON|LOG_NOTICE, "MAP slot %d empty",
					a - START_MAP);
		}
	}
	if (verbose)
		syslog(LOG_DAEMON|LOG_NOTICE, "map contents: %s", msg);
	send_msg(msg, LIBRARY_Q + 1);
}

static void loadMap(void)
{
}

/*
 * Respond to messageQ 'empty map' by clearing 'ocuplied' status in map slots.
 */
static void emptyMap(void)
{
	struct s_info *sp;
	int	a;

	for (a = START_MAP; a < START_MAP + NUM_MAP; a++) {
		sp = slot2struct(a);
		if (slotOccupied(sp)) {
			setSlotEmpty(sp);
			if (verbose)
				syslog(LOG_DAEMON|LOG_NOTICE,
					"MAP slot %d emptied", a - START_MAP);
		}
	}
}

/*
 * Return 1, exit program
 */
static int processMessageQ(char *mtext) {

	syslog(LOG_DAEMON|LOG_NOTICE, "Q msg : %s", mtext);

	if (! strncmp(mtext, "debug", 5)) {
		if (debug) {
			debug--;
		} else {
			debug++;
			verbose = 2;
		}
	}
	if (! strncmp(mtext, "empty map", 9))
		emptyMap();
	if (! strncmp(mtext, "exit", 4))
		return 1;
	if (! strncmp(mtext, "list map", 8))
		listMap();
	if (! strncmp(mtext, "load map", 8))
		loadMap();
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
		syslog(LOG_DAEMON|LOG_NOTICE, "Verbose %s (%d)",
				 verbose ? "enabled" : "disabled", verbose);
	}

return 0;
}

int
init_queue(void) {
	int	queue_id;

	/* Attempt to create or open message queue */
	if ( (queue_id = msgget(QKEY, IPC_CREAT | QPERM)) == -1)
		syslog(LOG_DAEMON|LOG_ERR, "%s %m", "msgget failed");

return (queue_id);
}

static void init_mode_pages(struct mode *m) {

	struct mode *mp;
	uint16_t *sp;

	// Disconnect-Reconnect: SPC-3 7.4.8
	if ((mp = alloc_mode_page(2, m, 16))) {
		mp->pcodePointer[2] = 50;	// Buffer full ratio
		mp->pcodePointer[3] = 50;	// Buffer empty ratio
	}

	// Control: SPC-3 7.4.6
	if ((mp = alloc_mode_page(0x0a, m, 12))) {
		// Init rest of page data..
	}

	// Power condition: SPC-3 7.4.12
	if ((mp = alloc_mode_page(0x1a, m, 12))) {
		// Init rest of page data..
	}

	// Informational Exception Control: SPC-3 7.4.11 (TapeAlert)
	if ((mp = alloc_mode_page(0x1c, m, 12))) {
		mp->pcodePointer[2] = 0x08;
	}

	// Device Capabilities mode page: SMC-3 7.3.2
	if ((mp = alloc_mode_page(0x1f, m, 20))) {
		mp->pcodePointer[2] = 0x0f;
		mp->pcodePointer[3] = 0x07;
		mp->pcodePointer[4] = 0x0f;
		mp->pcodePointer[5] = 0x0f;
		mp->pcodePointer[6] = 0x0f;
		mp->pcodePointer[7] = 0x0f;
		// [8-11] -> reserved
		mp->pcodePointer[12] = 0x0f;
		mp->pcodePointer[13] = 0x0f;
		mp->pcodePointer[14] = 0x0f;
		mp->pcodePointer[15] = 0x0f;
		// [16-19] -> reserved
	}

	// Element Address Assignment mode page: SMC-3 7.3.3
	if ((mp = alloc_mode_page(0x1d, m, 20))) {
		sp = (uint16_t *)mp->pcodePointer;
		sp++;
		*sp = htons(START_PICKER);	// First medium transport
		sp++;
		*sp = htons(NUM_PICKER);	// Number of transport elem.
		sp++;
		*sp = htons(START_STORAGE);	// First storage slot
		sp++;
		*sp = htons(NUM_STORAGE);	// Number of storage slots
		sp++;
		*sp = htons(START_MAP);		// First i/e address
		sp++;
		*sp = htons(NUM_MAP);		// Number of i/e slots
		sp++;
		*sp = htons(START_DRIVE);	// Number of Drives
		sp++;
		*sp = htons(NUM_DRIVES);
	}

	// Transport Geometry Parameters mode page: SMC-3 7.3.4
	if ((mp = alloc_mode_page(0x1e, m, 4))) {
		// Both bytes set to zero...
	}

}

/*
 * Allocate enough storage for (size) drives & init to zero
 * - Returns pointer or NULL on error
 */
static struct d_info *init_d_struct(int size) {
	struct d_info d_info;
	struct d_info *dp;

	if ((dp = (struct d_info *)malloc(size * sizeof(d_info))) == NULL)
		syslog(LOG_DAEMON|LOG_ERR, "d_struct() Malloc failed: %m");
	else
		memset(dp, 0, sizeof(size * sizeof(d_info)));
return dp;
}

/*
 * Allocate enough storage for (size) elements & init to zero
 * - Returns pointer or NULL on error
 */
static struct s_info *init_s_struct(int size) {
	struct s_info s_info;
	struct s_info *sp;

	if ((sp = (struct s_info *)malloc(size * sizeof(s_info))) == NULL)
		syslog(LOG_DAEMON|LOG_ERR, "s_struct() Malloc failed: %m");
	else {
		memset(sp, 0, sizeof(size * sizeof(s_info)));
		DEBC(
			printf("init s_struct(%d)\n",
					(int)(size * sizeof(s_info)));
		) ;
	}
return sp;
}

/*
 * If barcode starts with string 'CLN' define it as a cleaning cart.
 * else its a data cartridge
 *
 * Return 1 = Data cartridge
 *        2 = Cleaning cartridge
 */
static int cart_type(char *barcode) {
	int retval = 0;

	retval = (strncmp(barcode, "CLN", 3)) ? 1 : 2;
	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO, "%s cart found: %s",
				(retval == 1) ? "Data" : "Cleaning", barcode);
		
return(retval);
}

/*
 * Read config file and populate d_info struct with library's drives
 */
#define MALLOC_SZ 1024
static void init_tape_info(void) {
	char *conf="/etc/vtl/vxlib.conf";
	FILE *ctrl;
	struct d_info *dp = NULL;
	struct s_info *sp = NULL;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	char *barcode;
	int slt;
	int x;

	if ((ctrl = fopen(conf , "r")) == NULL) {
		syslog(LOG_DAEMON|LOG_ERR, "Can not open config file %s : %m",
								conf);
		printf("Can not open config file %s : %m\n", conf);
		exit(1);
	}

	// Grab a couple of generic MALLOC_SZ buffers..
	if ((s = malloc(MALLOC_SZ)) == NULL) {
		perror("Could not allocate memory");
		exit(1);
	}
	if ((b = malloc(MALLOC_SZ)) == NULL) {
		perror("Could not allocate memory");
		exit(1);
	}

	/* While read in a line */
	while( fgets(b, MALLOC_SZ, ctrl) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		if (sscanf(b, "NumberDrives: %d", &slt) > 0)
			NUM_DRIVES = slt;
		if (sscanf(b, "NumberSlots: %d", &slt) > 0)
			NUM_STORAGE = slt;
		if (sscanf(b, "NumberMAP: %d", &slt) > 0)
			NUM_MAP = slt;
		if (sscanf(b, "NumberPicker: %d", &slt) > 0)
			NUM_PICKER = slt;
	}
	rewind(ctrl);

	if (debug)
		printf("%d Drives, %d Storage slots\n",
						NUM_DRIVES, NUM_STORAGE);
	else
		syslog(LOG_DAEMON|LOG_INFO, "%d Drives, %d Storage slots\n",
						NUM_DRIVES, NUM_STORAGE);

	/* Allocate enough memory for drives */
	if ((drive_info = init_d_struct(NUM_DRIVES + 1)) == NULL) 
		exit(1);
	else {	/* Now allocate each 'slot' within each drive struct */
		for (x = 0; x < NUM_DRIVES; x++) {
			dp = &drive_info[x];
			if ((dp->slot = init_s_struct(1)) == NULL)
				exit(1);
		}
	}

	/* Allocate enough memory for storage slots */
	if ((storage_info = init_s_struct(NUM_STORAGE + 1)) == NULL) 
		exit(1);

	/* Allocate enough memory for MAP slots */
	if ((map_info = init_s_struct(NUM_MAP + 1)) == NULL) 
		exit(1);

	/* Allocate enough memory for picker slots */
	if ((picker_info = init_s_struct(NUM_PICKER + 1)) == NULL) 
		exit(1);

	/* While read in a line */
	while( fgets(b, MALLOC_SZ, ctrl) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		if (sscanf(b, "Drive: %d", &slt) > 0)
			dp = &drive_info[slt - 1];
		if (sscanf(b, " Unit serial number: %s", s) > 0)
			strncpy(dp->inq_product_sno, s, 10);
//		if (sscanf(b, " Product serial number: %s", s) > 0)
//			strncpy(dp->inq_product_sno, s, 10);
		if (sscanf(b, " Product identification: %s", s) > 0)
			strncpy(dp->inq_product_id, s, 16);
		if (sscanf(b, " Product revision level: %s", s) > 0)
			strncpy(dp->inq_product_rev, s, 4);
		if (sscanf(b, " Vendor identification: %s", s) > 0)
			strncpy(dp->inq_vendor_id, s, 8);
	}
	fclose(ctrl);

	if ((ctrl = fopen("/etc/vtl/library_contents", "r")) == NULL) {
		printf("Can not open config file %s\n",
					"/etc/vtl/library_contents");
		syslog(LOG_DAEMON|LOG_ERR, "Can not open config file %s : %m",
					"/etc/vtl/library_contents");
		exit(1);
	}
	barcode = s;
	while( fgets(b, MALLOC_SZ, ctrl) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		barcode[0] = '\0';
		if ((x = (sscanf(b, "Drive %d: %s", &slt, barcode))) > 0) {
			dp = &drive_info[slt - 1];
			if (slt > NUM_DRIVES)
				syslog(LOG_DAEMON|LOG_ERR, "Too many drives");
			else if (x == 1) {
				dp->slot->slot_location = slt + START_DRIVE - 1;
				dp->slot->status = 0x08;
				dp->slot->cart_type = 0;
			} else {
			/*
				if (debug)
					printf("Barcode %s in drive %d\n",
								barcode, slt);
				snprintf((char *)dp->slot->barcode,10,"%-10s",barcode);
				dp->slot->barcode[10] = '\0';
				// Occupied  1 = data, 2 = Clean cart.
				dp->slot->cart_type = cart_type(barcode);
				// Full
				dp->slot->status = 0x09;
				dp->slot->slot_location = slt + START_DRIVE - 1;
			}
			*/
				dp->slot->cart_type = 0;
				dp->slot->status = 0x08;
				dp->slot->slot_location = slt + START_DRIVE - 1;
			}
		}
		if ((x = (sscanf(b, "MAP %d: %s", &slt, barcode))) > 0) {
			sp = &map_info[slt - 1];
			if (slt > NUM_MAP)
				syslog(LOG_DAEMON|LOG_ERR, "Too many MAPs");
			else if (x == 1) {
				sp->slot_location = slt + START_MAP - 1;
				sp->status = 0x3a;
				sp->cart_type = 0;
			} else {
				if (debug)
					printf("Barcode %s in MAP %d\n",
								barcode, slt);
				snprintf((char *)sp->barcode, 10, "%-10s", barcode);
				sp->barcode[10] = '\0';
				// 1 = data, 2 = Clean
				sp->cart_type = cart_type(barcode);
				sp->status = 0x3b;
				sp->slot_location = slt + START_MAP - 1;
			}
		}
		if ((x = (sscanf(b, "Picker %d: %s", &slt, barcode))) > 0) {
			sp = &picker_info[slt - 1];
			if (slt > NUM_PICKER)
				syslog(LOG_DAEMON|LOG_ERR, "Too many pickers");
			else if (x == 1) {
				sp->slot_location = slt + START_PICKER - 1;
				sp->cart_type = 0;
				sp->status = 0;
			} else {
				if (debug)
					printf("Barcode %s in Picker %d\n",
								barcode, slt);
				snprintf((char *)sp->barcode, 10, "%-10s", barcode);
				sp->barcode[10] = '\0';
				// 1 = data, 2 = Clean
				sp->cart_type = cart_type(barcode);
				sp->slot_location = slt + START_PICKER - 1;
				sp->status = 0x01;
			}
		}
		if ((x = (sscanf(b, "Slot %d: %s", &slt, barcode))) > 0) {
			sp = &storage_info[slt - 1];
			if (slt > NUM_STORAGE)
				syslog(LOG_DAEMON|LOG_ERR,
					"Storage slot %d out of range", slt);
			else if (x == 1) {
				sp->slot_location = slt + START_STORAGE - 1;
				sp->status = 0;
				sp->cart_type = 0x08;
			} else {
				if (debug)
					printf("Barcode %s in slot %d\n",
								 barcode, slt);
				snprintf((char *)sp->barcode, 10, "%-10s", barcode);
				sp->barcode[10] = '\0';
				sp->slot_location = slt + START_STORAGE - 1;
				// 1 = data, 2 = Clean
				sp->cart_type = cart_type(barcode);
				// Slot full
				sp->status = 0x09;
			}
		}
	}
	fclose(ctrl);
	free(b);
	free(s);
}

/*
 * main()
 *
 * e'nuf sed
 */
int main(int argc, char *argv[])
{
	int cdev, k;
	int ret;
	int vx_status;
	int q_priority = 0;
	int exit_status = 0;
	uint32_t serialNo = 0L;
	uint32_t	byteCount;
	uint8_t *buf;
	uint8_t *SCpnt;

	struct d_info *dp;
	char s[100];
	int a;

	pid_t pid;

	char *progname = argv[0];
	char *name = "vtl";
	uint8_t	minor = 0;

	/* Message Q */
	int	mlen, r_qid;
	struct q_entry r_entry;

	struct vtl_header vtl_head;

	while(argc > 0) {
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
	syslog(LOG_DAEMON|LOG_INFO, "%s: version %s", progname, Version);
	if (verbose)
		printf("%s: version %s\n", progname, Version);

	if ((cdev = chrdev_open(name, minor)) == -1) {
		syslog(LOG_DAEMON|LOG_ERR,
				"Could not open /dev/%s%d: %m", name, minor);
		printf("Could not open /dev/%s%d: %m", name, minor);
		fflush(NULL);
		exit(1);
	}

	k = TYPE_MEDIUM_CHANGER;
	if (ioctl(cdev, VX_TAPE_ONLINE, &k) < 0) {
		syslog(LOG_DAEMON|LOG_ERR,
				"Failed to connect to device %s%d: %m",
								name, minor);
		perror("Failed bringing unit online");
		exit(1);
	}

	if (ioctl(cdev, VX_TAPE_FIFO_SIZE, &bufsize) < 0) {
		perror("Failed quering FIFO size");
		exit(1);
	} else {
		syslog(LOG_DAEMON|LOG_INFO, "Size of kfifo is %d", bufsize);
		buf = (uint8_t *)malloc(bufsize);
		if (NULL == buf) {
			perror("Problems allocating memory");
			exit(1);
		}
	}

	/* Clear Sense arr */
	memset(sense, 0, sizeof(sense));
	reset = 1;

	/* Initialise message queue as necessary */
	if ((r_qid = init_queue()) == -1) {
		printf("Could not initialise message queue\n");
		exit(1);
	}

	/* Clear out message Q by reading anthing there.. */
	mlen = msgrcv(r_qid, &r_entry, MAXOBN, q_priority, IPC_NOWAIT);
	while (mlen > 0) {
		r_entry.mtext[mlen] = '\0';
		syslog(LOG_DAEMON|LOG_WARNING,
			"Found \"%s\" still in message Q\n", r_entry.mtext);
		mlen = msgrcv(r_qid, &r_entry, MAXOBN, q_priority, IPC_NOWAIT);
	}

	init_tape_info();
	init_mode_pages(sm);
	initTapeAlert(&TapeAlert);

	/* Send a message to each tape drive so they know the
	 * controlling library's message Q id
	 */
	for (a = 0; a < NUM_DRIVES; a++) {
		send_msg("Register", a + 1);

		if (debug) {
			dp = &drive_info[a];

			printf("\nDrive %d\n", a);

			strncpy(s, dp->inq_vendor_id, 8);
			s[8] = '\0';
			printf("Vendor ID     : \"%s\"\n", s);

			strncpy(s, dp->inq_product_id, 16);
			s[16] = '\0';
			printf("Product ID    : \"%s\"\n", s);

			strncpy(s, dp->inq_product_rev, 4);
			s[4] = '\0';
			printf("Revision Level: \"%s\"\n", s);

			strncpy(s, dp->inq_product_sno, 10);
			s[10] = '\0';
			printf("Product S/No  : \"%s\"\n", s);

			printf("Drive location: %d\n", dp->slot->slot_location);
			printf("Drive occupied: %s\n",
				(dp->slot->status & 0x01) ? "No" : "Yes");
		}
	}

	/* If debug, don't fork/run in background */
	if ( ! debug) {
		switch(pid = fork()) {
		case 0:         /* Child */
			break;
		case -1:
			printf("Failed to fork daemon\n");
			break;
		default:
			printf("%s process PID is %d\n", progname, (int)pid);
			break;
		}
 
		/* Time for the parent to terminate */
		if (pid != 0)
			exit(pid != -1 ? 0 : 1);
	}

	oom_adjust();
	
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

		if ((ret = ioctl(cdev, VX_TAPE_POLL_STATUS, &vx_status)) < 0) {
			syslog(LOG_DAEMON|LOG_WARNING, "ret: %d : %m", ret);
		} else {
			fflush(NULL);	/* So I can pipe debug o/p thru tee */
			switch(ret) {
			case STATUS_QUEUE_CMD:	// The new & improved method
				/* Get the SCSI cdb from vxtape driver
				 * - Returns SCSI command S/No. */
				getCommand(cdev, &vtl_head, vx_status);

				SCpnt = (uint8_t *)&vtl_head.cdb;
				serialNo = htonl(vtl_head.serialNo);

				/* Interpret the SCSI command & process
				-> Returns no. of bytes to send back to kernel
				 */
				byteCount = processCommand(cdev, SCpnt, buf,
								&request_sense);

				/* Complete SCSI cmd processing */
				completeSCSICommand(cdev,
						serialNo,
						buf,
						sense,
						&request_sense,
						byteCount);
				break;

			case STATUS_OK:
				usleep(100000);	// 0.1 Seconds
				break;

			default:
				syslog(LOG_DAEMON|LOG_NOTICE,
					"ioctl(0x%x) returned %d: argv %d",
					VX_TAPE_POLL_STATUS, ret, vx_status);
				sleep(1);
				break;
			}	// End switch(vx_status)
		}
	}
exit:
	close(cdev);
	free(buf);
	free(drive_info);

	exit(0);
}

