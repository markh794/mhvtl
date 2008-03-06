/*
 * The userspace tape/library header file for the vtl virtual
 * tape kernel module.
 *
 * $Id: vx.h,v 1.19.2.2 2006-08-30 06:35:01 markh Exp $
 *
 * Copyright (C) 2005 Mark Harvey markh794 at gmail dot com
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
 */

typedef unsigned long long u64;
typedef long long 	s64;
typedef unsigned int	u32;
typedef int		s32;
typedef unsigned short	u16;
typedef short		s16;
typedef unsigned char	u8;
typedef char		s8;

#ifdef Solaris
 typedef s64	loff_t;
#endif

#ifndef Solaris
  #include <endian.h>
  #include <byteswap.h>
#endif

#if __BYTE_ORDER == __BIG_ENDIAN

#define ntohll(x)	(x)
#define ntohl(x)	(x)
#define ntohs(x)	(x)
#define htonll(x)	(x)
#define htonl(x)	(x)
#define htons(x)	(x)
#else
# if __BYTE_ORDER == __LITTLE_ENDIAN
#  define ntohll(x)	__bswap_64 (x)
#  define ntohl(x)	__bswap_32 (x)
#  define ntohs(x)	__bswap_16 (x)
#  define htonll(x)	__bswap_64 (x)
#  define htonl(x)	__bswap_32 (x)
#  define htons(x)	__bswap_16 (x)
# endif
#endif

#define EOM_FLAG 0x40

#define VX_TAPE_ONLINE		0x80
#define VX_TAPE_OFFLINE		0x81
#define VX_TAPE_FIFO_SIZE	0x82
#define VX_TAPE_POLL_STATUS	0x83
#define VX_TAPE_ACK_STATUS	0x84

#define VX_FINISH_SCSI_CMD	0x184
#define VX_ACK_SCSI_CDB		0x185
#define VX_ADD_HOST		0x186

#define VTL_GET_HEADER		0x200
#define VTL_GET_DATA		0x201
#define VTL_SET_SERIAL		0x202
#define VTL_PUT_DATA		0x203

#define STATUS_OK 0

#define STATUS_QUEUE_CMD 0xfe

#define SENSE_BUF_SIZE 38

/* Where all the tape data files belong */
#define HOME_PATH "/opt/vtl"

/*
 * Process the LOG_SENSE page definations
 */
#define BUFFER_UNDER_OVER_RUN 0x01
#define WRITE_ERROR_COUNTER 0x02
#define READ_ERROR_COUNTER 0x03
#define READ_REVERSE_ERROR_COUNTER 0x04
#define VERIFY_ERROR_COUNTER 0x05
#define NON_MEDIUM_ERROR_COUNTER 0x06
#define LAST_n_ERROR 0x07
#define FORMAT_STATUS 0x08
#define LAST_n_DEFERRED_ERROR 0x0b
#define SEQUENTIAL_ACCESS_DEVICE 0x0c
#define TEMPERATURE_PAGE 0x0d
#define START_STOP_CYCLE_COUNTER 0x0e
#define APPLICATION_CLIENT 0x0f
#define SELFTEST_RESULTS 0x10
#define TAPE_ALERT 0x2e
#define INFORMATIONAL_EXCEPTIONS 0x2f
#define TAPE_USAGE 0x30
#define TAPE_CAPACITY 0x31
#define DATA_COMPRESSION 0x32

#define MAX_INQ_ARR_SZ 64

/*
 * Medium Type Definations
 */
#define MEDIA_TYPE_DATA 0
#define MEDIA_TYPE_WORM 1
#define MEDIA_TYPE_CLEAN 6

struct	vtl_header {
	u32 serialNo;
	u8 cdb[16];
	u8 *buf;
};


void completeSCSICommand(int, u32, u8 *, u8 *, u8 *, u32);
void getCommand(int, struct vtl_header *, int);
