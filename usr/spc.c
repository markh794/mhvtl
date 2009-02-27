/*
 * This handles any SCSI OP codes defined in the standards as 'PRIMARY'
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
