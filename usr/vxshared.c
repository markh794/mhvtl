/*
 * Shared routines between vxtape & vxlibrary
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
 */
#include <stdint.h>

#ifndef Solaris
 #include <byteswap.h>
#endif

#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <err.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include "scsi.h"
#include "q.h"
#include "vx.h"
#include "vxshared.h"

#ifndef Solaris
	int ioctl(int, int, void *);
#endif

#ifndef DEBUG

/* Change to 0 to disable some debug statements */
#define DEBUG 0

#define DEB(a) a
#define DEBC(a) if (debug) { a ; }

#else

#define DEB(a)
#define DEBC(a)

#endif

extern u8 blockDescriptorBlock[];
extern int verbose;

DEB(
extern int debug;
) ;


int send_msg(char *cmd, int q_id)
{
	int len, s_qid;
	struct q_entry s_entry;
	len = strlen(cmd);

	s_qid = init_queue();
	if (s_qid == -1)
		return (-1);

	s_entry.mtype = (long)q_id;
	strncpy(s_entry.mtext, cmd, len);

	if (msgsnd(s_qid, &s_entry, len, 0) == -1) {
		syslog(LOG_DAEMON|LOG_ERR, "msgsnd failed: %m");
		return (-1);
	} else {
		return (0);
	}
}

/*
 * log the SCSI command.
 * If debug -> display to stdout.
 * else to syslog
 */
void logSCSICommand(u8 * SCpnt)
{
	int groupCode;
	int cmd_len = 6;
	int k;

	groupCode = (SCpnt[0] & 0xe0) >> 5;
	switch(groupCode) {
	case 0:	/*  6 byte commands */
		cmd_len = 6;
		syslog(LOG_DAEMON|LOG_INFO, "%02x %02x %02x %02x %02x %02x",
			SCpnt[0], SCpnt[1], SCpnt[2], SCpnt[3],
			SCpnt[4], SCpnt[5]);
		break;
	case 1: /* 10 byte commands */
	case 2: /* 10 byte commands */
		cmd_len = 10;
		syslog(LOG_DAEMON|LOG_INFO, "%02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x",
			SCpnt[0], SCpnt[1], SCpnt[2], SCpnt[3],
			SCpnt[4], SCpnt[5], SCpnt[6], SCpnt[7],
			SCpnt[8], SCpnt[9]);
		break;
	case 3: /* Reserved - There is always one exception ;) */
		cmd_len = 6;
		syslog(LOG_DAEMON|LOG_INFO, "%02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x %02x",
			SCpnt[0], SCpnt[1], SCpnt[2], SCpnt[3],
			SCpnt[4], SCpnt[5], SCpnt[6], SCpnt[7],
			SCpnt[8], SCpnt[9], SCpnt[10], SCpnt[11]);
		break;
	case 4: /* 16 byte commands */
		cmd_len = 16;
		syslog(LOG_DAEMON|LOG_INFO, "%02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
			SCpnt[0], SCpnt[1], SCpnt[2], SCpnt[3],
			SCpnt[4], SCpnt[5], SCpnt[6], SCpnt[7],
			SCpnt[8], SCpnt[9], SCpnt[10], SCpnt[11],
			SCpnt[12], SCpnt[13], SCpnt[14], SCpnt[15]);
		break;
	case 5: /* 12 byte commands */
		cmd_len = 12;
	syslog(LOG_DAEMON|LOG_INFO, "%02x %02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x",
			SCpnt[0], SCpnt[1], SCpnt[2], SCpnt[3],
			SCpnt[4], SCpnt[5], SCpnt[6], SCpnt[7],
			SCpnt[8], SCpnt[9], SCpnt[10], SCpnt[11]);
		break;
	case 6: /* Vendor Specific */
	case 7: /* Vendor Specific */
		cmd_len = 6;
		syslog(LOG_DAEMON|LOG_INFO, "VENDOR SPECIFIC !! "
			" %02x %02x %02x %02x %02x %02x",
			SCpnt[0], SCpnt[1], SCpnt[2], SCpnt[3],
			SCpnt[4], SCpnt[5]);
		break;
	}

	if (debug) {
		printf("\n");
		for (k = 0; k < cmd_len; k++)
			printf("%02x ", (u32)SCpnt[k]);
		printf("\n");
	}
}

/*
 * Fills in a global array with current sense data
 * Sets 'sense_valid' to 1.
 */
extern u8 sense[];

void mkSenseBuf(u8 sense_d, u32 sense_q, u8 *sense_flg)
{
	u16 *sp;

	/* Clear Sense key status */
	memset(sense, 0, SENSE_BUF_SIZE);

	* sense_flg = CHECK_CONDITION;

	sense[0] = 0xf0;        /* Valid, current error */
	sense[2] = sense_d;
	sense[7] = SENSE_BUF_SIZE - 8;
	sp = (u16 *)&sense[12];
	*sp = htons(sense_q);

	if (debug)
		syslog(LOG_DAEMON|LOG_INFO,
			"Setting Sense Data [Response Code/Sense Key/ASC/ASCQ]"
			" [%02x %02x %02x %02x]",
				sense[0], sense[2], sense[12], sense[13]);
}

extern int reset;

int check_reset(u8 *sense_flg)
{
	int retval = reset;

	if (reset) {
		mkSenseBuf(UNIT_ATTENTION, E_POWERON_RESET, sense_flg);
		reset = 0;
	}
return(retval);
}

/*
 * Allow/Prevent media removal.
 *
 * basically a no-op but log the cmd.
 */

void resp_allow_prevent_removal(u8 * SCpnt, u8 * sense_flg)
{

	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO, "%s",
				(SCpnt[4]) ? "Prevent MEDIUM removal **" :
					 "Allow MEDIUM Removal **");
	if (debug)
		printf("%s\n",
			(SCpnt[4]) ? "Prevent MEDIUM removal **" :
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

void resp_log_select(u8 * SCpnt, u8 * sense_flg)
{

	char pcr = SCpnt[1] & 0x1;
	u16	parmList;
	char	* parmString = "Undefined";

	parmList = ntohs((u16)SCpnt[7]); /* bytes 7 & 8 are parm list. */

	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO, "LOG SELECT %s",
				(pcr) ? ": Parameter Code Reset **" : "**");
	if (debug)
		printf(" LOG SELECT %s\n",
				(pcr) ? ": Parameter Code Reset **" : "**");

	if (pcr)	{	/* Check for Parameter code reset */
		if (parmList) {	/* If non-zero, error */
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
						sense_flg);
			return;
		}
		switch((SCpnt[2] & 0xc0) >> 5) {
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
		if (verbose)
			syslog(LOG_DAEMON|LOG_INFO, "  %s", parmString);
		if (debug)
			printf("  %s", parmString);
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
int resp_read_position_long(loff_t pos, u8 * buf, u8 * sense_flg)
{
	u64 partition = 1;
	u64 *lp;

	memset(buf, 0, READ_POSITION_LONG_LEN);	/* Clear 'array' */

	if ((pos == 0) || (pos == 1))
		buf[0] = 0x80;	/* Begining of Partition */
	lp = (u64 *)&buf[4];
	*lp = htonl(partition);

	buf[8] = buf[9]  = (pos >> 16) & 0xff;
	buf[6] = buf[10] = (pos >> 8) & 0xff;
	buf[7] = buf[11] = pos & 0xff;

return(READ_POSITION_LONG_LEN);
}

#define READ_POSITION_LEN 20
/* Return tape position - short format */
int resp_read_position(loff_t pos, u8 * buf, u8 * sense_flg)
{
	u64 *lp;

	memset(buf, 0, READ_POSITION_LEN);	/* Clear 'array' */

	if ((pos == 0) || (pos == 1))
		buf[0] = 0x80;	/* Begining of Partition */
	lp = (u64 *)&buf[4];
	*lp = htonl(pos);

return(READ_POSITION_LEN);
}


/*
 * Copy data in struct 'report_luns' into bufer and return length
 */
int resp_report_lun(struct report_luns * rpLUNs, u8 *buf, u8 *sense_flg)
{
	u64 size = ntohl(rpLUNs->size) + 8;

	memcpy( buf, (u8 *)&rpLUNs, size);
	return size;
}

/*
 * Respond with S/No. of media currently mounted
 */
int resp_read_media_serial(u8 *sno, u8 *buf, u8 *sense_flg)
{
	u64 size = 0L;

	syslog(LOG_DAEMON|LOG_ERR, "Read media S/No not implemented yet!");

	return size;
}

/*
 * Look thru NULL terminated array of struct mode[] for a match to pcode.
 * Return: struct mode * or NULL for no pcode
 */
struct mode *find_pcode(u8 pcode, struct mode *m)
{
	int a;

	DEBC(	 printf("Entered: %s(0x%x)\n", __func__, pcode);
		fflush(NULL);
	)

	for (a = 0; a < 0x3f; a++, m++) { /* Possibility for 0x3f page codes */
		if (m->pcode == 0x0)
			break;	/* End of list */
		if (m->pcode == pcode) {
			if (debug)
				printf("%s(0x%x): match pcode %d\n",
					__func__, pcode, m->pcode);
			else if (verbose > 1)
				syslog(LOG_DAEMON|LOG_WARNING,
					"%s(0x%x): match pcode %d",
					__func__, pcode, m->pcode);
			return m;
		}
	}

	if (debug)
		printf("%s: page code 0x%x not found\n", __func__, pcode);
	else if (verbose > 2)
		syslog(LOG_DAEMON|LOG_INFO,
			"%s: page code 0x%x not found\n", __func__, pcode);

	return NULL;
}

/*
 * Add data for pcode to buffer pointed to by p
 * Return: Number of chars moved.
 */
static int add_pcode(struct mode *m, u8 *p)
{
	memcpy(p, m->pcodePointer, m->pcodeSize);
	return(m->pcodeSize);
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
struct mode * alloc_mode_page(u8 pcode, struct mode *m, int size)
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
		return(mp);
	}
	return(NULL);
}


/*
 * Build mode sense data into *buf
 * Return size of data.
 */
int resp_mode_sense(u8 *cmd, u8 *buf, struct mode *m, u8 *sense_flg)
{
	int pcontrol, pcode, subpcode;
	int media_type;
	int alloc_len, msense_6;
	int dev_spec, len = 0;
	int offset = 0;
	u8 * ap;
	u16 *sp;		/* Short pointer */
	struct mode *smp;	/* Struct mode pointer... */
	int a;

	char *pcontrolString[] = {
		"Current configuration",
		"Every bit that can be modified",
		"Power-on configuration",
		"Power-on configuration"
	};

	/* Disable Block Descriptors */
	u8 blockDescriptorLen = (cmd[1] & 0x8) ? 0 : 8;

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

	if (verbose > 2) {
		syslog(LOG_DAEMON|LOG_INFO, " Mode Sense %d byte version",
			(msense_6) ? 6 : 10);
		syslog(LOG_DAEMON|LOG_INFO, " Page Control  : %s(0x%x)",
				pcontrolString[pcontrol], pcontrol);
		syslog(LOG_DAEMON|LOG_INFO, " Page Code     : 0x%x", pcode);
		syslog(LOG_DAEMON|LOG_INFO, " Disable Block Descriptor => %s",
				(blockDescriptorLen) ? "Yes" : "No");
		syslog(LOG_DAEMON|LOG_INFO, " Allocation len: %d", alloc_len);
	}

	if (0x3 == pcontrol) {  /* Saving values not supported */
		mkSenseBuf(ILLEGAL_REQUEST, E_SAVING_PARMS_UNSUP, sense_flg);
		return (0);
	}

	memset(buf, 0, alloc_len);	/* Set return data to null */
	dev_spec = 0x10;
	media_type = 0x0;

	offset += blockDescriptorLen;
	ap = buf + offset;

	if (0 != subpcode) { /* TODO: Control Extension page */
		syslog(LOG_DAEMON|LOG_WARNING,
				"Non-zero sub-page sense code not supported");
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sense_flg);
		return(0);
	}

	if (debug)
		printf("pcode: 0x%02x\n", pcode);

	if (0x0 == pcode) {
		len = 0;
	} else if (0x3f == pcode) {	/* Return all pages */
		for (a = 1; a < 0x3f; a++) { /* Walk thru all possibilities */
			smp = find_pcode(a, m);
			if (smp)
				len += add_pcode(smp, (u8 *)ap + len);
		}
	} else {
		smp = find_pcode(pcode, m);
		if (smp)
			len = add_pcode(smp, (u8 *)ap);
	}
	offset += len;

	if (pcode != 0)	/* 0 = No page code requested */
		if (0 == len) {	/* Page not found.. */
		syslog(LOG_DAEMON|LOG_WARNING, "Unknown mode page : %d", pcode);
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB,sense_flg);
		return (0);
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
		sp = (u16 *)&buf[0];
		*sp = htons(offset - 2); /* size - sizeof(buf[0]) field */
		buf[2] = media_type;
		buf[3] = dev_spec;
		sp = (u16 *)&buf[6];
		*sp = htons(blockDescriptorLen);
		/* If the length > 0, copy Block Desc. */
		if (blockDescriptorLen)
			memcpy(&buf[8],blockDescriptorBlock,blockDescriptorLen);
	}

	if (debug) {
		printf("mode sense: Returning %d bytes", offset);
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
	if (verbose > 1)
		syslog(LOG_DAEMON|LOG_INFO,
				"Setting TapeAlert flags 0x%" PRIx64 "\n", flg);

	int a;
	for (a = 0; a < 64; a++) {
		ta->TapeAlert[a].value = (flg & (1ull << a)) ? 1 : 0;
		if (verbose > 2)
			syslog(LOG_DAEMON|LOG_INFO,
				"TapeAlert flag %016" PRIx64 " : %s\n",
				(uint64_t)1ull << a,
				(ta->TapeAlert[a].value) ? "set" : "unset");
	}

}


/*
 * First send SCSI S/No. to char device
 * Then the 'check condition' status (1 = check, 0 no check)
 * Finally any other data queued up to be sent for this command
 *
 * Returns nothing.
 */
void completeSCSICommand(int cdev,
			u32 serialNo,
			u8 *buf,
			u8 *sense_buf,
			u8 *check_condition,
			u32 count)
{
	loff_t nwrite;
	u8	s[4];
	int	k;
	u32	*lp;

	lp = (u32 *)&s[0];
	*lp = htonl(serialNo);
	
	/* If debug is high enough, print out all. Otherwise just print
	 out data if something of interest. */
	if (debug > 2)
		printf("Completing SerialNo: %d, total of %d bytes\n",
							 serialNo, count);
	else if (debug > 1)
		if (count > 5)
			printf("Comple SerialNo: %d, total of %d bytes\n",
							 serialNo, count);
	nwrite = write(cdev, s, 4);
	if (nwrite != 4)
		syslog(LOG_DAEMON|LOG_ERR,
			"Could not send SCSI serial number: %d", serialNo);

	if (debug > 2)
		printf("Returning sense status : %d\n", *check_condition);

	DEB(
	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO, "Return: CHECK_CONDITION: %s",
					(*check_condition) ? "yes" : "no");
	) ;

	nwrite = write(cdev, check_condition, 1);
	if (nwrite != 1)
		syslog(LOG_DAEMON|LOG_ERR, "%s %d",
			"Problems returning Sense Check Condition",
							 *check_condition);

	/* Print out (in hex) the returned block of data */
	if ((debug > 1) && (count < 64) && (count > 5)) {
		for (k = 0; k < (count - 5); k++) {
			if ((k % 16) == 0)
				printf("\n");
			printf("%02x ", buf[k]);
		}
		printf("\n");
	}
	nwrite = write(cdev, buf, count - 5);
	if (nwrite != count - 5)
		syslog(LOG_DAEMON|LOG_ERR, "%s %m",
					"Problems returning SCSI data");

	/* Automagically append SENSE data if check_condition set
	 *
	 * FIXME: SENSE_BUF_SIZE has to be manually kept in sync with kernel
	 * driver defination. i.e. They have to match..
	 */
	if (*check_condition) {
		nwrite = write(cdev, sense_buf, SENSE_BUF_SIZE);
		if (SENSE_BUF_SIZE != nwrite)
			syslog(LOG_DAEMON|LOG_ERR, "%s %m",
					"Problems returning SCSI SENSE data");
		*check_condition = 0;	/* Clear Sense Flag */
	}

	ioctl(cdev, VX_FINISH_SCSI_CMD, &count);
}

/*
 * Read the SCSI command from the char device
 *
 * Called with:
 * 	cdev   -> Char dev file handle,
 * 	vheadp -> vtl_header struct pointer.
 *	count  -> Number of bytes in SCSI cdb
 *
 * Return: void
 */
void getCommand(int cdev, struct vtl_header *vheadp, int count)
{
	int	status = 0;
	u8	* SCpnt;

	/*  Call the vtl kernel module to populate the vtl_header struct. */
	ioctl(cdev, VTL_GET_HEADER, vheadp);

	SCpnt = (u8 *)&vheadp->cdb;

	if (verbose > 1) {
		syslog(LOG_DAEMON|LOG_INFO, "SCSI S/No: %d",
						ntohl(vheadp->serialNo));
		if (verbose > 2) {	/* High verbose - log every cmd. */
			logSCSICommand(SCpnt);
		} else {		/* Lower verbosity,.. */
			if (SCpnt[0] != 0)	/* Skip TUR */
				logSCSICommand(SCpnt);
		}
	}

	/* Ack kernel - we have read cdb */
	ioctl(cdev, VX_ACK_SCSI_CDB, &status);
}

/* Hex dump 'count' bytes at p  */
void hex_dump(u8 *p, int count)
{
	int	j;

	for (j = 0; j < count; j++) {
		if ((j != 0) && (j % 16 == 0))
			printf("\n");
		printf("%02x ", p[j]);
	}
	printf("\n");
}

int chrdev_open(char *name, uint8_t minor)
{
	FILE *f;
	char	devname[256];
	char	buf[256];
	int	devn;
	int	ctlfd;

	f = fopen("/proc/devices", "r");
	if (!f) {
		printf("Cannot open control path to the driver\n");
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
	ctlfd = open(devname, O_RDWR);
	if (ctlfd < 0) {
		printf("Cannot open %s %s\n", devname, strerror(errno));
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
	int	fd, err;
	char path[64];

	/* Avoid oom-killer */
	sprintf(path, "/proc/%d/oom_adj", getpid());
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		syslog(LOG_DAEMON|LOG_WARNING,
				"Can't open oom-killer's pardon %s, %m\n",
				path);
		return 0;
	}
	err = write(fd, "-17\n", 4);
	if (err < 0) {
		syslog(LOG_DAEMON|LOG_WARNING,
				"Can't adjust oom-killer's pardon %s, %m\n",
				path);
	}
	close(fd);
	return 0;
}

void log_opcode(char *opcode, uint8_t *cdb, uint8_t *sense_flg)
{
	if (verbose)
		syslog(LOG_DAEMON|LOG_INFO, "*** Unsupported op code: %s ***",
				opcode);
	mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sense_flg);
	logSCSICommand(cdb);
}

int resp_a3_service_action(uint8_t *cdb, uint8_t *sense_flg)
{
	uint8_t sa = cdb[1];
	switch(sa) {
	case MANAGEMENT_PROTOCOL_IN:
		log_opcode("MANAGEMENT PROTOCOL IN **", cdb, sense_flg);
		break;
	case REPORT_ALIASES:
		log_opcode("REPORT ALIASES **", cdb, sense_flg);
		break;
	}
	log_opcode("Unknown service action A3 **", cdb, sense_flg);
	return 0;
}

int resp_a4_service_action(uint8_t *cdb, uint8_t *sense_flg)
{
	uint8_t sa = cdb[1];

	switch(sa) {
	case MANAGEMENT_PROTOCOL_OUT:
		log_opcode("MANAGEMENT PROTOCOL OUT **", cdb, sense_flg);
		break;
	case CHANGE_ALIASES:
		log_opcode("CHANGE ALIASES **", cdb, sense_flg);
		break;
	}
	log_opcode("Unknown service action A4 **", cdb, sense_flg);
	return 0;
}

int ProcessSendDiagnostic(uint8_t *SCpnt, int sz, uint8_t *buf, uint32_t block_size, uint8_t *sense_flg)
{
	syslog(LOG_DAEMON|LOG_INFO,
			"*** Unsupported op code: SEND DIAGNOSTICS **");
	return 0;
}

