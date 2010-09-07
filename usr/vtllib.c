/*
 * Shared routines between vxtape & vxlibrary
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

	sense[0] = 0xf0;        /* Valid, current error */
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

/*
 * Allow/Prevent media removal.
 *
 * basically a no-op but log the cmd.
 */

void resp_allow_prevent_removal(uint8_t *cdb, uint8_t *sam_stat)
{
	MHVTL_DBG(1, "%s", (cdb[4]) ? "Prevent MEDIUM removal **" :
					 "Allow MEDIUM Removal **");
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

void resp_log_select(uint8_t *cdb, uint8_t *sam_stat)
{

	char pcr = cdb[1] & 0x1;
	uint16_t	parmList;
	char	*parmString = "Undefined";

	parmList = ntohs((uint16_t)cdb[7]); /* bytes 7 & 8 are parm list. */

	MHVTL_DBG(1, "LOG SELECT %s",
				(pcr) ? ": Parameter Code Reset **" : "**");

	if (pcr) {	/* Check for Parameter code reset */
		if (parmList) {	/* If non-zero, error */
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
						sam_stat);
			return;
		}
		switch ((cdb[2] & 0xc0) >> 5) {
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
	MHVTL_DBG(1, "Position %ld", (long)pos);

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
struct mode *find_pcode(uint8_t pcode, struct mode *m)
{
	int a;

	MHVTL_DBG(3, "Entered: pcode 0x%x", pcode);

	for (a = 0; a < 0x3f; a++, m++) { /* Possibility for 0x3f page codes */
		if (m->pcode == 0x0)
			break;	/* End of list */
		if (m->pcode == pcode) {
			MHVTL_DBG(2, "(0x%x): match pcode %d",
					pcode, m->pcode);
			return m;
		}
	}

	MHVTL_DBG(3, "Page code 0x%x not found", pcode);

	return NULL;
}

/*
 * Add data for pcode to buffer pointed to by p
 * Return: Number of chars moved.
 */
static int add_pcode(struct mode *m, uint8_t *p)
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
struct mode * alloc_mode_page(uint8_t pcode, struct mode *m, int size)
{
	struct mode * mp;

	if ((mp = find_pcode(pcode, m))) { /* Find correct page */
		mp->pcodePointer = malloc(size);
		if (mp->pcodePointer) {	/* If ! null, set size of data */
			memset(mp->pcodePointer, 0, size);
			mp->pcodeSize = size;
			mp->pcodePointer[0] = pcode;
			mp->pcodePointer[1] = size
					 - sizeof(mp->pcodePointer[0])
					 - sizeof(mp->pcodePointer[1]);
		}
		return mp;
	}
	return NULL;
}


/*
 * Build mode sense data into *buf
 * Return size of data.
 */
int resp_mode_sense(uint8_t *cmd, uint8_t *buf, struct mode *m, uint8_t WriteProtect, uint8_t *sam_stat)
{
	int pcontrol, pcode, subpcode;
	int media_type;
	int alloc_len, msense_6;
	int dev_spec, len = 0;
	int offset = 0;
	uint8_t *ap;
	struct mode *smp;	/* Struct mode pointer... */
	int a;

#ifdef MHVTL_DEBUG
	char *pcontrolString[] = {
		"Current configuration",
		"Every bit that can be modified",
		"Power-on configuration",
		"Power-on configuration"
	};
#endif

	/* Disable Block Descriptors */
	uint8_t blockDescriptorLen = (cmd[1] & 0x8) ? 0 : 8;

	/*
	 pcontrol => page control
		00 -> 0: Report current vaules
		01 -> 1: Report Changable Vaules
		10 -> 2: Report default values
		11 -> 3: Report saved values
	*/
	pcontrol = (cmd[2] & 0xc0) >> 6;
	/* pcode -> Page Code */
	pcode = cmd[2] & 0x3f;
	subpcode = cmd[3];
	msense_6 = (MODE_SENSE == cmd[0]);

	alloc_len = msense_6 ? cmd[4] : ((cmd[7] << 8) | cmd[8]);
	offset = msense_6 ? 4 : 8;

	MHVTL_DBG(2, " Mode Sense %d byte version", (msense_6) ? 6 : 10);
	MHVTL_DBG(2, " Page Control  : %s(0x%x)",
				pcontrolString[pcontrol], pcontrol);
	MHVTL_DBG(2, " Page Code     : 0x%x", pcode);
	MHVTL_DBG(2, " Disable Block Descriptor => %s",
				(blockDescriptorLen) ? "No" : "Yes");
	MHVTL_DBG(2, " Allocation len: %d", alloc_len);

	if (0x3 == pcontrol) {  /* Saving values not supported */
		mkSenseBuf(ILLEGAL_REQUEST, E_SAVING_PARMS_UNSUP, sam_stat);
		return 0;
	}

	memset(buf, 0, alloc_len);	/* Set return data to null */
	dev_spec = (WriteProtect ? 0x80 : 0x00) | 0x10;
	media_type = 0x0;

	offset += blockDescriptorLen;
	ap = buf + offset;

	if (0 != subpcode) { /* TODO: Control Extension page */
		MHVTL_DBG(1, "Non-zero sub-page sense code not supported");
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sam_stat);
		return 0;
	}

	MHVTL_DBG(3, "pcode: 0x%02x", pcode);

	if (0x0 == pcode) {
		len = 0;
	} else if (0x3f == pcode) {	/* Return all pages */
		for (a = 1; a < 0x3f; a++) { /* Walk thru all possibilities */
			smp = find_pcode(a, m);
			if (smp)
				len += add_pcode(smp, (uint8_t *)ap + len);
		}
	} else {
		smp = find_pcode(pcode, m);
		if (smp)
			len = add_pcode(smp, (uint8_t *)ap);
	}
	offset += len;

	if (pcode != 0)	/* 0 = No page code requested */
		if (0 == len) {	/* Page not found.. */
		MHVTL_DBG(2, "Unknown mode page : %d", pcode);
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sam_stat);
		return 0;
		}

	/* Fill in header.. */
	if (msense_6) {
		buf[0] = offset - 1;	/* size - sizeof(buf[0]) field */
		buf[1] = media_type;
		buf[2] = dev_spec;
		buf[3] = blockDescriptorLen;
		/* If the length > 0, copy Block Desc. */
		if (blockDescriptorLen)
			memcpy(&buf[4],blockDescriptorBlock,blockDescriptorLen);
	} else {
		put_unaligned_be16(offset - 2, &buf[0]);
		buf[2] = media_type;
		buf[3] = dev_spec;
		put_unaligned_be16(blockDescriptorLen, &buf[6]);
		/* If the length > 0, copy Block Desc. */
		if (blockDescriptorLen)
			memcpy(&buf[8],blockDescriptorBlock,blockDescriptorLen);
	}

	if (debug) {
		printf("mode sense: Returning %d bytes\n", offset);
		hex_dump(buf, offset);
	}
	return offset;
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
	MHVTL_DBG(2, "Setting TapeAlert flags 0x%.8x %.8x",
				(uint32_t)(flg >> 32) & 0xffffffff,
				(uint32_t)flg & 0xffffffff);

	int a;
	for (a = 0; a < 64; a++) {
		ta->TapeAlert[a].value = (flg & (1ull << a)) ? 1 : 0;
/*		MHVTL_DBG(2, "TapeAlert flag %016" PRIx64 " : %s",
 *				(uint64_t)1ull << a,
 *				(ta->TapeAlert[a].value) ? "set" : "unset");
 */
	}

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
	int ret = 0;
	int x;
	int majno;

	f = fopen(filename, "r");
	if (!f) {
		MHVTL_DBG(1, "Can't open %s: %s", filename, strerror(errno));
		ret = -1;
		goto err;
	}
	x = fscanf(f, "%d", &majno);
	if (!x) {
		MHVTL_DBG(1, "Cant identify major number for mhvtl");
		ret = -1;
	} else
		ret = majno;

err:
	fclose(f);
	return ret;
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
	if (majno == -1) {
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
void blank_fill(uint8_t *dest, uint8_t *src, int len)
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

int resp_a3_service_action(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	uint8_t sa = cdb[1];
	switch (sa) {
	case MANAGEMENT_PROTOCOL_IN:
		log_opcode("MANAGEMENT PROTOCOL IN **", cdb, dbuf_p);
		break;
	case REPORT_ALIASES:
		log_opcode("REPORT ALIASES **", cdb, dbuf_p);
		break;
	}
	log_opcode("Unknown service action A3 **", cdb, dbuf_p);
	return 0;
}

int resp_a4_service_action(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	uint8_t sa = cdb[1];

	switch (sa) {
	case MANAGEMENT_PROTOCOL_OUT:
		log_opcode("MANAGEMENT PROTOCOL OUT **", cdb, dbuf_p);
		break;
	case CHANGE_ALIASES:
		log_opcode("CHANGE ALIASES **", cdb, dbuf_p);
		break;
	}
	log_opcode("Unknown service action A4 **", cdb, dbuf_p);
	return 0;
}
#if !defined(ROS2)
int ProcessSendDiagnostic(uint8_t *cdb, unsigned int sz, struct vtl_ds *dbuf_p)
{
	log_opcode("Send Diagnostics", cdb, dbuf_p);
	return 0;
}

int ProcessReceiveDiagnostic(uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	log_opcode("Receive Diagnostics", cdb, dbuf_p);
	return 0;
}
#endif

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

