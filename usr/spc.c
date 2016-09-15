/*
 * This handles any SCSI OP codes defined in the standards as 'PRIMARY'
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
 *                                mark.harvey at veritas dot com
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
#include "q.h"
#include "ssc.h"

extern unsigned char sense[];

uint32_t SPR_Reservation_Generation;
uint8_t SPR_Reservation_Type;
uint64_t SPR_Reservation_Key;

struct vpd *alloc_vpd(uint16_t sz)
{
	struct vpd *vpd_pg;

	vpd_pg = zalloc(sizeof(struct vpd));
	if (!vpd_pg) {
		MHVTL_ERR("Could not malloc %d bytes of mem",
					(int)sizeof(struct vpd));
		return NULL;
	}
	vpd_pg->data = zalloc(sz);
	if (!vpd_pg->data) {
		MHVTL_ERR("Could not malloc %d bytes of mem", sz);
		free(vpd_pg);
		return NULL;
	}
	vpd_pg->sz = sz;

	return vpd_pg;
}

void dealloc_vpd(struct vpd *pg)
{
	free(pg->data);
	free(pg);
}

uint8_t spc_inquiry(struct scsi_cmd *cmd)
{
	int len = 0;
	struct vpd *vpd_pg;
	uint8_t *data = (uint8_t *)cmd->dbuf_p->data;
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct lu_phy_attr *lu = cmd->lu;
	struct s_sd sd;

	MHVTL_DBG(1, "INQUIRY ** (%ld)", (long)cmd->dbuf_p->serialNo);

	if (((cdb[1] & 0x3) == 0x3) || (!(cdb[1] & 0x3) && cdb[2])) {
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (cdb[1] & 0x3) /* VPD bit set - clear memory */
		memset(data, 0, MAX_INQUIRY_SZ);
	else {	/* Standard inquiry - copy in-mem data struct */
		memcpy(cmd->dbuf_p->data, lu->inquiry, MAX_INQUIRY_SZ);
		len = lu->inquiry[4] + 5;
	}
	if (cdb[1] & 0x2) {
		/* CmdDt bit is set */
		/* We do not support it now. */
		data[1] = 0x1;
		data[5] = 0;
		len = 6;

	} else if (cdb[1] & 0x1) {
		uint8_t pcode = cdb[2];

		MHVTL_DBG(2, "Page code 0x%02x", pcode);

		if (pcode == 0x00) {
			uint8_t *p;
			unsigned int i, cnt;

			data[0] = lu->ptype;
			data[1] = 0;
			data[2] = 0;

			cnt = 1;
			p = data + 5;
			for (i = 0; i < ARRAY_SIZE(lu->lu_vpd); i++) {
				if (lu->lu_vpd[i]) {
					*p++ = i | 0x80;
					cnt++;
				}
			}
			data[3] = cnt;
			data[4] = 0x0;
			len = cnt + 4;
		} else if (lu->lu_vpd[PCODE_OFFSET(pcode)]) {
			vpd_pg = lu->lu_vpd[PCODE_OFFSET(pcode)];

			MHVTL_DBG(2, "Found page 0x%x", pcode);

			data[0] = lu->ptype;
			data[1] = pcode;
			data[2] = (vpd_pg->sz >> 8);
			data[3] = vpd_pg->sz & 0xff;
			memcpy(&data[4], vpd_pg->data, vpd_pg->sz);
			len = vpd_pg->sz + 4;
		}
	}
	cmd->dbuf_p->sz = len;
	return SAM_STAT_GOOD;
}

#ifdef MHVTL_DEBUG
/*
 * Process PERSITENT RESERVE OUT scsi command
 * Returns 0 if OK
 *         or -1 on failure.
 */
static char *type_str[] = {
	"Obsolete",
	"Write exclusive",
	"Obsolete",
	"Exclusive access",
	"Obsolete",
};

static char *type_unknown = "Undefined";

static char *lookup_type(uint8_t type)
{
	if (type > 4)
		return type_unknown;
	else
		return type_str[type];
}

static char *serv_action_str[] = {
	 "Register",
	 "Reserve",
	 "Release",
	 "Clear",
	 "Preempt",
	 "Preempt & abort",
	 "Register & ignore existing key",
	 "Register & move",
};
static char *sa_unknown = "Undefined";

static char *lookup_sa(uint8_t sa)
{
	if (sa > 7)
		return sa_unknown;
	else
		return serv_action_str[sa];
}
#endif

#define SPR_EXCLUSIVE_ACCESS 3
uint8_t resp_spc_pro(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	uint64_t RK;
	uint64_t SARK;
	uint16_t SA;
	uint8_t TYPE;
	uint8_t *sam_stat = &dbuf_p->sam_stat;
	uint8_t *buf = (uint8_t *)dbuf_p->data;
	struct s_sd sd;

	if (dbuf_p->sz != 24) {
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 5;
		sam_illegal_request(E_PARAMETER_LIST_LENGTH_ERR, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	*sam_stat = SAM_STAT_GOOD;

	SA = cdb[1] & 0x1f;
	TYPE = cdb[2] & 0x0f;

	RK = get_unaligned_be64(&buf[0]);
	SARK = get_unaligned_be64(&buf[8]);

	MHVTL_DBG(2, "Key 0x%.8x %.8x SA Key 0x%.8x %.8x "
			"Service Action: %s, Type: %s",
			(uint32_t)(RK >> 32) & 0xffffffff,
			(uint32_t) RK & 0xffffffff,
			(uint32_t)(SARK >> 32) & 0xffffffff,
			(uint32_t)SARK & 0xffffffff,
			lookup_sa(SA), lookup_type(TYPE));
	MHVTL_DBG(2, "Reservation key was: 0x%.8x 0x%.8x",
			(uint32_t)(SPR_Reservation_Key >> 32) & 0xffffffff,
			(uint32_t)(SPR_Reservation_Key & 0xffffffff));

	switch (SA) {
	case 0: /* REGISTER */
		if (SPR_Reservation_Key) {
			if (RK == SPR_Reservation_Key) {
				if (SARK) {
					SPR_Reservation_Key = SARK;
				} else {
					SPR_Reservation_Key = 0UL;
					SPR_Reservation_Type = 0;
				}
				SPR_Reservation_Generation++;
			} else {
				*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
			}
		} else {
			if (RK) {
				*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
			} else {
				SPR_Reservation_Key = SARK;
				SPR_Reservation_Generation++;
			}
		}
		break;
	case 1: /* RESERVE */
		if (SPR_Reservation_Key)
			if (RK == SPR_Reservation_Key)
				if (TYPE == SPR_EXCLUSIVE_ACCESS) {
					SPR_Reservation_Type = TYPE;
					break;
				}
		*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		break;
	case 2: /* RELEASE */
		if (SPR_Reservation_Key)
			if (RK == SPR_Reservation_Key)
				if (TYPE == SPR_EXCLUSIVE_ACCESS) {
					SPR_Reservation_Type = 0;
					break;
				}
		*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		break;
	case 3: /* CLEAR */
		if (!SPR_Reservation_Key && !SPR_Reservation_Key) {
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		} else {
			if (RK == SPR_Reservation_Key) {
				SPR_Reservation_Key = 0UL;
				SPR_Reservation_Type = 0;
				SPR_Reservation_Generation++;
			} else {
				*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
			}
		}
		break;
	case 4: /* PREEMT */
	case 5: /* PREEMPT AND ABORT */
		/* this is pretty weird,
		 * in that we can only have a single key registered,
		 * so preempt is pretty simplified */
		if ((!SPR_Reservation_Key) && (!RK) && (!SARK)) {
			*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		} else {
			if (SPR_Reservation_Type) {
				if (SARK == SPR_Reservation_Key) {
					SPR_Reservation_Key = RK;
					SPR_Reservation_Type = TYPE;
					SPR_Reservation_Generation++;
				}
			} else {
				if (SARK == SPR_Reservation_Key) {
					SPR_Reservation_Key = 0UL;
					SPR_Reservation_Generation++;
				}
			}
		}

		break;
	case 6: /* REGISTER AND IGNORE EXISTING KEY */
		if (SPR_Reservation_Key) {
			if (SARK) {
				SPR_Reservation_Key = SARK;
			} else {
				SPR_Reservation_Key = 0UL;
				SPR_Reservation_Type = 0;
			}
		} else {
			SPR_Reservation_Key = SARK;
		}
		SPR_Reservation_Generation++;
		break;
	case 7: /* REGISTER AND MOVE */
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	default:
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}
	MHVTL_DBG(2, "Reservation key now: 0x%.8x 0x%.8x",
			(uint32_t)(SPR_Reservation_Key >> 32) & 0xffffffff,
			(uint32_t)(SPR_Reservation_Key & 0xffffffff));
	return *sam_stat;
}

/*
 * Process PERSITENT RESERVE IN scsi command
 */
uint8_t resp_spc_pri(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	uint16_t alloc_len;
	uint16_t SA;
	uint8_t *buf = (uint8_t *)dbuf_p->data;
	uint8_t *sam_stat = &dbuf_p->sam_stat;
	uint8_t sam_status;
	struct s_sd sd;

	SA = cdb[1] & 0x1f;

	alloc_len = get_unaligned_be16(&cdb[7]);

	memset(buf, 0, alloc_len);	/* Clear memory */

	MHVTL_DBG(1, "service action: %d", SA);

	sam_status = SAM_STAT_GOOD;
	switch (SA) {
	case 0: /* READ KEYS */
		put_unaligned_be32(SPR_Reservation_Generation, &buf[0]);
		if (!SPR_Reservation_Key) {
			dbuf_p->sz = 8;
			break;
		}
		buf[7] = 8;
		put_unaligned_be64(SPR_Reservation_Key, &buf[8]);
		dbuf_p->sz = 16;
		break;
	case 1: /* READ RESERVATON */
		put_unaligned_be32(SPR_Reservation_Generation, &buf[0]);
		if (!SPR_Reservation_Type) {
			dbuf_p->sz = 8;
			break;
		}
		buf[7] = 16;
		put_unaligned_be64(SPR_Reservation_Key, &buf[8]);
		buf[21] = SPR_Reservation_Type;
		dbuf_p->sz = 24;
		break;
	case 2: /* REPORT CAPABILITIES */
		buf[1] = 8;
		buf[2] = 0x10;
		buf[3] = 0x80;
		buf[4] = 0x08;
		dbuf_p->sz = 8;
		break;
	case 3: /* READ FULL STATUS */
	default:
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 1;
		dbuf_p->sz = 0;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		sam_status = SAM_STAT_CHECK_CONDITION;
		break;
	}
	return sam_status;
}

uint8_t spc_tur(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "** %s (%ld) %s **", "TEST UNIT READY : Returning => ",
				(long)cmd->dbuf_p->serialNo,
				(cmd->lu->online) ? "Online" : "Offline");
	if (cmd->lu->online)
		return SAM_STAT_GOOD;

	sam_not_ready(NO_ADDITIONAL_SENSE, &cmd->dbuf_p->sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}

uint8_t spc_illegal_op(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct s_sd sd;

	MHVTL_DBG(1, "UNSUPPORTED OP CODE **");

	sd.byte0 = SKSV | CD;
	sd.field_pointer = 0;	/* byte 0 in cdb is invalid (the op code) */

	sam_illegal_request(E_INVALID_OP_CODE, &sd, sam_stat);

	return SAM_STAT_CHECK_CONDITION;
}

uint8_t spc_request_sense(struct scsi_cmd *cmd)
{
	uint8_t *sense_buf = (uint8_t *)cmd->dbuf_p->sense_buf;
	uint8_t *cdb = cmd->scb;

	MHVTL_DBG(1, "REQUEST SENSE (%ld) : KEY/ASC/ASCQ "
			"[0x%02x 0x%02x 0x%02x]"
			" Filemark: %s, EOM: %s, ILI: %s",
				(long)cmd->dbuf_p->serialNo,
				sense_buf[2] & 0x0f,
				sense_buf[12],
				sense_buf[13],
				(sense_buf[2] & SD_FILEMARK) ? "yes" : "no",
				(sense_buf[2] & SD_EOM) ? "yes" : "no",
				(sense_buf[2] & SD_ILI) ? "yes" : "no");

	assert(cmd->dbuf_p->data);
	/* Clear out the request sense flag */
	cmd->dbuf_p->sam_stat = 0;

	/* set buf size */
	cmd->dbuf_p->sz = min(cdb[4], (uint8_t)SENSE_BUF_SIZE);
	memcpy(cmd->dbuf_p->data, sense_buf, cmd->dbuf_p->sz);
	memset(sense_buf, 0, 18); /* First 18bytes contain usual suspect */
	sense_buf[0] = SD_CURRENT_INFORMATION_FIXED;
	return SAM_STAT_GOOD;
}

/*
 * Log Select
 *
 * Set the logs to a known state.
 *
 * Currently a no-op
 */
static char LOG_SELECT_00[] = "Current threshold values";
static char LOG_SELECT_01[] = "Current cumulative values";
static char LOG_SELECT_10[] = "Default threshold values";
static char LOG_SELECT_11[] = "Default cumulative values";

uint8_t spc_log_select(struct scsi_cmd *cmd)
{
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	char sp = cdb[1] & 0x1; /* Save Parameters */
	char pcr = cdb[1] & 0x2; /* Parameter Code Reset */
	uint16_t parmList;
	char *parmString = "Undefined";
	struct s_sd sd;

	parmList = get_unaligned_be16(&cdb[7]); /* bytes 7 & 8 are parm list. */

	MHVTL_DBG(1, "LOG SELECT (%ld)%s **",
				(long)cmd->dbuf_p->serialNo,
				(pcr) ? " : Parameter Code Reset " : "");
	if (sp) {
		MHVTL_DBG(1, " Log Select - Save Parameters not supported");
		sd.byte0 = SKSV | CD | BPV | 1; /* bit 1 */
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (pcr) {	/* Check for Parameter code reset */
		if (parmList) {	/* If non-zero, error */
			sd.byte0 = SKSV | CD;
			sd.field_pointer = 7;
			sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd,
								sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		switch ((cdb[2] & 0xc0) >> 6) {
		case 0:
			parmString = LOG_SELECT_00;
			break;
		case 1:
			parmString = LOG_SELECT_01;
			break;
		case 2:
			parmString = LOG_SELECT_10;
			break;
		case 3:
			parmString = LOG_SELECT_11;
			break;
		}
		MHVTL_DBG(1, "  %s", parmString);
	}
	return SAM_STAT_GOOD;
}

/*
 * Process the MODE_SELECT command
 */
uint8_t spc_mode_select(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "MODE SELECT (%ld) **", (long)cmd->dbuf_p->serialNo);

	cmd->dbuf_p->sz = 0;
	return SAM_STAT_GOOD;
}

/*
 * Add data for pcode to buffer pointed to by p
 * Return: Number of chars moved.
 */
static int add_pcode(struct mode *m, uint8_t pc, uint8_t *p)
{
	if (pc == 1)	/* Report Changeable bitmap */
		memcpy(p, m->pcodePointerBitMap, m->pcodeSize);
	else
		memcpy(p, m->pcodePointer, m->pcodeSize);
	return m->pcodeSize;
}

/*
 * Build mode sense data into *buf
 * Return SAM STATUS
 */
uint8_t spc_mode_sense(struct scsi_cmd *cmd)
{
	int pc, pcode, subpcode;
	int alloc_len, msense_6;
	int len = 0;
	int offset = 0;
	uint8_t *ap;
	struct mode *smp;	/* Struct mode pointer... */
	struct priv_lu_ssc *ssc;
	int i, j;
	int WriteProtect = 0;
	struct s_sd sd;

	uint8_t *buf = (uint8_t *)cmd->dbuf_p->data;
	uint8_t *scb = cmd->scb;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct list_head *m = &cmd->lu->mode_pg;

	if (cmd->lu->ptype == TYPE_TAPE) {
		ssc = cmd->lu->lu_private;
		WriteProtect = ssc->MediaWriteProtect;
	}

#ifdef MHVTL_DEBUG
	char *pcString[] = {
		"Current values",
		"Changeable values",
		"Default values",
		"Saved values",
	};
#endif

	/* Disable Block Descriptors */
	uint8_t blockDescriptorLen = (scb[1] & 0x8) ? 0 : 8;

	/*
	 pc => page control
		00 -> 0: Report Current values
		01 -> 1: Report Changeable values
		10 -> 2: Report Default values
		11 -> 3: Report Saved values
	*/
	pc = (scb[2] & 0xc0) >> 6;
	/* pcode -> Page Code */
	pcode = scb[2] & 0x3f;
	subpcode = scb[3];
	msense_6 = (MODE_SENSE == scb[0]);

	alloc_len = msense_6 ? scb[4] : ((scb[7] << 8) | scb[8]);
	offset = msense_6 ? 4 : 8;

	MHVTL_DBG(1, "MODE SENSE %d (%ld) **",
				(msense_6) ? 6 : 10,
				(long)cmd->dbuf_p->serialNo);
	MHVTL_DBG(2, " Page Control     : %s(0x%02x)",
				pcString[pc], pc);
	MHVTL_DBG(2, " Page/Subpage Code: 0x%02x/0x%02x", pcode, subpcode);
	MHVTL_DBG(2, " %s Block Descriptor",
				(blockDescriptorLen) ? "Report" : "Disable");
	MHVTL_DBG(2, " Allocation len   : %d", alloc_len);

	if (0x3 == pc) {  /* Saving values not supported */
		MHVTL_DBG(2, "Reporting on Saved Values not supported");
		sam_illegal_request(E_SAVING_PARMS_UNSUP, NULL, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (pcode == 0x3f && (subpcode != 0x0 && subpcode != 0xff)) {
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 3;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	memset(buf, 0, alloc_len);	/* Set return data to null */

	offset += blockDescriptorLen;
	ap = buf + offset;

	switch (pcode) {
	case 0:
		len = 0;
		break;
	case 0x3f:
		/* Walk thru all possibilities */
		if (subpcode == 0) {
			for (i = 1; i < 0x3f; i++) {
				smp = lookup_pcode(m, i, subpcode);
				if (smp)
					len += add_pcode(smp, pc,
							(uint8_t *)ap + len);
			}
		} else { /* 0x01 - 0xfe are reserved. Should only be 0xff */
			for (i = 1; i < 0x3f; i++) {
				for (j = 0; j < 0xff; j++) {
					smp = lookup_pcode(m, i, j);
					if (smp)
						len += add_pcode(smp, pc,
							(uint8_t *)ap + len);
				}
			}
		}
		break;
	default:
		if (subpcode == 0xff) { /* All sub-pcodes for this pcode */
			for (i = 0; i < 0xff; i++) {
				smp = lookup_pcode(m, pcode, i);
				if (smp)
					len += add_pcode(smp, pc,
							(uint8_t *)ap + len);
			}
		} else {
			smp = lookup_pcode(m, pcode, subpcode);
			if (smp)
				len = add_pcode(smp, pc, (uint8_t *)ap);
		}
		break;
	}

	offset += len;

	if (pcode != 0)	/* 0 = No page code requested */
		if (0 == len) {	/* Page not found.. */
		MHVTL_DBG(2, "Unknown mode page: 0x%02x sub-page code: 0x%02x",
							pcode, subpcode);
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 2;	/* Byte 2 page code */
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		}

	/* Fill in header.. */
	if (msense_6) {
		buf[0] = offset - 1;	/* size - sizeof(buf[0]) field */
		buf[1] = cmd->lu->mode_media_type;
		buf[2] = (WriteProtect ? 0x80 : 0x00) | 0x10;
		buf[3] = blockDescriptorLen;
		/* If the length > 0, copy Block Desc. */
		if (blockDescriptorLen) {
			switch (pc) {
			case 0:
			case 2:
				memcpy(&buf[4], modeBlockDescriptor,
							blockDescriptorLen);
				break;
			case 1:
				buf[9] = 0xff;
				buf[10] = 0xff;
				buf[11] = 0xff;
				break;
			}
		}
	} else {
		put_unaligned_be16(offset - 2, &buf[0]);
		buf[2] = cmd->lu->mode_media_type;
		buf[3] = (WriteProtect ? 0x80 : 0x00) | 0x10;
		put_unaligned_be16(blockDescriptorLen, &buf[6]);
		/* If the length > 0, copy Block Desc. */
		if (blockDescriptorLen) {
			switch (pc) {
			case 0:
			case 2:
				memcpy(&buf[8], modeBlockDescriptor,
							blockDescriptorLen);
				break;
			case 1:
				buf[13] = 0xff;
				buf[14] = 0xff;
				buf[15] = 0xff;
				break;
			}
		}
	}

#ifdef MHVTL_DEBUG
	if (debug) {
		printf("mode sense: Returning %d bytes\n", offset);
		hex_dump(buf, offset);
	}
#endif

	cmd->dbuf_p->sz = offset;

	return SAM_STAT_GOOD;
}

uint8_t spc_release(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "RELEASE UNIT (%ld) **", (long)cmd->dbuf_p->serialNo);
	return SAM_STAT_GOOD;
}

uint8_t spc_reserve(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "RESERVE UNIT (%ld) **", (long)cmd->dbuf_p->serialNo);
	return SAM_STAT_GOOD;
}

uint8_t spc_send_diagnostics(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "SEND DIAGNOSTICS (%ld) **", (long)cmd->dbuf_p->serialNo);
	return SAM_STAT_GOOD;
}

uint8_t spc_recv_diagnostics(struct scsi_cmd *cmd)
{
	uint8_t *data;
	uint8_t pc;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct s_sd sd;

	MHVTL_DBG(1, "RECEIVE DIAGNOSTIC (%ld) **",
						(long)cmd->dbuf_p->serialNo);

	if (cmd->scb[1] & 0x01) {	/* Page Code Valid bit */
		pc = cmd->scb[2];

		MHVTL_DBG(3, "Page code: %d", pc);
		if (pc == 0) {
			data = cmd->dbuf_p->data;

			memset(data, 0, 10);	/* Clear any junk */
			put_unaligned_be16(1, &data[2]);
			cmd->dbuf_p->sz = 5;

			return SAM_STAT_GOOD;
		}
	}

	sd.byte0 = SKSV | CD;
	sd.field_pointer = 1;
	cmd->dbuf_p->sz = 0;
	sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}
