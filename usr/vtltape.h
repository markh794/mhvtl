/*
 * Describes the header layout of each tape 'block'
 *
 * $Id: vtltape.h,v 1.1.2.1 2006-08-06 07:58:44 markh Exp $
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

/* Block type definitations */
#define B_DATA		11
#define B_FILEMARK	 3
#define B_BOT_V1	 4	// Beginning of Tape TAPE_FMT_VERSION 1
#define B_BOT		14	// Beginning of Tape TAPE_FMT_VERSION 2
#define B_EOD		 5	// End of data
#define B_NOOP		 8	// No Operation - fake it

#define BLKHDR_FLG_COMPRESSED 0x01
#define BLKHDR_FLG_ENCRYPTED  0x02

#define TAPE_FMT_VERSION	2

/*
 * Header before each block of data in 'file'
 *
 *	block_type	-> See above 'Block type definations'
 *	blk_size	-> Uncompressed size of data block
 *		   (Specifies capacity of tape (used in BOT header) in Mbytes.
 *	disk_blk_size	-> Amount of space block takes up in 'file'
 *	prev_blk	-> Allow quick seek
 *	curr_blk	-> Allow quick seek
 *	next_blk	-> Allow quick seek
 * encryption_key_length   -> what length was the key used to 'encrypt' this block
 * encryption_ukad_length  -> what length was the ukad used to 'encrypt' this block
 * encryption_akad_length  -> what length was the akad used to 'encrypt' this block
 * encryption_key  -> what key was used to 'encrypt' this block
 * encryption_ukad -> what ukad was used to 'encrypt' this block
 * encryption_kkad -> what akad was used to 'encrypt' this block
 */
struct blk_header {
	uint32_t	blk_type;
	uint32_t	blk_size;
	uint32_t	disk_blk_size;
	uint32_t	blk_flags;
	loff_t		blk_number;
	loff_t		prev_blk;
	loff_t		curr_blk;
	loff_t		next_blk;
	uint32_t	unused;	/* Available */
	uint32_t	encryption_key_length;
	uint32_t	encryption_ukad_length;
	uint32_t	encryption_akad_length;
	uint8_t		encryption_key[32];
	uint8_t		encryption_ukad[32];
	uint8_t		encryption_akad[32];
	/*
	 * Add other things right here...
	 * Be careful to keep data 64bit aligned
	 */
	/* Adjust pad to mantain a 512byte structure */
	char		pad[352];
};

/* Default tape size specified in Mbytes */
#define DEFAULT_TAPE_SZ 8000

#define medium_density_code_lto1	0x40
#define medium_density_code_lto2	0x42
#define medium_density_code_lto3	0x44
#define medium_density_code_lto4	0x46
#define medium_density_code_j1a		0x51
#define medium_density_code_e05		0x52
#define medium_density_code_e06		0x53
#define medium_density_code_ait4	0x33
#define medium_density_code_10kA	0x4a
#define medium_density_code_10kB	0x4b
#define medium_density_code_600		0x40
