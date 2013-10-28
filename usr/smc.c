/*
 * This handles any SCSI OP codes defined in the standards as 'MEDIUM CHANGER'
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
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
 */

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
#include <assert.h>
#include "be_byteshift.h"
#include "scsi.h"
#include "list.h"
#include "vtl_common.h"
#include "vtllib.h"
#include "logging.h"
#include "smc.h"
#include "q.h"
#include "log.h"
#include "subprocess.h"

int current_state;

uint8_t smc_allow_removal(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "%s MEDIUM REMOVAL (%ld) **",
				(cmd->scb[4]) ? "PREVENT" : "ALLOW",
				(long)cmd->dbuf_p->serialNo);
	return SAM_STAT_GOOD;
}

uint8_t smc_initialize_element_status(struct scsi_cmd *cmd)
{
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

uint8_t smc_initialize_element_status_with_range(struct scsi_cmd *cmd)
{
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
static int slot_type(struct smc_priv *smc_p, int addr)
{
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
static int slot_number(struct smc_personality_template *pm, struct s_info *sp)
{
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
static struct s_info *slot2struct(struct smc_priv *smc_p, int addr)
{
	struct list_head *slot_head;
	struct s_info *sp;

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
static struct d_info *drive2struct(struct smc_priv *smc_p, int addr)
{
	struct s_info *s;

	s = slot2struct(smc_p, addr);
	if (s)
		return s->drive;

	return NULL;
}

/* Returns true if slot has media in it */
int slotOccupied(struct s_info *s)
{
	return s->status & STATUS_Full;
}

/* Returns true if drive has media in it */
static int driveOccupied(struct d_info *d)
{
	return slotOccupied(d->slot);
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
void setImpExpStatus(struct s_info *s, int flg)
{
	if (flg)
		s->status |= STATUS_ImpExp;
	else
		s->status &= ~STATUS_ImpExp;
}

/*
 * Sets the 'Full' bit true/false in the status field
 */
void setFullStatus(struct s_info *s, int flg)
{
	if (flg)
		s->status |= STATUS_Full;
	else
		s->status &= ~STATUS_Full;
}

void setSlotEmpty(struct s_info *s)
{
	setFullStatus(s, 0);
}

static void setDriveEmpty(struct d_info *d)
{
	setFullStatus(d->slot, 0);
}

void setSlotFull(struct s_info *s)
{
	setFullStatus(s, 1);
}

void setDriveFull(struct d_info *d)
{
	setFullStatus(d->slot, 1);
}

/* Returns 1 (true) if slot is MAP slot */
static int is_map_slot(struct s_info *s)
{
	MHVTL_DBG(2, "slot type %d: %s", s->element_type,
			(s->element_type == MAP_ELEMENT) ? "MAP" : "NOT A MAP");
	return s->element_type == MAP_ELEMENT;
}

static int map_access_ok(struct smc_priv *smc_p, struct s_info *s)
{
	if (is_map_slot(s)) {
		MHVTL_DBG(3, "Returning status of %d", smc_p->cap_closed);
		return smc_p->cap_closed;
	}
	MHVTL_DBG(3, "Returning 0");
	return 0;
}

static int dump_element_desc(uint8_t *p, int voltag, int num_elem, int len,
				char dvcid_serial_only)
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
		MHVTL_DBG(3, "  Code Set                     : 0x%02x",
					p[i] & 0xf);
		MHVTL_DBG(3, "  Identifier type              : 0x%02x",
					p[i + 1] & 0xf);
		idlen = p[i + 3];
		MHVTL_DBG(3, "  Identifier length            : %d", idlen);
		if (idlen) {
			if (dvcid_serial_only) {
				MHVTL_DBG(3,
					"  ASCII data                   : %10s",
							&p[i + 4]);
			} else {
				MHVTL_DBG(3,
					"  ASCII data                   : %8s",
							&p[i + 4]);
				MHVTL_DBG(3,
					"  ASCII data                   : %16s",
							&p[i + 12]);
				MHVTL_DBG(3,
					"  ASCII data                   : %10s",
							&p[i + 28]);
			}
		}
		i = (j + 1) * len;
	}
	return i;
}

static void decode_element_status(struct smc_priv *smc_p, uint8_t *p)
{
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

	fflush(NULL);
}

/*
 * Calculate length of one element
 */
static int sizeof_element(struct scsi_cmd *cmd, int type)
{
	struct smc_priv *smc_p = (struct smc_priv *)cmd->lu->lu_private;
	int dvcid;
	int voltag;

	voltag = (cmd->scb[1] & 0x10) >> 4;
	dvcid = cmd->scb[6] & 0x01;	/* Device ID */

	return 16 + (voltag ? VOLTAG_LEN : 0) +
		(dvcid && (type == DATA_TRANSFER) ? smc_p->pm->dvcid_len : 0);
}

/*
 * Fill in a single element descriptor
 *
 * Returns number of bytes in element data.
 */
static int fill_element_descriptor(struct scsi_cmd *cmd, uint8_t *p,
						struct s_info *s)
{
	struct smc_priv *smc_p = (struct smc_priv *)cmd->lu->lu_private;
	struct d_info *d = NULL;
	int j = 0;
	uint8_t voltag;
	uint8_t dvcid;

	voltag = (cmd->scb[1] & 0x10) >> 4;
	dvcid = cmd->scb[6] & 0x01;	/* Device ID */

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
	p[j++] = (s->asc_ascq >> 8) & 0xff;  /* Additional Sense Code */
	p[j++] = s->asc_ascq & 0xff; /* Additional Sense Code Qualifer */

	p[j++] = 0;		/* Reserved */
	if (s->element_type == DATA_TRANSFER)
		p[j++] = d->SCSI_ID;
	else
		p[j++] = 0;	/* Reserved */

	p[j++] = 0;	/* Reserved */

	/* bit 8 set if Source Storage Element is valid | s->occupied */
	if (s->media)
		p[j] = (s->media->last_location > 0) ? 0x80 : 0;
	else
		p[j] = 0;

	/* Ref: smc3r12 - Table 28
	 * 0 - empty,
	 * 1 - data,
	 * 2 - cleaning tape,
	 * 3 - Cleaning,
	 * 4 - WORM,
	 * 5 - Microcode image medium
	 */
	if (s->media)
		p[j] |= (s->media->cart_type & 0x0f);

	j++;

	/* Source Storage Element Address */
	put_unaligned_be16(s->last_location, &p[j]);
	j += 2;

	MHVTL_DBG(2, "Slot location: %d, DVCID: %d, VOLTAG: %d",
			s->slot_location, dvcid, voltag);

	if (voltag) {
		/* Barcode with trailing space(s) */
		if (s->status & STATUS_Full) {
			if (!(s->media->internal_status & INSTATUS_NO_BARCODE))
				blank_fill(&p[j], s->media->barcode, VOLTAG_LEN);
			else
				memset(&p[j], 0, VOLTAG_LEN);
		} else
			memset(&p[j], 0, VOLTAG_LEN);

		j += VOLTAG_LEN;	/* Account for barcode */
	}

	if (dvcid && s->element_type == DATA_TRANSFER) {
		p[j++] = 2;	/* Code set 2 = ASCII */
		p[j++] = 1;	/* Identifier type */
		p[j++] = 0;	/* Reserved */
		p[j++] = smc_p->pm->dvcid_len;	/* Identifier Length */
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
		p[j++] = 0;	/* Reserved */
		p[j++] = 0;	/* Reserved */
		p[j++] = 0;	/* Reserved */
		p[j++] = 0;	/* Reserved */
	}
	MHVTL_DBG(3, "Returning %d (0x%02x) bytes", j, j);

return j;
}

/*
 * Fill in element status page Header (8 bytes)
 */
static void fill_element_status_page_hdr(struct scsi_cmd *cmd, uint8_t *p,
					uint16_t element_count,
					uint8_t type)
{
	int element_sz;
	uint32_t element_len;
	uint8_t voltag;

	voltag = (cmd->scb[1] & 0x10) >> 4;

	element_sz = sizeof_element(cmd, type);

	p[0] = type;	/* Element type Code */

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
	MHVTL_DBG(3, "  First element Address    : %d (0x%02x)",
					get_unaligned_be16(&p[0]),
					get_unaligned_be16(&p[0]));
	MHVTL_DBG(3, "  Number elements reported : %d (0x%02x)",
					get_unaligned_be16(&p[2]),
					get_unaligned_be16(&p[2]));
	MHVTL_DBG(3, "  Total byte count         : %d (0x%04x)",
					get_unaligned_be32(&p[4]),
					get_unaligned_be32(&p[4]));

return 8;	/* Header is 8 bytes in size.. */
}

/* Returns address of first available elements from starting number */
static uint32_t find_first_matching_element(struct smc_priv *priv,
						uint32_t start,
						uint8_t type)
{
	struct list_head *slot_head;
	struct s_info *sp;

	slot_head = &priv->slot_list;

	list_for_each_entry(sp, slot_head, siblings) {
		if (!type) {	/* Element type not defined */
			if (sp->slot_location >= start)
				return sp->slot_location;
		} else if (sp->element_type == type) {
			if (sp->slot_location >= start)
				return sp->slot_location;
		}
	}
	return 0;
}

/* Returns number of available elements left from starting number */
static uint32_t num_available_elements(struct smc_priv *priv, uint8_t type,
					uint32_t start, uint32_t max)
{
	struct list_head *slot_head;
	struct s_info *sp;
	unsigned int counted = 0;

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

	MHVTL_DBG(2, "Determing %d element%s of type %s starting at %d"
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
				uint16_t residual)
{
	struct smc_priv *smc_p;
	int j;
	uint8_t *cdb = cmd->scb;
	struct s_info *sp;

	uint16_t max_count;	/* Max element count */
	uint16_t avail_count;
	uint32_t element_sz;
	uint16_t begin_element;
	int slot_count;

	max_count = get_unaligned_be16(&cdb[4]);
	if (max_count == 0)
		max_count = ~0;

	slot_count = max_count - residual;
	if (slot_count <= 0)
		return 0;	/* No more slots to report on */

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
	if (begin_element == 0) {
		MHVTL_DBG(1, "Start element is still 0, line %d", __LINE__);
		return 0;
	}

	avail_count =  num_available_elements(smc_p, type, start, max_count);

	MHVTL_DBG(3, "Available count: %d, type: %d", avail_count, type);

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
				continue;	/* Don't report on this one */
		} else {
			/* Any type.. Need to fill in one type at a time */
			if (sp->slot_location == start)
				type = sp->element_type;
		}
		element_sz = fill_element_descriptor(cmd, p, sp);
		avail_count += element_sz;	/* inc byte count */
		p += element_sz;		/* inc pointer into dest buf */
		MHVTL_DBG(3, "Count: %d, max_count: %d, slot: %d",
				j, max_count, sp->slot_location);
		j++;
		if (j > max_count)
			break;
	}

return avail_count;
}

/*
 * Build READ ELEMENT STATUS data.
 *
 * Returns number of bytes to xfer back to host.
 */
uint8_t smc_read_element_status(struct scsi_cmd *cmd)
{
	struct smc_priv *smc_p = (struct smc_priv *)cmd->lu->lu_private;
	uint8_t *cdb = cmd->scb;
	uint8_t *p;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t	type = cdb[1] & 0x0f;
	uint16_t sum;
	uint16_t req_start_elem;
	uint16_t req_number;	/* Num of elements initiator requested */
	uint32_t alloc_len;	/* Amount of space initiator has pre alloc */
	uint16_t start;		/* First valid slot location */
	uint16_t start_any;	/* First valid slot location - temp count */
	uint32_t elem_byte_count;
	uint32_t byte_count;
	uint32_t cur_count;
	struct s_sd sd;
#ifdef MHVTL_DEBUG
	uint8_t	voltag = (cdb[1] & 0x10) >> 4;
	uint8_t	dvcid = cdb[6] & 0x01;	/* Device ID */
#endif

	MHVTL_DBG(1, "READ ELEMENT STATUS (%ld) **",
				(long)cmd->dbuf_p->serialNo);

	req_start_elem = get_unaligned_be16(&cdb[2]);
	req_number = get_unaligned_be16(&cdb[4]);
	alloc_len = 0xffffff & get_unaligned_be32(&cdb[6]);

	MHVTL_DBG(3, " Element type(%d) => %s", type, slot_type_str(type));
	MHVTL_DBG(3, "  Starting Element Address: %d", req_start_elem);
	MHVTL_DBG(3, "  Number of Elements      : %d", req_number);
	MHVTL_DBG(3, "  Allocation length       : %d (0x%04x)",
						 alloc_len, alloc_len);
	MHVTL_DBG(3, "  Device ID: %s, voltag: %s",
					(dvcid == 0) ? "No" :  "Yes",
					(voltag == 0) ? "No" :  "Yes");

	p = (uint8_t *)cmd->dbuf_p->data;

	/* Set alloc_len to smallest value */
	alloc_len = min(alloc_len, smc_p->bufsize);

	cmd->dbuf_p->sz = 0;

	/* Init buffer */
	memset(p, 0, alloc_len);

	if (cdb[11] != 0x0) {	/* Reserved byte.. */
		MHVTL_DBG(3, "cdb[11] : Illegal value");
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 11;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* Find first matching slot number which matches the type. */
	start = find_first_matching_element(smc_p, req_start_elem, type);
	if (start == 0) {	/* Nothing found.. */
		MHVTL_DBG(1, "Start element is still 0, line %d", __LINE__);
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
		start_any = start;

		if (slot_type(smc_p, start_any) == DATA_TRANSFER) {
			byte_count = fill_element_page(cmd, p, start_any,
							DATA_TRANSFER, sum);
			elem_byte_count += byte_count;
			p += byte_count;
			start_any = smc_p->pm->start_drive;
			sum = byte_count /
				sizeof_element(cmd, DATA_TRANSFER);
		}
		if (slot_type(smc_p, start_any) == MEDIUM_TRANSPORT) {
			byte_count = fill_element_page(cmd, p, start_any,
							MEDIUM_TRANSPORT, sum);
			elem_byte_count += byte_count;
			p += byte_count;
			start_any = smc_p->pm->start_picker;
			sum += byte_count /
				sizeof_element(cmd, MEDIUM_TRANSPORT);
		}
		if (slot_type(smc_p, start_any) == MAP_ELEMENT) {
			byte_count = fill_element_page(cmd, p, start_any,
							MAP_ELEMENT, sum);
			elem_byte_count += byte_count;
			p += byte_count;
			start_any = smc_p->pm->start_map;
			sum += byte_count /
				sizeof_element(cmd, MAP_ELEMENT);
		}
		if (slot_type(smc_p, start_any) == STORAGE_ELEMENT) {
			byte_count = fill_element_page(cmd, p, start_any,
							STORAGE_ELEMENT, sum);
			elem_byte_count += byte_count;
			p += byte_count;
		}
		break;
	default:	/* Illegal descriptor type. */
		sd.byte0 = SKSV | CD;
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

	cmd->dbuf_p->sz = min(elem_byte_count + 8, alloc_len);

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
static int check_tape_load(void)
{
	int mlen, r_qid;
	struct q_entry q;

	/* Initialise message queue as necessary */
	r_qid = init_queue();
	if (r_qid == -1) {
		printf("Could not initialise message queue\n");
		exit(1);
	}

	mlen = msgrcv(r_qid, &q, MAXOBN, my_id, MSG_NOERROR);
	if (mlen > 0)
		MHVTL_DBG(2, "Received \"%s\" from message Q", q.msg.text);

	return strncmp("Loaded OK", q.msg.text, 9);
}

/*
 * Logically move information from 'src' address to 'dest' address
 */
static void move_cart(struct s_info *src, struct s_info *dest)
{

	dest->media = src->media;

	dest->last_location = src->slot_location;
	dest->media->last_location = src->slot_location;

	setSlotFull(dest);
	if (is_map_slot(dest))
		setImpExpStatus(dest, ROBOT_ARM); /* Placed by robot arm */

	src->media = NULL;
	src->last_location = 0;		/* Forget where the old media was */
	setSlotEmpty(src);		/* Clear Full bit */
}

static int run_move_command(struct smc_priv *smc_p, struct s_info *src,
			struct s_info *dest, uint8_t *sam_stat)
{
	char *movecommand;
	char barcode[MAX_BARCODE_LEN + 1];
	int res = 0;
	int cmdlen;

	if (!smc_p->movecommand) {
		/* no command: do nothing */
		return SAM_STAT_GOOD;
	}

	cmdlen = strlen(smc_p->movecommand)+MAX_BARCODE_LEN+4*10;
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
			barcode
	);
	res = run_command(movecommand, smc_p->commandtimeout);
	if (res) {
		MHVTL_ERR("move command returned %d", res);
		sam_hardware_error(E_MANUAL_INTERVENTION_REQ, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	return SAM_STAT_GOOD;
}

static int move_slot2drive(struct smc_priv *smc_p,
		 int src_addr, int dest_addr, uint8_t *sam_stat)
{
	struct s_info *src;
	struct d_info *dest;
	char cmd[MAX_BARCODE_LEN + 12];
	int retval;

	current_state = MHVTL_STATE_MOVING_SLOT_2_DRIVE;

	src  = slot2struct(smc_p, src_addr);
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
		sprintf(cmd, "%s", src->media->barcode);
		truncate_spaces(&cmd[0], MAX_BARCODE_LEN + 1);

		sprintf(smc_p->state_msg,
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

	retval = run_move_command(smc_p, src, dest->slot, sam_stat);
	if (retval)
		return retval;
	move_cart(src, dest->slot);
	setDriveFull(dest);

	return retval;
}

static int move_slot2slot(struct smc_priv *smc_p, int src_addr,
			int dest_addr, uint8_t *sam_stat)
{
	struct s_info *src;
	struct s_info *dest;
	char cmd[MAX_BARCODE_LEN + 1];
	int retval;

	current_state = MHVTL_STATE_MOVING_SLOT_2_SLOT;

	src  = slot2struct(smc_p, src_addr);
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
		smc_p->state_msg = zalloc(64);
	if (smc_p->state_msg) {
		sprintf(cmd, "%s", src->media->barcode);
		truncate_spaces(&cmd[0], MAX_BARCODE_LEN + 1);
		sprintf(smc_p->state_msg,
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
static int valid_slot(struct smc_priv *smc_p, int addr)
{
	struct s_info *slt;
	struct d_info *drv;

	MHVTL_DBG(3, "%s slot %d", slot_type_str(slot_type(smc_p, addr)), addr);
	switch (slot_type(smc_p, addr)) {
	case STORAGE_ELEMENT:
	case MAP_ELEMENT:
		slt  = slot2struct(smc_p, addr);
		if (slt)
			return TRUE;	/* slot, return true */
		break;
	case DATA_TRANSFER:
		drv  = drive2struct(smc_p, addr);
		if (!drv) {
			MHVTL_DBG(1, "No target drive %d in device.conf", addr);
			return FALSE;	/* No drive, return false */
		}
		if (drv->drv_id) {
			MHVTL_DBG(3, "Found drive id: %d", (int)drv->drv_id);
			return TRUE;	/* Found a drive ID */
		} else {
			MHVTL_ERR("No drive in slot: %d", addr);
		}
		break;
	}
	return FALSE;
}

static int move_drive2slot(struct smc_priv *smc_p,
			int src_addr, int dest_addr, uint8_t *sam_stat)
{
	char cmd[MAX_BARCODE_LEN + 1];
	struct d_info *src;
	struct s_info *dest;
	int retval;

	current_state = MHVTL_STATE_MOVING_DRIVE_2_SLOT;

	src  = drive2struct(smc_p, src_addr);
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

	/* Send 'unload' message to drive b4 the move.. */
	send_msg("unload", src->drv_id);

	if (!smc_p->state_msg)
		smc_p->state_msg = zalloc(64);
	if (smc_p->state_msg) {
		sprintf(cmd, "%s", src->slot->media->barcode);
		truncate_spaces(&cmd[0], MAX_BARCODE_LEN + 1);
		sprintf(smc_p->state_msg,
			"Moving %s from drive %d to %s slot %d",
					cmd,
					slot_number(smc_p->pm, src->slot),
					slot_type_str(dest->element_type),
					slot_number(smc_p->pm, dest));
	}

	retval = run_move_command(smc_p, src->slot, dest, sam_stat);
	if (retval)
		return retval;
	move_cart(src->slot, dest);
	setDriveEmpty(src);

return retval;
}

/* Move media in drive 'src_addr' to drive 'dest_addr' */
static int move_drive2drive(struct smc_priv *smc_p,
			int src_addr, int dest_addr, uint8_t *sam_stat)
{
	struct d_info *src;
	struct d_info *dest;
	char cmd[MAX_BARCODE_LEN + 12];
	int retval;

	current_state = MHVTL_STATE_MOVING_DRIVE_2_DRIVE;

	src  = drive2struct(smc_p, src_addr);
	dest = drive2struct(smc_p, dest_addr);

	if (!driveOccupied(src)) {
		sam_illegal_request(E_MEDIUM_SRC_EMPTY, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (driveOccupied(dest)) {
		sam_illegal_request(E_MEDIUM_DEST_FULL, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* Send 'unload' message to drive b4 the move.. */
	MHVTL_DBG(2, "Unloading %s from drive %d",
				src->slot->media->barcode,
				slot_number(smc_p->pm, src->slot));

	send_msg("unload", src->drv_id);

	retval = run_move_command(smc_p, src->slot, dest->slot, sam_stat);
	if (retval)
		return retval;
	move_cart(src->slot, dest->slot);

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
		smc_p->state_msg = zalloc(64);
	if (smc_p->state_msg) {
		/* Re-use 'cmd[]' var */
		sprintf(cmd, "%s", dest->slot->media->barcode);
		truncate_spaces(&cmd[0], MAX_BARCODE_LEN + 1);
		sprintf(smc_p->state_msg,
			"Moving %s from drive %d to drive %d",
					cmd,
					slot_number(smc_p->pm, src->slot),
					slot_number(smc_p->pm, dest->slot));
	}

return retval;
}

/* Move a piece of medium from one slot to another */
uint8_t smc_move_medium(struct scsi_cmd *cmd)
{
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	int transport_addr;
	int src_addr, src_type;
	int dest_addr, dest_type;
	int retval = SAM_STAT_GOOD;
	struct smc_priv *smc_p = cmd->lu->lu_private;
	struct s_sd sd;

	MHVTL_DBG(1, "MOVE MEDIUM (%ld) **", (long)cmd->dbuf_p->serialNo);

	transport_addr = get_unaligned_be16(&cdb[2]);
	src_addr  = get_unaligned_be16(&cdb[4]);
	dest_addr = get_unaligned_be16(&cdb[6]);
	src_type = slot_type(smc_p, src_addr);
	dest_type = slot_type(smc_p, dest_addr);

	if (cdb[11] & 0xc0) {
		MHVTL_DBG(1, "%s",
			(cdb[11] & 0x80) ? "  Retract I/O port" :
					   "  Extend I/O port");
	} else {
		MHVTL_DBG(1,
	 "Moving from slot %d to slot %d using transport %d, Invert media: %s",
				src_addr, dest_addr, transport_addr,
				(cdb[10]) ? "yes" : "no");
	}

	if (cdb[10] != 0) {	/* Can not Invert media */
		MHVTL_ERR("Can not invert media");
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 10;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (cdb[11] == 0xc0) {	/* Invalid combo of Extend/retract I/O port */
		MHVTL_ERR("Extend/retract I/O port invalid");
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 11;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (cdb[11]) /* Must be an Extend/Retract I/O port cdb.. NO-OP */
		return SAM_STAT_GOOD;

	if (transport_addr == 0)
		transport_addr = smc_p->pm->start_picker;
	if (slot_type(smc_p, transport_addr) != MEDIUM_TRANSPORT) {
		MHVTL_ERR("Can't move media using slot type %d",
				slot_type(smc_p, transport_addr));
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 2;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		retval = SAM_STAT_CHECK_CONDITION;
	}
	if (!valid_slot(smc_p, src_addr)) {
		MHVTL_ERR("Invalid source slot: %d", src_addr);
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 4;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		retval = SAM_STAT_CHECK_CONDITION;
	}
	if (!valid_slot(smc_p, dest_addr)) {
		MHVTL_ERR("Invalid dest slot: %d", dest_addr);
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 6;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
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
		} else {   /* Move between (non-drive) slots */
			retval = move_slot2slot(smc_p, src_addr, dest_addr,
						sam_stat);
		}
	}

return retval;
}

uint8_t smc_rezero(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "REZERO (%ld) **", (long)cmd->dbuf_p->serialNo);

	if (!cmd->lu->online) {
		sam_not_ready(NO_ADDITIONAL_SENSE, &cmd->dbuf_p->sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	sleep(1);
	return SAM_STAT_GOOD;
}

uint8_t smc_open_close_import_export_element(struct scsi_cmd *cmd)
{
	uint8_t *cdb = cmd->scb;
	struct smc_priv *smc_p = cmd->lu->lu_private;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	int addr;
	int action_code;
	struct s_sd sd;

	MHVTL_DBG(1, "OPEN/CLOSE IMPORT/EXPORT ELEMENT (%ld) **",
					(long)cmd->dbuf_p->serialNo);

	addr = get_unaligned_be16(&cdb[2]);
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
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 4;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	return SAM_STAT_GOOD;
}

uint8_t smc_log_sense(struct scsi_cmd *cmd)
{
	struct lu_phy_attr *lu;
	uint8_t	*b = cmd->dbuf_p->data;
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat;
	int retval;
	int i;
	uint16_t alloc_len;
	struct list_head *l_head;
	struct log_pg_list *l;
	struct s_sd sd;

	MHVTL_DBG(1, "LOG SENSE (%ld) **", (long)cmd->dbuf_p->serialNo);

	alloc_len = get_unaligned_be16(&cdb[7]);
	cmd->dbuf_p->sz = alloc_len;

	lu = cmd->lu;
	sam_stat = &cmd->dbuf_p->sam_stat;
	l_head = &lu->log_pg;
	retval = 0;

	switch (cdb[2] & 0x3f) {
	case 0:	/* Send supported pages */
		MHVTL_DBG(1, "LOG SENSE: Sending supported pages");
		memset(b, 0, 4);	/* Clear first few (4) bytes */
		i = 4;
		b[i++] = 0;	/* b[0] is log page '0' (this one) */
		list_for_each_entry(l, l_head, siblings) {
			MHVTL_DBG(3, "found page 0x%02x", l->log_page_num);
			b[i] = l->log_page_num;
			i++;
		}
		put_unaligned_be16(i - 4, &b[2]);
		retval = i;
		break;
	case TEMPERATURE_PAGE:	/* Temperature page */
		MHVTL_DBG(1, "LOG SENSE: Temperature page");
		l = lookup_log_pg(&lu->log_pg, TEMPERATURE_PAGE);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	case TAPE_ALERT:	/* TapeAlert page */
		MHVTL_DBG(1, "LOG SENSE: TapeAlert page");
/*		MHVTL_DBG(2, " Returning TapeAlert flags: 0x%" PRIx64,
				get_unaligned_be64(&seqAccessDevice.TapeAlert));
*/

		l = lookup_log_pg(&lu->log_pg, TAPE_ALERT);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;

		/* Clear flags after value read. */
		if (alloc_len > 4)
			update_TapeAlert(lu, TA_NONE);
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
	cmd->dbuf_p->sz = 0;
	sd.byte0 = SKSV | CD;
	sd.field_pointer = 2;
	sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}

void unload_drive_on_shutdown(struct s_info *src, struct s_info *dest)
{
	if (!dest)
		return;

	MHVTL_DBG(1, "Force unload of media %s to slot %d",
				src->media->barcode, dest->slot_location);
	move_cart(src, dest);
}
