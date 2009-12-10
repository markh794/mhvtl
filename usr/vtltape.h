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

#ifndef _VTLTAPE_H_
#define _VTLTAPE_H_

#include "vtllib.h"

/* Block type definitations */
#define B_DATA		11
#define B_FILEMARK	 3
#define B_EOD		 5	// End of data
#define B_NOOP		 8	// No Operation - fake it

#define BLKHDR_FLG_COMPRESSED 0x01
#define BLKHDR_FLG_ENCRYPTED  0x02

#define TAPE_FMT_VERSION	3

struct	encryption {
	uint32_t	key_length;
	uint32_t	ukad_length;
	uint32_t	akad_length;
	uint32_t	pad;
	uint8_t		key[32];
	uint8_t		ukad[32];
	uint8_t		akad[32];
};

/*
 * Header before each block of data in 'file'
 *
 *	block_type	-> See above 'Block type definations'
 *	blk_size	-> Uncompressed size of data block
 *		   (Specifies capacity of tape (used in BOT header) in Mbytes.
 *	disk_blk_size	-> Amount of space block takes up in 'file'
 * encryption.key_length   -> what length was the key used to 'encrypt' this block
 * encryption.ukad_length  -> what length was the ukad used to 'encrypt' this block
 * encryption.akad_length  -> what length was the akad used to 'encrypt' this block
 * encryption.key  -> what key was used to 'encrypt' this block
 * encryption.ukad -> what ukad was used to 'encrypt' this block
 * encryption.kkad -> what akad was used to 'encrypt' this block
 */

struct blk_header {
	uint32_t	blk_type;
	uint32_t	blk_flags;
	uint32_t	blk_number;
	uint32_t	blk_size;
	uint32_t	disk_blk_size;
	uint32_t	pad;
	struct encryption encryption;

	/*
	 * Add other things right here...
	 * Be careful to keep data 64bit aligned
	 */
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
#define medium_density_code_600		0x4a

/* Sense Data format bits & pieces */
/* Incorrect Length Indicator */
#define SD_VALID 0x80
#define SD_FILEMARK 0x80
#define SD_EOM 0x40
#define SD_ILI 0x20

/* The remainder of this file defines the interface between the tape drive
   software and the implementation of a tape cartridge as one or more disk
   files.
*/

extern struct MAM mam;
extern struct blk_header *c_pos;
extern int OK_to_write;

int create_tape(const char *pcl, const struct MAM *mamp, uint8_t *sam_stat);

int load_tape(const char *pcl, uint8_t *sam_stat);
void unload_tape(uint8_t *sam_stat);

int rewind_tape(uint8_t *sam_stat);
int position_to_eod(uint8_t *sam_stat);
int position_to_block(uint32_t blk_no, uint8_t *sam_stat);
int position_blocks_forw(uint32_t count, uint8_t *sam_stat);
int position_blocks_back(uint32_t count, uint8_t *sam_stat);
int position_filemarks_forw(uint32_t count, uint8_t *sam_stat);
int position_filemarks_back(uint32_t count, uint8_t *sam_stat);

uint32_t read_tape_block(uint8_t *buf, uint32_t size, uint8_t *sam_stat);

int write_filemarks(uint32_t count, uint8_t *sam_stat);
int write_tape_block(const uint8_t *buf, uint32_t uncomp_size,
	uint32_t comp_size, const struct encryption *cp, uint8_t *sam_stat);
int format_tape(uint8_t *sam_stat);

int rewriteMAM(uint8_t *sam_stat);
uint64_t current_tape_offset(void);

void print_raw_header(void);

#endif /* _VTLTAPE_H_ */
