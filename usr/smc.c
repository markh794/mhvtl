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
#include "smc.h"
#include "q.h"

int smc_allow_removal(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "%s MEDIUM Removal (%ld) **",
				(cmd->scb[4]) ? "Prevent" : "Allow",
				(long)cmd->dbuf_p->serialNo);
	return SAM_STAT_GOOD;
}

int smc_initialize_element(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "%s (%ld) **", "INITIALIZE ELEMENT",
				(long)cmd->dbuf_p->serialNo);
	if (!cmd->lu->online) {
		mkSenseBuf(NOT_READY, NO_ADDITIONAL_SENSE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	sleep(1);
	return SAM_STAT_GOOD;
}

int smc_initialize_element_range(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "%s (%ld) **", "INITIALIZE ELEMENT RANGE",
				(long)cmd->dbuf_p->serialNo);

	if (!cmd->lu->online) {
		mkSenseBuf(NOT_READY, NO_ADDITIONAL_SENSE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	sleep(1);
	return SAM_STAT_GOOD;
}

/* Return the element type of a particular element address */
static int slot_type(struct smc_priv *smc_p, int addr)
{
	if ((addr >= START_DRIVE) &&
			(addr < START_DRIVE + smc_p->num_drives))
		return DATA_TRANSFER;
	if ((addr >= START_PICKER) &&
			(addr < START_PICKER + smc_p->num_picker))
		return MEDIUM_TRANSPORT;
	if ((addr >= START_MAP) &&
			(addr < START_MAP + smc_p->num_map))
		return MAP_ELEMENT;
	if ((addr >= START_STORAGE) &&
			(addr < START_STORAGE + smc_p->num_storage))
		return STORAGE_ELEMENT;
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
		if (sp->slot_location == addr)
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

static void dump_element_desc(uint8_t *p, int voltag, int num_elem, int len,
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
}

static void decode_element_status(struct smc_priv *smc_p, uint8_t *p)
{
	int voltag;
	int elem_len;
	int page_elements, page_bytes;

	MHVTL_DBG(3, "Element Status Data");
	MHVTL_DBG(3, "  First element reported       : %d",
					get_unaligned_be16(&p[0]));
	MHVTL_DBG(3, "  Number of elements available : %d",
					get_unaligned_be16(&p[2]));
	MHVTL_DBG(3, "  Byte count of report         : %d",
					get_unaligned_be24(&p[5]));

	p += 8;

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

	dump_element_desc(p, voltag, page_elements, elem_len,
					smc_p->dvcid_serial_only);

	fflush(NULL);
}

/*
 * Calculate length of one element
 */
static int determine_element_sz(struct scsi_cmd *cmd, int type)
{
	struct smc_priv *smc_p = cmd->lu->lu_private;
	int dvcid;
	int voltag;

	voltag = (cmd->scb[1] & 0x10) >> 4;
	dvcid = cmd->scb[6] & 0x01;	/* Device ID */

	return 16 + (voltag ? VOLTAG_LEN : 0) +
		(dvcid && (type == DATA_TRANSFER) ? smc_p->dvcid_len : 0);
}

/*
 * Fill in a single element descriptor
 *
 * Returns number of bytes in element data.
 */
static int fill_element_descriptor(struct scsi_cmd *cmd, uint8_t *p, int addr)
{
	struct smc_priv *smc_p = cmd->lu->lu_private;
	struct d_info *d = NULL;
	struct s_info *s = NULL;
	int type;
	int j = 0;
	uint8_t voltag;
	uint8_t dvcid;

	voltag = (cmd->scb[1] & 0x10) >> 4;
	dvcid = cmd->scb[6] & 0x01;	/* Device ID */

	type = slot_type(smc_p, addr);

	switch (type) {
	case DATA_TRANSFER:
		d = drive2struct(smc_p, addr);
		s = slot2struct(smc_p, addr);
		break;
	case MEDIUM_TRANSPORT:
		s = slot2struct(smc_p, addr);
		break;
	case MAP_ELEMENT:
		s = slot2struct(smc_p, addr);
		break;
	case STORAGE_ELEMENT:
		s = slot2struct(smc_p, addr);
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
	p[j++] = (s->asc_ascq >> 8) | 0xff;  /* Additional Sense Code */
	p[j++] = s->asc_ascq | 0xff; /* Additional Sense Code Qualifer */

	j++;		/* Reserved */
	if (type == DATA_TRANSFER) {
		p[j++] = d->SCSI_ID;
	} else {
		j++;	/* Reserved */
	}
	j++;		/* Reserved */

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

	MHVTL_DBG(2, "DVCID: %d, VOLTAG: %d, Index: %d", dvcid, voltag, j);

	if (voltag) {
		/* Barcode with trailing space(s) */
		if ((s->status & STATUS_Full) &&
		    !(s->media->internal_status & INSTATUS_NO_BARCODE))
			blank_fill(&p[j], s->media->barcode, VOLTAG_LEN);
		else
			memset(&p[j], 0, VOLTAG_LEN);

		j += VOLTAG_LEN;	/* Account for barcode */
	}

	if (dvcid && type == DATA_TRANSFER) {
		p[j++] = 2;	/* Code set 2 = ASCII */
		p[j++] = 1;	/* Identifier type */
		j++;		/* Reserved */
		p[j++] = smc_p->dvcid_len;	/* Identifier Length */
		if (smc_p->dvcid_serial_only) {
			blank_fill(&p[j], d->inq_product_sno,
							smc_p->dvcid_len);
			j += smc_p->dvcid_len;
		} else {
			blank_fill(&p[j], d->inq_vendor_id, 8);
			j += 8;
			blank_fill(&p[j], d->inq_product_id, 16);
			j += 16;
			blank_fill(&p[j], d->inq_product_sno, 10);
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
static int fill_element_status_page_hdr(struct scsi_cmd *cmd, uint8_t *p,
					uint16_t element_count,
					uint8_t typeCode)
{
	int element_sz;
	uint32_t element_len;
	uint8_t voltag;
	uint8_t dvcid;

	voltag = (cmd->scb[1] & 0x10) >> 4;
	dvcid = cmd->scb[6] & 0x01;	/* Device ID */

	element_sz = determine_element_sz(cmd, typeCode);

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


return 8;	/* Always 8 bytes in header */
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

return 8;	/* Header is 8 bytes in size.. */
}

/*
 * Read Element Status command will pass 'start element address' & type of slot
 *
 * We return the first valid slot number which matches.
 * or zero on no matching slots..
 */
static int find_first_matching_element(struct smc_priv *smc_p,
					uint16_t start,
					uint8_t typeCode)
{
	switch (typeCode) {
	case ANY:	/* Don't care what 'type' */
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
			return START_DRIVE;

		/* If we are above Storage range, return nothing. */
		if (start >= START_STORAGE + smc_p->num_storage)
			return 0;
		if (start >= START_STORAGE)
			return start;
		/* If we are above I/O Range -> return START_STORAGE */
		if (start >= START_MAP + smc_p->num_map)
			return START_STORAGE;
		if (start >= START_MAP)
			return start;
		/* If we are above the Picker range -> Return I/O Range.. */
		if (start >= START_PICKER + smc_p->num_picker)
			return START_MAP;
		if (start >= START_PICKER)
			return start;
		/* If we are above the Drive range, return Picker.. */
		if (start >= START_DRIVE + smc_p->num_drives)
			return START_PICKER;
		if (start >= START_DRIVE)
			return start;
		break;
	case MEDIUM_TRANSPORT:	/* Medium Transport. */
		if ((start >= START_PICKER) &&
		   (start < (START_PICKER + smc_p->num_picker)))
			return start;
		if (start < START_PICKER)
			return START_PICKER;
		break;
	case STORAGE_ELEMENT:	/* Storage Slots */
		if ((start >= START_STORAGE) &&
		   (start < (START_STORAGE + smc_p->num_storage)))
			return start;
		if (start < START_STORAGE)
			return START_STORAGE;
		break;
	case MAP_ELEMENT:	/* Import/Export */
		if ((start >= START_MAP) &&
		   (start < (START_MAP + smc_p->num_map)))
			return start;
		if (start < START_MAP)
			return START_MAP;
		break;
	case DATA_TRANSFER:	/* Data transfer */
		if ((start >= START_DRIVE) &&
		   (start < (START_DRIVE + smc_p->num_drives)))
			return start;
		if (start < START_DRIVE)
			return START_DRIVE;
		break;
	}
return 0;
}

/* Returns number of available elements left from starting number */
static uint32_t num_available_elements(struct smc_priv *priv, uint8_t type,
					uint32_t start, uint32_t max)
{
	struct list_head *slot_head;
	struct s_info *sp;
	int counted = 0;

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

	MHVTL_DBG(2, "Determing %d element%s of type %d starting at %d"
			", returning %d",
				max, max == 1 ? "" : "s",
				type, start, counted);

	return counted;
}

/*
 * Fill in Element status page header + each Element descriptor
 *
 * Returns zero on success, or error code if illegal request.
 */
static uint32_t fill_element_page(struct scsi_cmd *cmd, uint16_t start,
				uint16_t *cur_count, uint32_t *cur_offset)
{
	struct smc_priv *smc_p;
	uint16_t begin;
	uint16_t count, avail, space;
	int min_addr, num_addr;
	int j;
	uint8_t *p = cmd->dbuf_p->data;
	uint8_t *cdb = cmd->scb;

	uint8_t	type = cdb[1] & 0x0f;
	uint16_t max_count;
	uint32_t max_bytes;

	max_count = get_unaligned_be16(&cdb[4]);
	max_bytes = 0xffffff & get_unaligned_be32(&cdb[6]);

	smc_p = cmd->lu->lu_private;

	if (type == ANY)
		type = slot_type(smc_p, start);

	switch (type) {
	case MEDIUM_TRANSPORT:
		min_addr = START_PICKER;
		num_addr = smc_p->num_picker;
		MHVTL_DBG(3, "Element type: Medium Transport, min: %d num: %d",
					min_addr, num_addr);
		break;
	case STORAGE_ELEMENT:
		min_addr = START_STORAGE;
		num_addr = smc_p->num_storage;
		MHVTL_DBG(3, "Element type: Storage Element, min: %d num: %d",
					min_addr, num_addr);
		break;
	case MAP_ELEMENT:
		min_addr = START_MAP;
		num_addr = smc_p->num_map;
		MHVTL_DBG(3, "Element type: MAP Element, min: %d num: %d",
					min_addr, num_addr);
		break;
	case DATA_TRANSFER:
		min_addr = START_DRIVE;
		num_addr = smc_p->num_drives;
		MHVTL_DBG(3, "Element type: Data Transfer (drive) Element, "
				"min: %d num: %d",
					min_addr, num_addr);
		break;
	default:
		MHVTL_DBG(1, "Invalid type: %d (valid values 1 - 4)", type);
		return E_INVALID_FIELD_IN_CDB;
	}

	/* Find first valid slot. */
	begin = find_first_matching_element(smc_p, start, type);
	if (begin == 0)
		return E_INVALID_FIELD_IN_CDB;

	/*
	 *   The number of elements to report is the minimum of:
	 * 1. the number the caller asked for (max_count - *cur_count).
	 * 2. the number that remain starting at address begin, and
	 * 3. the number that will fit in the remaining
	 *    (max_bytes - *cur_offset) bytes, allowing for an 8-byte header.
	 */

	MHVTL_DBG(3, "max_count: %d, max_bytes: %d", max_count, max_bytes);

	avail = min_addr + num_addr - begin;
	count = avail < max_count - *cur_count ? avail : max_count - *cur_count;
	space = (max_bytes - *cur_offset - 8) /
				determine_element_sz(cmd, type);
	count = space < count ? space : count;
	MHVTL_DBG(2, "avail: %d, count: %d, space: %d *cur_count: %d",
			avail, count, space, *cur_count);

	if (count == 0) {
		if (*cur_count == 0)
			return E_PARAMETER_LIST_LENGTH_ERR;
		else
			return SAM_STAT_GOOD;
	}

	/* Create Element Status Page Header. */
	*cur_offset += fill_element_status_page_hdr(cmd, &p[*cur_offset],
						count, type);

	/* Now loop over each slot and fill in details. */

	for (j = 0; j < count; j++, begin++) {
		MHVTL_DBG(2, "Slot: %d", begin);
		*cur_offset += fill_element_descriptor(cmd, &p[*cur_offset],
						begin);
	}
	*cur_count += count;

return SAM_STAT_GOOD;
}

/*
 * Build READ ELEMENT STATUS data.
 *
 * Returns number of bytes to xfer back to host.
 */
int smc_read_element_status(struct scsi_cmd *cmd)
{
	struct smc_priv *smc_p = cmd->lu->lu_private;
	uint8_t *cdb = cmd->scb;
	uint8_t *buf = cmd->dbuf_p->data;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t	*p;
	uint8_t	typeCode = cdb[1] & 0x0f;
	uint8_t	voltag = (cdb[1] & 0x10) >> 4;
	uint16_t req_start_elem;
	uint16_t number;
	uint8_t	dvcid = cdb[6] & 0x01;	/* Device ID */
	uint32_t alloc_len;
	uint16_t start;	/* First valid slot location */
	uint16_t start_any;	/* First valid slot location */
	uint32_t cur_offset;
	uint16_t cur_count;
	uint32_t ec;

	MHVTL_DBG(1, "READ ELEMENT STATUS (%ld) **",
				(long)cmd->dbuf_p->serialNo);

	req_start_elem = get_unaligned_be16(&cdb[2]);
	number = get_unaligned_be16(&cdb[4]);
	alloc_len = 0xffffff & get_unaligned_be32(&cdb[6]);

	switch (typeCode) {
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
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}
	MHVTL_DBG(3, "  Starting Element Address: %d", req_start_elem);
	MHVTL_DBG(3, "  Number of Elements      : %d", number);
	MHVTL_DBG(3, "  Allocation length       : %d", alloc_len);
	MHVTL_DBG(3, "  Device ID: %s, voltag: %s",
					(dvcid == 0) ? "No" :  "Yes",
					(voltag == 0) ? "No" :  "Yes");

	/* This segfaulted somewhere between here and end of function
	 * Now that I've added 'Debug' printf statements, it's not faulting
	 * Leaving statements for now..
	 * Strange thing was no core file generated from segfault ???
	 */

	/* Set alloc_len to smallest value */
	alloc_len = min(alloc_len, cmd->lu->bufsize);

	/* Init buffer */
	memset(buf, 0, alloc_len);

	if (cdb[11] != 0x0) {	/* Reserved byte.. */
		MHVTL_DBG(3, "cmd[11] : Illegal value");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* Find first matching slot number which matches the typeCode. */
	start = find_first_matching_element(smc_p, req_start_elem, typeCode);
	if (start == 0) {	/* Nothing found.. */
		MHVTL_DBG(3, "Start element is still 0");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* Leave room for 'master' header which is filled in at the end... */
	p = buf;
	cur_offset = 8;
	cur_count = 0;
	ec = 0;

	switch (typeCode) {
	case MEDIUM_TRANSPORT:
	case STORAGE_ELEMENT:
	case MAP_ELEMENT:
	case DATA_TRANSFER:
		ec = fill_element_page(cmd, start, &cur_count, &cur_offset);
		break;
	case ANY:
		/* Don't modify 'start' value as it is needed later */
		start_any = start;

		/* Logic here depends on Storage slots being
		 * higher (numerically) than MAP which is higher than
		 * Picker, which is higher than the drive slot number..
		 * See DWR: near top of this file !!
		 */
		if (slot_type(smc_p, start_any) == DATA_TRANSFER) {
			ec = fill_element_page(cmd, start_any,
						&cur_count, &cur_offset);
			if (ec)
				break;
			start_any = START_PICKER;
		}
		if (slot_type(smc_p, start_any) == MEDIUM_TRANSPORT) {
			ec = fill_element_page(cmd, start_any,
						&cur_count, &cur_offset);
			if (ec)
				break;
			start_any = START_MAP;
		}
		if (slot_type(smc_p, start_any) == MAP_ELEMENT) {
			ec = fill_element_page(cmd, start_any,
						&cur_count, &cur_offset);
			if (ec)
				break;
			start_any = START_STORAGE;
		}
		if (slot_type(smc_p, start_any) == STORAGE_ELEMENT) {
			ec = fill_element_page(cmd, start_any,
						&cur_count, &cur_offset);
			if (ec)
				break;
		}
		break;
	default:	/* Illegal descriptor type. */
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	if (ec != 0) {
		mkSenseBuf(ILLEGAL_REQUEST, ec, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	cur_count = num_available_elements(smc_p, typeCode, start, number);

	/* Now populate the 'main' header structure with byte count.. */
	fill_element_status_data_hdr(&buf[0], start, cur_count, cur_offset - 8);

	MHVTL_DBG(3, "Returning %d bytes", cur_offset);

	if (debug)
		hex_dump(buf, cur_offset);

	decode_element_status(smc_p, buf);

	/* Return the smallest number */
	cmd->dbuf_p->sz = min(cur_offset, alloc_len);

	return SAM_STAT_GOOD;
}

/* Expect a response from tape drive on load success/failure
 * Returns 0 on success
 * non-zero on load failure

 * FIXME: I really need a timeout here..
 */
static int check_tape_load(char *pcl)
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

static int move_slot2drive(struct smc_priv *smc_p,
		 int src_addr, int dest_addr, uint8_t *sam_stat)
{
	struct s_info *src;
	struct d_info *dest;
	char cmd[128];
	int x;

	src  = slot2struct(smc_p, src_addr);
	dest = drive2struct(smc_p, dest_addr);

	if (!slotOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (driveOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	sprintf(cmd, "lload %s", src->media->barcode);
	/* Remove traling spaces */
	for (x = 6; x < 16; x++)
		if (cmd[x] == ' ') {
			cmd[x] = '\0';
			break;
		}

	/* FIXME: About here would be a good spot to create any 'missing'
	 *	  media. That way, the user would not have to pre-create
	 *	  media.
	 */

	MHVTL_DBG(1, "About to send cmd: \'%s\' to drive %ld",
					cmd, dest->drv_id);

	send_msg(cmd, dest->drv_id);

	if (check_tape_load(src->media->barcode)) {
		mkSenseBuf(HARDWARE_ERROR, E_MANUAL_INTERVENTION_REQ, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	move_cart(src, dest->slot);
	setDriveFull(dest);

	return 0;
}

static int move_slot2slot(struct smc_priv *smc_p, int src_addr,
			int dest_addr, uint8_t *sam_stat)
{
	struct s_info *src;
	struct s_info *dest;

	src  = slot2struct(smc_p, src_addr);
	dest = slot2struct(smc_p, dest_addr);

	MHVTL_DBG(1, "Moving from slot %d to slot %d",
				src->slot_location, dest->slot_location);

	if (!slotOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (slotOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (src->element_type == MAP_ELEMENT) {
		if (!map_access_ok(smc_p, src)) {
			MHVTL_DBG(2, "SOURCE MAP port not accessable");
			mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_REMOVAL_PREVENTED,
						sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	if (dest->element_type == MAP_ELEMENT) {
		if (!map_access_ok(smc_p, dest)) {
			MHVTL_DBG(2, "DESTINATION MAP port not accessable");
			mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_REMOVAL_PREVENTED,
						sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	move_cart(src, dest);
	return 0;
}

/* Return OK if 'addr' is within either a MAP, Drive or Storage slot */
static int valid_slot(struct smc_priv *smc_p, int addr)
{
	switch (slot_type(smc_p, addr)) {
	case STORAGE_ELEMENT:
	case MAP_ELEMENT:
	case DATA_TRANSFER:
		return 1;
	}
	return 0;
}

static int move_drive2slot(struct smc_priv *smc_p,
			int src_addr, int dest_addr, uint8_t *sam_stat)
{
	struct d_info *src;
	struct s_info *dest;

	src  = drive2struct(smc_p, src_addr);
	dest = slot2struct(smc_p, dest_addr);

	if (!driveOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (slotOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (dest->element_type == MAP_ELEMENT) {
		if (!map_access_ok(smc_p, dest)) {
			mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_REMOVAL_PREVENTED,
						sam_stat);
				return SAM_STAT_CHECK_CONDITION;
		}
	}

	/* Send 'unload' message to drive b4 the move.. */
	send_msg("unload", src->drv_id);

	move_cart(src->slot, dest);
	setDriveEmpty(src);

return SAM_STAT_GOOD;
}

/* Move media in drive 'src_addr' to drive 'dest_addr' */
static int move_drive2drive(struct smc_priv *smc_p,
			int src_addr, int dest_addr, uint8_t *sam_stat)
{
	struct d_info *src;
	struct d_info *dest;
	char cmd[128];
	int x;

	src  = drive2struct(smc_p, src_addr);
	dest = drive2struct(smc_p, dest_addr);

	if (!driveOccupied(src)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_SRC_EMPTY, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (driveOccupied(dest)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_DEST_FULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	move_cart(src->slot, dest->slot);

	/* Send 'unload' message to drive b4 the move.. */
	send_msg("unload", src->drv_id);

	sprintf(cmd, "lload %s", dest->slot->media->barcode);

	/* Remove trailing spaces */
	for (x = 6; x < 16; x++)
		if (cmd[x] == ' ') {
			cmd[x] = '\0';
			break;
		}
	MHVTL_DBG(2, "Sending cmd: \'%s\' to drive %ld",
					cmd, dest->drv_id);

	send_msg(cmd, dest->drv_id);

return SAM_STAT_GOOD;
}

/* Move a piece of medium from one slot to another */
int smc_move_medium(struct scsi_cmd *cmd)
{
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	int transport_addr;
	int src_addr, src_type;
	int dest_addr, dest_type;
	int retval = SAM_STAT_GOOD;
	struct smc_priv *smc_p = cmd->lu->lu_private;

	MHVTL_DBG(1, "MOVE MEDIUM (%ld) **", (long)cmd->dbuf_p->serialNo);

	transport_addr = get_unaligned_be16(&cdb[2]);
	src_addr  = get_unaligned_be16(&cdb[4]);
	dest_addr = get_unaligned_be16(&cdb[6]);
	src_type = slot_type(smc_p, src_addr);
	dest_type = slot_type(smc_p, dest_addr);

	if (verbose) {
		if (cdb[11] & 0xc0) {
			MHVTL_LOG("%s",
				(cdb[11] & 0x80) ? "  Retract I/O port" :
						   "  Extend I/O port");
		} else {
			MHVTL_LOG(
	 "Moving from slot %d to Slot %d using transport %d, Invert media: %s",
					src_addr, dest_addr, transport_addr,
					(cdb[10]) ? "yes" : "no");
		}
	}

	if (cdb[10] != 0) {	/* Can not Invert media */
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (cdb[11] == 0xc0) {	/* Invalid combo of Extend/retract I/O port */
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (cdb[11]) /* Must be an Extend/Retract I/O port cdb.. NO-OP */
		return SAM_STAT_GOOD;

	if (transport_addr == 0)
		transport_addr = START_PICKER;
	if (slot_type(smc_p, transport_addr) != MEDIUM_TRANSPORT) {
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		retval = SAM_STAT_CHECK_CONDITION;
	}
	if (!valid_slot(smc_p, src_addr)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		retval = SAM_STAT_CHECK_CONDITION;
	}
	if (!valid_slot(smc_p, dest_addr)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
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

int smc_rezero(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "Rezero (%ld) **", (long)cmd->dbuf_p->serialNo);

	if (!cmd->lu->online) {
		mkSenseBuf(NOT_READY, NO_ADDITIONAL_SENSE, &cmd->dbuf_p->sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	sleep(1);
	return SAM_STAT_GOOD;
}

int smc_start_stop(struct scsi_cmd *cmd)
{
	if (cmd->scb[4] & 0x1) {
		cmd->lu->online = 1;
		MHVTL_DBG(1, "Library now online (%ld) **",
				(long)cmd->dbuf_p->serialNo);
	} else {
		cmd->lu->online = 0;
		MHVTL_DBG(1, "Library now offline (%ld) **",
				(long)cmd->dbuf_p->serialNo);
	}
	return SAM_STAT_GOOD;
}

