/*
 * Shared routines between vtltape & vtllibrary
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
 */
#include <stdint.h>

#ifndef Solaris
 #include <byteswap.h>
#endif

#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include "be_byteshift.h"
#include "list.h"
#include "scsi.h"
#include "vtl_common.h"
#include "vtllib.h"
#include <zlib.h>

#ifndef Solaris
	int ioctl(int, int, void *);
#endif

static int reset = 0;

uint8_t sense[SENSE_BUF_SIZE];
uint8_t blockDescriptorBlock[8] = {0, 0, 0, 0, 0, 0, 0, 0 };

void mhvtl_prt_cdb(int lvl, uint64_t sn, uint8_t *cdb)
{
	int groupCode;

	groupCode = (cdb[0] & 0xe0) >> 5;
	switch (groupCode) {
	case 0:	/*  6 byte commands */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") "
				"%02x %02x %02x %02x %02x %02x",
			sn, cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5]);
		break;
	case 1: /* 10 byte commands */
	case 2: /* 10 byte commands */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") "
			"%02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x",
			sn, cdb[0], cdb[1], cdb[2], cdb[3],
			cdb[4], cdb[5], cdb[6], cdb[7],
			cdb[8], cdb[9]);
		break;
	case 3: /* Reserved - There is always one exception ;) */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") "
			"%02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x %02x",
			sn, cdb[0], cdb[1], cdb[2], cdb[3],
			cdb[4], cdb[5], cdb[6], cdb[7],
			cdb[8], cdb[9], cdb[10], cdb[11]);
		break;
	case 4: /* 16 byte commands */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") "
			"%02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			sn, cdb[0], cdb[1], cdb[2], cdb[3],
			cdb[4], cdb[5], cdb[6], cdb[7],
			cdb[8], cdb[9], cdb[10], cdb[11],
			cdb[12], cdb[13], cdb[14], cdb[15]);
		break;
	case 5: /* 12 byte commands */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") "
			"%02x %02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x",
			sn, cdb[0], cdb[1], cdb[2], cdb[3],
			cdb[4], cdb[5], cdb[6], cdb[7],
			cdb[8], cdb[9], cdb[10], cdb[11]);
		break;
	case 6: /* Vendor Specific */
	case 7: /* Vendor Specific */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") VENDOR SPECIFIC !! "
			" %02x %02x %02x %02x %02x %02x",
			sn, cdb[0], cdb[1], cdb[2], cdb[3],
			cdb[4], cdb[5]);
		break;
	}
}

/*
 * Fills in a global array with current sense data
 * Sets 'sam status' to SAM_STAT_CHECK_CONDITION.
 */

void mkSenseBuf(uint8_t sense_d, uint32_t sense_q, uint8_t *sam_stat)
{
	/* Clear Sense key status */
	memset(sense, 0, SENSE_BUF_SIZE);

	*sam_stat = SAM_STAT_CHECK_CONDITION;

	sense[0] = SD_CURRENT_INFORMATION_FIXED;
	/* SPC4 (Revision 30) Ch: 4.5.1 states:
	 * The RESPONSE CODE field show be set to 70h in all unit attention
	 * condition sense data in which:
	 * - The ADDITIONAL SENSE CODE field is set to 29h
	 * - The ADDITIONAL SENSE CODE is set to MODE PARAMETERS CHANGED
	 */
	switch (sense_d) {
	case UNIT_ATTENTION:
		if ((sense_q >> 8) == 0x29)
			break;
		if (sense_q == E_MODE_PARAMETERS_CHANGED)
			break;
		/* Fall thru to default handling */
	default:
		sense[0] |= SD_VALID;
		break;
	}

	sense[2] = sense_d;
	sense[7] = SENSE_BUF_SIZE - 8;
	put_unaligned_be16(sense_q, &sense[12]);

	MHVTL_DBG(3, "Sense buf: %p", &sense);
	MHVTL_DBG(1, "SENSE [Key/ASC/ASCQ] [%02x %02x %02x]",
				sense[2], sense[12], sense[13]);
}

int check_reset(uint8_t *sam_stat)
{
	int retval = reset;

	if (reset) {
		mkSenseBuf(UNIT_ATTENTION, E_POWERON_RESET, sam_stat);
		reset = 0;
	}
return retval;
}

void
reset_device(void)
{
	reset = 1;
}

#define READ_POSITION_LONG_LEN 32
/* Return tape position - long format
 *
 * Need to implement.
 * [ 4 -  7] Partition No.
 *           - The partition number for the current logical position
 * [ 8 - 15] Logical Object No.
 *           - The number of logical blocks between the beginning of the
 *           - partition and the current logical position.
 * [16 - 23] Logical Field Identifier
 *           - Number of Filemarks between the beginning of the partiion and
 *           - the logical position.
 * [24 - 31] Logical Set Identifier
 *           - Number of Setmarks between the beginning of the partiion and
 *           - the logical position.
 */
int resp_read_position_long(loff_t pos, uint8_t *buf, uint8_t *sam_stat)
{
	uint64_t partition = 1L;

	MHVTL_DBG(1, "Position %ld", (long)pos);

	memset(buf, 0, READ_POSITION_LONG_LEN);	/* Clear 'array' */

	if ((pos == 0) || (pos == 1))
		buf[0] = 0x80;	/* Begining of Partition */

	/* partition should be zero, as we only support one */
	buf[4] = (partition >> 24);
	buf[5] = (partition >> 16);
	buf[6] = (partition >> 8);
	buf[7] = partition;

	/* we don't know logical field identifier */
	/* FIXME: this code is wrong -- pos should be returned in [8 - 15] */
	buf[8] = buf[9]  = (pos >> 16);
	buf[6] = buf[10] = (pos >> 8);
	buf[7] = buf[11] = pos;

	return READ_POSITION_LONG_LEN;
}

#define READ_POSITION_LEN 20
/* Return tape position - short format */
int resp_read_position(loff_t pos, uint8_t *buf, uint8_t *sam_stat)
{
	memset(buf, 0, READ_POSITION_LEN);	/* Clear 'array' */

	if ((pos == 0) || (pos == 1))
		buf[0] = 0x80;	/* Begining of Partition */

	buf[4] = buf[8] = (pos >> 24);
	buf[5] = buf[9] = (pos >> 16);
	buf[6] = buf[10] = (pos >> 8);
	buf[7] = buf[11] = pos;
	MHVTL_DBG(1, "Positioned at block %ld", (long)pos);

	return READ_POSITION_LEN;
}

#define READBLOCKLIMITS_ARR_SZ 6
int resp_read_block_limits(struct vtl_ds *dbuf_p, int sz)
{
	uint8_t *arr = dbuf_p->data;

	memset(arr, 0, READBLOCKLIMITS_ARR_SZ);
	arr[1] = (sz >> 16);
	arr[2] = (sz >> 8);
	arr[3] = sz;
	arr[5] = 0x4;	/* Minimum block size */

	return READBLOCKLIMITS_ARR_SZ;
}

/*
 * Copy data in struct 'report_luns' into bufer and return length
 */
int resp_report_lun(struct report_luns *rpLUNs, uint8_t *buf, uint8_t *sam_stat)
{
	uint64_t size = ntohl(rpLUNs->size) + 8;

	memcpy( buf, (uint8_t *)&rpLUNs, size);
	return size;
}

/*
 * Respond with S/No. of media currently mounted
 */
int resp_read_media_serial(uint8_t *sno, uint8_t *buf, uint8_t *sam_stat)
{
	uint64_t size = 0L;

	MHVTL_DBG(1, "Read media S/No not implemented yet!");

	return size;
}

/*
 * Look thru NULL terminated array of struct mode[] for a match to pcode.
 * Return: struct mode * or NULL for no pcode
 */
struct mode *find_pcode(struct mode *m, uint8_t pcode, uint8_t subpcode)
{
	int i;

	MHVTL_DBG(3, "Looking for: pcode 0x%02x, subpcode 0x%02x",
					pcode, subpcode);

	for (i = 0; i < 32; i++, m++) {
		if (!m->pcode)
			break;	/* End of array */

		if ((m->pcode == pcode) && (m->subpcode == subpcode)) {
			MHVTL_DBG(2, "Found mode pages 0x%02x/0x%02x",
					m->pcode, m->subpcode);
			return m;
		}
	}

	MHVTL_DBG(3, "Page/subpage code 0x%02x/0x%02x not found",
					pcode, subpcode);

	return NULL;
}

/*
 * Add data for pcode to buffer pointed to by p
 * Return: Number of chars moved.
 */
int add_pcode(struct mode *m, uint8_t *p)
{
	memcpy(p, m->pcodePointer, m->pcodeSize);
	return m->pcodeSize;
}

/*
 * Used by mode sense/mode select struct.
 *
 * Allocate 'size' bytes & init to 0
 * set first 2 bytes:
 *  byte[0] = pcode
 *  byte[1] = size - sizeof(byte[0]
 *
 * Return pointer to mode structure being init. or NULL if alloc failed
 */
struct mode *alloc_mode_page(struct mode *m,
				uint8_t pcode, uint8_t subpcode, int size)
{
	struct mode * mp;

	MHVTL_DBG(3, "%p : Allocate mode page 0x%02x, size %d", m, pcode, size);

	mp = find_pcode(m, pcode, subpcode);
	if (mp) {
		mp->pcodePointer = malloc(size);
		MHVTL_DBG(3, "pcodePointer: %p for mode page 0x%02x",
			mp->pcodePointer, pcode);
		if (mp->pcodePointer) {	/* If ! null, set size of data */
			memset(mp->pcodePointer, 0, size);
			mp->pcodeSize = size;
			mp->pcodePointer[0] = pcode;
			mp->pcodePointer[1] = size
					 - sizeof(mp->pcodePointer[0])
					 - sizeof(mp->pcodePointer[1]);
			return mp;
		}
	}
	return NULL;
}

void initTapeAlert(struct TapeAlert_page *ta)
{
	int a;

	ta->pcode_head.pcode = TAPE_ALERT;
	ta->pcode_head.res = 0;
	ta->pcode_head.len = 100;
	for (a = 0; a < 64; a++) {
		ta->TapeAlert[a].flag.head0 = 0;
		ta->TapeAlert[a].flag.head1 = a + 1;
		ta->TapeAlert[a].flag.flags = 0xc0;
		ta->TapeAlert[a].flag.len = 1;
		ta->TapeAlert[a].value = 0;
	}
}

void setTapeAlert(struct TapeAlert_page *ta, uint64_t flg)
{
	int a;

	MHVTL_DBG(2, "Setting TapeAlert flags 0x%.8x %.8x",
				(uint32_t)(flg >> 32) & 0xffffffff,
				(uint32_t)flg & 0xffffffff);

	for (a = 0; a < 64; a++)
		ta->TapeAlert[a].value = (flg & (1ull << a)) ? 1 : 0;
}

/*
 * Simple function to read 'count' bytes from the chardev into 'buf'.
 */
int retrieve_CDB_data(int cdev, struct vtl_ds *ds)
{
	MHVTL_DBG(3, "retrieving %d bytes from kernel", ds->sz);
	ioctl(cdev, VTL_GET_DATA, ds);
	return ds->sz;
}


/*
 * Passes struct vtl_ds to kernel module.
 *   struct contains amount of data, status and pointer to data struct.
 *
 * Returns nothing.
 */
void completeSCSICommand(int cdev, struct vtl_ds *ds)
{
	uint8_t *s;

	MHVTL_DBG(2, "OP s/n: (%ld), sz: %d, sam_status: %d",
			(unsigned long)ds->serialNo,
			ds->sz, ds->sam_stat);

	ioctl(cdev, VTL_PUT_DATA, ds);

	s = ds->sense_buf;

	if (ds->sam_stat == SAM_STAT_CHECK_CONDITION)
		MHVTL_DBG(3, "[Key/ASC/ASCQ] [%02x %02x %02x]",
						s[2], s[12], s[13]);

	ds->sam_stat = 0;
}

/* Hex dump 'count' bytes at p  */
void hex_dump(uint8_t *p, int count)
{
	int j;

	for (j = 0; j < count; j++) {
		if ((j != 0) && (j % 16 == 0))
			printf("\n");
		printf("%02x ", p[j]);
	}
	printf("\n");
}

/* Writing to the kernel module will block until the device is created.
 * Unfortunately, we need to be polling the device and process the
 * SCSI op code before the lu can be created.
 * Chicken & Egg.
 * So spawn child process and don't wait for return.
 * Let the child process write to the kernel module
 */
pid_t add_lu(int minor, struct vtl_ctl *ctl)
{
	char str[1024];
	pid_t pid;
	ssize_t retval;
	int pseudo;
	char *pseudo_filename = "/sys/bus/pseudo/drivers/mhvtl/add_lu";
	char errmsg[512];

	sprintf(str, "add %d %d %d %d\n",
			minor, ctl->channel, ctl->id, ctl->lun);

	switch(pid = fork()) {
	case 0:         /* Child */
		pseudo = open(pseudo_filename, O_WRONLY);
		if (pseudo < 0) {
			sprintf(errmsg, "Could not open %s", pseudo_filename);
			MHVTL_DBG(1, "%s : %s", errmsg, strerror(errno));
			perror("Cound not open 'add_lu'");
			exit(-1);
		}
		retval = write(pseudo, str, strlen(str));
		MHVTL_DBG(2, "Wrote %s (%d bytes)", str, (int)retval);
		close(pseudo);
		MHVTL_DBG(1, "Child anounces 'lu created'.");
		exit(0);
		break;
	case -1:
		perror("Failed to fork()");
		MHVTL_DBG(1, "Fail to fork() %s", strerror(errno));
		return 0;
		break;
	default:
		MHVTL_DBG(1, "From a proud parent - birth of PID %ld",
					(long)pid);
		return pid;
		break;
	}
	return 0;
}

static int chrdev_get_major(void)
{
	FILE *f;
	char *filename = "/sys/bus/pseudo/drivers/mhvtl/major";
	int rc = 0;
	int x;
	int majno;

	f = fopen(filename, "r");
	if (!f) {
		MHVTL_DBG(1, "Can't open %s: %s", filename, strerror(errno));
		return -ENOENT;
	}
	x = fscanf(f, "%d", &majno);
	if (!x) {
		MHVTL_DBG(1, "Cant identify major number for mhvtl");
		rc = -1;
	} else
		rc = majno;

	fclose(f);
	return rc;
}

int chrdev_chown(uint8_t minor, uid_t uid, uid_t gid)
{
	char pathname[64];
	int x;

	snprintf(pathname, sizeof(pathname), "/dev/mhvtl%d", minor);

	MHVTL_DBG(3, "chown(%s, %d, %d)", pathname, (int)uid, (int)gid);
	x = chown(pathname, uid, uid);
	if (x == -1) {
		MHVTL_DBG(1, "Can't change ownership for device node mhvtl: %s",
			strerror(errno));
		return -1;
	}
	return 0;
}

int chrdev_create(uint8_t minor)
{
	int majno;
	int x;
	int ret = 0;
	dev_t dev;
	char pathname[64];

	snprintf(pathname, sizeof(pathname), "/dev/mhvtl%d", minor);

	majno = chrdev_get_major();
	if (majno == -ENOENT) {
		MHVTL_DBG(1, "** Incorrect version of kernel module loaded **");
		ret = -1;
		goto err;
	}

	dev = makedev(majno, minor);
	MHVTL_DBG(2, "Major number: %d, minor number: %d",
			major(dev), minor(dev));
	MHVTL_DBG(3, "mknod(%s, %02o, major: %d minor: %d",
		pathname, S_IFCHR|S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP,
		major(dev), minor(dev));
	x = mknod(pathname, S_IFCHR|S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP, dev);
	if (x < 0) {
		if (errno == EEXIST) /* Success if node already exists */
			return 0;

		MHVTL_DBG(1, "Error creating device node for mhvtl: %s",
			strerror(errno));
		ret = -1;
	}

err:
	return ret;
}

int chrdev_open(char *name, uint8_t minor)
{
	FILE *f;
	char devname[256];
	char buf[256];
	int devn;
	int ctlfd;

	f = fopen("/proc/devices", "r");
	if (!f) {
		printf("Cannot open control path to the driver: %s\n",
			strerror(errno));
		return -1;
	}

	devn = 0;
	while (!feof(f)) {
		if (!fgets(buf, sizeof(buf), f))
			break;
		if (sscanf(buf, "%d %s", &devn, devname) != 2)
			continue;
		if (!strcmp(devname, name))
			break;
		devn = 0;
	}
	fclose(f);
	if (!devn) {
		printf("Cannot find %s in /proc/devices - "
				"make sure the module is loaded\n", name);
		return -1;
	}
	snprintf(devname, sizeof(devname), "/dev/%s%d", name, minor);
	ctlfd = open(devname, O_RDWR|O_NONBLOCK);
	if (ctlfd < 0) {
		printf("Cannot open %s %s\n", devname, strerror(errno));
		fflush(NULL);
		printf("\n\n");
		return -1;
	}
	return ctlfd;
}

/*
 * Pinched straight from SCSI tgt code
 * Thanks guys..
 *
 * Modified to always return success. Earlier kernels don't have oom_adjust
 * 'feature' so don't fail if we can't find it..
 */
int oom_adjust(void)
{
	int fd, ret;
	char path[64];

	/* Avoid oom-killer */
	sprintf(path, "/proc/%d/oom_adj", getpid());
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		MHVTL_DBG(3, "Can't open oom-killer's pardon %s, %s",
				path, strerror(errno));
		return 0;
	}
	ret = write(fd, "-17\n", 4);
	if (ret < 0) {
		MHVTL_DBG(3, "Can't adjust oom-killer's pardon %s, %s",
				path, strerror(errno));
	}
	close(fd);
	return 0;
}

/* fgets but replace '\n' with null */
char *readline(char *buf, int len, FILE *s)
{
	int i;
	char *ret;

	ret = fgets(buf, len, s);
	if (!ret)
		return ret;

	/* Skip blank line */
	for (i = 1; i < len; i++)
		if (buf[i] == '\n')
			buf[i] = 0;

	MHVTL_DBG(3, "%s", buf);
	return ret;
}

/* Copy bytes from 'src' to 'dest, blank-filling to length 'len'.  There will
 * not be a NULL byte at the end.
*/
void blank_fill(uint8_t *dest, char *src, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (*src != '\0')
			*dest++ = *src++;
		else
			*dest++ = ' ';
	}
}

void log_opcode(char *opcode, uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	MHVTL_DBG(1, "*** Unsupported op code: %s ***", opcode);
	mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, &dbuf_p->sam_stat);
	MHVTL_DBG_PRT_CDB(1, dbuf_p->serialNo, cdb);
}

/*
 * Send a ping message to the queue & wait for a response...
 * If we get a response after 2 seconds, there must be another
 * daemon listening on this message queue.
 */
int check_for_running_daemons(int minor)
{
	return 0;
}

/* Abort if string length > len */
void checkstrlen(char *s, int len)
{
	if (strlen(s) > len) {
		MHVTL_DBG(1, "String %s is > %d... Aborting", s, len);
		printf("String %s longer than %d chars\n", s, len);
		printf("Please fix config file\n");
		abort();
	}
}

int device_type_register(struct lu_phy_attr *lu, struct device_type_template *t)
{
	lu->scsi_ops = t;
	return 0;
}

uint8_t set_compression_mode_pg(struct mode *sm, int lvl)
{
	struct mode *m;
	uint8_t *p;

	MHVTL_DBG(3, "*** Trace ***");

	/* Find pointer to Data Compression mode Page */
	m = find_pcode(sm, 0x0f, 0);
	MHVTL_DBG(3, "sm: %p, m: %p, m->pcodePointer: %p",
			sm, m, m->pcodePointer);
	if (m) {
		p = m->pcodePointer;
		p[2] |= 0x80;	/* Set data compression enable */
	}
	/* Find pointer to Device Configuration mode Page */
	m = find_pcode(sm, 0x10, 0);
	MHVTL_DBG(3, "sm: %p, m: %p, m->pcodePointer: %p",
			sm, m, m->pcodePointer);
	if (m) {
		p = m->pcodePointer;
		p[14] = lvl;
	}
	return SAM_STAT_GOOD;
}

uint8_t clear_compression_mode_pg(struct mode *sm)
{
	struct mode *m;
	uint8_t *p;

	MHVTL_DBG(3, "*** Trace ***");

	/* Find pointer to Data Compression mode Page */
	m = find_pcode(sm, 0x0f, 0);
	MHVTL_DBG(3, "sm: %p, m: %p, m->pcodePointer: %p",
			sm, m, m->pcodePointer);
	if (m) {
		p = m->pcodePointer;
		p[2] &= 0x7f;	/* clear data compression enable */
	}
	/* Find pointer to Device Configuration mode Page */
	m = find_pcode(sm, 0x10, 0);
	MHVTL_DBG(3, "sm: %p, m: %p, m->pcodePointer: %p",
			sm, m, m->pcodePointer);
	if (m) {
		p = m->pcodePointer;
		p[14] = Z_NO_COMPRESSION;
	}
	return SAM_STAT_GOOD;
}

uint8_t clear_WORM(struct mode *sm)
{
	uint8_t *smp_dp;
	struct mode *m;

	m = find_pcode(sm, 0x1d, 0);
	MHVTL_DBG(3, "sm: %p, m: %p, m->pcodePointer: %p",
			sm, m, m->pcodePointer);
	if (m) {
		smp_dp = m->pcodePointer;
		if (!smp_dp)
			return SAM_STAT_GOOD;
		smp_dp[2] = 0x0;
	}

	return SAM_STAT_GOOD;
}

uint8_t set_WORM(struct mode *sm)
{
	uint8_t *smp_dp;
	struct mode *m;

	MHVTL_DBG(3, "*** Trace ***");

	m = find_pcode(sm, 0x1d, 0);
	MHVTL_DBG(3, "sm: %p, m: %p, m->pcodePointer: %p",
			sm, m, m->pcodePointer);
	if (m) {
		smp_dp = m->pcodePointer;
		if (!smp_dp)
			return SAM_STAT_GOOD;
		smp_dp[2] = 0x10;
		smp_dp[4] = 0x01; /* Indicate label overwrite */
	}

	return SAM_STAT_GOOD;
}

/* Remove newline from string and fill rest of 'len' with char 'c' */
void rmnl(char *s, unsigned char c, int len)
{
	int i;
	int found = 0;

	for (i = 0; i < len; i++) {
		if (s[i] == '\n')
			found = 1;
		if (found)
			s[i] = c;
	}
}

void update_vpd_b0(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xb0)];
	uint8_t *worm;

	worm = p;

	*vpd_pg->data = (*worm) ? 1 : 0;        /* Set WORM bit */
}

void update_vpd_b1(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xb1)];

	memcpy(vpd_pg->data, p, vpd_pg->sz);
}

void update_vpd_b2(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xb2)];

	memcpy(vpd_pg->data, p, vpd_pg->sz);
}

void update_vpd_c0(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xc0)];

	memcpy(&vpd_pg->data[20], p, strlen(p));
}

void update_vpd_c1(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xc1)];

	memcpy(vpd_pg->data, p, vpd_pg->sz);
}

