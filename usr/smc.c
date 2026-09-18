/*
 * This handles any SCSI OP codes defined in the standards as 'MEDIUM CHANGER'
 *
 * Copyright (C) 2005 - 2025 Mark Harvey markh794 at gmail dot com
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
 */

#include <unistd.h>
#include <sys/msg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include "be_byteshift.h"
#include "mhvtl_scsi.h"
#include "mhvtl_list.h"
#include "vtl_common.h"
#include "vtllib.h"
#include "logging.h"
#include "smc.h"
#include "q.h"
#include "mhvtl_log.h"
#include "subprocess.h"

uint8_t smc_allow_removal(struct scsi_cmd *cmd) {
	MHVTL_DBG(1, "%s MEDIUM REMOVAL (%ld) **",
			  (cmd->scb[4]) ? "PREVENT" : "ALLOW",
			  (long)cmd->dbuf_p->serialNo);
	return SAM_STAT_GOOD;
}

uint8_t smc_initialize_element_status(struct scsi_cmd *cmd) {
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	current_state = MHVTL_STATE_INITIALISE_ELEMENTS;

	MHVTL_DBG(1, "%s (%ld) **", "INITIALIZE ELEMENT",
			  (long)cmd->dbuf_p->serialNo);
	if (!cmd->lu->online) {
		sam_not_ready(NO_ADDITIONAL_SENSE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	sleep(1);
	return SAM_STAT_GOOD;
}

uint8_t smc_initialize_element_status_with_range(struct scsi_cmd *cmd) {
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	current_state = MHVTL_STATE_INITIALISE_ELEMENTS;

	MHVTL_DBG(1, "%s (%ld) **", "INITIALIZE ELEMENT RANGE",
			  (long)cmd->dbuf_p->serialNo);

	if (!cmd->lu->online) {
		sam_not_ready(NO_ADDITIONAL_SENSE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	sleep(1);
	return SAM_STAT_GOOD;
}

/* Return the element type of a particular element address */
static int slot_type(struct smc_priv *smc_p, int addr) {
	if ((addr >= smc_p->pm->start_drive) &&
		(addr < smc_p->pm->start_drive + smc_p->num_drives))
		return DATA_TRANSFER;
	if ((addr >= smc_p->pm->start_picker) &&
		(addr < smc_p->pm->start_picker + smc_p->num_picker))
		return MEDIUM_TRANSPORT;
	if ((addr >= smc_p->pm->start_map) &&
		(addr < smc_p->pm->start_map + smc_p->num_map))
		return MAP_ELEMENT;
	if ((addr >= smc_p->pm->start_storage) &&
		(addr < smc_p->pm->start_storage + smc_p->num_storage))
		return STORAGE_ELEMENT;
	return 0;
}

/*
 * Returns a 'human frendly' slot number
 * i.e. One with the internal offset removed (start counting at 1).
 */
static int slot_number(struct smc_personality_template *pm, struct s_info *sp) {
	switch (sp->element_type) {
	case MEDIUM_TRANSPORT:
		return sp->slot_location - pm->start_picker + 1;
	case STORAGE_ELEMENT:
		return sp->slot_location - pm->start_storage + 1;
	case MAP_ELEMENT:
		return sp->slot_location - pm->start_map + 1;
	case DATA_TRANSFER:
		return sp->slot_location - pm->start_drive + 1;
	}
	return 0;
}

/*
 * Takes a slot number and returns a struct pointer to the slot
 */
static struct s_info *slot2struct(struct smc_priv *smc_p, int addr) {
	struct list_head *slot_head;
	struct s_info	 *sp;

	slot_head = &smc_p->slot_list;

	list_for_each_entry(sp, slot_head, siblings) {
		if (sp->slot_location == (unsigned int)addr)
			return sp;
	}

	MHVTL_DBG(1, "Arrr... Could not find slot %d", addr);

	return NULL;
}

/*
 * Takes a Drive number and returns a struct pointer to the drive
 */
static struct d_info *drive2struct(struct smc_priv *smc_p, int addr) {
	struct s_info *s;

	s = slot2struct(smc_p, addr);
	if (s)
		return s->drive;

	return NULL;
}

/* Query the drive to check if it thinks it is empty */
static int is_drive_empty(struct d_info *drv) {
	int			   mlen, r_qid;
	struct q_entry q;

	/* Initialise message queue as necessary */
	r_qid = init_queue();
	if (r_qid == -1) {
		printf("Could not initialise message queue\n");
		exit(1);
	}

	MHVTL_DBG(1, "%ld: Sending \"%s\" to snd_id %ld",
			  my_id, msg_mount_state, drv->drv_id);
	send_msg(msg_mount_state, drv->drv_id);

	/* A failed receive leaves q untouched, so it must not be read */
	memset(&q, 0, sizeof(q));

	mlen = msgrcv(r_qid, &q, MAXOBN, my_id, MSG_NOERROR);
	if (mlen <= 0) {
		MHVTL_ERR("%ld: No reply from the drive: %s",
				  my_id, strerror(errno));
		return 1; /* Assume occupied rather than act on garbage */
	}

	MHVTL_DBG(1, "%ld: Received \"%s\" from snd_id %ld",
			  my_id,
			  q.msg.text,
			  q.msg.snd_id);

	/* string defined in q.h */
	return strncmp(msg_not_occupied, q.msg.text, 12);
}

/* returns true if medium transport access to slot is OK */
int slotAccess(struct s_info *s) {
	return s->status & STATUS_Access;
}

/* Returns true if slot has media in it */
int slotOccupied(struct s_info *s) {
	return s->status & STATUS_Full;
}

/* Returns true if drive has media in it */
static int driveOccupied(struct d_info *d) {
	int ret;

	ret = slotOccupied(d->slot);
	ret |= is_drive_empty(d);

	return ret;
}

static int check_tape_unload(void) {
	int			   mlen, r_qid;
	struct q_entry q;

	/* Initialise message queue as necessary */
	r_qid = init_queue();
	if (r_qid == -1) {
		printf("Could not initialise message queue\n");
		exit(1);
	}

	/* A failed receive leaves q untouched, so it must not be read */
	memset(&q, 0, sizeof(q));

	mlen = msgrcv(r_qid, &q, MAXOBN, my_id, MSG_NOERROR);
	if (mlen <= 0) {
		MHVTL_ERR("%ld: No reply from the drive: %s",
				  my_id, strerror(errno));
		return 1; /* Treat as an unload failure */
	}

	MHVTL_DBG(1, "%ld: Received \"%s\" from snd_id %ld",
			  my_id,
			  q.msg.text,
			  q.msg.snd_id);

	/* msg defined in q.h */
	return strncmp(msg_unload_ok, q.msg.text, 11);
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
 * 1 / 0 Set/Clear the Access bit.
 * Access bit when set, indicates the medium transport can access media
 */
void setAccessStatus(struct s_info *s, int flg) {
	if (flg)
		s->status |= STATUS_Access;
	else
		s->status &= ~STATUS_Access;
}

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
void setImpExpStatus(struct s_info *s, int flg) {
	if (flg)
		s->status |= STATUS_ImpExp;
	else
		s->status &= ~STATUS_ImpExp;
}

/*
 * Sets the 'Full' bit true/false in the status field
 */
void setFullStatus(struct s_info *s, int flg) {
	if (flg)
		s->status |= STATUS_Full;
	else
		s->status &= ~STATUS_Full;
}

void setSlotEmpty(struct s_info *s) {
	setFullStatus(s, 0);
}

static void setDriveEmpty(struct d_info *d) {
	setFullStatus(d->slot, 0);
	send_msg(msg_set_empty, d->drv_id);
	/* Wait for ack from drive */
	check_tape_unload();
}

void setSlotFull(struct s_info *s) {
	setFullStatus(s, 1);
}

void setDriveFull(struct d_info *d) {
	setFullStatus(d->slot, 1);
}

/* Returns 1 (true) if slot is MAP slot */
static int is_map_slot(struct s_info *s) {
	MHVTL_DBG(2, "slot type %d: %s", s->element_type,
			  (s->element_type == MAP_ELEMENT) ? "MAP" : "NOT A MAP");
	return s->element_type == MAP_ELEMENT;
}

static int map_access_ok(struct smc_priv *smc_p, struct s_info *s) {
	if (is_map_slot(s)) {
		MHVTL_DBG(3, "Returning status of %d", smc_p->cap_closed);
		return smc_p->cap_closed;
	}
	MHVTL_DBG(3, "Returning 0");
	return 0;
}

static int dump_element_desc(uint8_t *p, int voltag, int num_elem, int len,
							 char dvcid_serial_only) {
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
		MHVTL_DBG(3, "  Code Set                     : 0x%02x",
				  p[i] & 0xf);
		MHVTL_DBG(3, "  Identifier type              : 0x%02x",
				  p[i + 1] & 0xf);
		idlen = p[i + 3];
		MHVTL_DBG(3, "  Identifier length            : %d", idlen);
		if (idlen) {
			if (dvcid_serial_only) {
				MHVTL_DBG(3, "  ASCII data                   : %.*s", idlen, &p[i + 4]);
			} else {
				MHVTL_DBG(3, "  ASCII data                   : %.*s", 8, &p[i + 4]);
				MHVTL_DBG(3, "  ASCII data                   : %.*s", 16, &p[i + 12]);
				MHVTL_DBG(3, "  ASCII data                   : %.*s", 10, &p[i + 28]);
			}
		}
		i = (j + 1) * len;
	}
	return i;
}

static void decode_element_status(struct smc_priv *smc_p, uint8_t *p) {
	int voltag;
	int elem_len;
	int page_elements, page_bytes;
	int total_count;
	int i;

	total_count = get_unaligned_be24(&p[5]);

	MHVTL_DBG(3, "Element Status Data");
	MHVTL_DBG(3, "  First element reported       : %d",
			  get_unaligned_be16(&p[0]));
	MHVTL_DBG(3, "  Number of elements available : %d",
			  get_unaligned_be16(&p[2]));
	MHVTL_DBG(3, "  Byte count of report         : %d",
			  get_unaligned_be24(&p[5]));

	p += 8;
	total_count -= 8;

	while (total_count > 0) {
		MHVTL_DBG(3, "Element Status Page");
		MHVTL_DBG(3, "  Element Type code            : %d (%s)",
				  p[0], slot_type_str(p[0]));

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
		total_count -= 8;

		MHVTL_DBG(3, "Element Descriptor(s) : Num of Elements %d",
				  page_elements);

		i = dump_element_desc(p, voltag, page_elements, elem_len,
							  smc_p->pm->dvcid_serial_only);
		p += i;
		total_count -= i;
	}

	if (debug)
		fflush(NULL);
}

/*
 * Calculate length of one element
 */
static int sizeof_element(struct scsi_cmd *cmd, int type) {
	struct smc_priv *smc_p = (struct smc_priv *)cmd->lu->lu_private;
	int				 dvcid;
	int				 voltag;

	voltag = (cmd->scb[1] & 0x10) >> 4;
	if (smc_p->pm->no_dvcid_flag)
		dvcid = 1;
	else
		dvcid = cmd->scb[6] & 0x01; /* Device ID */

	return 16 + (voltag ? VOLTAG_LEN : 0) +
		   (dvcid && (type == DATA_TRANSFER) ? smc_p->pm->dvcid_len : 0);
}

/*
 * Fill in a single element descriptor
 *
 * Returns number of bytes in element data.
 */
static int fill_ed(struct scsi_cmd *cmd, uint8_t *p, struct s_info *s) {
	struct smc_priv *smc_p = (struct smc_priv *)cmd->lu->lu_private;
	struct d_info	*d	   = NULL;
	int				 j	   = 0;
	uint8_t			 voltag;
	uint8_t			 dvcid;

	voltag = (cmd->scb[1] & 0x10) >> 4;
	if (smc_p->pm->no_dvcid_flag)
		dvcid = 1;
	else
		dvcid = cmd->scb[6] & 0x01; /* Device ID */

	/* Should never occur, but better to trap then core */
	if (!s) {
		MHVTL_DBG(1, "Slot out of range");
		return 0;
	}

	if (s->element_type == DATA_TRANSFER)
		d = s->drive;

	put_unaligned_be16(s->slot_location, &p[j]);
	j += 2;

	p[j] = s->status;
	if (s->element_type == MAP_ELEMENT) {
		if (smc_p->cap_closed)
			p[j] |= STATUS_Access;
		else
			p[j] &= ~STATUS_Access;
	}
	j++;

	p[j++] = 0; /* Reserved */

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
	p[j++] = (s->asc_ascq >> 8) & 0xff; /* Additional Sense Code */
	p[j++] = s->asc_ascq & 0xff;		/* Additional Sense Code Qualifer */

	/* ID VALID (bit 5) and the SCSI bus address that follows it were
	 * removed in SMC-2, but libraries with parallel SCSI drives still
	 * report them. Reporting an address without ID VALID - which is
	 * what this did - matches no library either way.
	 */
	if (s->element_type == DATA_TRANSFER &&
		smc_p->pm->report_scsi_bus_address) {
		p[j++] = 0x20; /* ID VALID */
		p[j++] = d->SCSI_ID;
	} else {
		p[j++] = 0;
		p[j++] = 0;
	}

	p[j++] = 0; /* Reserved */

	/* bit 8 set if Source Storage Element is valid | s->occupied */
	if (s->media)
		p[j] = (s->media->last_location > 0) ? 0x80 : 0;
	else
		p[j] = 0;

	/* Medium type, bits 2-0. Bit 3 above it is ED (element disabled),
	 * so the field is three bits wide and must be masked as such.
	 *
	 * Ref: smc3r12 - Table 28
	 *   0 - unspecified, 1 - data, 2 - cleaning,
	 *   3 - diagnostic,  4 - WORM, 5 - microcode image
	 *
	 * SMC-2 table 17 reserves 3 to 5; they were added in SMC-3 and are
	 * reported by IBM, HP and Quantum libraries, so they are used here.
	 */
	if (s->media)
		p[j] |= s->media->cart_type & 0x07;

	j++;

	/* Source Storage Element Address */
	put_unaligned_be16(s->last_location, &p[j]);
	j += 2;

	MHVTL_DBG(2, "Slot location: %d, DVCID: %d, VOLTAG: %d, status: 0x%02x",
			  s->slot_location, dvcid, voltag, s->status);

	if (voltag) {
		/* Only the identifier is blank padded. The qualifier, the
		 * reserved byte and the sequence number follow it and must be
		 * left at zero unless they carry a value.
		 */
		memset(&p[j], 0, VOLTAG_LEN);

		if (s->status & STATUS_Full) {
			if (!(s->media->internal_status & INSTATUS_NO_BARCODE))
				blank_fill(&p[j], s->media->barcode,
						   VOLTAG_ID_LEN);
			else
				p[j + VOLTAG_ID_LEN] = VOLTAG_VIQ_UNREADABLE;
		}

		j += VOLTAG_LEN; /* Account for barcode */
	}

	if (dvcid && s->element_type == DATA_TRANSFER) {
		p[j++] = 2; /* Code set 2 = ASCII */
		/* Identifier type - If serial number only - 0, otherwise 1 */
		p[j++] = smc_p->pm->dvcid_serial_only ? 0 : 1;
		p[j++] = 0;					   /* Reserved */
		p[j++] = smc_p->pm->dvcid_len; /* Identifier Length */
		if (smc_p->pm->dvcid_serial_only) {
			blank_fill(&p[j], d->inq_product_sno,
					   smc_p->pm->dvcid_len);
			j += smc_p->pm->dvcid_len;
		} else {
			blank_fill(&p[j], d->inq_vendor_id, 8);
			j += 8;
			blank_fill(&p[j], d->inq_product_id, 16);
			j += 16;
			blank_fill(&p[j], d->inq_product_sno, 10);
			j += 10;
		}
	} else {
		p[j++] = 0; /* Reserved */
		p[j++] = 0; /* Reserved */
		p[j++] = 0; /* Reserved */
		p[j++] = 0; /* Reserved */
	}
	MHVTL_DBG(3, "Element Descriptor - Returning %d (0x%02x) bytes", j, j);

	return j;
}

/*
 * Fill in element status page Header (8 bytes)
 */
static void fill_element_status_page_hdr(struct scsi_cmd *cmd, uint8_t *p,
										 uint16_t element_count,
										 uint8_t  type) {
	int		 element_sz;
	uint32_t element_len;
	uint8_t	 voltag;

	voltag = (cmd->scb[1] & 0x10) >> 4;

	element_sz = sizeof_element(cmd, type);

	p[0] = type; /* Element type Code */

	/* Primary Volume Tag set - Returning Barcode info.
	 *
	 * AVolTag stays clear: it would tell the initiator that a second
	 * 36 byte tag follows each descriptor, and none is emitted. No
	 * library in the references supports dual sided media, so every
	 * one of them reports the alternate tag as absent.
	 */
	p[1] = (voltag == 0) ? 0 : 0x80;

	/* Number of bytes per element */
	put_unaligned_be16(element_sz, &p[2]);

	element_len = element_sz * element_count;

	/* Total number of bytes in all element descriptors */
	put_unaligned_be24(element_len, &p[5]);

	/* Reserved */
	p[4] = 0; /* Above mask should have already set this to 0... */

	MHVTL_DBG(2, "Element Status Page Header: "
				 "%02x %02x %02x %02x %02x %02x %02x %02x",
			  p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
}

/*
 * Build the initial ELEMENT STATUS HEADER
 *
 */
static int fill_element_status_data_hdr(uint8_t *p, int start, int count,
										uint32_t byte_count) {
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
	put_unaligned_be24(byte_count, &p[5]);

	MHVTL_DBG(2, " Element Status Data HEADER: "
				 "%02x %02x %02x %02x %02x %02x %02x %02x",
			  p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
	MHVTL_DBG(3, " Decoded:");
	MHVTL_DBG(3, "  First element Address    : %d (0x%02x)",
			  get_unaligned_be16(&p[0]),
			  get_unaligned_be16(&p[0]));
	MHVTL_DBG(3, "  Number elements reported : %d (0x%02x)",
			  get_unaligned_be16(&p[2]),
			  get_unaligned_be16(&p[2]));
	MHVTL_DBG(3, "  Total byte count         : %d (0x%04x)",
			  get_unaligned_be32(&p[4]),
			  get_unaligned_be32(&p[4]));

	return 8; /* Header is 8 bytes in size.. */
}

/* Returns address of first available elements from starting number */
/* No element matched. Not zero: an element may legitimately live at
 * address 0 - the Overland personality puts its MAP there - and an
 * initiator asking for "starting element address 0" means "from the
 * lowest", so returning 0 for "nothing found" made that element
 * unreportable and READ ELEMENT STATUS failed for the whole library.
 */
#define NO_ELEMENT ((uint32_t)~0)

static uint32_t find_first_matching_element(struct smc_priv *priv,
											uint32_t		 start,
											uint8_t			 type) {
	struct list_head *slot_head;
	struct s_info	 *sp;

	slot_head = &priv->slot_list;

	list_for_each_entry(sp, slot_head, siblings) {
		if (!type) { /* Element type not defined */
			if (sp->slot_location >= start)
				return sp->slot_location;
		} else if (sp->element_type == type) {
			if (sp->slot_location >= start)
				return sp->slot_location;
		}
	}
	return NO_ELEMENT;
}

/* Returns number of available elements left from starting number */
static uint32_t num_available_elements(struct smc_priv *priv, uint8_t type,
									   uint32_t start, uint32_t max) {
	struct list_head *slot_head;
	struct s_info	 *sp;
	unsigned int	  counted = 0;

	slot_head = &priv->slot_list;

	list_for_each_entry(sp, slot_head, siblings) {
		if (!type) { /* Element type not defined */
			if (sp->slot_location >= start)
				if (counted < max)
					counted++;
		} else if (sp->element_type == type) {
			if (sp->slot_location >= start)
				if (counted < max)
					counted++;
		}
	}

	MHVTL_DBG(2, "Determining %d element%s of type %s starting at %d"
				 ", returning %d",
			  max, max == 1 ? "" : "s",
			  slot_type_str(type),
			  start, counted);

	return counted;
}

/*
 * Fill in Element status page header + each Element descriptor
 *
 * uint8_t *p -> Pointer to data buffer address
 * uint16_t start -> Starting slot number
 * uint8_t type -> Slot type
 * uint16_t residual -> Sum of slots already reported on
 *
 * Returns number of bytes in element page(s)
 */
static uint32_t fill_element_page(struct scsi_cmd *cmd, uint8_t *p,
								  uint16_t start, uint8_t type,
								  uint16_t residual) {
	struct smc_priv *smc_p;
	int				 j;
	uint8_t			*cdb = cmd->scb;
	struct s_info	*sp;

	uint16_t max_count; /* Max element count */
	uint32_t avail_count;
	uint32_t element_sz;
	uint16_t begin_element;
	int		 slot_count;

	/* SMC-2 6.10.1: the maximum number of descriptors to create. Zero
	 * means none - it is not a request for every element.
	 */
	max_count = get_unaligned_be16(&cdb[4]);

	slot_count = max_count - residual;
	if (slot_count <= 0)
		return 0; /* No more slots to report on */

	/* Update max_count to reflect 'sum' value */
	max_count = (uint16_t)slot_count;

	smc_p = (struct smc_priv *)cmd->lu->lu_private;

	MHVTL_DBG(2, "Query %d element%s starting from addr: %d"
				 " of type: (%d) %s",
			  max_count,
			  (max_count == 1) ? "" : "s",
			  start,
			  type, slot_type_str(type));

	/* Find first valid slot. */
	begin_element = find_first_matching_element(smc_p, start, type);
	if (begin_element == NO_ELEMENT) {
		MHVTL_DBG(1, "No element of this type at or above the start "
					 "address, line %d", __LINE__);
		return 0;
	}

	avail_count = num_available_elements(smc_p, type, start, max_count);

	MHVTL_DBG(3, "Available count: %d, type: (%d) %s",
			  avail_count, type, slot_type_str(type));

	/* Create Element Status Page Header. */
	fill_element_status_page_hdr(cmd, p, avail_count, type);

	/* Account for the 8 bytes in element status page header */
	p += 8;
	avail_count = 8; /* Reuse avail_count as available byte count */

	/* Now loop over each slot and fill in details. */
	j = 1;
	list_for_each_entry(sp, &smc_p->slot_list, siblings) {
		if (sp->slot_location < start)
			continue;
		if (type) {
			if (sp->element_type != type)
				continue; /* Don't report on this one */
		} else {
			/* Any type.. Need to fill in one type at a time */
			if (sp->slot_location == start)
				type = sp->element_type;
		}
		element_sz = fill_ed(cmd, p, sp);
		avail_count += element_sz; /* inc byte count */
		MHVTL_DBG(3, "Count: %d, max_count: %d, slot: %d, "
					 "byte_count: 0x%04x (%d)",
				  j, max_count, sp->slot_location,
				  avail_count, avail_count);
		if (debug)
			hex_dump(p, element_sz);
		p += element_sz; /* inc pointer into dest buf */
		j++;
		if (j > max_count)
			break;
	}

	return avail_count;
}

/*
 * Round a transfer length down so that it ends on an element descriptor
 * boundary.
 *
 * SMC-2 6.10.1: when the allocation length is too small for everything,
 * only descriptors whose complete contents fit are transferred. Cutting
 * mid-descriptor leaves the initiator with a fragment it cannot tell
 * apart from a short transfer.
 *
 * 'buf' points at the element status data header, 'len' is the length of
 * all valid data and 'limit' is the initiator's allocation length.
 */
static uint32_t trim_to_element_boundary(uint8_t *buf, uint32_t len,
										 uint32_t limit) {
	uint32_t offset = 8; /* Element status data header */
	uint32_t good	= 8;

	if (limit >= len)
		return len;
	if (limit < offset)
		return limit;

	/* Walk each element status page: 8 byte header, then a whole
	 * number of equally sized descriptors.
	 */
	while (offset + 8 <= len) {
		uint32_t desc_sz	= get_unaligned_be16(&buf[offset + 2]);
		uint32_t page_bytes = get_unaligned_be24(&buf[offset + 5]);
		uint32_t used;

		if (!desc_sz)
			break;
		if (offset + 8 > limit)
			break;

		offset += 8;
		good = offset;

		for (used = 0; used + desc_sz <= page_bytes; used += desc_sz) {
			if (offset + desc_sz > limit)
				return good;
			offset += desc_sz;
			good = offset;
		}
	}

	return good;
}

/*
 * Build READ ELEMENT STATUS data.
 *
 * Returns number of bytes to xfer back to host.
 */

#define NUM_SLOT 4 /* 4 Element types */

uint8_t smc_read_element_status(struct scsi_cmd *cmd) {
	struct smc_priv		*smc_p = (struct smc_priv *)cmd->lu->lu_private;
	struct lu_phy_attr	*lu	   = cmd->lu;
	uint8_t				*cdb   = cmd->scb;
	uint8_t				*p;
	uint8_t				*sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t				 type	  = cdb[1] & 0x0f;
	uint16_t			 sum;
	uint16_t			 req_start_elem;
	uint16_t			 req_number; /* Num of elements initiator requested */
	uint32_t			 alloc_len;	 /* Amount of space initiator has pre alloc */
	uint16_t			 start;		 /* First valid slot location */
	uint16_t			 start_any;	 /* First valid slot location - temp count */
	uint32_t			 elem_byte_count;
	uint32_t			 byte_count;
	uint32_t			 cur_count;
	struct s_sd			 sd;
	struct smc_type_slot type_arr[NUM_SLOT]; /* Hold sorted slot type */
	char				 start_slot_type;
	int					 i;

#ifdef MHVTL_DEBUG
	uint8_t voltag = (cdb[1] & 0x10) >> 4;
	uint8_t dvcid; /* Device ID */
	if (smc_p->pm->no_dvcid_flag)
		dvcid = 1;
	else
		dvcid = cmd->scb[6] & 0x01; /* Device ID */
#endif

	MHVTL_DBG(1, "READ ELEMENT STATUS (%ld) **",
			  (long)cmd->dbuf_p->serialNo);

	req_start_elem = get_unaligned_be16(&cdb[2]);
	req_number	   = get_unaligned_be16(&cdb[4]);
	alloc_len	   = get_unaligned_be24(&cdb[7]);

	MHVTL_DBG(3, " Element type(%d) => %s", type, slot_type_str(type));
	MHVTL_DBG(3, "  Starting Element Address: %d", req_start_elem);
	MHVTL_DBG(3, "  Number of Elements      : %d", req_number);
	MHVTL_DBG(3, "  Allocation length       : %d (0x%04x)",
			  alloc_len, alloc_len);
	MHVTL_DBG(3, "  Device ID: %s, voltag: %s",
			  (dvcid == 0) ? "No" : "Yes",
			  (voltag == 0) ? "No" : "Yes");

	p = (uint8_t *)cmd->dbuf_p->data;

	/* Set alloc_len to smallest value */
	alloc_len = min(alloc_len, smc_p->bufsize);

	cmd->dbuf_p->sz = 0;

	/* Init buffer */
	memset(p, 0, alloc_len);

	/* CONTROL byte. NACA is not supported (NORMACA is 0 in INQUIRY),
	 * and linking is obsolete, so no bit here may be set.
	 */
	if (cdb[11] != 0x0) {
		MHVTL_DBG(2, "cdb[11] : Illegal value of %02x", cdb[11]);
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 11;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* Find first matching slot number which matches the type. */
	start = find_first_matching_element(smc_p, req_start_elem, type);
	if (start == NO_ELEMENT) { /* Nothing found.. */
		MHVTL_DBG(1, "No element of this type at or above the start "
					 "address, line %d", __LINE__);
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 2; /* Starting element address */
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* Leave room for 'Element Status data' header which is filled in
	 * after we figure out how many elements to report
	 */
	p += 8;

	/* Byte count of report available all pages, n-7
	 * Reference: table 44 6.12.2 smc3r15
	 * i.e. Don't include 8 byte 'Element Status data' header in the count
	 */
	elem_byte_count = 0;

	sum = 0; /* keep track of number of elements already reported */

	switch (type) {
	case MEDIUM_TRANSPORT:
	case STORAGE_ELEMENT:
	case MAP_ELEMENT:
	case DATA_TRANSFER:
		elem_byte_count += fill_element_page(cmd, p, start, type, sum);
		break;
	case ANY:
		/* Don't modify 'start' value as it is needed later */
		start_any		= start;
		start_slot_type = slot_type(smc_p, start_any);
		sort_library_slot_type(lu, &type_arr[0]);

		/* Find out where we are up to */
		for (i = 0; i < NUM_SLOT; i++) {
			MHVTL_DBG(2, "Testing type %d with %d",
					  type_arr[i].type, start_slot_type);
			if (type_arr[i].type == start_slot_type)
				break;
		}
		if (i >= NUM_SLOT) { /* Not found in above for loop */
			MHVTL_ERR("Couldn't find starting slot type !!");
			sd.byte0		 = SKSV | CD;
			sd.field_pointer = 3;
			sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd,
								sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}

		MHVTL_DBG(3, "All element type: Starting at %d", start_any);
		for (; i < NUM_SLOT; i++) {
			MHVTL_DBG(3, "i: %d, type_arr[i].type: (%d) %s, "
						 "type_arr[i].start: %d",
					  i, type_arr[i].type,
					  slot_type_str(type_arr[i].type),
					  type_arr[i].start);

			byte_count = fill_element_page(cmd, p, start_any,
										   type_arr[i].type, sum);
			elem_byte_count += byte_count;
			p += byte_count;
			sum += byte_count /
				   sizeof_element(cmd, type_arr[i].type);

			if (i < NUM_SLOT - 1) /* Next slot type number */
				start_any = type_arr[i + 1].start;
		}
		break;
	default: /* Illegal descriptor type. */
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	cur_count = num_available_elements(smc_p, type, start, req_number);

	/* Now populate the 'main' header structure with byte count.. */
	fill_element_status_data_hdr(cmd->dbuf_p->data, start, cur_count,
								 elem_byte_count);

#ifdef MHVTL_DEBUG
	if (debug)
		hex_dump(cmd->dbuf_p->data, elem_byte_count);
#endif

	if (verbose > 2)
		decode_element_status(smc_p, cmd->dbuf_p->data);

	cmd->dbuf_p->sz = trim_to_element_boundary(cmd->dbuf_p->data,
											   elem_byte_count + 8,
											   alloc_len);

	MHVTL_DBG(2, "Element count: %d, Elem byte count: %d (0x%04x),"
				 " alloc_len: %d, returning %d",
			  cur_count,
			  elem_byte_count, elem_byte_count,
			  alloc_len, cmd->dbuf_p->sz);

	return SAM_STAT_GOOD;
}

/* Expect a response from tape drive on load success/failure
 * Returns 0 on success
 * non-zero on load failure

 * FIXME: I really need a timeout here..
 */
static int check_tape_load(void) {
	int			   mlen, r_qid;
	struct q_entry q;

	/* Initialise message queue as necessary */
	r_qid = init_queue();
	if (r_qid == -1) {
		printf("Could not initialise message queue\n");
		exit(1);
	}

	/* A failed receive leaves q untouched, so it must not be read */
	memset(&q, 0, sizeof(q));

	mlen = msgrcv(r_qid, &q, MAXOBN, my_id, MSG_NOERROR);
	if (mlen <= 0) {
		MHVTL_ERR("%ld: No reply from the drive: %s",
				  my_id, strerror(errno));
		return 1; /* Treat as a load failure */
	}

	MHVTL_DBG(1, "%ld: Received \"%s\" from snd_id %ld",
			  my_id,
			  q.msg.text,
			  q.msg.snd_id);

	/* msg defined in q.h */
	return strncmp(msg_load_ok, q.msg.text, strlen(msg_load_ok));
}

/*
 * Logically move information from 'src' address to 'dest' address
 */
static void move_cart(struct s_info *src, struct s_info *dest) {

	dest->media = src->media;

	/* SMC-2 6.10.4: this is the last *storage* element the medium
	 * occupied, so a drive or import/export address must not overwrite
	 * it. Keeping it pointing at the home slot is also what lets
	 * previous_storage_slot() put the medium back on shutdown.
	 */
	if (src->element_type == STORAGE_ELEMENT) {
		dest->last_location		   = src->slot_location;
		dest->media->last_location = src->slot_location;
	} else {
		dest->last_location = src->media->last_location;
	}

	setSlotFull(dest);
	if (is_map_slot(dest))
		setImpExpStatus(dest, ROBOT_ARM); /* Placed by robot arm */

	src->media		   = NULL;
	src->last_location = 0; /* Forget where the old media was */
	setSlotEmpty(src);		/* Clear Full bit */
	MHVTL_DBG(1, "Setting src slot access true");
	setAccessStatus(src, 1); /* Set the access bit now it's empty */
}

static int run_move_command(struct smc_priv *smc_p, struct s_info *src,
							struct s_info *dest, uint8_t *sam_stat) {
	char *movecommand;
	char  barcode[MAX_BARCODE_LEN + 1];
	int	  res = 0;
	int	  cmdlen;

	if (!smc_p->movecommand) {
		/* no command: do nothing */
		return SAM_STAT_GOOD;
	}

	cmdlen		= strlen(smc_p->movecommand) + MAX_BARCODE_LEN + 4 * 10;
	movecommand = zalloc(cmdlen + 1);

	if (!movecommand) {
		MHVTL_ERR("malloc failed");
		sam_hardware_error(E_MANUAL_INTERVENTION_REQ, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	sprintf(barcode, "%s", src->media->barcode);
	truncate_spaces(&barcode[0], MAX_BARCODE_LEN + 1);
	snprintf(movecommand, cmdlen, "%s %s %d %s %d %s",
			 smc_p->movecommand,
			 slot_type_str(src->element_type),
			 slot_number(smc_p->pm, src),
			 slot_type_str(dest->element_type),
			 slot_number(smc_p->pm, dest),
			 barcode);
	MHVTL_DBG(3, "Calling external script: %s", movecommand);
	res = run_command(movecommand, smc_p->commandtimeout);
	if (res) {
		MHVTL_ERR("move command returned %d", res);
		sam_hardware_error(E_MANUAL_INTERVENTION_REQ, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	return SAM_STAT_GOOD;
}

static int move_slot2drive(struct smc_priv *smc_p,
						   int src_addr, int dest_addr, uint8_t *sam_stat) {
	struct s_info *src;
	struct d_info *dest;
	char		   cmd[MAX_BARCODE_LEN + 12];
	int			   retval;

	current_state = MHVTL_STATE_MOVING_SLOT_2_DRIVE;

	src	 = slot2struct(smc_p, src_addr);
	dest = drive2struct(smc_p, dest_addr);

	if (!slotOccupied(src)) {
		sam_illegal_request(E_MEDIUM_SRC_EMPTY, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (driveOccupied(dest)) {
		sam_illegal_request(E_MEDIUM_DEST_FULL, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (src->element_type == MAP_ELEMENT) {
		if (!map_access_ok(smc_p, src)) {
			MHVTL_DBG(2, "SOURCE MAP port not accessable");
			sam_not_ready(E_MAP_OPEN, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	/* Call any external cmd first before changing state */
	retval = run_move_command(smc_p, src, dest->slot, sam_stat);
	if (retval)
		return retval;

	sprintf(cmd, "lload %s", src->media->barcode);
	/* Remove traling spaces */
	truncate_spaces(&cmd[6], MAX_BARCODE_LEN + 1);

	/* FIXME: About here would be a good spot to create any 'missing'
	 *	  media. That way, the user would not have to pre-create
	 *	  media.
	 */

	MHVTL_DBG(1, "About to send cmd: \'%s\' to drive %d",
			  cmd, slot_number(smc_p->pm, dest->slot));

	send_msg(cmd, dest->drv_id);

	if (!smc_p->state_msg)
		smc_p->state_msg = (char *)zalloc(DEF_SMC_PRIV_STATE_MSG_LENGTH);
	if (smc_p->state_msg) {
		/* Re-use 'cmd[]' var */
		snprintf(cmd, sizeof(cmd), "%s", src->media->barcode);
		truncate_spaces(&cmd[0], MAX_BARCODE_LEN + 1);

		snprintf(smc_p->state_msg, DEF_SMC_PRIV_STATE_MSG_LENGTH,
				 "Moving %s from %s slot %d to drive %d",
				 cmd,
				 slot_type_str(src->element_type),
				 slot_number(smc_p->pm, src),
				 slot_number(smc_p->pm, dest->slot));
	}

	if (check_tape_load()) {
		MHVTL_ERR("Load of %s into drive %d failed",
				  cmd, slot_number(smc_p->pm, dest->slot));
		sam_hardware_error(E_MANUAL_INTERVENTION_REQ, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	move_cart(src, dest->slot);
	setDriveFull(dest);
	/* Set the 'Access bit' to zero - i.e. the picker arm can't access it */
	setAccessStatus(dest->slot, 0);

	return retval;
}

static int move_slot2slot(struct smc_priv *smc_p, int src_addr,
						  int dest_addr, uint8_t *sam_stat) {
	struct s_info *src;
	struct s_info *dest;
	char		   cmd[MAX_BARCODE_LEN + 1];
	int			   retval;

	current_state = MHVTL_STATE_MOVING_SLOT_2_SLOT;

	src	 = slot2struct(smc_p, src_addr);
	dest = slot2struct(smc_p, dest_addr);

	MHVTL_DBG(1, "Moving from %s slot %d to %s slot %d",
			  slot_type_str(src->element_type),
			  src->slot_location,
			  slot_type_str(dest->element_type),
			  dest->slot_location);

	if (!slotOccupied(src)) {
		sam_illegal_request(E_MEDIUM_SRC_EMPTY, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (slotOccupied(dest)) {
		sam_illegal_request(E_MEDIUM_DEST_FULL, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (src->element_type == MAP_ELEMENT) {
		if (!map_access_ok(smc_p, src)) {
			MHVTL_DBG(2, "SOURCE MAP port not accessable");
			sam_not_ready(E_MAP_OPEN, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	if (dest->element_type == MAP_ELEMENT) {
		if (!map_access_ok(smc_p, dest)) {
			MHVTL_DBG(2, "DESTINATION MAP port not accessable");
			sam_not_ready(E_MAP_OPEN, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	if (!smc_p->state_msg)
		smc_p->state_msg = zalloc(DEF_SMC_PRIV_STATE_MSG_LENGTH);
	if (smc_p->state_msg) {
		snprintf(cmd, sizeof(cmd), "%s", src->media->barcode);
		truncate_spaces(&cmd[0], MAX_BARCODE_LEN + 1);
		snprintf(smc_p->state_msg, DEF_SMC_PRIV_STATE_MSG_LENGTH,
				 "Moving %s from %s slot %d to %s slot %d",
				 cmd,
				 slot_type_str(src->element_type),
				 slot_number(smc_p->pm, src),
				 slot_type_str(dest->element_type),
				 slot_number(smc_p->pm, dest));
	}

	retval = run_move_command(smc_p, src, dest, sam_stat);
	if (retval)
		return retval;
	move_cart(src, dest);
	return retval;
}

/* Return OK if 'addr' is within either a MAP, Drive or Storage slot */
static int valid_slot(struct smc_priv *smc_p, int addr) {
	struct s_info *slt;
	struct d_info *drv;

	MHVTL_DBG(3, "%s slot %d", slot_type_str(slot_type(smc_p, addr)), addr);
	switch (slot_type(smc_p, addr)) {
	case STORAGE_ELEMENT:
	case MAP_ELEMENT:
	/* SMC-2 6.7 allows a medium transport element as either address,
	 * and the device capabilities page already advertises it.
	 */
	case MEDIUM_TRANSPORT:
		slt = slot2struct(smc_p, addr);
		if (slt)
			return TRUE; /* slot, return true */
		break;
	case DATA_TRANSFER:
		drv = drive2struct(smc_p, addr);
		if (!drv) {
			MHVTL_DBG(1, "No target drive %d in device.conf", addr);
			return FALSE; /* No drive, return false */
		}
		if (drv->drv_id) {
			MHVTL_DBG(3, "Found drive id: %d", (int)drv->drv_id);
			return TRUE; /* Found a drive ID */
		} else {
			MHVTL_ERR("No drive in slot: %d", addr);
		}
		break;
	}
	return FALSE;
}

static int move_drive2slot(struct smc_priv *smc_p,
						   int src_addr, int dest_addr, uint8_t *sam_stat) {
	char		   cmd[MAX_BARCODE_LEN + 1 + 12]; /* 12 being the longest msg string */
	struct d_info *src;
	struct s_info *dest;
	int			   retval;

	current_state = MHVTL_STATE_MOVING_DRIVE_2_SLOT;

	src	 = drive2struct(smc_p, src_addr);
	dest = slot2struct(smc_p, dest_addr);

	if (!driveOccupied(src)) {
		sam_illegal_request(E_MEDIUM_SRC_EMPTY, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (slotOccupied(dest)) {
		sam_illegal_request(E_MEDIUM_DEST_FULL, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (dest->element_type == MAP_ELEMENT) {
		if (!map_access_ok(smc_p, dest)) {
			sam_not_ready(E_MAP_OPEN, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	/* Send any external command before any changes here */
	retval = run_move_command(smc_p, src->slot, dest, sam_stat);
	if (retval)
		return retval;

	/* Send 'unload' message to drive b4 the move.. If not already unloaded */
	if (!slotAccess(src->slot)) {
		sprintf(cmd, "unload %s", src->slot->media->barcode);
		send_msg(cmd, src->drv_id);
		/* Now we wait for the tape device to respond with status of unload */
		if (check_tape_unload()) {
			MHVTL_ERR("Unload of %s from drive %d failed",
					  cmd, slot_number(smc_p->pm, src->slot));
			sam_hardware_error(E_MANUAL_INTERVENTION_REQ, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	if (!smc_p->state_msg)
		smc_p->state_msg = zalloc(DEF_SMC_PRIV_STATE_MSG_LENGTH);
	if (smc_p->state_msg) {
		snprintf(cmd, sizeof(cmd), "%s", src->slot->media->barcode);
		truncate_spaces(&cmd[0], MAX_BARCODE_LEN + 1);
		snprintf(smc_p->state_msg, DEF_SMC_PRIV_STATE_MSG_LENGTH,
				 "Moving %s from drive %d to %s slot %d",
				 cmd,
				 slot_number(smc_p->pm, src->slot),
				 slot_type_str(dest->element_type),
				 slot_number(smc_p->pm, dest));
	}

	move_cart(src->slot, dest);
	setDriveEmpty(src);

	return retval;
}

/* Move media in drive 'src_addr' to drive 'dest_addr' */
static int move_drive2drive(struct smc_priv *smc_p,
							int src_addr, int dest_addr, uint8_t *sam_stat) {
	struct d_info *src;
	struct d_info *dest;
	char		   cmd[MAX_BARCODE_LEN + 12];
	int			   retval;

	current_state = MHVTL_STATE_MOVING_DRIVE_2_DRIVE;

	src	 = drive2struct(smc_p, src_addr);
	dest = drive2struct(smc_p, dest_addr);

	if (src_addr == dest_addr) {
		/* Did not find documentation for this behavior but feels like it should not fail */
		MHVTL_DBG(1, "Same source and destination address : %d : do nothing", src_addr);
		return SAM_STAT_GOOD;
	}

	if (!driveOccupied(src)) {
		sam_illegal_request(E_MEDIUM_SRC_EMPTY, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (driveOccupied(dest)) {
		sam_illegal_request(E_MEDIUM_DEST_FULL, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* Execute any external commands before changing state */
	retval = run_move_command(smc_p, src->slot, dest->slot, sam_stat);
	if (retval)
		return retval;

	/* Send 'unload' message to drive b4 the move.. */
	MHVTL_DBG(2, "Unloading %s from drive %d",
			  src->slot->media->barcode,
			  slot_number(smc_p->pm, src->slot));

	sprintf(cmd, "unload %s", src->slot->media->barcode);
	send_msg(cmd, src->drv_id);
	/* Now we wait for the tape device to respond with status of unload */
	if (check_tape_unload()) {
		MHVTL_ERR("Failed to unload tape from drive %d",
				  slot_number(smc_p->pm, src->slot));
		sam_hardware_error(E_MANUAL_INTERVENTION_REQ, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	move_cart(src->slot, dest->slot);
	setDriveEmpty(src);

	sprintf(cmd, "lload %s", dest->slot->media->barcode);

	truncate_spaces(&cmd[6], MAX_BARCODE_LEN + 1);
	MHVTL_DBG(2, "Sending cmd: \'%s\' to drive %d",
			  cmd, slot_number(smc_p->pm, dest->slot));

	send_msg(cmd, dest->drv_id);

	if (check_tape_load()) {
		/* Failed, so put the tape back where it came from */
		MHVTL_ERR("Failed to move to drive %d, "
				  "placing back into drive %d",
				  slot_number(smc_p->pm, dest->slot),
				  slot_number(smc_p->pm, src->slot));
		move_cart(dest->slot, src->slot);
		sprintf(cmd, "lload %s", src->slot->media->barcode);
		truncate_spaces(&cmd[6], MAX_BARCODE_LEN + 1);
		send_msg(cmd, src->drv_id);
		check_tape_load();
		sam_hardware_error(E_MANUAL_INTERVENTION_REQ, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (!smc_p->state_msg)
		smc_p->state_msg = zalloc(DEF_SMC_PRIV_STATE_MSG_LENGTH);
	if (smc_p->state_msg) {
		/* Re-use 'cmd[]' var */
		snprintf(cmd, sizeof(cmd), "%s", dest->slot->media->barcode);
		truncate_spaces(&cmd[0], MAX_BARCODE_LEN + 1);
		snprintf(smc_p->state_msg, DEF_SMC_PRIV_STATE_MSG_LENGTH,
				 "Moving %s from drive %d to drive %d",
				 cmd,
				 slot_number(smc_p->pm, src->slot),
				 slot_number(smc_p->pm, dest->slot));
	}

	/* Set the 'Access bit' to zero - i.e. the picker arm can't access it */
	setAccessStatus(dest->slot, 0);

	return retval;
}

/* Move a piece of medium from one slot to another */
uint8_t smc_move_medium(struct scsi_cmd *cmd) {
	uint8_t			*cdb	  = cmd->scb;
	uint8_t			*sam_stat = &cmd->dbuf_p->sam_stat;
	int				 transport_addr;
	int				 src_addr, src_type;
	int				 dest_addr, dest_type;
	int				 retval = SAM_STAT_GOOD;
	struct smc_priv *smc_p	= cmd->lu->lu_private;
	struct s_sd		 sd;

	MHVTL_DBG(1, "MOVE MEDIUM (%ld) **", (long)cmd->dbuf_p->serialNo);

	transport_addr = get_unaligned_be16(&cdb[2]);
	src_addr	   = get_unaligned_be16(&cdb[4]);
	dest_addr	   = get_unaligned_be16(&cdb[6]);
	src_type	   = slot_type(smc_p, src_addr);
	dest_type	   = slot_type(smc_p, dest_addr);

	if (cdb[11] & 0xc0) {
		MHVTL_DBG(1, "%s",
				  (cdb[11] & 0x80) ? "  Retract I/O port" : "  Extend I/O port");
	} else {
		MHVTL_DBG(1,
				  "Moving from slot %d to slot %d using transport %d, Invert media: %s",
				  src_addr, dest_addr, transport_addr,
				  (cdb[10]) ? "yes" : "no");
	}

	if (cdb[10] != 0) { /* Can not Invert media */
		MHVTL_ERR("Can not invert media");
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 10;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if ((cdb[11] & 0xc0) == 0xc0) { /* Invalid combo of Extend/retract I/O port */
		MHVTL_ERR("Extend/retract I/O port invalid");
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 11;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	/* Bits 7-6 of the CONTROL byte are vendor specific and are used
	 * here to extend or retract the I/O port, which moves no medium.
	 * Only those two bits may do that: treating the whole byte as the
	 * extension would silently discard a real move.
	 */
	if (cdb[11] & 0xc0)
		return SAM_STAT_GOOD;
	if (cdb[11]) { /* NACA and linking are not supported */
		MHVTL_DBG(2, "cdb[11] : Illegal value of %02x", cdb[11]);
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 11;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (transport_addr == 0)
		transport_addr = smc_p->pm->start_picker;
	if (slot_type(smc_p, transport_addr) != MEDIUM_TRANSPORT) {
		MHVTL_ERR("Can't move media using slot type %d",
				  slot_type(smc_p, transport_addr));
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 2;
		sam_illegal_request(E_INVALID_ELEMENT_ADDR, &sd, sam_stat);
		retval = SAM_STAT_CHECK_CONDITION;
	}
	if (!valid_slot(smc_p, src_addr)) {
		MHVTL_ERR("Invalid source slot: %d", src_addr);
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 4;
		sam_illegal_request(E_INVALID_ELEMENT_ADDR, &sd, sam_stat);
		retval = SAM_STAT_CHECK_CONDITION;
	}
	if (!valid_slot(smc_p, dest_addr)) {
		MHVTL_ERR("Invalid dest slot: %d", dest_addr);
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 6;
		sam_illegal_request(E_INVALID_ELEMENT_ADDR, &sd, sam_stat);
		retval = SAM_STAT_CHECK_CONDITION;
	}

	if (retval == SAM_STAT_GOOD) {
		if (src_type == DATA_TRANSFER && dest_type == DATA_TRANSFER) {
			/* Move between drives */
			retval = move_drive2drive(smc_p, src_addr, dest_addr,
									  sam_stat);
		} else if (src_type == DATA_TRANSFER) {
			retval = move_drive2slot(smc_p, src_addr, dest_addr,
									 sam_stat);
		} else if (dest_type == DATA_TRANSFER) {
			retval = move_slot2drive(smc_p, src_addr, dest_addr,
									 sam_stat);
		} else { /* Move between (non-drive) slots */
			retval = move_slot2slot(smc_p, src_addr, dest_addr,
									sam_stat);
		}
	}

	return retval;
}

/*
 * POSITION TO ELEMENT (2Bh) - SMC-2 6.8
 *
 * Places the transport element where a following MOVE MEDIUM between it
 * and the destination needs no further motion. It moves no medium: there
 * is no source address in the CDB and nothing to carry.
 */
uint8_t smc_position_to_element(struct scsi_cmd *cmd) {
	uint8_t			*cdb	  = cmd->scb;
	uint8_t			*sam_stat = &cmd->dbuf_p->sam_stat;
	struct smc_priv *smc_p	  = cmd->lu->lu_private;
	struct s_sd		 sd;
	int				 transport_addr;
	int				 dest_addr;

	transport_addr = get_unaligned_be16(&cdb[2]);
	dest_addr	   = get_unaligned_be16(&cdb[4]);

	MHVTL_DBG(1, "POSITION TO ELEMENT (%ld) ** transport %d, destination %d",
			  (long)cmd->dbuf_p->serialNo, transport_addr, dest_addr);

	if (!cmd->lu->online) {
		sam_not_ready(NO_ADDITIONAL_SENSE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (cdb[8] & 0x01) { /* Can not invert media */
		MHVTL_ERR("Can not invert media");
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 8;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* A transport address of zero selects the default picker */
	if (transport_addr == 0)
		transport_addr = smc_p->pm->start_picker;

	if (slot_type(smc_p, transport_addr) != MEDIUM_TRANSPORT) {
		MHVTL_ERR("Invalid medium transport address: %d", transport_addr);
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 2;
		sam_illegal_request(E_INVALID_ELEMENT_ADDR, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (!valid_slot(smc_p, dest_addr)) {
		MHVTL_ERR("Invalid destination address: %d", dest_addr);
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 4;
		sam_illegal_request(E_INVALID_ELEMENT_ADDR, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* Nothing to emulate beyond validating the request: the transport
	 * has no position that any other command can observe.
	 */
	return SAM_STAT_GOOD;
}

/*
 * Match a stored volume identifier against a SEND VOLUME TAG template.
 *
 * SMC-2 6.12 defines exactly two wildcards: '?' matches one character
 * and '*' matches any remaining string, after which the rest of the
 * template is unused. Comparison ignores the blank padding both sides
 * carry.
 */
static int svt_match(const char *tmpl, const char *barcode) {
	int t = 0;
	int b = 0;

	while (tmpl[t] && tmpl[t] != ' ') {
		if (tmpl[t] == '*')
			return 1;
		if (!barcode[b] || barcode[b] == ' ')
			return 0;
		if (tmpl[t] != '?' && tmpl[t] != barcode[b])
			return 0;
		t++;
		b++;
	}

	/* Template exhausted: the barcode must be too */
	return (!barcode[b] || barcode[b] == ' ');
}

/* True if the element satisfies the search left by SEND VOLUME TAG */
static int svt_selected(struct smc_priv *smc_p, struct s_info *s) {
	if (smc_p->svt_element_type && s->element_type != smc_p->svt_element_type)
		return 0;
	if (!(s->status & STATUS_Full) || !s->media)
		return 0;

	/* Codes 2h and 6h search alternate volume tags. mhvtl reports no
	 * alternate tag on any element, so nothing can match.
	 */
	if (smc_p->svt_action_code == 0x02 || smc_p->svt_action_code == 0x06)
		return 0;

	return svt_match(smc_p->svt_template, s->media->barcode);
}

/*
 * SEND VOLUME TAG (B6h) - SMC-2 6.12
 *
 * Only the translate (search) functions are implemented. Assert,
 * replace and undefine are optional in SMC-2, and in mhvtl the barcode
 * is the medium's identity - the data file is named for it - so letting
 * an initiator rewrite it would detach the element from its media.
 */
uint8_t smc_send_volume_tag(struct scsi_cmd *cmd) {
	uint8_t			*cdb	  = cmd->scb;
	uint8_t			*buf	  = (uint8_t *)cmd->dbuf_p->data;
	uint8_t			*sam_stat = &cmd->dbuf_p->sam_stat;
	struct smc_priv *smc_p	  = cmd->lu->lu_private;
	struct s_sd		 sd;
	uint8_t			 type	   = cdb[1] & 0x0f;
	uint8_t			 action	   = cdb[5] & 0x1f;
	uint16_t		 param_len = get_unaligned_be16(&cdb[8]);
	int				 count;
	int				 i;

	MHVTL_DBG(1, "SEND VOLUME TAG (%ld) ** type %d, action 0x%02x",
			  (long)cmd->dbuf_p->serialNo, type, action);

	if (!cmd->lu->online) {
		sam_not_ready(NO_ADDITIONAL_SENSE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (type > DATA_TRANSFER) {
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* Translate codes are 0h-2h and 4h-6h; 3h and 7h are reserved and
	 * 8h upwards modify the stored tag.
	 */
	if (action > 0x06 || action == 0x03 || action == 0x07) {
		MHVTL_DBG(1, "Send action code 0x%02x not supported", action);
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 5;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (param_len < SVT_TEMPLATE_LEN) {
		sam_illegal_request(E_PARAMETER_LIST_LENGTH_ERR, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	cmd->dbuf_p->sz = param_len;
	count			= retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);
	if (count < SVT_TEMPLATE_LEN) {
		sam_illegal_request(E_PARAMETER_LIST_LENGTH_ERR, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	memcpy(smc_p->svt_template, buf, SVT_TEMPLATE_LEN);
	smc_p->svt_template[SVT_TEMPLATE_LEN] = '\0';

	/* Trim the blank padding so the match does not have to */
	for (i = SVT_TEMPLATE_LEN - 1; i >= 0 && smc_p->svt_template[i] == ' '; i--)
		smc_p->svt_template[i] = '\0';

	smc_p->svt_action_code	= action;
	smc_p->svt_element_type = type;
	if (count >= 40) {
		smc_p->svt_min_seq = get_unaligned_be16(&buf[34]);
		smc_p->svt_max_seq = get_unaligned_be16(&buf[38]);
	} else {
		smc_p->svt_min_seq = 0;
		smc_p->svt_max_seq = 0;
	}

	MHVTL_DBG(2, "Volume tag search template: \'%s\'", smc_p->svt_template);

	return SAM_STAT_GOOD;
}

/*
 * REQUEST VOLUME ELEMENT ADDRESS (B5h) - SMC-2 6.11
 *
 * Reports the elements matching the search left by the last SEND VOLUME
 * TAG translate. The response is the READ ELEMENT STATUS layout with one
 * changed header byte, so fill_ed() and the page header are reused.
 */
uint8_t smc_request_volume_element_address(struct scsi_cmd *cmd) {
	uint8_t			 *cdb	   = cmd->scb;
	uint8_t			 *sam_stat = &cmd->dbuf_p->sam_stat;
	struct smc_priv	 *smc_p	   = cmd->lu->lu_private;
	uint8_t			 *p;
	struct s_info	 *sp;
	struct s_sd		  sd;
	uint16_t		  start	  = get_unaligned_be16(&cdb[2]);
	uint16_t		  req_num = get_unaligned_be16(&cdb[4]);
	uint32_t		  alloc_len;
	uint32_t		  byte_count = 0;
	uint16_t		  first		 = 0;
	uint16_t		  reported	 = 0;
	uint8_t			 *page_hdr	 = NULL;
	uint8_t			  page_type	 = 0;
	uint16_t		  page_count = 0;
	uint32_t		  element_sz;

	alloc_len = get_unaligned_be24(&cdb[7]);

	MHVTL_DBG(1, "REQUEST VOLUME ELEMENT ADDRESS (%ld) **",
			  (long)cmd->dbuf_p->serialNo);

	if (!cmd->lu->online) {
		sam_not_ready(NO_ADDITIONAL_SENSE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (cdb[11] != 0) { /* CONTROL byte - NACA and linking unsupported */
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 11;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	p		  = (uint8_t *)cmd->dbuf_p->data;
	alloc_len = min(alloc_len, smc_p->bufsize);
	memset(p, 0, alloc_len);

	/* Reserve the 8 byte volume element address header */
	byte_count = 8;

	list_for_each_entry(sp, &smc_p->slot_list, siblings) {
		if (sp->slot_location < start)
			continue;
		/* As in READ ELEMENT STATUS, this is a maximum and zero means
		 * report nothing.
		 */
		if (reported >= req_num)
			break;
		if (!svt_selected(smc_p, sp))
			continue;

		/* Elements are grouped into one page per element type. The
		 * slot list is ordered by type, so a change of type closes
		 * the current page.
		 */
		if (!page_hdr || sp->element_type != page_type) {
			if (page_hdr)
				fill_element_status_page_hdr(cmd, page_hdr,
											 page_count, page_type);
			page_hdr   = p + byte_count;
			page_type  = sp->element_type;
			page_count = 0;
			byte_count += 8;
		}

		element_sz = fill_ed(cmd, p + byte_count, sp);
		byte_count += element_sz;
		page_count++;
		if (!reported)
			first = sp->slot_location;
		reported++;
	}

	if (page_hdr)
		fill_element_status_page_hdr(cmd, page_hdr, page_count, page_type);

	/* Volume element address header, SMC-2 table 23. Same as the
	 * element status data header except byte 4, which echoes the send
	 * action code of the search being reported.
	 */
	put_unaligned_be16(first, &p[0]);
	put_unaligned_be16(reported, &p[2]);
	p[4] = smc_p->svt_action_code & 0x1f;
	put_unaligned_be24(byte_count - 8, &p[5]);

	cmd->dbuf_p->sz = min(byte_count, alloc_len);

	MHVTL_DBG(2, "Reporting %d element%s, %d bytes",
			  reported, (reported == 1) ? "" : "s", byte_count);

	return SAM_STAT_GOOD;
}

/*
 * READ BUFFER (3Ch) - SPC-5 6.18
 *
 * Descriptor and data mode against a single logical unit buffer. That is
 * what the vendor references document for a changer, and it is enough
 * for a host to probe the buffer before a WRITE BUFFER download.
 */
uint8_t smc_read_buffer(struct scsi_cmd *cmd) {
	uint8_t	   *cdb		  = cmd->scb;
	uint8_t	   *buf		  = (uint8_t *)cmd->dbuf_p->data;
	uint8_t	   *sam_stat  = &cmd->dbuf_p->sam_stat;
	struct s_sd sd;
	uint8_t		mode	  = cdb[1] & 0x1f;
	uint8_t		buffer_id = cdb[2];
	uint32_t	offset	  = get_unaligned_be24(&cdb[3]);
	uint32_t	alloc_len = get_unaligned_be24(&cdb[6]);
	uint32_t	len;

	MHVTL_DBG(1, "READ BUFFER (%ld) ** mode 0x%02x, buffer %d",
			  (long)cmd->dbuf_p->serialNo, mode, buffer_id);

	if (buffer_id != 0) { /* Only one buffer is defined */
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 2;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	switch (mode) {
	case 0x02: /* Data */
		if (offset >= SMC_READ_BUFFER_SZ) {
			sd.byte0		 = SKSV | CD;
			sd.field_pointer = 3;
			sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		len = min(alloc_len, SMC_READ_BUFFER_SZ - offset);
		memset(buf, 0, len);
		cmd->dbuf_p->sz = len;
		break;

	case 0x03: /* Descriptor */
		len = min(alloc_len, (uint32_t)4);
		memset(buf, 0, 4);
		buf[0] = 0; /* Offset boundary: no alignment restriction */
		put_unaligned_be24(SMC_READ_BUFFER_SZ, &buf[1]);
		cmd->dbuf_p->sz = len;
		break;

	default:
		MHVTL_DBG(1, "READ BUFFER mode 0x%02x not supported", mode);
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	return SAM_STAT_GOOD;
}

uint8_t smc_rezero(struct scsi_cmd *cmd) {
	MHVTL_DBG(1, "REZERO (%ld) **", (long)cmd->dbuf_p->serialNo);

	if (!cmd->lu->online) {
		sam_not_ready(NO_ADDITIONAL_SENSE, &cmd->dbuf_p->sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	sleep(1);
	return SAM_STAT_GOOD;
}

uint8_t smc_open_close_import_export_element(struct scsi_cmd *cmd) {
	uint8_t			*cdb	  = cmd->scb;
	struct smc_priv *smc_p	  = cmd->lu->lu_private;
	uint8_t			*sam_stat = &cmd->dbuf_p->sam_stat;
	int				 addr;
	int				 action_code;
	struct s_sd		 sd;

	MHVTL_DBG(1, "OPEN/CLOSE IMPORT/EXPORT ELEMENT (%ld) **",
			  (long)cmd->dbuf_p->serialNo);

	addr		= get_unaligned_be16(&cdb[2]);
	action_code = cdb[4] & 0x1f;
	MHVTL_DBG(2, "addr: %d action_code: %d", addr, action_code);

	if (slot_type(smc_p, addr) != MAP_ELEMENT) {
		sam_illegal_request(E_INVALID_ELEMENT_ADDR, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	switch (action_code) {
	case 0: /* open */
		if (smc_p->cap_closed == CAP_CLOSED) {
			MHVTL_DBG(2, "opening CAP");
			smc_p->cap_closed = CAP_OPEN;
		}
		break;
	case 1: /* close */
		if (smc_p->cap_closed == CAP_OPEN) {
			MHVTL_DBG(2, "closing CAP");
			smc_p->cap_closed = CAP_CLOSED;
		}
		break;
	default:
		MHVTL_DBG(1, "unknown action code: %d", action_code);
		sd.byte0		 = SKSV | CD;
		sd.field_pointer = 4;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	return SAM_STAT_GOOD;
}

uint8_t smc_log_sense(struct scsi_cmd *cmd) {
	struct lu_phy_attr *lu;
	uint8_t			   *b	= cmd->dbuf_p->data;
	uint8_t			   *cdb = cmd->scb;
	uint8_t			   *sam_stat;
	int					retval;
	int					i;
	uint16_t			alloc_len;
	struct list_head   *l_head;
	struct log_pg_list *l;
	struct s_sd			sd;

	MHVTL_DBG(1, "LOG SENSE (%ld) **", (long)cmd->dbuf_p->serialNo);

	alloc_len		= get_unaligned_be16(&cdb[7]);
	cmd->dbuf_p->sz = alloc_len;

	lu		 = cmd->lu;
	sam_stat = &cmd->dbuf_p->sam_stat;
	l_head	 = &lu->log_pg;
	retval	 = 0;

	switch (cdb[2] & 0x3f) {
	case 0: /* Send supported pages */
		MHVTL_DBG(1, "LOG SENSE: Sending supported pages");
		memset(b, 0, 4); /* Clear first few (4) bytes */
		i	   = 4;
		b[i++] = 0; /* b[0] is log page '0' (this one) */
		list_for_each_entry(l, l_head, siblings) {
			MHVTL_DBG(3, "found page 0x%02x", l->log_page_num);
			b[i] = l->log_page_num;
			i++;
		}
		put_unaligned_be16(i - 4, &b[2]);
		retval = i;
		break;
	case TEMPERATURE_PAGE: /* Temperature page */
		MHVTL_DBG(1, "LOG SENSE: Temperature page");
		l = lookup_log_pg(&lu->log_pg, TEMPERATURE_PAGE, NO_SUBPAGE);
		if (!l)
			goto log_page_not_found;

		b	   = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	case TAPE_ALERT: /* TapeAlert page */
		MHVTL_DBG(1, "LOG SENSE: TapeAlert page");
		/*		MHVTL_DBG(2, " Returning TapeAlert flags: 0x%" PRIx64,
						get_unaligned_be64(&SequentialAccessDevice_pg.TapeAlert));
		*/

		l = lookup_log_pg(&lu->log_pg, TAPE_ALERT, NO_SUBPAGE);
		if (!l)
			goto log_page_not_found;

		b	   = memcpy(b, l->p, l->size);
		retval = l->size;

		/* Clear flags after value read. */
		if (alloc_len > 4)
			update_TapeAlert(TA_NONE);
		else
			MHVTL_DBG(1, "TapeAlert : Alloc len short -"
						 " Not clearing TapeAlert flags.");
		break;
	default:
		MHVTL_DBG(1, "LOG SENSE: Unknown code: 0x%x", cdb[2] & 0x3f);
		goto log_page_not_found;
		break;
	}
	cmd->dbuf_p->sz = retval;

	return SAM_STAT_GOOD;

log_page_not_found:
	cmd->dbuf_p->sz	 = 0;
	sd.byte0		 = SKSV | CD;
	sd.field_pointer = 2;
	sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}

void unload_drive_on_shutdown(struct s_info *src, struct s_info *dest) {
	if (!dest)
		return;

	MHVTL_DBG(1, "Force unload of media %s to slot %d",
			  src->media->barcode, dest->slot_location);
	move_cart(src, dest);
}
