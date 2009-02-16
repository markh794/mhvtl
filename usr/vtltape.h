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
#define B_UNCOMPRESS_DATA 1
#define B_COMPRESSED_DATA 2
#define B_FILEMARK	3
#define B_BOT	4	// Beginning of Tape
#define B_EOD	5	// End of data
#define B_EOM_WARN 6	// End of Media - early warning
#define B_EOM	7	// End of Media
#define B_NOOP	8	// No Operation - fake it
// #define B_CLEANING 9	// Cleaning cartridge
#define B_WORM 10	// Write Once Read Many media type
#define B_ENCRYPT 11	// Encrypted block

#define TAPE_FMT_VERSION	1

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
 */
struct blk_header {
	uint32_t	blk_type;
	uint32_t	blk_size;
	uint32_t	disk_blk_size;
	loff_t		blk_number;
	loff_t		prev_blk;
	loff_t		curr_blk;
	loff_t		next_blk;
	
};

/* Default tape size specified in Mbytes */
#define DEFAULT_TAPE_SZ 8000

