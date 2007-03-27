/*
 * The shared library libvxshared.so function defination
 *
 * $Id: vxshared.h,v 1.3.2.1 2006-08-06 07:58:44 markh Exp $
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

// Log Page header
struct	log_pg_header {
	u8		pcode;
	u8		res;
	u16		len;
	};

// Page Code header struct.
struct	pc_header {
	u8		head0;
	u8		head1;
	u8		flags;
	u8		len;
	};

// Vendor Specific : 0x32 (Taken from IBM Ultrium doco)
struct	DataCompression {
	struct log_pg_header	pcode_head;

	struct pc_header	h_ReadCompressionRatio;
	u16			ReadCompressionRatio;
	struct pc_header	h_WriteCompressionRatio;
	u16			WriteCompressionRatio;
	struct pc_header	h_MBytesToServer;
	u32			MBytesToServer;
	struct pc_header	h_BytesToServer;
	u32			BytesToServer;
	struct pc_header	h_MBytesReadFromTape;
	u32			MBytesReadFromTape;
	struct pc_header	h_BytesReadFromTape;
	u32			BytesReadFromTape;

	struct pc_header	h_MBytesFromServer;
	u32			MBytesFromServer;
	struct pc_header	h_BytesFromServer;
	u32			BytesFromServer;
	struct pc_header	h_MBytesWrittenToTape;
	u32			MBytesWrittenToTape;
	struct pc_header	h_BytesWrittenToTape;
	u32			BytesWrittenToTape;
	};

// Buffer Under/Over Run log page - 0x01 : SPC-3 (7.2.3)
struct	BufferUnderOverRun {
	struct log_pg_header	pcode_head;
	};

struct	TapeUsage {
	struct log_pg_header	pcode_head;
	struct pc_header	flagNo01;
	u32			value01;
	struct pc_header	flagNo02;
	u64			value02;
	struct pc_header	flagNo03;
	u32			value03;
	struct pc_header	flagNo04;
	u16			value04;
	struct pc_header	flagNo05;
	u16			value05;
	struct pc_header	flagNo06;
	u16			value06;
	struct pc_header	flagNo07;
	u64			value07;
	struct pc_header	flagNo08;
	u32			value08;
	struct pc_header	flagNo09;
	u16			value09;
	struct pc_header	flagNo10;
	u16			value10;
	struct pc_header	flagNo11;
	u16			value11;
	};

struct	TapeCapacity {
	struct log_pg_header	pcode_head;
	struct pc_header	flagNo01;
	u32			value01;
	struct pc_header	flagNo02;
	u32			value02;
	struct pc_header	flagNo03;
	u32			value03;
	struct pc_header	flagNo04;
	u32			value04;
	};

// Tape Alert Log Page - 0x2E : SSC-3 (8.2.3)
// FIXME: There has to be a neater way of doing this - but I've got a
//	  brain freeze at the moment :(
struct	TapeAlert_page {
	struct log_pg_header	pcode_head;

	struct pc_header	flagNo01;
	u8			value01;
	struct pc_header	flagNo02;
	u8			value02;
	struct pc_header	flagNo03;
	u8			value03;
	struct pc_header	flagNo04;
	u8			value04;
	struct pc_header	flagNo05;
	u8			value05;
	struct pc_header	flagNo06;
	u8			value06;
	struct pc_header	flagNo07;
	u8			value07;
	struct pc_header	flagNo08;
	u8			value08;
	struct pc_header	flagNo09;
	u8			value09;
	struct pc_header	flagNo0a;
	u8			value0a;
	struct pc_header	flagNo0b;
	u8			value0b;
	struct pc_header	flagNo0c;
	u8			value0c;
	struct pc_header	flagNo0d;
	u8			value0d;
	struct pc_header	flagNo0e;
	u8			value0e;
	struct pc_header	flagNo0f;
	u8			value0f;

	struct pc_header	flagNo10;
	u8			value10;
	struct pc_header	flagNo11;
	u8			value11;
	struct pc_header	flagNo12;
	u8			value12;
	struct pc_header	flagNo13;
	u8			value13;
	struct pc_header	flagNo14;
	u8			value14;
	struct pc_header	flagNo15;
	u8			value15;
	struct pc_header	flagNo16;
	u8			value16;
	struct pc_header	flagNo17;
	u8			value17;
	struct pc_header	flagNo18;
	u8			value18;
	struct pc_header	flagNo19;
	u8			value19;
	struct pc_header	flagNo1a;
	u8			value1a;
	struct pc_header	flagNo1b;
	u8			value1b;
	struct pc_header	flagNo1c;
	u8			value1c;
	struct pc_header	flagNo1d;
	u8			value1d;
	struct pc_header	flagNo1e;
	u8			value1e;
	struct pc_header	flagNo1f;
	u8			value1f;

	struct pc_header	flagNo20;
	u8			value20;
	struct pc_header	flagNo21;
	u8			value21;
	struct pc_header	flagNo22;
	u8			value22;
	struct pc_header	flagNo23;
	u8			value23;
	struct pc_header	flagNo24;
	u8			value24;
	struct pc_header	flagNo25;
	u8			value25;
	struct pc_header	flagNo26;
	u8			value26;
	struct pc_header	flagNo27;
	u8			value27;
	struct pc_header	flagNo28;
	u8			value28;
	struct pc_header	flagNo29;
	u8			value29;
	struct pc_header	flagNo2a;
	u8			value2a;
	struct pc_header	flagNo2b;
	u8			value2b;
	struct pc_header	flagNo2c;
	u8			value2c;
	struct pc_header	flagNo2d;
	u8			value2d;
	struct pc_header	flagNo2e;
	u8			value2e;
	struct pc_header	flagNo2f;
	u8			value2f;

	struct pc_header	flagNo30;
	u8			value30;
	struct pc_header	flagNo31;
	u8			value31;
	struct pc_header	flagNo32;
	u8			value32;
	struct pc_header	flagNo33;
	u8			value33;
	struct pc_header	flagNo34;
	u8			value34;
	struct pc_header	flagNo35;
	u8			value35;
	struct pc_header	flagNo36;
	u8			value36;
	struct pc_header	flagNo37;
	u8			value37;
	struct pc_header	flagNo38;
	u8			value38;
	struct pc_header	flagNo39;
	u8			value39;
	struct pc_header	flagNo3a;
	u8			value3a;
	struct pc_header	flagNo3b;
	u8			value3b;
	struct pc_header	flagNo3c;
	u8			value3c;
	struct pc_header	flagNo3d;
	u8			value3d;
	struct pc_header	flagNo3e;
	u8			value3e;
	struct pc_header	flagNo3f;
	u8			value3f;

	struct pc_header	flagNo40;
	u8			value40;
	};

// Temperature Log Page - 0x0d : SPC-3 (7.2.13)
struct	Temperature_page {
	struct log_pg_header	pcode_head;
	struct pc_header	header;
	u16		temperature;
	};

// Write/Read/Read Reverse
// Error Counter log page - 0x02, 0x03, 0x04 : SPC-3 (7.2.4)
struct	error_counter {
	struct log_pg_header	pcode_head;

	struct pc_header	h_err_correctedWODelay;
	u32			err_correctedWODelay;
	struct pc_header	h_err_correctedWDelay;
	u32			err_correctedWDelay;
	struct pc_header	h_totalReTry;
	u32			totalReTry;
	struct pc_header	h_totalErrorsCorrected;
	u32			totalErrorsCorrected;
	struct pc_header	h_correctAlgorithm;
	u32			correctAlgorithm;
	struct pc_header	h_bytesProcessed;
	u64	bytesProcessed;
	struct pc_header	h_uncorrectedErrors;
	u32			uncorrectedErrors;
	struct pc_header	h_readErrorsSinceLast;
	u32			readErrorsSinceLast;
	struct pc_header	h_totalRawReadError;
	u32			totalRawReadError;
	struct pc_header	h_totalDropoutError;
	u32			totalDropoutError;
	struct pc_header	h_totalServoTracking;
	u32			totalServoTracking;
	};

// Sequential-Access
// Device log page - 0x0C : SSC-3 (Ch 8.2.2)
struct	seqAccessDevice {
	struct log_pg_header	pcode_head;

	struct pc_header	h_writeDataB4;
	u64	writeDataB4Compression;
	struct pc_header 	h_writeData_Af;
	u64	writeDataAfCompression;

	struct pc_header	h_readData_b4;
	u64	readDataB4Compression;
	struct pc_header 	h_readData_Af;
	u64	readDataAfCompression;

	struct pc_header	h_cleaning;
	u64	TapeAlert;

	struct pc_header	h_mbytes_processed;
	u32			mbytes_processed;

	struct pc_header	h_load_cycle;
	u32			load_cycle;

	struct pc_header	h_clean;	// Header of clean
	u32			clean_cycle;

	};


struct report_luns {
	u32		size;
	u32		reserved;
	u32		LUN_0;
	};

struct mode {
	u8		pcode;		// Page code
	u8		subpcode;	// Sub page code
	s32		pcodeSize;	// Size of page code data.
	u8		*pcodePointer;	// Pointer to page code - NULL end of array..
	};

/* v2 of the tape media
 * Between BOT & blk #1, is the MAM (Medium Auxiliary Memory)
 */
struct MAM {
	u32		tape_fmt_version;
	u8		spare;

	u64		remaining_capacity;
	u64		max_capacity;
	u64		TapeAlert;
	u64		LoadCount;
	u64		MAMSpaceRemaining;
	u8 		AssigningOrganization_1[8];
	u8 		FormattedDensityCode;
	u8 		InitializationCount[2];
	u8 		DevMakeSerialLastLoad[40];
	u8 		DevMakeSerialLastLoad1[40];
	u8 		DevMakeSerialLastLoad2[40];
	u8 		DevMakeSerialLastLoad3[40];
	u64		WrittenInMediumLife;
	u64		ReadInMediumLife;
	u64		WrittenInLastLoad;
	u64		ReadInLastLoad;

	u8 		MediumManufacturer[8];
	u8 		MediumSerialNumber[32];
	u32		MediumLength;
	u32		MediumWidth;
	u8 		AssigningOrganization_2[8];
	u8 		MediumDensityCode;
	u8 		MediumManufactureDate[8];
	u64		MAMCapacity;
	u8 		MediumType;	// 0 -> Data, 1, WORM, 6 -> Clean
	u16		MediumTypeInformation;	// If Clean, max mount

	u8 		ApplicationVendor[8];
	u8 		ApplicationName[32];
	u8 		ApplicationVersion[8];
	u8 		UserMediumTextLabel[160];
	u8 		DateTimeLastWritten[12];
	u8 		LocalizationIdentifier;
	u8 		Barcode[32];
	u8 		OwningHostTextualName[80];
	u8 		MediaPool[160];

	u8 		VendorUnique[256];

	// 0 = Record clean, non-zero umount failed.
	u8		record_dirty;
};

s32 send_msg(s8 *, s32);
void logSCSICommand(u8 *);
s32 check_reset(u8 *);
void mkSenseBuf(u8, u32, u8 *);
void resp_allow_prevent_removal(u8 *, u8 *);
void resp_log_select(u8 *, u8 *);
s32 resp_read_position_long(loff_t, u8 *, u8 *);
s32 resp_read_position(loff_t, u8 *, u8 *);
s32 resp_report_lun(struct report_luns *, u8 *, u8 *);
s32 resp_read_media_serial(u8 *, u8 *, u8 *);
s32 resp_mode_sense(u8 *, u8 *, struct mode *, u8 *);
s32 resp_write_attribute(u8 *, u64, struct MAM *, u8 *);
struct mode *find_pcode(u8, struct mode *);
struct mode *alloc_mode_page(u8, struct mode *, s32);

void setTapeAlert(struct TapeAlert_page *, u64);

void hex_dump(u8 *, int);
int	chrdev_open(char *name, uint8_t);
int	oom_adjust(void);
