/*
 * This handles any SCSI OP codes defined in the standards as 'PRIMARY'
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
#include "be_byteshift.h"
#include "scsi.h"
#include "vtl_common.h"
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

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

extern int verbose;
extern int debug;
extern int reset;

extern uint8_t sense[SENSE_BUF_SIZE]; /* Request sense buffer */
extern uint8_t sense_flg;

extern uint8_t blockDescriptorBlock[];

extern uint32_t SPR_Reservation_Generation;
extern uint8_t SPR_Reservation_Type;
extern uint64_t SPR_Reservation_Key;

struct vpd *alloc_vpd(uint16_t sz)
{
	struct vpd *vpd_pg = NULL;

	vpd_pg = malloc(sizeof(struct vpd) + sz);
	if (!vpd_pg)
		return NULL;
	memset(vpd_pg, 0, sizeof(struct vpd) + sz);
	vpd_pg->sz = sz;

	return vpd_pg;
}

#define INQUIRY_LEN 512
int spc_inquiry(uint8_t *cdb, struct vtl_ds *ds, struct lu_phy_attr *lu)
{
	int len = 0;
	struct vpd *vpd_pg;
	unsigned char key = ILLEGAL_REQUEST;
	uint16_t asc = E_INVALID_FIELD_IN_CDB;
	int ret = 0;
	uint8_t *data = ds->data;

	if (((cdb[1] & 0x3) == 0x3) || (!(cdb[1] & 0x3) && cdb[2]))
		goto sense;

	memset(data, 0, INQUIRY_LEN);

	if (!(cdb[1] & 0x3)) {
		int i;
		uint16_t *desc;

		data[0] = lu->ptype;
		data[1] = (lu->removable) ? 0x80 : 0;
		data[2] = 5;	/* SPC-3 */
		data[3] = 0x42;
		data[7] = 0x02;

		memset(data + 8, 0x20, 28);
		memcpy(data + 8,  &lu->vendor_id, VENDOR_ID_LEN);
		memcpy(data + 16, &lu->product_id, PRODUCT_ID_LEN);
		memcpy(data + 32, &lu->product_rev, PRODUCT_REV_LEN);

		desc = (uint16_t *)(data + 58);
		for (i = 0; i < ARRAY_SIZE(lu->version_desc); i++)
			*desc++ = htons(lu->version_desc[i]);

		len = 66;
		data[4] = len - 5;	/* Additional Length */

		ds->sz = len;
		ret = SAM_STAT_GOOD;
	} else if (cdb[1] & 0x2) {
		/* CmdDt bit is set */
		/* We do not support it now. */
		data[1] = 0x1;
		data[5] = 0;
		len = 6;
		ds->sz = len;
		ret = SAM_STAT_GOOD;
	} else if (cdb[1] & 0x1) {
		uint8_t pcode = cdb[2];

		if (pcode == 0x00) {
			uint8_t *p;
			int i, cnt;

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
			ret = SAM_STAT_GOOD;
		} else if (lu->lu_vpd[PCODE_OFFSET(pcode)]) {
			vpd_pg = lu->lu_vpd[PCODE_OFFSET(pcode)];

			data[0] = lu->ptype;
			data[1] = pcode;
			data[2] = (vpd_pg->sz >> 8);
			data[3] = vpd_pg->sz & 0xff;
			memcpy(&data[4], vpd_pg->data, vpd_pg->sz);
			len = vpd_pg->sz + 4;
			ret = SAM_STAT_GOOD;
		}
	}

	return len;
sense:
	mkSenseBuf(key, asc, &ds->sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}

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

#define SPR_EXCLUSIVE_ACCESS 3
int resp_spc_pro(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	uint64_t RK;
	uint64_t SARK;
	uint16_t SA;
	uint8_t TYPE;
	uint8_t *sam_stat = &dbuf_p->sam_stat;
	uint8_t *buf = dbuf_p->data;

	if (dbuf_p->sz != 24) {
		mkSenseBuf(ILLEGAL_REQUEST, E_PARAMETER_LIST_LENGTH_ERR, sam_stat);
		return(-1);
	}

	SA = cdb[1] & 0x1f;
	TYPE = cdb[2] & 0x0f;

	RK = get_unaligned_be64(&buf[0]);
	SARK = get_unaligned_be64(&buf[8]);

	if (verbose) {
		syslog(LOG_DAEMON|LOG_WARNING,
			"Key 0x%.8x %.8x SA Key 0x%.8x %.8x "
			"Service Action: %s, Type: %s\n",
			(uint32_t)(RK >> 32) & 0xffffffff,
			(uint32_t) RK & 0xffffffff,
			(uint32_t)(SARK >> 32) & 0xffffffff,
			(uint32_t)SARK & 0xffffffff,
			lookup_sa(SA), lookup_type(TYPE));
		syslog(LOG_DAEMON|LOG_WARNING,
			"Reservation key was: 0x%.8x 0x%.8x\n",
			(uint32_t)(SPR_Reservation_Key >> 32) & 0xffffffff,
			(uint32_t)(SPR_Reservation_Key & 0xffffffff));
	}

	switch(SA) {
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
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		break;
	default:
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		break;
	}
	if (verbose)
		syslog(LOG_DAEMON|LOG_WARNING,
			"Reservation key now: 0x%.8x 0x%.8x\n",
			(uint32_t)(SPR_Reservation_Key >> 32) & 0xffffffff,
			(uint32_t)(SPR_Reservation_Key & 0xffffffff));
	return(0);
}

/*
 * Process PERSITENT RESERVE IN scsi command
 * Returns bytes to return if OK
 *         or -1 on failure.
 */
int resp_spc_pri(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	u16 alloc_len;
	u16 SA;
	uint8_t *buf = dbuf_p->data;
	uint8_t *sam_stat = &dbuf_p->sam_stat;

	SA = cdb[1] & 0x1f;

	alloc_len = get_unaligned_be16(&cdb[7]);

	memset(buf, 0, alloc_len);	// Clear memory

	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO, "%s: service action: %d\n",
			__func__, SA);

	switch(SA) {
	case 0: /* READ KEYS */
		put_unaligned_be32(SPR_Reservation_Generation, &buf[0]);
		if (!SPR_Reservation_Key)
			return(8);
		buf[7] = 8;
		put_unaligned_be64(SPR_Reservation_Key, &buf[8]);
		return(16);
	case 1: /* READ RESERVATON */
		put_unaligned_be32(SPR_Reservation_Generation, &buf[0]);
		if (!SPR_Reservation_Type)
			return(8);
		buf[7] = 16;
		put_unaligned_be64(SPR_Reservation_Key, &buf[8]);
		buf[21] = SPR_Reservation_Type;
		return(24);
	case 2: /* REPORT CAPABILITIES */
		buf[1] = 8;
		buf[2] = 0x10;
		buf[3] = 0x80;
		buf[4] = 0x08;
		return(8);
	case 3: /* READ FULL STATUS */
	default:
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		break;
	}
	return(0);
}

