/*
 * Shared routines between vtltape & vtllibrary
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
 */
#include <stdint.h>

#ifndef Solaris
 #include <byteswap.h>
#endif

#define __STDC_FORMAT_MACROS

#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/ipc.h>
#include <semaphore.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <time.h>
#include <assert.h>
#include "be_byteshift.h"
#include "list.h"
#include "scsi.h"
#include "vtl_common.h"
#include "logging.h"
#include "vtllib.h"
#include "smc.h"
#include "vtltape.h"
#include "mode.h"
#include "q.h"
#include "ssc.h"
#include "log.h"
#include "q.h"

static int reset = 0;

static struct state_description {
	char *state_desc;
} state_desc[] = {
	{ "Initialising v2", },
	{ "Idle", },
	{ "Unloading", },
	{ "Loading", },
	{ "Loading Cleaning Tape", },
	{ "Loading WORM media", },
	{ "Loading NULL media", },
	{ "Loaded", },
	{ "Loaded - Idle", },
	{ "Load failed", },
	{ "Rewinding", },
	{ "Positioning", },
	{ "Locate", },
	{ "Reading", },
	{ "Writing", },
	{ "Unloading", },
	{ "Erasing", },

	{ "Moving media from drive to slot", },
	{ "Moving media from slot to drive", },
	{ "Moving media from drive to MAP", },
	{ "Moving media from MAP to drive", },
	{ "Moving media from slot to MAP", },
	{ "Moving media from MAP to slot", },
	{ "Moving media from drive to drive", },
	{ "Moving media from slot to slot", },
	{ "Opening MAP", },
	{ "Closing MAP", },
	{ "Robot Inventory", },
	{ "Initialise Elements", },
	{ "Online", },
	{ "Offline", },
	{ "System Uninitialised", },
};

static char *slot_type_string[] = {
	"ANY",
	"Picker",
	"Storage",
	"MAP",
	"Drive",
};

uint8_t sense[SENSE_BUF_SIZE];
uint8_t modeBlockDescriptor[8] = {0, 0, 0, 0, 0, 0, 0, 0 };

void mhvtl_prt_cdb(int lvl, struct scsi_cmd *cmd)
{
	int groupCode;
	uint8_t *cdb = cmd->scb;
#ifdef MHVTL_DEBUG
	uint64_t sn = cmd->dbuf_p->serialNo;
	uint64_t delay = (uint64_t)cmd->pollInterval;
#endif

	groupCode = (cdb[0] & 0xe0) >> 5;
	switch (groupCode) {
	case 0:	/*  6 byte commands */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") (delay %" PRId64 "): "
				"%02x %02x %02x %02x %02x %02x",
			sn, delay,
			cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5]);
		break;
	case 1: /* 10 byte commands */
	case 2: /* 10 byte commands */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") (delay %" PRId64 "): "
			"%02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x",
			sn, delay,
			cdb[0], cdb[1], cdb[2], cdb[3],
			cdb[4], cdb[5], cdb[6], cdb[7],
			cdb[8], cdb[9]);
		break;
	case 3: /* Reserved - There is always one exception ;) */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") (delay %" PRId64 "): "
			"%02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x %02x",
			sn, delay,
			cdb[0], cdb[1], cdb[2], cdb[3],
			cdb[4], cdb[5], cdb[6], cdb[7],
			cdb[8], cdb[9], cdb[10], cdb[11]);
		break;
	case 4: /* 16 byte commands */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") (delay %" PRId64 "): "
			"%02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			sn, delay,
			cdb[0], cdb[1], cdb[2], cdb[3],
			cdb[4], cdb[5], cdb[6], cdb[7],
			cdb[8], cdb[9], cdb[10], cdb[11],
			cdb[12], cdb[13], cdb[14], cdb[15]);
		break;
	case 5: /* 12 byte commands */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") (delay %" PRId64 "): "
			"%02x %02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x",
			sn, delay,
			cdb[0], cdb[1], cdb[2], cdb[3],
			cdb[4], cdb[5], cdb[6], cdb[7],
			cdb[8], cdb[9], cdb[10], cdb[11]);
		break;
	case 6: /* Vendor Specific */
	case 7: /* Vendor Specific */
		MHVTL_DBG_NO_FUNC(lvl, "CDB (%" PRId64 ") (delay %" PRId64 "), "
					"VENDOR SPECIFIC !! "
			" %02x %02x %02x %02x %02x %02x",
			sn, delay,
			cdb[0], cdb[1], cdb[2], cdb[3],
			cdb[4], cdb[5]);
		break;
	}
}

/*
 * zalloc(int size)
 *
 * Wrapper to first call malloc() and zero out any allocated space.
 */
void *zalloc(int sz)
{
	void *p = malloc(sz);

	if (p)
		memset(p, 0, sz);

	return p;
}

/*
 * Fills in a global array with current sense data
 * Sets 'sam status' to SAM_STAT_CHECK_CONDITION.
 */

static void return_sense(uint8_t key, uint32_t asc_ascq, struct s_sd *sd,
						uint8_t *sam_stat)
{
	char extended[32];

	/* Clear Sense key status */
	memset(sense, 0, SENSE_BUF_SIZE);

	*sam_stat = SAM_STAT_CHECK_CONDITION;

	sense[0] = SD_CURRENT_INFORMATION_FIXED;
	/* SPC4 (Revision 30) Ch: 4.5.1 states:
	 * The RESPONSE CODE field shall be set to 70h in all unit attention
	 * condition sense data in which:
	 * - The ADDITIONAL SENSE CODE field is set to 29h
	 * - The ADDITIONAL SENSE CODE is set to MODE PARAMETERS CHANGED
	 */
	switch (key) {
	case UNIT_ATTENTION:
		if ((asc_ascq >> 8) == 0x29)
			break;
		if (asc_ascq == E_MODE_PARAMETERS_CHANGED)
			break;
		/* Fall thru to default handling */
	default:
		sense[0] |= SD_VALID;
		break;
	}

	sense[2] = key;
	sense[7] = SENSE_BUF_SIZE - 8;
	put_unaligned_be16(asc_ascq, &sense[12]);

	if (sd) {
		sense[15] = sd->byte0;
		put_unaligned_be16(sd->field_pointer, &sense[16]);
		sprintf(extended, " 0x%02x %04x", sd->byte0, sd->field_pointer);
	}

	MHVTL_DBG(1, "[Key/ASC/ASCQ] [%02x %02x %02x]%s",
				sense[2], sense[12], sense[13],
				(sd) ? extended : "");
}

void sam_unit_attention(uint16_t ascq, uint8_t *sam_stat)
{
	return_sense(UNIT_ATTENTION, ascq, NULL, sam_stat);
}

void sam_not_ready(uint16_t ascq, uint8_t *sam_stat)
{
	return_sense(NOT_READY, ascq, NULL, sam_stat);
}

void sam_illegal_request(uint16_t ascq, struct s_sd *sd, uint8_t *sam_stat)
{
	return_sense(ILLEGAL_REQUEST, ascq, sd, sam_stat);
}

void sam_medium_error(uint16_t ascq, uint8_t *sam_stat)
{
	return_sense(MEDIUM_ERROR, ascq, NULL, sam_stat);
}

void sam_blank_check(uint16_t ascq, uint8_t *sam_stat)
{
	return_sense(BLANK_CHECK, ascq, NULL, sam_stat);
}

void sam_data_protect(uint16_t ascq, uint8_t *sam_stat)
{
	return_sense(DATA_PROTECT, ascq, NULL, sam_stat);
}

void sam_hardware_error(uint16_t ascq, uint8_t *sam_stat)
{
	return_sense(HARDWARE_ERROR, ascq, NULL, sam_stat);
}

void sam_no_sense(uint8_t key, uint16_t ascq, uint8_t *sam_stat)
{
	return_sense(NO_SENSE | key, ascq, NULL, sam_stat);
}

int check_reset(uint8_t *sam_stat)
{
	int retval = reset;

	if (reset) {
		sam_unit_attention(E_POWERON_RESET, sam_stat);
		reset = 0;
	}
return retval;
}

void reset_device(void)
{
	reset = 1;

/*
http://scaryreasoner.wordpress.com/2009/02/28/checking-sizeof-at-compile-time/

If this fails to compile - sizeof MAM != 1024 bytes !
*/
	BUILD_BUG_ON(sizeof(struct MAM) % 1024);
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
 *           - Number of Filemarks between the beginning of the partition and
 *           - the logical position.
 * [24 - 31] Logical Set Identifier
 *           - Number of Setmarks between the beginning of the partition and
 *           - the logical position.
 */
int resp_read_position_long(loff_t pos, uint8_t *buf, uint8_t *sam_stat)
{
	uint32_t partition = 0;

	memset(buf, 0, READ_POSITION_LONG_LEN);	/* Clear 'array' */

	if ((pos == 0) || (pos == 1))
		buf[0] = 0x80;	/* Beginning of Partition */
	buf[0] |= 0x04;	/* Set LONU bit valid */

	/* FIXME: Need to update EOP & BPEW bits too */

	put_unaligned_be32(partition, &buf[4]);
	put_unaligned_be64(pos, &buf[8]);

	MHVTL_DBG(1, "Positioned at block %ld", (long)pos);
	return READ_POSITION_LONG_LEN;
}

#define READ_POSITION_LEN 20
/* Return tape position - short format */
int resp_read_position(loff_t pos, uint8_t *buf, uint8_t *sam_stat)
{
	uint8_t partition = 0;

	memset(buf, 0, READ_POSITION_LEN);	/* Clear 'array' */

	if ((pos == 0) || (pos == 1))
		buf[0] = 0x80;	/* Beginning of Partition */
	buf[0] |= 0x20;	/* Logical object count unknown */
	buf[0] |= 0x10;	/* Logical byte count unknown  */

	/* FIXME: Need to update EOP & BPEW bits too */

	buf[1] = partition;
	put_unaligned_be32(pos, &buf[4]);
	put_unaligned_be32(pos, &buf[8]);
	MHVTL_DBG(1, "Positioned at block %ld", (long)pos);

	return READ_POSITION_LEN;
}

#define READBLOCKLIMITS_ARR_SZ 6
int resp_read_block_limits(struct vtl_ds *dbuf_p, int sz)
{
	uint8_t *arr = (uint8_t *)dbuf_p->data;

	MHVTL_DBG(2, "Min/Max sz: %d/%d", 4, sz);
	memset(arr, 0, READBLOCKLIMITS_ARR_SZ);
	put_unaligned_be24(sz, &arr[1]);
	arr[5] = 0x4;	/* Minimum block size */

	return READBLOCKLIMITS_ARR_SZ;
}

/*
 * Respond with S/No. of media currently mounted
 */
uint32_t resp_read_media_serial(uint8_t *sno, uint8_t *buf, uint8_t *sam_stat)
{
	uint32_t size = 38;

	snprintf((char *)&buf[4], size - 5, "%-34s", sno);
	put_unaligned_be16(size, &buf[2]);

	return size;
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
	int ioctl_err;

	MHVTL_DBG(3, "retrieving %d bytes from kernel", ds->sz);
	ioctl_err = ioctl(cdev, VTL_GET_DATA, ds);
	if (ioctl_err < 0) {
		MHVTL_ERR("Failed retriving data via ioctl(): %s",
				strerror(errno));
		return 0;
	}
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

	ioctl(cdev, VTL_PUT_DATA, ds);

	s = (uint8_t *)ds->sense_buf;

	if (ds->sam_stat == SAM_STAT_CHECK_CONDITION) {
		MHVTL_DBG(2, "s/n: (%ld), sz: %d, sam_status: %d"
			" [%02x %02x %02x]",
			(unsigned long)ds->serialNo,
			ds->sz, ds->sam_stat,
			s[2], s[12], s[13]);
	} else {
		MHVTL_DBG(2, "OP s/n: (%ld), sz: %d, sam_status: %d",
			(unsigned long)ds->serialNo,
			ds->sz, ds->sam_stat);
	}

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
pid_t add_lu(unsigned minor, struct vtl_ctl *ctl)
{
	char str[1024];
	pid_t pid;
	ssize_t retval;
	int pseudo;
	char pseudo_filename[256];
	char errmsg[512];
	struct stat km;

	sprintf(str, "add %u %d %d %d\n",
			minor, ctl->channel, ctl->id, ctl->lun);

	snprintf(pseudo_filename, ARRAY_SIZE(pseudo_filename),
				"/sys/bus/pseudo9/drivers/mhvtl/add_lu");
	pseudo = stat(pseudo_filename, &km);
	if (pseudo < 0)
		snprintf(pseudo_filename, ARRAY_SIZE(pseudo_filename),
				"/sys/bus/pseudo/drivers/mhvtl/add_lu");

	switch (pid = fork()) {
	case 0:         /* Child */
		pseudo = open(pseudo_filename, O_WRONLY);
		if (pseudo < 0) {
			snprintf(errmsg, ARRAY_SIZE(errmsg),
					"Could not open %s", pseudo_filename);
			MHVTL_DBG(1, "%s : %s", errmsg, strerror(errno));
			perror("Could not open 'add_lu'");
			exit(-1);
		}
		retval = write(pseudo, str, strlen(str));
		MHVTL_DBG(2, "Wrote %s (%d bytes)", str, (int)retval);
		close(pseudo);
		MHVTL_DBG(1, "Child anounces 'lu [%d:%d:%d] created'.",
					ctl->channel, ctl->id, ctl->lun);
		exit(0);
		break;
	case -1:
		perror("Failed to fork()");
		MHVTL_DBG(1, "Fail to fork() %s", strerror(errno));
		return 0;
		break;
	default:
		MHVTL_DBG(1, "Child PID %ld starting logical unit [%d:%d:%d]",
					(long)pid, ctl->channel,
					ctl->id, ctl->lun);
		return pid;
		break;
	}
	return 0;
}

static int chrdev_get_major(void)
{
	FILE *f;
	char filename[256];
	int pseudo;
	int rc = 0;
	int x;
	int majno;
	struct stat km;

	snprintf(filename, ARRAY_SIZE(filename),
				"/sys/bus/pseudo9/drivers/mhvtl/major");
	pseudo = stat(filename, &km);
	if (pseudo < 0)
		snprintf(filename, ARRAY_SIZE(filename),
				"/sys/bus/pseudo/drivers/mhvtl/major");

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

int chrdev_chown(unsigned minor, uid_t uid, uid_t gid)
{
	char pathname[64];
	int x;

	snprintf(pathname, sizeof(pathname), "/dev/mhvtl%u", minor);

	MHVTL_DBG(3, "chown(%s, %d, %d)", pathname, (int)uid, (int)gid);
	x = chown(pathname, uid, uid);
	if (x == -1) {
		MHVTL_DBG(1, "Can't change ownership for device node mhvtl: %s",
			strerror(errno));
		return -1;
	}
	return 0;
}

int chrdev_create(unsigned minor)
{
	int majno;
	int x;
	int ret = 0;
	dev_t dev;
	char pathname[64];

	snprintf(pathname, sizeof(pathname), "/dev/mhvtl%u", minor);

	majno = chrdev_get_major();
	if (majno == -ENOENT) {
		MHVTL_DBG(1, "** Incorrect version of kernel module loaded **");
		ret = -1;
		goto err;
	}

	dev = makedev(majno, minor);
	MHVTL_DBG(2, "Major number: %d, minor number: %u",
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

int chrdev_open(char *name, unsigned minor)
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
	snprintf(devname, sizeof(devname), "/dev/%s%u", name, minor);
	ctlfd = open(devname, O_RDWR|O_NONBLOCK|O_EXCL);
	if (ctlfd < 0) {
		printf("Cannot open %s %s\n", devname, strerror(errno));
		fflush(NULL);
		printf("\n\n");
		return -1;
	}
	return ctlfd;
}

/* Create the fifo and open it for writing (appending)
 * Return 0 on success,
 * Return errno on failure
 */
int open_fifo(FILE **fifo_fd, char *fifoname)
{
	int ret;

	umask(0);
	ret = 0;

	ret = mknod(fifoname, S_IFIFO|0644, 0);
	if ((ret < 0) && (errno != EEXIST)) {
		MHVTL_LOG("Sorry, cant create %s: %s, Disabling fifo feature",
				fifoname, strerror(errno));
		ret = errno;
	} else {
		*fifo_fd = fopen(fifoname, "w+");
		if (*fifo_fd) {
			MHVTL_DBG(2, "Successfully opened named pipe: %s",
						fifoname);
		} else {
			MHVTL_LOG("Sorry, cant open %s: %s, "
					"Disabling fifo feature",
						fifoname, strerror(errno));
			ret = errno;
		}
	}
	return ret;
}

void status_change(FILE *fifo_fd, int current_status, int m_id, char **msg)
{
	time_t t;
	char *timestamp;
	unsigned i;

	if (!fifo_fd)
		return;

	t = time(NULL);
	timestamp = ctime(&t);

	for (i = 14; i < strlen(timestamp); i++)
		if (timestamp[i] == '\n')
			timestamp[i] = '\0';

	if (*msg) {
		fprintf(fifo_fd, "%s - %d: - %s\n",
				timestamp, m_id, *msg);
		free(*msg);
		*msg = NULL;
	} else
		fprintf(fifo_fd, "%s - %d: - %s\n", timestamp, m_id,
				state_desc[current_status].state_desc);

	fflush(fifo_fd);

	return;
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

void truncate_spaces(char *s, int maxlen)
{
	int x;

	for (x = 0; x < maxlen; x++)
		if ((s[x] == ' ') || (s[x] == '\0')) {
			s[x] = '\0';
			return;
		}
}

/* MHVTL_VERSION looks like : 0.18.xx or 1.xx.xx
 *
 * Convert into a string after converting the 18.xx into "18xx"
 *
 * NOTE: Caller has to free string after use.
 */
char *get_version(void)
{
	char b[64];
	int x, y, z;
	char *c;

	c = (char *)zalloc(32);	/* Way more than enough for a 4 byte string */
	if (!c)
		return NULL;

	sprintf(b, "%s", MHVTL_VERSION);

	sscanf(b, "%d.%d.%d", &x, &y, &z);
	if (x)
		sprintf(c, "%02d%02d", x, y);
	else
		sprintf(c, "%02d%02d", y, z);

	return c;
}

void log_opcode(char *opcode, struct scsi_cmd *cmd)
{
	struct s_sd sd;

	MHVTL_DBG(1, "*** Unsupported op code: %s ***", opcode);
	sd.byte0 = SKSV | CD;
	sd.field_pointer = 0;
	sam_illegal_request(E_INVALID_OP_CODE, &sd, &cmd->dbuf_p->sam_stat);
	MHVTL_DBG_PRT_CDB(1, cmd);
}

/*
 * Send a ping message to the queue & wait for a response...
 * If we get a response after 2 seconds, there must be another
 * daemon listening on this message queue.
 */
int check_for_running_daemons(unsigned minor)
{
	return 0;
}

/* Abort if string length > len */
void checkstrlen(char *s, unsigned len, int lineno)
{
	if (strlen(s) > len) {
		MHVTL_DBG(1, "Line #: %d, String %s is > %d... Aborting",
						lineno, s, len);
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

uint8_t set_compression_mode_pg(struct list_head *l, int lvl)
{
	struct mode *m;
	uint8_t *p;

	MHVTL_DBG(3, "*** Trace ***");

	/* Find pointer to Data Compression mode Page */
	m = lookup_pcode(l, MODE_DATA_COMPRESSION, 0);
	MHVTL_DBG(3, "l: %p, m: %p, m->pcodePointer: %p",
			l, m, m->pcodePointer);
	if (m) {
		p = m->pcodePointer;
		p[2] |= 0x80;	/* Set data compression enable */
	}
	/* Find pointer to Device Configuration mode Page */
	m = lookup_pcode(l, MODE_DEVICE_CONFIGURATION, 0);
	MHVTL_DBG(3, "l: %p, m: %p, m->pcodePointer: %p",
			l, m, m->pcodePointer);
	if (m) {
		p = m->pcodePointer;
		p[14] = lvl;
	}
	return SAM_STAT_GOOD;
}

uint8_t clear_compression_mode_pg(struct list_head *l)
{
	struct mode *m;
	uint8_t *p;

	MHVTL_DBG(3, "*** Trace ***");

	/* Find pointer to Data Compression mode Page */
	m = lookup_pcode(l, MODE_DATA_COMPRESSION, 0);
	MHVTL_DBG(3, "l: %p, m: %p, m->pcodePointer: %p",
			l, m, m->pcodePointer);
	if (m) {
		p = m->pcodePointer;
		p[2] &= 0x7f;	/* clear data compression enable */
	}
	/* Find pointer to Device Configuration mode Page */
	m = lookup_pcode(l, MODE_DEVICE_CONFIGURATION, 0);
	MHVTL_DBG(3, "l: %p, m: %p, m->pcodePointer: %p",
			l, m, m->pcodePointer);
	if (m) {
		p = m->pcodePointer;
		p[14] = MHVTL_NO_COMPRESSION;
	}
	return SAM_STAT_GOOD;
}

uint8_t clear_WORM(struct list_head *l)
{
	uint8_t *smp_dp;
	struct mode *m;

	m = lookup_pcode(l, MODE_MEDIUM_CONFIGURATION, 0);
	if (!m) {
		MHVTL_DBG(3, "Did not find MODE_MEDIUM_CONFIGURATION page");
	} else {
		MHVTL_DBG(3, "l: %p, m: %p, m->pcodePointer: %p",
			l, m, m->pcodePointer);

		smp_dp = m->pcodePointer;
		if (!smp_dp)
			return SAM_STAT_GOOD;
		smp_dp[2] = 0x0;
	}

	return SAM_STAT_GOOD;
}

uint8_t set_WORM(struct list_head *l)
{
	uint8_t *smp_dp;
	struct mode *m;

	MHVTL_DBG(3, "*** Trace ***");

	m = lookup_pcode(l, MODE_MEDIUM_CONFIGURATION, 0);
	if (!m) {
		MHVTL_DBG(3, "Did not find MODE_MEDIUM_CONFIGURATION page");
	} else {
		MHVTL_DBG(3, "l: %p, m: %p, m->pcodePointer: %p",
			l, m, m->pcodePointer);

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

void update_vpd_86(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xb0)];
	uint8_t *worm;

	worm = (uint8_t *)p;

	*vpd_pg->data = (*worm) ? 1 : 0;        /* Set WORM bit */
}

void update_vpd_b0(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xb0)];
	uint8_t *worm;

	worm = (uint8_t *)p;

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

	memcpy(&vpd_pg->data[20], p, strlen((const char *)p));
}

void update_vpd_c1(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0xc1)];

	memcpy(vpd_pg->data, p, vpd_pg->sz);
}

void cleanup_density_support(struct list_head *l)
{
	struct supported_density_list *dp, *ndp;

	list_for_each_entry_safe(dp, ndp, l, siblings) {
		list_del(&dp->siblings);
		free(dp);
	}
}

int add_density_support(struct list_head *l, struct density_info *di, int rw)
{
	struct supported_density_list *supported;

	supported = zalloc(sizeof(struct supported_density_list));
	if (!supported)
		return -ENOMEM;

	supported->density_info = di;
	supported->rw = rw;
	list_add_tail(&supported->siblings, l);
	return 0;
}

void process_fifoname(struct lu_phy_attr *lu, char *s, int flag)
{
	MHVTL_DBG(3, "entry: %s, flag: %d, existing name: %s",
				s, flag, lu->fifoname);
	if (lu->fifo_flag)	/* fifo set via '-f <fifo>' switch */
		return;
	checkstrlen(s, MALLOC_SZ - 1, 0);
	free(lu->fifoname);
	lu->fifoname = (char *)zalloc(strlen(s) + 2);
	if (!lu->fifoname) {
		printf("Unable to malloc fifo buffer");
		exit(-ENOMEM);
	}
	lu->fifo_flag = flag;
	/* Already checked for sane length */
	strcpy(lu->fifoname, s);
}

/* Remove message queue */
void cleanup_msg(void)
{
	int msqid;
	int retval;
	struct msqid_ds ds;

	msqid = init_queue();
	if (msqid < 0) {
		MHVTL_ERR("Failed to open msg queue: %s", strerror(errno));
		return;
	}
	retval = msgctl(msqid, IPC_RMID, &ds);
	if (retval < 0) {
		MHVTL_ERR("Failed to remove msg queue: %s", strerror(errno));
	} else {
		MHVTL_DBG(2, "Removed ipc resources");
	}
}

#define QUERYSHM 0
#define INCSHM	1
#define DECSHM	2

static int mhvtl_shared_mem(int flag)
{
	int mhvtl_shm;
	int retval = -1;
	int *base;
	key_t key;
	struct shmid_ds buf;

	key = 0x4d61726b;

	mhvtl_shm = shmget(key, 16, IPC_CREAT|0666);
	if (mhvtl_shm < 0) {
		printf("Attempt to get Shared memory failed\n");
		MHVTL_ERR("Attempt to get shared memory failed");
		return -ENOMEM;
	}

	base = (int *)shmat(mhvtl_shm, NULL, 0);
	if (base == (void *) -1) {
		MHVTL_ERR("Failed to attach to shm: %s", strerror(errno));
		return -1;
	}

	MHVTL_DBG(3, "shm count is: %d", *base);

	switch (flag) {
	case QUERYSHM:
		break;
	case INCSHM:
		(*base)++;
		break;
	case DECSHM:
		if (*base)
			(*base)--;

		/* No more consumers - mark as remove */
		if (*base == 0) {
			shmctl(mhvtl_shm, IPC_STAT, &buf);
			shmctl(mhvtl_shm, IPC_RMID, &buf);
			MHVTL_DBG(3, "pid of creator: %d,"
					" pid of last shmat(): %d, "
					" Number of current attach: %d",
					buf.shm_cpid,
					buf.shm_lpid,
					(int)buf.shm_nattch);
			/* Should be no more users of the message Q either */
			cleanup_msg();
		}
		break;
	}
	MHVTL_DBG(3, "shm count now: %d", *base);

	retval = *base;

	shmdt(base);

	return retval;
}

static int mhvtl_fifo_count(int direction)
{
	sem_t *mhvtl_sem;
	int sval;
	int i;
	char errmsg[] = "mhvtl_sem";
	int retval = -1;

	mhvtl_sem = sem_open("/mhVTL", O_CREAT, 0664, 1);
	if (SEM_FAILED == mhvtl_sem) {
		MHVTL_ERR("%s : %s", errmsg, strerror(errno));
		return retval;
	}

	sem_getvalue(mhvtl_sem, &sval);
	for (i = 0; i < 10; i++) {
		if (sem_trywait(mhvtl_sem)) {
			MHVTL_LOG("Waiting for semaphore: %p", mhvtl_sem);
			sleep(1);
			if (i > 8)
				/* Give up.. Clear the semaphore & do it */
				MHVTL_ERR("waiting for semaphore: %p",
								mhvtl_sem);
				sem_post(mhvtl_sem);
		} else {
			retval = mhvtl_shared_mem(direction);
			sem_post(mhvtl_sem);
			break;
		}
	}

	sem_close(mhvtl_sem);
	return retval;
}

int dec_fifo_count(void)
{
	return mhvtl_fifo_count(DECSHM);
}

int inc_fifo_count(void)
{
	return mhvtl_fifo_count(INCSHM);
}

int get_fifo_count(void)
{
	return mhvtl_fifo_count(QUERYSHM);
}

void find_media_home_directory(char *home_directory, long lib_id)
{
	char *config = MHVTL_CONFIG_PATH"/device.conf";
	FILE *conf;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	long i;
	int found;

	found = 0;
	home_directory[0] = '\0';

	conf = fopen(config , "r");
	if (!conf) {
		MHVTL_ERR("Can not open config file %s : %s", config,
					strerror(errno));
		perror("Can not open config file");
		exit(1);
	}
	s = zalloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = zalloc(MALLOC_SZ);
	if (!b) {
		perror("Could not allocate memory");
		exit(1);
	}
	while (readline(b, MALLOC_SZ, conf) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		if (strlen(b) < 3)	/* Reset drive number of blank line */
			i = 0xff;
		if (sscanf(b, "Library: %ld ", &i)) {
			MHVTL_DBG(2, "Found Library %ld, looking for %ld",
							i, lib_id);
			if (i == lib_id)
				found = 1;
		}
		if (found == 1) {
			int a;
			a = sscanf(b, " Home directory: %s", s);
			if (a > 0) {
				strncpy(home_directory, s, HOME_DIR_PATH_SZ);
				MHVTL_DBG(2, "Found home directory  : %s",
						home_directory);
				goto finished; /* Found what we came for */
			}
		}
	}

	/* Not found, then append the library id to default path */
	snprintf(home_directory, HOME_DIR_PATH_SZ, "%s/%ld",
						MHVTL_HOME_PATH, lib_id);
	MHVTL_DBG(1, "Append library id %ld to default path %s: %s",
						lib_id, MHVTL_HOME_PATH,
						home_directory);

finished:
	free(s);
	free(b);
	fclose(conf);
}

unsigned int set_media_params(struct MAM *mamp, char *density)
{
	/* Invent some defaults */
	mamp->MediaType = Media_undefined;
	put_unaligned_be32(2048, &mamp->media_info.bits_per_mm);
	put_unaligned_be16(1, &mamp->media_info.tracks);
	put_unaligned_be32(127, &mamp->MediumWidth);
	put_unaligned_be32(1024, &mamp->MediumLength);
	memcpy(&mamp->media_info.description, "mhvtl", 5);
	mamp->max_partitions = 1;
	mamp->num_partitions = 1;

	if (!(strncmp(density, "LTO1", 4))) {
		mamp->MediumDensityCode = medium_density_code_lto1;
		mamp->MediaType = Media_LTO1;
		put_unaligned_be32(384, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 1/8T", 12);
		memcpy(&mamp->media_info.density_name, "U-18  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(4880, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "LTO2", 4))) {
		mamp->MediumDensityCode = medium_density_code_lto2;
		mamp->MediaType = Media_LTO2;
		put_unaligned_be32(512, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 2/8T", 12);
		memcpy(&mamp->media_info.density_name, "U-28  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(7398, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "LTO3", 4))) {
		mamp->MediumDensityCode = medium_density_code_lto3;
		mamp->MediaType = Media_LTO3;
		put_unaligned_be32(704, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 3/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-316 ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(9638, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "LTO4", 4))) {
		mamp->MediumDensityCode = medium_density_code_lto4;
		mamp->MediaType = Media_LTO4;
		put_unaligned_be32(896, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 4/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-416  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(12725, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "LTO5", 4))) {
		mamp->MediumDensityCode = medium_density_code_lto5;
		mamp->MediaType = Media_LTO5;
		put_unaligned_be32(1280, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 5/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-516  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(15142, &mamp->media_info.bits_per_mm);
		mamp->max_partitions = 2;
		mamp->num_partitions = 2;
	} else if (!(strncmp(density, "LTO6", 4))) { /* FIXME */
		mamp->MediumDensityCode = medium_density_code_lto6;
		mamp->MediaType = Media_LTO6;
		put_unaligned_be32(2176, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 6/16T", 13);
		memcpy(&mamp->media_info.density_name, "U-616  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(18441, &mamp->media_info.bits_per_mm);
		mamp->max_partitions = 2;
		mamp->num_partitions = 2;
	} else if (!(strncmp(density, "LTO7", 4))) { /* FIXME */
		mamp->MediumDensityCode = medium_density_code_lto7;
		mamp->MediaType = Media_LTO7;
		put_unaligned_be32(960, &mamp->MediumLength);
		put_unaligned_be32(127, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "Ultrium 7/32T", 13);
		memcpy(&mamp->media_info.density_name, "U-732  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "LTO-CVE", 7);
		put_unaligned_be32(19107, &mamp->media_info.bits_per_mm);
		mamp->max_partitions = 2;
		mamp->num_partitions = 2;
	} else if (!(strncmp(density, "AIT1", 4))) {
	/* Vaules for AIT taken from "Product Manual SDX-900V v1.0" */
		mamp->MediumDensityCode = medium_density_code_ait1;
		mamp->MediaType = Media_AIT1;
		put_unaligned_be32(384, &mamp->MediumLength);
		put_unaligned_be32(0x50, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "AdvIntelligentTape1", 20);
		memcpy(&mamp->media_info.density_name, "AIT-1 ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		put_unaligned_be32(0x11d7, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "AIT2", 4))) {
		mamp->MediumDensityCode = medium_density_code_ait2;
		mamp->MediaType = Media_AIT2;
		put_unaligned_be32(384, &mamp->MediumLength);
		put_unaligned_be32(0x50, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "AdvIntelligentTape2", 20);
		memcpy(&mamp->media_info.density_name, "AIT-2  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		put_unaligned_be32(0x17d6, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "AIT3", 4))) {
		mamp->MediumDensityCode = medium_density_code_ait3;
		mamp->MediaType = Media_AIT3;
		put_unaligned_be32(384, &mamp->MediumLength);
		put_unaligned_be32(0x50, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "AdvIntelligentTape3", 20);
		memcpy(&mamp->media_info.density_name, "AIT-3  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		put_unaligned_be32(0x17d6, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "AIT4", 4))) {
		mamp->MediumDensityCode = medium_density_code_ait4;
		mamp->MediaType = Media_AIT4;
		put_unaligned_be32(384, &mamp->MediumLength);
		put_unaligned_be32(0x50, &mamp->MediumWidth);
		memcpy(&mamp->media_info.description, "AdvIntelligentTape4", 20);
		memcpy(&mamp->media_info.density_name, "AIT-4  ", 6);
		memcpy(&mamp->AssigningOrganization_1, "SONY", 4);
		put_unaligned_be32(0x17d6, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "DLT3", 4))) {
		mamp->MediumDensityCode = medium_density_code_dlt3;
		mamp->MediaType = Media_DLT3;
		memcpy(&mamp->media_info.description, "DLTtape III", 11);
		memcpy(&mamp->media_info.density_name, "DLT-III", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
	} else if (!(strncmp(density, "DLT4", 4))) {
		mamp->MediumDensityCode = medium_density_code_dlt4;
		mamp->MediaType = Media_DLT4;
		memcpy(&mamp->media_info.description, "DLTtape IV", 10);
		memcpy(&mamp->media_info.density_name, "DLT-IV", 6);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
	} else if (!(strncmp(density, "SDLT1", 5))) {
		mamp->MediumDensityCode = 0x48;
		mamp->MediaType = Media_SDLT;
		memcpy(&mamp->media_info.description, "SDLT I media", 12);
		memcpy(&mamp->media_info.density_name, "SDLT-1", 6);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		put_unaligned_be32(133000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "SDLT220", 7))) {
		mamp->MediumDensityCode = medium_density_code_220;
		mamp->MediaType = Media_SDLT220;
		memcpy(&mamp->media_info.description, "SDLT I media", 12);
		memcpy(&mamp->media_info.density_name, "SDLT220", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		put_unaligned_be32(133000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "SDLT320", 7))) {
		mamp->MediumDensityCode = medium_density_code_320;
		mamp->MediaType = Media_SDLT320;
		memcpy(&mamp->media_info.description, "SDLT I media", 12);
		memcpy(&mamp->media_info.density_name, "SDLT320", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		put_unaligned_be32(190000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "SDLT600", 7))) {
		mamp->MediumDensityCode = medium_density_code_600;
		mamp->MediaType = Media_SDLT600;
		memcpy(&mamp->media_info.description, "SDLT II media", 13);
		memcpy(&mamp->media_info.density_name, "SDLT600", 7);
		memcpy(&mamp->AssigningOrganization_1, "QUANTUM", 7);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "9840A", 5))) {
		mamp->MediumDensityCode = medium_density_code_9840A;
		mamp->MediaType = Media_9840A;
		memcpy(&mamp->media_info.description, "Raven 20 GB", 11);
		memcpy(&mamp->media_info.density_name, "R-20", 4);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(288, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "9840B", 5))) {
		mamp->MediumDensityCode = medium_density_code_9840B;
		mamp->MediaType = Media_9840B;
		memcpy(&mamp->media_info.description, "Raven 20 GB", 11);
		memcpy(&mamp->media_info.density_name, "R-20", 4);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(288, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "9840C", 5))) {
		mamp->MediumDensityCode = medium_density_code_9840C;
		mamp->MediaType = Media_9840C;
		memcpy(&mamp->media_info.description, "Raven 40 GB", 11);
		memcpy(&mamp->media_info.density_name, "R-40", 4);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(288, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "9840D", 5))) {
		mamp->MediumDensityCode = medium_density_code_9840D;
		mamp->MediaType = Media_9840D;
		memcpy(&mamp->media_info.description, "Raven 75 GB", 11);
		memcpy(&mamp->media_info.density_name, "R-75", 4);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(576, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "9940A", 5))) {
		mamp->MediumDensityCode = medium_density_code_9940A;
		mamp->MediaType = Media_9940A;
		memcpy(&mamp->media_info.description, "PeakCapacity 60 GB", 18);
		memcpy(&mamp->media_info.density_name, "P-60", 4);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(288, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "9940B", 5))) {
		mamp->MediumDensityCode = medium_density_code_9940B;
		mamp->MediaType = Media_9940B;
		memcpy(&mamp->media_info.description,
						"PeakCapacity 200 GB", 19);
		memcpy(&mamp->media_info.density_name, "P-200", 5);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(0, &mamp->media_info.bits_per_mm);
		put_unaligned_be16(576, &mamp->media_info.tracks);
		put_unaligned_be32(127, &mamp->MediumWidth);
		put_unaligned_be32(1024, &mamp->MediumLength);
	} else if (!(strncmp(density, "T10KA", 5))) {
		mamp->MediumDensityCode = medium_density_code_10kA;
		mamp->MediaType = Media_T10KA;
		memcpy(&mamp->media_info.description, "STK T10KA media", 15);
		memcpy(&mamp->media_info.density_name, "T10000A", 7);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "T10KB", 5))) {
		mamp->MediumDensityCode = medium_density_code_10kB;
		mamp->MediaType = Media_T10KB;
		memcpy(&mamp->media_info.description, "STK T10KB media", 15);
		memcpy(&mamp->media_info.density_name, "T10000B", 7);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "T10KC", 5))) {
		mamp->MediumDensityCode = medium_density_code_10kC;
		mamp->MediaType = Media_T10KC;
		memcpy(&mamp->media_info.description, "STK T10KC media", 15);
		memcpy(&mamp->media_info.density_name, "T10000C", 7);
		memcpy(&mamp->AssigningOrganization_1, "STK", 3);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "DDS1", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS1;
		mamp->MediaType = Media_DDS1;
		memcpy(&mamp->media_info.description, "4MM DDS-1 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS1", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "DDS2", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS2;
		mamp->MediaType = Media_DDS2;
		memcpy(&mamp->media_info.description, "4MM DDS-2 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS2", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "DDS3", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS3;
		mamp->MediaType = Media_DDS3;
		memcpy(&mamp->media_info.description, "4MM DDS-3 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS3", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "DDS4", 4))) {
		mamp->MediumDensityCode = medium_density_code_DDS4;
		mamp->MediaType = Media_DDS4;
		memcpy(&mamp->media_info.description, "4MM DDS-4 media", 15);
		memcpy(&mamp->media_info.density_name, "DDS4", 4);
		memcpy(&mamp->AssigningOrganization_1, "HP", 2);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "J1A", 3))) {
		mamp->MediumDensityCode = medium_density_code_j1a;
		mamp->MediaType = Media_3592_JA;
		memcpy(&mamp->media_info.description, "3592 J1A media", 14);
		memcpy(&mamp->media_info.density_name, "3592J1A", 7);
		memcpy(&mamp->AssigningOrganization_1, "IBM", 3);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "E05", 3))) {
		mamp->MediumDensityCode = medium_density_code_e05;
		mamp->MediaType = Media_3592_JB;
		memcpy(&mamp->media_info.description, "3592 E05 media", 14);
		memcpy(&mamp->media_info.density_name, "3592E05", 7);
		memcpy(&mamp->AssigningOrganization_1, "IBM", 3);
		put_unaligned_be32(233000, &mamp->media_info.bits_per_mm);
	} else if (!(strncmp(density, "E06", 3))) {
		mamp->MediumDensityCode = medium_density_code_e06;
		mamp->MediaType = Media_3592_JX;
		memcpy(&mamp->media_info.description, "3592 E06 media", 14);
		memcpy(&mamp->media_info.density_name, "3592E06", 7);
		memcpy(&mamp->AssigningOrganization_1, "IBM", 3);
	} else if (!(strncmp(density, "E07", 3))) {
		mamp->MediumDensityCode = medium_density_code_e07;
		mamp->MediaType = Media_3592_JK;
		memcpy(&mamp->media_info.description, "3592 E07 media", 14);
		memcpy(&mamp->media_info.density_name, "3592E07", 7);
		memcpy(&mamp->AssigningOrganization_1, "IBM", 3);
	} else
		printf("'%s' is an invalid density\n", density);

	if (mamp->MediaType == Media_undefined)	{
		printf("Warning: mamp->MediaType is still undefined\n");
		return 1;
	}
	mamp->FormattedDensityCode = mamp->MediumDensityCode;
	return 0;
}

void ymd(int *year, int *month, int *day, int *hh, int *min, int *sec)
{
	sscanf(__TIME__, "%d:%d:%d", hh, min, sec);

	if (sscanf(__DATE__, "Jan %d %d", day, year) == 2)
		*month = 1;
	if (sscanf(__DATE__, "Feb %d %d", day, year) == 2)
		*month = 2;
	if (sscanf(__DATE__, "Mar %d %d", day, year) == 2)
		*month = 3;
	if (sscanf(__DATE__, "Apr %d %d", day, year) == 2)
		*month = 4;
	if (sscanf(__DATE__, "May %d %d", day, year) == 2)
		*month = 5;
	if (sscanf(__DATE__, "Jun %d %d", day, year) == 2)
		*month = 6;
	if (sscanf(__DATE__, "Jul %d %d", day, year) == 2)
		*month = 7;
	if (sscanf(__DATE__, "Aug %d %d", day, year) == 2)
		*month = 8;
	if (sscanf(__DATE__, "Sep %d %d", day, year) == 2)
		*month = 9;
	if (sscanf(__DATE__, "Oct %d %d", day, year) == 2)
		*month = 10;
	if (sscanf(__DATE__, "Nov %d %d", day, year) == 2)
		*month = 11;
	if (sscanf(__DATE__, "Dec %d %d", day, year) == 2)
		*month = 12;
}

void rw_6(struct scsi_cmd *cmd, int *num, int *sz, int dbg)
{
	uint8_t *cdb = cmd->scb;

	if (cdb[1] & FIXED) {	/* If Fixed block writes */
		*num = get_unaligned_be24(&cdb[2]);
		*sz = get_unaligned_be24(&modeBlockDescriptor[5]);
	} else {		/* else - Variable Block writes */
		*num = 1;
		*sz = get_unaligned_be24(&cdb[2]);
	}
	MHVTL_DBG(dbg, "%s: %d block%s of %d bytes (%ld) **",
				cdb[0] == READ_6 ? "READ" : "WRITE",
				*num, *num == 1 ? "" : "s",
				*sz,
				(long)cmd->dbuf_p->serialNo);
}

char *slot_type_str(int type)
{
	return slot_type_string[type];
}

void init_smc_log_pages(struct lu_phy_attr *lu)
{
	add_log_temperature_page(lu);
	add_log_tape_alert(lu);
}

void init_smc_mode_pages(struct lu_phy_attr *lu)
{
	add_mode_disconnect_reconnect(lu);
	add_mode_control_extension(lu);
	add_mode_power_condition(lu);
	add_mode_information_exception(lu);
	add_mode_element_address_assignment(lu);
	add_mode_transport_geometry(lu);
	add_mode_device_capabilities(lu);
}

void bubbleSort(int *array, int size)
{
	int swapped;
	int i;
	int j;

	for (i = 1; i < size; i++) {
		swapped = 0;
		for (j = 0; j < size - i; j++) {
			if (array[j] > array[j+1]) {
				int temp = array[j];
				array[j] = array[j+1];
				array[j+1] = temp;
				swapped = 1;
			}
		}
		if (!swapped)
			break; /* if it is sorted then stop */
	}
}

void sort_library_slot_type(struct lu_phy_attr *lu, struct smc_type_slot *type)
{
	int i;
	struct smc_priv *smc_p = lu->lu_private;
	int arr[4];

	arr[0] = smc_p->pm->start_drive;
	arr[1] = smc_p->pm->start_picker;
	arr[2] = smc_p->pm->start_map;
	arr[3] = smc_p->pm->start_storage;

	bubbleSort(arr, 4);

	for (i = 0; i < 4; i++) {
		if (smc_p->pm->start_drive == arr[i]) {
			type[i].type = DATA_TRANSFER;
			type[i].start = smc_p->pm->start_drive;
		}
		if (smc_p->pm->start_picker == arr[i]) {
			type[i].type = MEDIUM_TRANSPORT;
			type[i].start = smc_p->pm->start_picker;
		}
		if (smc_p->pm->start_map == arr[i]) {
			type[i].type = MAP_ELEMENT;
			type[i].start = smc_p->pm->start_map;
		}
		if (smc_p->pm->start_storage == arr[i]) {
			type[i].type = STORAGE_ELEMENT;
			type[i].start = smc_p->pm->start_storage;
		}
	}
}

/* Set VPD data with device serial number */
void update_vpd_80(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0x80)];

	assert(vpd_pg);		/* space should have been pre-allocated */

	memcpy(vpd_pg->data, p, strlen((const char *)p));
}

void update_vpd_83(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0x83)];
	uint8_t *d;
	char *ptr;
	int num;
	int len, j;

	assert(vpd_pg);		/* space should have been pre-allocated */

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
	/* Bug reported by Stefan Hauser.
	 * [num +4] is always 0x5x
	 */
	d[num + 4] &= 0x0f;
	d[num + 4] |= 0x50;
}
