/*
 * This handles any SCSI OP codes defined in the standards as 'STREAM'
 *
 * Copyright (C) 2005 - 2025 Mark Harvey markh794 at gmail dot com
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
#include <assert.h>
#include <sys/time.h>
#include "be_byteshift.h"
#include "mhvtl_scsi.h"
#include "mhvtl_list.h"
#include "vtl_common.h"
#include "logging.h"
#include "vtllib.h"
#include "spc.h"
#include "q.h"
#include "ssc.h"
#include "vtltape.h"
#include "mhvtl_log.h"
#include "mode.h"

#ifdef MHVTL_DEBUG
static struct allow_overwrite_state {
	char *desc;
} allow_overwrite_desc[] = {
	{ "Disabled", },
	{ "Current Position", },
	{ "Format", },
	{ "Opps, Invalid field in CDB", },
};
#endif

uint8_t ssc_allow_overwrite(struct scsi_cmd *cmd)
{
	uint8_t *cdb = cmd->scb;
	uint8_t allow_overwrite = cdb[2] & 0x0f;
	uint8_t partition = cdb[3];
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t ret_stat = SAM_STAT_GOOD;
	uint64_t allow_overwrite_block;
	struct priv_lu_ssc *lu_ssc;
	struct s_sd sd;

	lu_ssc = cmd->lu->lu_private;

	if (allow_overwrite > 2) /* Truncate bad values 3 to 15 -> '3' */
		allow_overwrite = 3;

	MHVTL_DBG(1, "ALLOW OVERWRITE (%ld) : %s **",
			(long)cmd->dbuf_p->serialNo,
			allow_overwrite_desc[allow_overwrite].desc);

	lu_ssc->allow_overwrite = FALSE;

	switch (allow_overwrite) {
	case 0:
		break;
	case 1:  /* current position */
		if (partition) { /* Paritions not supported at this stage */
			MHVTL_LOG("Partitions not implemented at this time");
			sd.byte0 = SKSV | CD;
			sd.field_pointer = 3;
			sam_illegal_request(E_INVALID_FIELD_IN_CDB,
					&sd, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		allow_overwrite_block = get_unaligned_be64(&cdb[4]);
		MHVTL_DBG(1, "Allow overwrite block: %lld",
					(long long)allow_overwrite_block);
		if (allow_overwrite_block == current_tape_block()) {
			lu_ssc->allow_overwrite_block = allow_overwrite_block;
			lu_ssc->allow_overwrite = TRUE;
		} else {
			/* Set allow_overwrite position to an invalid number */
			lu_ssc->allow_overwrite_block = 0;
			lu_ssc->allow_overwrite_block--;
			sam_illegal_request(E_SEQUENTIAL_POSITIONING_ERROR,
						NULL, sam_stat);
			ret_stat = SAM_STAT_CHECK_CONDITION;
		}
		break;
	case 2:
		lu_ssc->allow_overwrite = 2;
		break;
	default:
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 2;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		ret_stat = SAM_STAT_CHECK_CONDITION;
		break;
	}

	return ret_stat;
}

uint8_t ssc_log_select(struct scsi_cmd *cmd)
{
	uint8_t sam_status;
	uint8_t pcr = cmd->scb[1] & 0x2;	/* Parameter code reset */
	struct priv_lu_ssc *lu_priv;

	lu_priv = cmd->lu->lu_private;

	sam_status = spc_log_select(cmd);
	if (sam_status)	/* spc_log_select() failed - return */
		return sam_status;

	if (pcr) {
		switch ((cmd->scb[2] & 0xc0) >> 6) {
		case 3:
			lu_priv->bytesRead_I = 0;
			lu_priv->bytesRead_M = 0;
			lu_priv->bytesWritten_I = 0;
			lu_priv->bytesWritten_M = 0;
			break;
		}
	}
	return SAM_STAT_GOOD;
}

uint8_t complete_read_6(struct scsi_cmd *cmd, int sz, int count)
{
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct mhvtl_ds *dbuf_p;
	struct priv_lu_ssc *lu_ssc;
	uint8_t *buf;
	int lbp_method;
	int k;
	int retval = 0;
	int fixed = cdb[1] & FIXED_BLOCK;

	lu_ssc = cmd->lu->lu_private;
	dbuf_p = cmd->dbuf_p;

	switch (get_tape_load_status()) {
	case TAPE_LOADING:
		sam_not_ready(E_BECOMING_READY, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	case TAPE_LOADED:
		if (mam.MediumType == MEDIA_TYPE_CLEAN) {
			MHVTL_DBG(3, "Cleaning cart loaded");
			sam_not_ready(E_CLEANING_CART_INSTALLED,
								sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		break;
	case TAPE_UNLOADED:
		MHVTL_DBG(3, "No media loaded");
		sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	default:
		MHVTL_DBG(1, "Media format corrupt");
		sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	buf = dbuf_p->data;
	for (k = 0; k < count; k++) {
		if (!lu_ssc->pm->valid_encryption_blk(cmd))
			return SAM_STAT_CHECK_CONDITION;
		/* If LBP Read bit is set, pass through the LBP_method 0: off, 1 RS-CRC, 2 CRC32C */
		lbp_method = (lu_ssc->LBP_R) ? lu_ssc->LBP_method : 0;
		retval = readBlock(buf, sz, cdb[1] & SILI, lbp_method, sam_stat);
		if (!retval && fixed) {
			/* Fixed block read hack:
			 *
			 * Overwrite INFORMATION field with:
			 *
			 * SSC4r01e states:
			 * The INFORMATION field shall be set to the requested
			 * transfer length minus the actual number of logical
			 * blocks read (not including the incorrect-length
			 * logical block).

			 * NOTE 35 - In the above case with the FIXED bit of
			 * one, only the position of the incorrect-length
			 * logical block may be determined from the sense
			 * data. The actual length of the incorrect logical
			 * block is not reported. Other means may be used to
			 * determine its actual length (e.g., read it again
			 * with the fixed bit set to zero).
			 */
			MHVTL_DBG(2, "Fixed block read short by %d blocks",
						count - k);
			put_unaligned_be32(count - k, &sense[3]);
			break;
		}
		buf += retval;
		dbuf_p->sz += retval;
	}
	if (retval > (sz * count))
		retval = sz * count;

	return *sam_stat;
}

/* VERIFY_6:
 *
 * Here is what the IBM_LTO SCSI Reference (18Aug2021) says about the op code.
 * GA32-0928-04 18 August 2021
 *
 * The following parameters apply:
 *
 * • VTE (verify to end-of-data): If the VTE bit is set to zero, then a verify to EOD is not requested.
 *  If the VTE bit is set to one, then the expected verification sequence termination condition is met
 *  when EOD is encountered. If a filemark is encountered during the sequence, processing continues.
 *  If the verify command fails, then the VALID bit and the INFORMATION field of sense data are set
 *  to zero. The VBF bit shall be set to zero. The VERIFICATION LENGTH field is ignored.
 *
 * • VLBPM (verify logical block protection method): This bit has no effect. The result is the same for either setting.
 *
 * • VBF (verify by filemarks): If the VBF bit is set to zero, then a verify of n filemarks is not requested.
 *  If the VBF bit is set to one, then the expected verification sequence termination condition is met
 *  if the number of filemarks specified by the VERIFICATION LENGTH field have been traversed. If a
 *  filemark is encountered during the sequence, processing continues. If EOD is encountered, the
 *  sense key is set to BLANK CHECK, the EOM bit is set to one if the logical position is at or
 *  after early warning, and the additional sense code is set to END-OF-DATA DETECTED. If a verify
 *  operation fails, then the verification sequence terminates and the VALID bit is set to one and
 *  the INFORMATION FIELD is set to the requested verification length minus the actual number of
 *  filemarks successfully traversed. The VTE bit shall be set to zero.
 *  NOTE 30 - Following the completion of a verify with the VBF bit set to one, the application
 *  client should issue a READ POSITION command to determine the logical object identifier associated
 *  with the current logical position.
 *
 * • IMMED (immediate) : An IMMED bit set to zero specifies the command shall not return status until
 *  the verify sequence has completed.
 *  An IMMED bit set to one specifies status shall be returned as soon as the command descriptor block
 *  has been validated. Verification sequences that complete unsuccessfully generate deferred sense data
 *  indicating the reason for termination (e.g., .an incorrect length logical block is encountered and
 *  the sense data is set to indicate an incorrect length block was encountered).
 *  NOTE 31 - In order to ensure that no errors are lost, the application client should set the IMMED
 *  bit to zero on the last VERIFY (6) command of a series of VERIFY (6) commands.
 *
 * • BYTCMP (byte compare): Byte compare is not supported by this device. The BYTCMP bit shall be set to
 *  zero to specify the verification shall be a verification of logical blocks on the medium (e.g., CRC, ECC).
 *   No data shall be transferred from the application client to the device server.
 *
 * • FIXED: If the VTE bit and the VBF bit are set to zero and the FIXED bit is set to one, then the
 *  expected verification sequence termination condition is met when the number of logical blocks
 *  specified in the VERIFICATION LENGTH field have been traversed. If the VERIFICATION LENGTH field
 *  is set to zero, then no logical objects are verified and the current logical position is not changed.
 *  This condition is not an error. If a file-mark is encountered during the sequence, processing
 *  terminates with filemark encountered as specified in the READ(6) command (see 5.2.15).
 *  If EOD is encountered, the sense key is set to BLANK CHECK, the EOM bit is set to one if the logical
 *  position is at or after early warning, and the additional sense code is set to END-OF-DATA DETECTED.
 *  If a verify operation fails, then the verification sequence terminates and the VALID bit is set to
 *  one and the INFORMATION FIELD is set to the requested verification length minus the actual number
 *  of logical blocks successfully traversed.
 *  If the VTE bit and the VBF bit are set to zero and the FIXED bit is set to zero, then the expected
 *  verification sequence termination condition is met when one logical block has been traversed.
 *  The length of the verified logical block is equal to the value specified in the VERIFICATION LENGTH field.
 *  If the VERIFICATION LENGTH field is set to zero, then no logical objects are verified and the
 *  current logical position is not changed. This condition is not considered an error. If a filemark
 *  is encountered during the sequence, pro- cessing terminates with filemark encountered as specified in the
 *  READ(6) command (see 5.2.15). If EOD is encountered, the sense key is set to BLANK CHECK, the EOM bit is
 *  set to one if the logical position is at or after early warning, and the additional sense code is set to
 *  END-OF-DATA DETECTED. If a verify oper- ation fails, then the verification sequence terminates and the
 *  VALID bit is set to one and the INFORMATION FIELD is set to the requested verification length minus
 *  the actual number of bytes successfully traversed.
 *  A FIXED bit set to zero and either the VTE bit set to one or the VBF bit set to one specifies that the
 *  block length shall not be checked.
 *  A FIXED bit set to one specifies that the length of verified logical blocks shall be equal to the the
 *  current block length reported in the mode parameters block descriptor. Refer to the READ(6) command
 *  (see 5.2.15) for a description of the FIXED bit and any error conditions that may result from incorrect usage.
 *
 * • VERIFICATION LENGTH: The VERIFICATION LENGTH field specifies the number of bytes, logical blocks,
 *  or file-marks to traverse during verification, as specified by the VBF bit and the FIXED bit.
 *  If the VTE bit is set to one, then the VERIFICATION LENGTH field is ignored.
 *  If the VERIFICATION LENGTH field is set to zero and the VTE bit is set to zero, then no logical objects
 *  are verified and the current logical position is not changed. This condition is not considered an error.
*/
uint8_t ssc_verify_6(struct scsi_cmd *cmd)
{
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct mhvtl_ds *dbuf_p;
	int blocks;
	int sz;
	struct s_sd sd;
	struct verify_6_bits *cdb1;
	cdb1 = (struct verify_6_bits *)&cdb[1];
	dbuf_p = cmd->dbuf_p;

	set_current_state(MHVTL_STATE_VERIFY);

	dbuf_p->sz = 0;	/* zero data xfer between application and target */

	opcode_6_params(cmd, &blocks, &sz);
	MHVTL_DBG(1, "%s(): %s: %d, fixed: %d, bytcmp: %d, immed: %d, vbf: %d, vlbpm: %d, vte: %d, cdb[1]: 0x%02x (%ld) **",
			__func__,
			(cdb1->FIXED) ? "Num blks" : "byte count",
			(cdb1->FIXED) ? blocks : sz,
			cdb1->FIXED, cdb1->BYTCMP, cdb1->IMMED, cdb1->VBF, cdb1->VLBPM, cdb1->VTE, cdb[1],
			(long)dbuf_p->serialNo);

	if (cdb1->VTE) {	/* To be implemented */
		MHVTL_DBG(1, "Verify to end of data is currently not implemented");
		sd.byte0 = SKSV | CD | BPV | 0x6;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	if (cdb1->VBF) {
		int count;

		count = (cdb1->FIXED) ? blocks : sz;
		MHVTL_DBG(1, "Verify %d filemarks", count);
		resp_space(count, 1, sam_stat); /* Lets try and move 'count' number of filemarks */
		return *sam_stat;
	}

	*sam_stat = complete_read_6(cmd, sz, blocks);
	return *sam_stat;
}

uint8_t ssc_read_6(struct scsi_cmd *cmd)
{
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	int count;
	int sz;
	struct s_sd sd;

	set_current_state(MHVTL_STATE_READING);

	opcode_6_params(cmd, &count, &sz);
	MHVTL_DBG(3, "%s(): %d block%s of %d bytes (%ld) **",
				__func__,
				count, count == 1 ? "" : "s",
				sz,
				(long)cmd->dbuf_p->serialNo);

	/* If both FIXED & SILI bits set, invalid combo.. */
	if ((cdb[1] & (SILI | FIXED_BLOCK)) == (SILI | FIXED_BLOCK)) {
		MHVTL_DBG(1, "Suppress ILI and Fixed block "
					"read not allowed by SSC3");
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	return complete_read_6(cmd, sz, count);
}

uint8_t ssc_write_6(struct scsi_cmd *cmd)
{
	struct mhvtl_ds *dbuf_p;
	struct priv_lu_ssc *lu_ssc;
	int count;
	int sz;
	int k;
	int retval = 0;

	lu_ssc = cmd->lu->lu_private;
	dbuf_p = cmd->dbuf_p;

	set_current_state(MHVTL_STATE_WRITING);

	opcode_6_params(cmd, &count, &sz);
	MHVTL_DBG(3, "%s(): %d block%s of %d bytes (%ld) **",
				__func__,
				count, count == 1 ? "" : "s",
				sz,
				(long)cmd->dbuf_p->serialNo);

	/* FIXME: Should handle this instead of 'check & warn' */
	if ((sz * count) > lu_ssc->bufsize)
		MHVTL_DBG(1,
			"Fatal: bufsize %d, requested write of %d bytes",
							lu_ssc->bufsize, sz);

	dbuf_p->sz = sz * count;

	/* Retrieve data from kernel - unless media type is 'null' */
	if (likely(mam.MediumType != MEDIA_TYPE_NULL))
		retrieve_CDB_data(cmd->cdev, dbuf_p);

	if (!lu_ssc->pm->check_restrictions(cmd))
		return SAM_STAT_CHECK_CONDITION;

	if (OK_to_write) {
		for (k = 0; k < count; k++) {
			retval = writeBlock(cmd, sz);
			dbuf_p->data += retval;

			/* If sam_stat != SAM_STAT_GOOD, return */
			if (cmd->dbuf_p->sam_stat)
				return cmd->dbuf_p->sam_stat;
		}
	}
	return SAM_STAT_GOOD;
}

/*
 * Check for any write restrictions - e.g. WORM, or Clean Cartridge mounted.
 * Return 1 = OK to write, zero -> Can't write.
 */
uint8_t check_restrictions(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct priv_lu_ssc *lu_ssc = cmd->lu->lu_private;

	/* Check that there is a piece of media loaded.. */
	switch (get_tape_load_status()) {
	case TAPE_LOADING:
		sam_not_ready(E_BECOMING_READY, sam_stat);
		*lu_ssc->OK_2_write = 0;
		return *lu_ssc->OK_2_write;
		break;
	case TAPE_LOADED:	/* Do nothing */
		break;
	case TAPE_UNLOADED:
		sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
		*lu_ssc->OK_2_write = 0;
		return *lu_ssc->OK_2_write;
		break;
	default:
		sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
		*lu_ssc->OK_2_write = 0;
		return *lu_ssc->OK_2_write;
		break;
	}

	switch (mam.MediumType) {
	case MEDIA_TYPE_CLEAN:
		sam_not_ready(E_CLEANING_CART_INSTALLED, sam_stat);
		MHVTL_DBG(2, "Can not write - Cleaning cart");
		*lu_ssc->OK_2_write = 0;
		break;
	case MEDIA_TYPE_WORM:
		/* If we are not at end of data for a write
		 * and media is defined as WORM, fail...
		 */

		 /* OK to append to end of 'tape' */
		if (c_pos->blk_type == B_EOD)
			*lu_ssc->OK_2_write = 1;

		if (!*lu_ssc->OK_2_write) {
			MHVTL_DBG(1, "Failed attempt to overwrite WORM data");
			sam_data_protect(E_MEDIUM_OVERWRITE_ATTEMPT, sam_stat);
		}
		break;
	case MEDIA_TYPE_DATA:
		*lu_ssc->OK_2_write = 1;
		break;
	case MEDIA_TYPE_NULL:
		*lu_ssc->OK_2_write = 1;
		/* Special case - no need for more checks */
		return *lu_ssc->OK_2_write;
		break;
	default:
		*lu_ssc->OK_2_write = 0;
		sam_illegal_request(E_MEDIUM_INCOMPATIBLE, NULL, sam_stat);
	}

	/* over-ride the above IF the virtual write protect switch is on */
	if (*lu_ssc->OK_2_write && lu_ssc->MediaWriteProtect) {
		*lu_ssc->OK_2_write = 0;
		sam_data_protect(E_WRITE_PROTECT, sam_stat);
	}

	/* over-ride the above IF running in append_only mode and this write
	 * isn't authorized.
	 * Some writes would be OK, like the first write to an empty tape or
	 * WORM media overwriting a filemark that is next to EOD
	 */
	if (*lu_ssc->OK_2_write && lu_ssc->append_only_mode) {
		if ((c_pos->blk_number != lu_ssc->allow_overwrite_block) &&
				(c_pos->blk_type != B_EOD)) {

			uint64_t TAflag;

			lu_ssc->OK_2_write = 0;
			lu_ssc->allow_overwrite = FALSE;
			sam_data_protect(E_MEDIUM_OVERWRITE_ATTEMPT, sam_stat);
			/* And set TapeAlert flg 09 -> WRITE PROTECT */
			TAflag = TA_WRITE_PROTECT;
			update_TapeAlert(cmd->lu, TAflag);
		}
	}

	MHVTL_DBG(2, "returning:%s writable",
			(*lu_ssc->OK_2_write) ? "" : " not");
	return *lu_ssc->OK_2_write;
}

/*
 * Returns true if blk header has correct encryption key data
 */
#define	UKAD_LENGTH	(encr->ukad_length)
#define	AKAD_LENGTH	(encr->akad_length)
#define	KEY_LENGTH	(encr->key_length)
#define	UKAD		(encr->ukad)
#define	AKAD		(encr->akad)
#define	KEY		(encr->key)
uint8_t valid_encryption_blk(struct scsi_cmd *cmd)
{
	uint8_t correct_key;
	int i;
	struct lu_phy_attr *lu = cmd->lu;
	struct priv_lu_ssc *lu_priv;
	struct encryption *encr;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	lu_priv = lu->lu_private;
	encr = lu_priv->app_encr_info;

	/* decryption logic */
	correct_key = TRUE;

	if (c_pos->blk_flags & BLKHDR_FLG_ENCRYPTED) {
		/* compare the keys */
		if (lu_priv->DECRYPT_MODE > 1) {
			if (c_pos->blk_encryption_info.key_length != KEY_LENGTH) {
				sam_data_protect(E_INCORRECT_KEY, sam_stat);
				correct_key = FALSE;
			}
			for (i = 0; i < c_pos->blk_encryption_info.key_length; ++i) {
				if (c_pos->blk_encryption_info.key[i] != KEY[i]) {
					sam_data_protect(E_INCORRECT_KEY, sam_stat);
					correct_key = FALSE;
					return correct_key;
				}
			}
		} else {
			sam_data_protect(E_UNABLE_TO_DECRYPT, sam_stat);
			correct_key = FALSE;
		}
	} else if (lu_priv->DECRYPT_MODE == 2) {
		sam_data_protect(E_UNENCRYPTED_DATA, sam_stat);
		correct_key = FALSE;
	}
	return correct_key;
}

uint8_t valid_encryption_media(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct lu_phy_attr *lu;
	struct priv_lu_ssc *lu_priv;

	lu = cmd->lu;
	lu_priv = lu->lu_private;

	if (c_pos->blk_number == 0) {
		modeBlockDescriptor[0] = lu_priv->pm->native_drive_density->density;
		mam.MediumDensityCode = modeBlockDescriptor[0];
		mam.FormattedDensityCode = modeBlockDescriptor[0];
		rewriteMAM(sam_stat);
	} else {
		if (mam.MediumDensityCode !=
				lu_priv->pm->native_drive_density->density) {
			sam_data_protect(E_WRITE_PROTECT, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	return SAM_STAT_GOOD;
}

uint8_t ssc_allow_prevent_removal(struct scsi_cmd *cmd)
{
	/* FIXME: Currently does nothing... */
	MHVTL_DBG(1, "%s MEDIA REMOVAL (%ld) **",
					(cmd->scb[4]) ? "PREVENT" : "ALLOW",
					(long)cmd->dbuf_p->serialNo);
	return SAM_STAT_GOOD;
}

uint8_t ssc_format_medium(struct scsi_cmd *cmd)
{
	struct lu_phy_attr *lu;
	struct priv_lu_ssc *lu_priv;

	lu = cmd->lu;
	lu_priv = lu->lu_private;

	MHVTL_DBG(1, "FORMAT MEDIUM (%ld) **", (long)cmd->dbuf_p->serialNo);

	if (!lu_priv->pm->check_restrictions(cmd))
		return SAM_STAT_CHECK_CONDITION;

	if (c_pos->blk_number != 0) {
		MHVTL_DBG(2, "Failed - Not at beginning");
		sam_illegal_request(E_POSITION_PAST_BOM, NULL,
					&cmd->dbuf_p->sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	format_tape(&cmd->dbuf_p->sam_stat);

	return SAM_STAT_GOOD;
}

uint8_t ssc_locate(struct scsi_cmd *cmd)
{
	uint32_t blk_no;

	set_current_state(MHVTL_STATE_LOCATE);

	MHVTL_DBG(1, "LOCATE %d (%ld) **", (cmd->scb[0] == LOCATE_16) ? 16 : 10,
			(long)cmd->dbuf_p->serialNo);

	blk_no = (cmd->scb[0] == LOCATE_16) ?
		get_unaligned_be64(&cmd->scb[4]) : get_unaligned_be32(&cmd->scb[3]);

	/* If we want to seek closer to beginning of file than
	 * we currently are, rewind and seek from there
	 */
	MHVTL_DBG(2, "Current blk: %d, seek: %d",
					c_pos->blk_number, blk_no);
	position_to_block(blk_no, &cmd->dbuf_p->sam_stat);

	return cmd->dbuf_p->sam_stat;
}

uint8_t ssc_load_display(struct scsi_cmd *cmd)
{
	unsigned char *d;
	char str1[9];
	char str2[9];

	MHVTL_DBG(1, "LOAD DISPLAY (%ld) - T10000 specific **",
					(long)cmd->dbuf_p->serialNo);

	cmd->dbuf_p->sz = cmd->scb[4];
	retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);
	d = cmd->dbuf_p->data;
	memcpy(str1, &d[1], 8);
	str1[8] = 0;
	memcpy(str2, &d[9], 8);
	str2[8] = 0;

	MHVTL_DBG(3, "Raw data: %02x  "
			"%02x %02x %02x %02x %02x %02x %02x %02x  "
			"%02x %02x %02x %02x %02x %02x %02x %02x",
			d[0], d[1], d[2], d[3], d[4],
			d[5], d[6], d[7], d[8],
			d[9], d[10], d[11], d[12],
			d[13], d[14], d[15], d[16]);

	switch (d[0] >> 5) { /* Bits 5, 6 & 7 are overlay */
	case 0:
		MHVTL_DBG(1, "Display \'%s\' until next"
				" command that initiates tape motion",
	/* Low/High bit */	(d[0] & 2) ? str2 : str1);
		break;
	case 1:
		MHVTL_DBG(1, "Maintain \'%s\' until the"
				" cartridge is unloaded",
	/* Low/High bit */	(d[0] & 2) ? str2 : str1);
		break;
	case 2:
		MHVTL_DBG(1, "Maintain \'%s\' until the drive"
				" is next loaded", str1);
		break;
	case 3:
		MHVTL_DBG(1, "Physically access tape drive with"
				"out changing the msg");
		break;
	case 7:
		MHVTL_DBG(1, "Display \'%s\' until the tape"
				" drive is unloaded then \'%s\'",
					str1, str2);
		break;
	}
	MHVTL_DBG(2, "Load display: msg1: %s msg2: %s",
					str1, str2);

	cmd->dbuf_p->sz = 0;
	return SAM_STAT_GOOD;
}

#define REPORT_TIMESTAMP_DATA_LEN 0x0a

/* If timestamp_source == 0 : timestamp is num of uS at logical unit initialization - get_timestamp will return delta of mS between init and now
 * If timestamp_source == 2 : timestamp is set to that provided by initiator - get_timestamp will return delta + timestamp
 */

static uint64_t timestamp;	/* Used for device clock - number uS since initialization */
static int64_t timestamp_offset;	/* Used for device clock - offset of local clock and initiator 'set timestamp' value */
static uint8_t timestamp_source;

void set_timestamp(uint8_t source, uint64_t ts)
{
	struct timeval tv;
	uint64_t now;

	timestamp_source = source;
	gettimeofday(&tv, NULL);
	now = 1000000 * tv.tv_sec + tv.tv_usec;
	if (source) {
		timestamp = ts * 1000;	/* save as uSec */
		timestamp_offset = now - timestamp;
	} else {
		timestamp = now;
		timestamp_offset = 0;
	}
	MHVTL_DBG(1, "SET timestamp: source %u, timestamp is %lu, offset is %ld", source, timestamp, timestamp_offset);
}

static uint64_t get_timestamp()
{
	struct timeval tv;
	uint64_t now;

	gettimeofday(&tv, NULL);
	now = 1000000 * tv.tv_sec + tv.tv_usec;

	if (timestamp_source) {
		MHVTL_DBG(1, "now: %lx, offset: %ld, ret val:  0x%lx", now, timestamp_offset, (now - timestamp_offset) / 1000);
		return (now - timestamp_offset) / 1000;	/* Account for any offset between local time and initiator set time */
	} else {
		MHVTL_DBG(1, "now: %lx, timestamp: %lx, ret val: 0x%lx", now, timestamp, (now - timestamp) / 1000);
		return (now - timestamp) / 1000;	/* Num of mS since init */
	}
}

static uint8_t report_timestamp(struct scsi_cmd *cmd)
{
	uint8_t *data;

	data = cmd->dbuf_p->data;
	memset(data, 0, REPORT_TIMESTAMP_DATA_LEN + 2);

	put_unaligned_be16(REPORT_TIMESTAMP_DATA_LEN, &data[0]);
	data[2] = timestamp_source;	/* Timestamp origin - Timestamp initialised to zero at power-on */
	put_unaligned_be48(get_timestamp() & 0xffffffffffff, &data[4]);

	MHVTL_DBG(1, "Returning timestamp 0x%08lx (%lu)", get_unaligned_be48(&data[4]), get_unaligned_be48(&data[4]));
	cmd->dbuf_p->sz = REPORT_TIMESTAMP_DATA_LEN + 2;
	cmd->dbuf_p->sam_stat = SAM_STAT_GOOD;
	return SAM_STAT_GOOD;
}

static uint8_t configure_timestamp(struct scsi_cmd *cmd)
{
	struct s_sd sd;
	uint8_t *data;

	cmd->dbuf_p->sz = get_unaligned_be32(&cmd->scb[6]);
	if (cmd->dbuf_p->sz == 0)
		return SAM_STAT_GOOD;
	if (cmd->dbuf_p->sz != 0x0c) {
		cmd->dbuf_p->sz = 0;
		sd.byte0 = SKSV;
		sd.field_pointer = 6;
		MHVTL_LOG("Unexpected timestamp parameter length..");
		sam_illegal_request(E_INVALID_FIELD_IN_PARMS, &sd, &cmd->dbuf_p->sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);
	data = cmd->dbuf_p->data;

	if (data[4] > 0xf0) {	/* overflow - Illegal request */
		cmd->dbuf_p->sz = 0;
		sd.byte0 = SKSV;
		sd.field_pointer = 4;
		MHVTL_LOG("Unexpected set timestamp value.. Value too large");
		sam_illegal_request(E_INVALID_FIELD_IN_PARMS, &sd, &cmd->dbuf_p->sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	set_timestamp(2, get_unaligned_be48(&data[4]));

	cmd->dbuf_p->sz = 0;
	cmd->dbuf_p->sam_stat = SAM_STAT_GOOD;
	return SAM_STAT_GOOD;
}

uint8_t ssc_a3_service_action(struct scsi_cmd *cmd)
{
	switch (cmd->scb[1]) {
	case MANAGEMENT_PROTOCOL_IN:
		log_opcode("MANAGEMENT PROTOCOL IN **", cmd);
		break;
	case REPORT_ALIASES:
		log_opcode("REPORT ALIASES **", cmd);
		break;
	case REPORT_SUPPORTED_OPCODES:
		log_opcode("REPORT SUPPORTED OPCODES **", cmd);
		break;
	case REPORT_TIMESTAMP:
		MHVTL_DBG(1, "REPORT TIMESTAMP (%ld) **", (long)cmd->dbuf_p->serialNo);
		return report_timestamp(cmd);
		break;
	default:
		log_opcode("UNKNOWN SERVICE ACTION A3 **", cmd);
		break;
	}
	return cmd->dbuf_p->sam_stat;
}

uint8_t ssc_a4_service_action(struct scsi_cmd *cmd)
{
	switch (cmd->scb[1]) {
	case MANAGEMENT_PROTOCOL_OUT:
		log_opcode("MANAGEMENT PROTOCOL OUT **", cmd);
		break;
	case CHANGE_ALIASES:
		log_opcode("CHANGE ALIASES **", cmd);
		break;
	case FORCED_EJECT:
		log_opcode("FORCED EJECT **", cmd);
		break;
	case SET_TIMESTAMP:
		MHVTL_DBG(1, "SET TIMESTAMP (%ld) **", (long)cmd->dbuf_p->serialNo);
		return configure_timestamp(cmd);
		break;
	default:
		log_opcode("UNKNOWN SERVICE ACTION A4 **", cmd);
		break;
	}
	return cmd->dbuf_p->sam_stat;
}

uint8_t ssc_spout(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "SECURITY PROTOCOL OUT (%ld) **",
						(long)cmd->dbuf_p->serialNo);

	cmd->dbuf_p->sz = get_unaligned_be32(&cmd->scb[6]);
	/* Check for '512 increment' bit & multiply sz by 512 if set */
	cmd->dbuf_p->sz *= (cmd->scb[4] & 0x80) ? 512 : 1;

	retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);

	return resp_spout(cmd);
}

uint8_t ssc_spin(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "SECURITY PROTOCOL IN (%ld) **",
					(long)cmd->dbuf_p->serialNo);

	return resp_spin(cmd);
}

uint8_t ssc_pr_out(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	lu_priv = cmd->lu->lu_private;

	MHVTL_DBG(1, "PERSISTENT RESERVE OUT (%ld) **",
						(long)cmd->dbuf_p->serialNo);
	if (lu_priv->I_am_SPC_2_Reserved) {
		MHVTL_DBG(1, "SPC 2 reserved");
		*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		return SAM_STAT_RESERVATION_CONFLICT;
	}
	cmd->dbuf_p->sz = get_unaligned_be32(&cmd->scb[5]);
	retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);
	return resp_spc_pro(cmd->scb, cmd->dbuf_p);
}

static uint8_t set_device_configuration_extension(struct scsi_cmd *cmd, uint8_t *p)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct lu_phy_attr *lu = cmd->lu;
	struct priv_lu_ssc *lu_priv = cmd->lu->lu_private;
	struct ssc_personality_template *pm;
	struct mode *mp;
	struct s_sd sd;
	int page_code_len;
	int write_mode;
	int pews;	/* Programable Early Warning Size */

	pm = lu_priv->pm;

	mp = lookup_pcode(&lu->mode_pg, MODE_DEVICE_CONFIGURATION, 1);

	/* Code error
	 * Any device supporting this should have this mode page defined */
	if (!mp) {
		sam_hardware_error(E_INTERNAL_TARGET_FAILURE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	page_code_len = get_unaligned_be16(&p[2]);

	if (page_code_len != 0x1c) {
		sd.byte0 = SKSV;
		sd.field_pointer = 2;
		MHVTL_LOG("Unexpected page code length.. Unexpected results");
		sam_illegal_request(E_INVALID_FIELD_IN_PARMS, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	write_mode = (p[5] & 0xf0) >> 4;
	if (write_mode > 1) {
		MHVTL_LOG("Unsupported write mode: 0x%x", write_mode);
		sd.byte0 = SKSV | BPV | 7;	/* bit 7 */
		sd.field_pointer = 5;
		sam_illegal_request(E_INVALID_FIELD_IN_PARMS, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	MHVTL_DBG(2, "%s mode", write_mode ? "Append-only" : "Overwrite-allowed");

	pews = get_unaligned_be16(&p[6]);
	if (pm->drive_supports_prog_early_warning) {
		MHVTL_DBG(2, "Set Programable Early Warning Size: %d", pews);
		lu_priv->prog_early_warning_sz = pews;
		update_prog_early_warning(lu);
	} else {
		MHVTL_DBG(2, "Programable Early Warning Size not supported"
				" by this device");
	}

	MHVTL_DBG(2, "Volume containing encrypted logical blocks "
			"requires encryption: %d",
			p[8] & 0x01);

	if (pm->drive_supports_append_only_mode) {
		/* Can't reset append-only mode via mode page ssc4 8.3.8 */
		if (lu_priv->append_only_mode && write_mode == 0) {
			MHVTL_LOG("Can't reset append only mode via mode page");
			sam_illegal_request(E_INVALID_FIELD_IN_PARMS,
						NULL, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		if (write_mode) {
			lu_priv->append_only_mode = write_mode;
			lu_priv->allow_overwrite = FALSE;
		}
	}

	/* Now update our copy of this mode page */
	mp->pcodePointer[5] &= 0x0f;
	mp->pcodePointer[5] |= write_mode << 4;

	return SAM_STAT_GOOD;
}

static void set_device_configuration(struct scsi_cmd *cmd, uint8_t *p)
{
	struct lu_phy_attr *lu = cmd->lu;
	struct priv_lu_ssc *lu_priv = cmd->lu->lu_private;
	struct ssc_personality_template *pm;

	pm = lu_priv->pm;

	MHVTL_DBG(2, " Report Early Warning   : %s",
			(p[8] & 0x01) ? "Yes" : "No");
	MHVTL_DBG(2, " Software Write Protect : %s",
			(p[10] & 0x04) ? "Yes" : "No");
	MHVTL_DBG(2, " WORM Tamper Read Enable: %s",
			(p[15] & 0x80) ? "Yes" : "No");

	MHVTL_DBG(2, " Setting device compression Algorithm");
	if (p[14]) { /* Select Data Compression Alg */
		MHVTL_DBG(2, "  Mode Select->Setting compression: %d", p[14]);
		if (pm->set_compression) {
			pm->set_compression(&lu->mode_pg,
				lu_priv->configCompressionFactor);
			set_lp11_compression(1);	/* Update LogPage 11 compression bit */
		}
	} else {
		MHVTL_DBG(2, "  Mode Select->Clearing compression");
		if (pm->clear_compression) {
			pm->clear_compression(&lu->mode_pg);
			set_lp11_compression(0);	/* Update LogPage 11 compression bit */
		}
	}
}

static void set_mode_compression(struct scsi_cmd *cmd, uint8_t *p)
{
	struct lu_phy_attr *lu = cmd->lu;
	struct priv_lu_ssc *lu_priv = lu->lu_private;
	struct ssc_personality_template *pm;
	int dce;

	pm = lu_priv->pm;
	dce = p[2] & 0x80;

	MHVTL_DBG(2, " Data Compression Enable   : %s (0x%02x)",
				(p[2] & 0x80) ? "Yes" : "No", p[2]);
	MHVTL_DBG(2, " Data Compression Capable  : %s",
				(p[2] & 0x40) ? "Yes" : "No");
	MHVTL_DBG(2, " Data DeCompression Enable : %s (0x%02x)",
				(p[3] & 0x80) ? "Yes" : "No", p[3]);
	MHVTL_DBG(2, " Compression Algorithm     : 0x%04x",
				get_unaligned_be32(&p[4]));
	MHVTL_DBG(2, " DeCompression Algorithm   : 0x%04x",
				get_unaligned_be32(&p[8]));
	MHVTL_DBG(2, " Report Exception on Decompression: 0x%02x",
				(p[3] & 0x6) >> 5);

	if (dce) { /* Data Compression Enable bit set */
		MHVTL_DBG(1, " Setting compression");
		if (pm->set_compression) {
			pm->set_compression(&lu->mode_pg,
					lu_priv->configCompressionFactor);
			set_lp11_compression(1);	/* Update LogPage 11 compression bit */
		}
	} else {
		MHVTL_DBG(1, " Clearing compression");
		if (pm->clear_compression) {
			pm->clear_compression(&lu->mode_pg);
			set_lp11_compression(0);	/* Update LogPage 11 compression bit */
		}
	}
}

static uint8_t set_lbp(struct scsi_cmd *cmd, uint8_t *buf, int len)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct priv_lu_ssc *lu_priv;
	struct ssc_personality_template *pm;
	struct s_sd sd;

	lu_priv = cmd->lu->lu_private;
	pm = lu_priv->pm;

	/* OK, the drive supports Logical Block Protection - good to go */
	if (pm->drive_supports_LBP) {
		return update_logical_block_protection(cmd->lu, buf);
	}
	sd.byte0 = SKSV | CD;
	sd.field_pointer = 1;
	sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}

/*
 * Process the MODE_SELECT command
 */
uint8_t ssc_mode_select(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t *buf = cmd->dbuf_p->data;
	uint8_t *bdb = NULL;
	int page_len = 0;
	int mode_param_h_sz = 0;
	int i, j;
	int count;
	int save_pages;
	int page_format;
	int mselect_6 = 0;
	int page;
	int offset;
	int mode_medium_type;
	int mode_dev_spec_param;
	int mode_block_descriptor_len;
	struct s_sd sd;

	save_pages = cmd->scb[1] & 0x01;
	page_format = (cmd->scb[1] & (1 << 4)) ? 1 : 0;

	switch (cmd->scb[0]) {
	case MODE_SELECT:
		mselect_6 = 1;
		cmd->dbuf_p->sz = cmd->scb[4];
		mode_param_h_sz = 4;
		break;
	case MODE_SELECT_10:
		cmd->dbuf_p->sz = get_unaligned_be16(&cmd->scb[7]);
		mode_param_h_sz = 8;
		break;
	default:
		cmd->dbuf_p->sz = 0;
	}

	MHVTL_DBG(1, "MODE SELECT %d (%ld) **",
			(mselect_6) ? 6 : 10, (long)cmd->dbuf_p->serialNo);
	MHVTL_DBG(1, " Save Pages: %d, Page Format conforms to %s standard",
			save_pages, (page_format) ? "T10" : "Vendor uniq");

	count = retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);

	switch (cmd->scb[0]) {
	case MODE_SELECT:
		page_len = buf[5];
		break;
	case MODE_SELECT_10:
		page_len = get_unaligned_be16(&buf[9]);
		break;
	}

/*
 * As per t10.org SPC4r31 (6.9)
 *
 * A page format (PF) bit set to zero specifies that all parameters after
 * the block descriptors are vendor specific. A PF bit set to one specifies
 * that the MODE SELECT parameters following the header and block
 * descriptor(s) are structured as pages of related parameters and are
 * as defined in this standard.
 */
	if (!page_format && page_len) {
		MHVTL_DBG(1, "PF bit cleared, yet page data supplied. Len: %d",
					page_len);
		sd.byte0 = SKSV | CD | BPV | 4;	/* bit 4 is invalid */
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

#ifdef MHVTL_DEBUG
	if (debug)
		hex_dump(buf, cmd->dbuf_p->sz);
#endif

	/* T10 spec => MODE DATA LEN is reserved for MODE SELECT */
	MHVTL_DBG(3, "count: %d, param header len: %d", count, mode_param_h_sz);
	switch (mode_param_h_sz) {
	case 4:	/* MODE SELECT 6 */
		mode_medium_type = buf[1];
		mode_dev_spec_param = buf[2];
		mode_block_descriptor_len = buf[3];
		bdb = &buf[4];
		break;
	case 8: /* MODE SELECT 10 */
		mode_medium_type = buf[2];
		mode_dev_spec_param = buf[3];
		mode_block_descriptor_len = get_unaligned_be16(&buf[6]);
		bdb = &buf[8];
		break;
	default: /* Shouldn't be possible */
		MHVTL_LOG("Should never see this: line %d", __LINE__);
		mode_medium_type = 0;
		mode_dev_spec_param = 0;
		mode_block_descriptor_len = 0;
	}

	i = j = 0;
	MHVTL_DBG(3, " %02d: %02x %02x %02x %02x"
				"  %02x %02x %02x %02x"
				"  %02x %02x %02x %02x"
				"  %02x %02x %02x %02x",
				j,
				buf[i+0], buf[i+1], buf[i+2], buf[i+3],
				buf[i+4], buf[i+5], buf[i+6], buf[i+7],
				buf[i+8], buf[i+9], buf[i+10], buf[i+11],
				buf[i+12], buf[i+13], buf[i+14], buf[i+15]);

	MHVTL_DBG(3, "Mode Param header: Medium type 0x%02x, "
				"Device spec param 0x%02x, "
				"Blk Descr Len 0x%02x, "
				"Buff mode %d, Speed %d",
			mode_medium_type,
			mode_dev_spec_param,
			mode_block_descriptor_len,
			(mode_dev_spec_param & 0x70) >> 4,
			(mode_dev_spec_param & 0x0f)
			);

	i = mode_param_h_sz;
	if (mode_block_descriptor_len) {
		memcpy(modeBlockDescriptor, bdb, mode_block_descriptor_len);
		MHVTL_DBG(3, "Descriptor block: density code 0x%02x, "
				"No. of blocks 0x%02x, "
				"Block length 0x%02x",
				buf[i],
				get_unaligned_be24(&buf[i+1]),
				get_unaligned_be24(&buf[i+5]));
	}

	/*
	 * As per t10.org SPC4r31 (6.9)
	 *
	 * A save pages (SP) bit set to zero specifies that the device server
	 * shall perform the specified MODE SELECT operation, and shall not
	 * save any mode pages. If the logical unit implements no distinction
	 * between current and saved mode pages and the SP bit is set to zero,
	 * the command shall be terminated with CHECK CONDITION status,
	 * with the sense key set to ILLEGAL REQUEST, and the additional
	 * sense code set to INVALID FIELD IN CDB.
	 * An SP bit set to one specifies that the device server shall perform
	 * the specified MODE SELECT operation, and shall save to a nonvolatile
	 * vendor specific location all the saveable mode pages including any
	 * sent in the Data-Out Buffer.
	 * Mode pages that are saved are specified by the parameter saveable
	 * (PS) bit that is returned in the first byte of each mode page by
	 * the MODE SENSE command (see 7.5). If the PS bit is set to one in
	 * the MODE SENSE data, then the mode page shall be saveable by
	 * issuing a MODE SELECT command with the SP bit set to one. If the
	 * logical unit does not implement saved mode pages and the SP bit is
	 * set to one, then the command shall be terminated with CHECK CONDITION
	 * status, with the sense key set to ILLEGAL REQUEST, and the additional
	 * sense code set to INVALID FIELD IN CDB.
	 */
	if (save_pages) {
		MHVTL_DBG(1, " Save pages bit set. Not supported");
		sd.byte0 = SKSV | CD | BPV | 1;	/* bit 1 is invalid */
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	i += mode_block_descriptor_len;
	j = 0;
	while (i < count) {
		offset = 2;
		page = buf[i];
		page_len = buf[i + 1];

		MHVTL_DBG(2, " Page: 0x%02x, Page Len: 0x%02x", page, page_len);

		if (page_len) {
			MHVTL_DBG(3, " %02d: %02x %02x %02x %02x"
					"  %02x %02x %02x %02x",
				j,
				buf[i+0], buf[i+1], buf[i+2], buf[i+3],
				buf[i+4], buf[i+5], buf[i+6], buf[i+7]);
		}
		if (page_len > 8) {
			if (page_len == 0x0e) { /* Common page len */
				MHVTL_DBG(3, " %02d: %02x %02x %02x %02x"
						"  %02x %02x",
				j + 8,
				buf[i+8], buf[i+9], buf[i+10], buf[i+11],
				buf[i+12], buf[i+13]);
			} else {
				MHVTL_DBG(3, " %02d: %02x %02x %02x %02x"
						"  %02x %02x %02x %02x",
				j + 8,
				buf[i+8], buf[i+9], buf[i+10], buf[i+11],
				buf[i+12], buf[i+13], buf[i+14], buf[i+15]);
			}
		}
		if (page_len > 16) {
			MHVTL_DBG(3, " %02d: %02x %02x %02x %02x"
					"  %02x %02x %02x %02x",
			j + 16,
			buf[i+16], buf[i+17], buf[i+18], buf[i+19],
			buf[i+20], buf[i+21], buf[i+22], buf[i+23]);
		}
		if (page_len > 24) {
			MHVTL_DBG(3, " %02d: %02x %02x %02x %02x"
					"  %02x %02x %02x %02x",
			j + 24,
			buf[i+24], buf[i+25], buf[i+26], buf[i+27],
			buf[i+28], buf[i+29], buf[i+30], buf[i+31]);
		}

		switch (page) {
		case MODE_DATA_COMPRESSION:
			if (page_len == 0x0e)
				set_mode_compression(cmd, &buf[i]);
			break;

		case MODE_DEVICE_CONFIGURATION:
			/* If this is '01' it's a subpage value
			 *     i.e. DEVICE CONFIGURATION EXTENSION
			 * If it's 0x0e, it indicates a page length
			 * for MODE DEVICE CONFIGURATION
			 */
			if (page_len == 0x01) {
				if (set_device_configuration_extension(cmd,
							&buf[i]))
					return SAM_STAT_CHECK_CONDITION;
				/* Subpage 1 - override default page length */
				page_len = get_unaligned_be16(&buf[i + 2]);
				offset = 4;
			} else if (page_len == 0x0e) {
				set_device_configuration(cmd, &buf[i]);
			} else {
				MHVTL_DBG(2, "Invalid page len: 0x%02x",
							page_len);
				sd.byte0 = SKSV;
				sd.field_pointer = i + 1;
				sam_illegal_request(E_INVALID_FIELD_IN_CDB,
							&sd, sam_stat);
				return SAM_STAT_CHECK_CONDITION;
			}
			break;

		case MODE_CONTROL:
			if (page_len == 0x0a) { /* Control mode page - byte[1] is page len */
				MHVTL_DBG(3, "Setting Mode Control");	/* Silently accept this worked, but really did not change anything */
			} else {
				/* Otherwise, subpage handling - where page len is byte[2] & byte[3] */
				page_len = get_unaligned_be16(&buf[i+2]);
				if (buf[1+i] == 0xf0) {
					/* Logical Block Protection */
					MHVTL_DBG(2, "Setting LBP method: %d, LBP length: %d, LBP_W: %s, LBP_R: %s",
							buf[4+i], buf[5+i],
							(buf[6+i] & 0x80) ? "True" : "False",
							(buf[6+i] & 0x40) ? "True" : "False");
					if (set_lbp(cmd, &buf[i], page_len))
						return SAM_STAT_CHECK_CONDITION;
				} else {
					MHVTL_DBG(2, "Mode Control - Subpage: 0x%02x not supported", buf[i+1]);
					sd.byte0 = SKSV;
					sd.field_pointer = i;
					sam_illegal_request(E_INVALID_FIELD_IN_CDB,
									&sd, sam_stat);
					return SAM_STAT_CHECK_CONDITION;
				}
			}
			return SAM_STAT_GOOD;
			break;
		default:
			MHVTL_DBG_PRT_CDB(1, cmd);
			MHVTL_LOG("Mode page 0x%02x not handled", page);
			sd.byte0 = SKSV;
			sd.field_pointer = i;
			sam_illegal_request(E_INVALID_FIELD_IN_CDB,
							&sd, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
			break;
		}
		if (page_len == 0) { /* Something wrong with data structure */
			page_len = cmd->dbuf_p->sz;
			MHVTL_LOG("Problem with mode select data structure");
			sd.byte0 = SKSV;
			sd.field_pointer = i + 1;
			sam_illegal_request(E_INVALID_FIELD_IN_CDB,
							&sd, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		i += page_len + offset;	/* Next mode page */
		j += page_len;
	}

	return SAM_STAT_GOOD;
}

uint8_t ssc_write_attributes(struct scsi_cmd *cmd)
{
	int sz;
	uint8_t *sam_stat;

	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "WRITE ATTRIBUTES (%ld) **", (long)cmd->dbuf_p->serialNo);

	switch (get_tape_load_status()) {
	case TAPE_UNLOADED:
		sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	case TAPE_LOADED:
		cmd->dbuf_p->sz = get_unaligned_be32(&cmd->scb[10]);
		sz = retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);
		MHVTL_DBG(1, "  --> Expected to read %d bytes"
				", read %d", cmd->dbuf_p->sz, sz);
		if (resp_write_attribute(cmd) > 0)
			rewriteMAM(sam_stat);
		break;
	default:
		sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}
	return SAM_STAT_GOOD;
}

uint8_t ssc_tur(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	char str[64];

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;
	*sam_stat = SAM_STAT_GOOD;

	sprintf(str, "Test Unit Ready (%ld) ** : ",
				(long)cmd->dbuf_p->serialNo);

	switch (get_tape_load_status()) {
	case TAPE_UNLOADED:
		strcat(str, "No, No tape loaded");
		sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
		*sam_stat = SAM_STAT_CHECK_CONDITION;
		break;
	case TAPE_LOADING:
		strcat(str, "No, Tape loading");
		sam_not_ready(E_BECOMING_READY, sam_stat);
		*sam_stat = SAM_STAT_CHECK_CONDITION;
		break;
	case TAPE_LOADED:
		if (mam.MediumType == MEDIA_TYPE_CLEAN) {
			int state;

			strcat(str, "No, Cleaning cart loaded");

			if (lu_priv->cleaning_media_state)
				state = *lu_priv->cleaning_media_state;
			else
				state = 0;

			switch (state) {
			case CLEAN_MOUNT_STAGE1:
				sam_not_ready(E_CLEANING_CART_INSTALLED,
								sam_stat);
				break;
			case CLEAN_MOUNT_STAGE2:
				sam_not_ready(E_CAUSE_NOT_REPORTABLE,
								sam_stat);
				break;
			case CLEAN_MOUNT_STAGE3:
				sam_not_ready(E_INITIALIZING_REQUIRED,
								sam_stat);
				break;
			default:
				MHVTL_ERR("Unknown cleaning media mount state");
				sam_not_ready(E_CLEANING_CART_INSTALLED,
								sam_stat);
				break;
			}

			*sam_stat = SAM_STAT_CHECK_CONDITION;
		} else
			strcat(str, "Yes");
		break;
	default:
		strcat(str, "No, Media format corrupt");
		sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
		*sam_stat = SAM_STAT_CHECK_CONDITION;
		break;
	}

	MHVTL_DBG(1, "%s", str);

	return *sam_stat;
}

uint8_t ssc_rewind(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	int retval;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "REWINDING (%ld) **", (long)cmd->dbuf_p->serialNo);

	set_current_state(MHVTL_STATE_REWIND);

	switch (get_tape_load_status()) {
	case TAPE_UNLOADED:
		sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	case TAPE_LOADED:
		retval = rewind_tape(sam_stat);
		delay_opcode(DELAY_REWIND, lu_priv->delay_rewind);
		if (retval < 0) {
			sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		break;
	default:
		sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	return SAM_STAT_GOOD;
}

uint8_t ssc_read_attributes(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat;
	struct s_sd sd;

	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "READ ATTRIBUTE (%ld) **",
						(long)cmd->dbuf_p->serialNo);

	switch (get_tape_load_status()) {
	case TAPE_UNLOADED:
		MHVTL_DBG(1, "Failed due to \"no media loaded\"");
		sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	case TAPE_LOADED:
		break;
	default:
		MHVTL_DBG(1, "Failed due to \"media corrupt\"");
		sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	/* Only support Service Action - Attribute Values */
	if (cmd->scb[1] > 1) {
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	cmd->dbuf_p->sz = resp_read_attribute(cmd);

	return SAM_STAT_GOOD;
}

uint8_t ssc_read_block_limits(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "READ BLOCK LIMITS (%ld) **",
						(long)cmd->dbuf_p->serialNo);

	switch (get_tape_load_status()) {
	case TAPE_LOADED:
	case TAPE_UNLOADED:
		cmd->dbuf_p->sz = resp_read_block_limits(cmd->dbuf_p,
							lu_priv->bufsize);
		break;
	case TAPE_LOADING:
		sam_not_ready(E_BECOMING_READY, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	default:
		sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	return SAM_STAT_GOOD;
}

/* SPC 6.17 - READ MEDIA SERIAL NUMBER */
uint8_t ssc_read_media_sn(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	uint32_t alloc_len;
	struct s_sd sd;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;
	alloc_len = get_unaligned_be32(&cmd->scb[6]);

	MHVTL_DBG(1, "READ MEDIUM SERIAL NO. (%ld) **",
						(long)cmd->dbuf_p->serialNo);

	if (cmd->scb[1] != 1) {	/* Service Action 1 only */
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	memset_ssc_buf(cmd, alloc_len);

	switch (get_tape_load_status()) {
	case TAPE_LOADED:
		cmd->dbuf_p->sz = resp_read_media_serial(lu_priv->mediaSerialNo,
							cmd->dbuf_p->data,
							sam_stat);
		break;
	case TAPE_UNLOADED:
		sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	default:
		sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}
	return *sam_stat;
}

#define READ_POSITION_LEN 20
#define READ_POSITION_LONG_LEN 32
uint8_t ssc_read_position(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat;
	int service_action;
	struct s_sd sd;
	struct priv_lu_ssc *lu_priv;
	uint8_t *buf;
	uint8_t partition = 0;	/* One of these days we'll support multiple partitions - but for now */
	uint64_t filemarks = 0;
	struct read_position_information_short *sp;
	struct read_position_information_long *lp;

	lu_priv = cmd->lu->lu_private;

	sam_stat = &cmd->dbuf_p->sam_stat;

	buf = cmd->dbuf_p->data;

	MHVTL_DBG(1, "READ POSITION (%ld) **", (long)cmd->dbuf_p->serialNo);

	service_action = cmd->scb[1] & 0x1f;
	/* service_action == 0 or 1 -> Returns 20 bytes of data (short) */

	MHVTL_DBG(1, "service_action: %d", service_action);

	*sam_stat = SAM_STAT_GOOD;

	switch (get_tape_load_status()) {
	case TAPE_LOADED:
		switch (service_action) {
		case 0:
			sp = (struct read_position_information_short *)&buf[0];

			memset(buf, 0, READ_POSITION_LEN);	/* Clear 'array' */

			if (c_pos->blk_number < 2) {
				MHVTL_DBG(3, "Setting Beginning of Parition (BOP)");
				sp->BOP = 1;	/* Beginning of partition */
			}

			sp->LOCU = 0;	/* Logical object count unknown - 0: Block count is exact */
			sp->BYCU = 1;	/* Logical byte count unknown - 1: Byte count is estimate */
			sp->LOLU = 0;	/* Logical Object Location Unknown - 0: Count is exact */

			if (c_pos->blk_number > 0xfffffffe) {	/* logical block address overflow - currently not possible as blk_number is a uint32_t */
				sp->PERR = 1;
				MHVTL_DBG(1, "More than supported number of blocks - Setting Logical Block overflow");
			}

			buf[1] = partition;
			put_unaligned_be32(c_pos->blk_number, &buf[4]);	/* First Logical Object Location - (current location) */
			put_unaligned_be32(c_pos->blk_number, &buf[8]);	/* After a write, Logical Object Location of the new write - If buffer empty: == first logical objecct */

			MHVTL_DBG(1, "Positioned at block %ld", (long)c_pos->blk_number);

			if (current_tape_offset() > lu_priv->early_warning_position) {
				sp->EOP = 1;
				MHVTL_DBG(3, "Setting End of Partition (EOP)");
			}
			if ((lu_priv->pm->drive_supports_prog_early_warning) && (current_tape_offset() >= lu_priv->prog_early_warning_position)) {
				MHVTL_DBG(3, "Drive supports prog early warning : Setting prog_early_warning of Partition (BPEW & EOP)");
				sp->BPEW = 1;
				sp->EOP = 1;
			} else {
				sp->BPEW = 0;
			}
			cmd->dbuf_p->sz = READ_POSITION_LEN;
			break;
		case 6:
			lp = (struct read_position_information_long *)&buf[0];

			/* Return tape position - long format
			 *
			 * Need to implement.
			 * [ 4 -  7] Partition No.
			 *           - The partition number for the current logical position
			 * [ 8 - 15] Logical Object No.
			 *           - The number of logical blocks between the beginning of the
			 *           - partition and the current logical position.
			 * [16 - 23] Logical File Identifier
			 *           - Number of Filemarks between the beginning of the partition and
			 *           - the logical position.
			 * [24 - 31] Logical Set Identifier - Obsolete... (IBM Ultrium LTO-9)
			 *           - Number of Setmarks between the beginning of the partition and
			 *           - the logical position.
			 */

			memset(buf, 0, READ_POSITION_LONG_LEN);	/* Clear 'array' */

			if (c_pos->blk_number < 2) {
				lp->BOP = 1;
				MHVTL_DBG(3, "Setting Beginning of Parition (BOP)");
			}

			lp->LONU = 0;	/* Set 'Logical Object Number Unknown' bit valid (block location info is valid) */
			lp->MPU = 0;	/* Mark Position Unknown : 0 = num filemarks is known */

			if (current_tape_offset() > lu_priv->early_warning_position) {
				MHVTL_DBG(3, "Setting End of Partition (EOP)");
				lp->BPEW = 0;
				lp->EOP = 1;
			}
			if ((lu_priv->pm->drive_supports_prog_early_warning) && (current_tape_offset() >= lu_priv->prog_early_warning_position)) {
				MHVTL_DBG(3, "Drive supports prog early warning : Setting prog_early_warning of Partition (BPEW & EOP)");
				lp->BPEW = 1;
				lp->EOP = 1;
			}

			filemarks = filemark_count(c_pos->blk_number);

			put_unaligned_be32(partition, &buf[4]);
			put_unaligned_be64(c_pos->blk_number, &buf[8]);
			put_unaligned_be64(filemarks, &buf[16]);

			MHVTL_DBG(1, "Positioned at block %ld, num filemarks: %ld", (long)c_pos->blk_number, filemarks);
			cmd->dbuf_p->sz =  READ_POSITION_LONG_LEN;
			break;

		default:
			MHVTL_DBG(1, "service_action not supported");
			sd.byte0 = SKSV | CD;
			sd.field_pointer = 1;
			sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd,
								sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		break;
	case TAPE_UNLOADED:
		sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	default:
		sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}
	return *sam_stat;
}

uint8_t ssc_release(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;

	lu_priv = cmd->lu->lu_private;

	MHVTL_DBG(1, "RELEASE (%ld) **", (long)cmd->dbuf_p->serialNo);
	if (!SPR_Reservation_Type && SPR_Reservation_Key)
		return SAM_STAT_RESERVATION_CONFLICT;

	lu_priv->I_am_SPC_2_Reserved = 0;
	return SAM_STAT_GOOD;
}

uint8_t ssc_report_density_support(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	uint8_t media;
	struct s_sd sd;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	media = cmd->scb[1] & 0x01;
	cmd->dbuf_p->sz = 0;

	MHVTL_DBG(1, "REPORT %s DENSITY SUPPORT (%ld) **",
					(media) ? "MOUNTED MEDIA" : "DRIVE",
					(long)cmd->dbuf_p->serialNo);

	if (cmd->scb[1] & 0x02) { /* Don't support Medium Type (yet) */
		MHVTL_DBG(1, "Medium Type - not currently supported");
		sd.byte0 = SKSV | CD;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (media == 1 && get_tape_load_status() != TAPE_LOADED) {
		MHVTL_DBG(1, "Media has to be mounted to return media density");
		sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	memset_ssc_buf(cmd, get_unaligned_be16(&cmd->scb[7]));

	cmd->dbuf_p->sz = resp_report_density(lu_priv, media, cmd->dbuf_p);

	return SAM_STAT_GOOD;
}

uint8_t ssc_reserve(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;

	lu_priv = cmd->lu->lu_private;

	MHVTL_DBG(1, "RESERVE (%ld) **", (long)cmd->dbuf_p->serialNo);
	if (!SPR_Reservation_Type && !SPR_Reservation_Key)
		lu_priv->I_am_SPC_2_Reserved = 1;
	if (!SPR_Reservation_Type && SPR_Reservation_Key)
		return SAM_STAT_RESERVATION_CONFLICT;
	return SAM_STAT_GOOD;
}

uint8_t ssc_erase(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "ERASING (%ld) **", (long)cmd->dbuf_p->serialNo);

	set_current_state(MHVTL_STATE_ERASE);

	if (!lu_priv->pm->check_restrictions(cmd))
		return SAM_STAT_CHECK_CONDITION;

	if (c_pos->blk_number != 0) {
		MHVTL_LOG("Not at BOT.. Can't erase unless at BOT");
		sam_not_ready(E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (OK_to_write)
		format_tape(sam_stat);
	else {
		MHVTL_LOG("Attempt to erase Write-protected media");
		sam_not_ready(E_MEDIUM_OVERWRITE_ATTEMPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	return SAM_STAT_GOOD;
}

uint8_t ssc_space_6(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat;
	uint32_t count;
	int32_t icount;
	uint8_t code;
	struct s_sd sd;

	sam_stat = &cmd->dbuf_p->sam_stat;

	*sam_stat = SAM_STAT_GOOD;

	set_current_state(MHVTL_STATE_POSITIONING);

	count = get_unaligned_be24(&cmd->scb[2]);
	code = cmd->scb[1] & 0x07;

	/* 'count' is only a 24-bit value.  If the top bit is set, it
	   should be treated as a twos-complement negative number.
	*/

	if (cmd->scb[2] >= 0x80) /* MSB of the count field */
		icount = -(0xffffff - count + 1);
	else
		icount = (int32_t)count;

	switch (code) {
	case 0:	/* Logical blocks - supported */
		MHVTL_DBG(1, "SPACE (%ld) ** %s %d block%s",
			(long)cmd->dbuf_p->serialNo,
			(icount >= 0) ? "forward" : "back",
			abs(icount),
			(1 == abs(icount)) ? "" : "s");
		break;
	case 1:	/* Filemarks - supported */
		MHVTL_DBG(1, "SPACE (%ld) ** %s %d filemark%s",
			(long)cmd->dbuf_p->serialNo,
			(icount >= 0) ? "forward" : "back",
			abs(icount),
			(1 == abs(icount)) ? "" : "s");
		break;
	case 3:	/* End of Data - supported */
		MHVTL_DBG(1, "SPACE (%ld) ** %s ",
			(long)cmd->dbuf_p->serialNo,
			"to End-of-data");
		break;
	case 2:	/* Sequential filemarks currently not supported */
	default: /* obsolete / reserved values */
		MHVTL_DBG(1, "SPACE (%ld) ** - Unsupported option %d",
			(long)cmd->dbuf_p->serialNo,
			code);

		sd.byte0 = SKSV | CD | BPV | code;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_PARMS, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	if (icount != 0 || code == 3)
		resp_space(icount, code, sam_stat);

	return *sam_stat;
}

uint8_t ssc_space_16(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat;
	int64_t icount;
	uint8_t code;
	struct s_sd sd;

	sam_stat = &cmd->dbuf_p->sam_stat;

	*sam_stat = SAM_STAT_GOOD;

	set_current_state(MHVTL_STATE_POSITIONING);

	icount = get_unaligned_be64(&cmd->scb[4]);
	code = cmd->scb[1] & 0x0f;

	switch (code) {
	case 0:	/* Logical blocks - supported */
		MHVTL_DBG(1, "SPACE (%ld) ** %s %d block%s",
			(long)cmd->dbuf_p->serialNo,
			(icount >= 0) ? "forward" : "back",
			abs(icount),
			(1 == abs(icount)) ? "" : "s");
		break;
	case 1:	/* Filemarks - supported */
		MHVTL_DBG(1, "SPACE (%ld) ** %s %d filemark%s",
			(long)cmd->dbuf_p->serialNo,
			(icount >= 0) ? "forward" : "back",
			abs(icount),
			(1 == abs(icount)) ? "" : "s");
		break;
	case 3:	/* End of Data - supported */
		MHVTL_DBG(1, "SPACE (%ld) ** %s ",
			(long)cmd->dbuf_p->serialNo,
			"to End-of-data");
		break;
	case 2:	/* Sequential filemarks currently not supported */
	default: /* obsolete / reserved values */
		MHVTL_DBG(1, "SPACE (%ld) ** - Unsupported option %d",
			(long)cmd->dbuf_p->serialNo,
			code);

		sd.byte0 = SKSV | CD | BPV | code;
		sd.field_pointer = 1;
		sam_illegal_request(E_INVALID_FIELD_IN_PARMS, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	if (icount != 0 || code == 3)
		resp_space(icount, code, sam_stat);

	return *sam_stat;
}

uint8_t ssc_load_unload(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	struct s_sd sd;
	int load_request;
	int media_state;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	load_request = cmd->scb[4] & 0x01;

	set_current_state((load_request) ? MHVTL_STATE_LOADING : MHVTL_STATE_UNLOADING);

	if (cmd->scb[4] & 0x04) { /* EOT bit */
		MHVTL_ERR("EOT bit set on load. Not supported");
		sd.byte0 = SKSV | CD | BPV | 4;
		sd.field_pointer = 4;
		sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	MHVTL_DBG(1, "%s TAPE (%ld) **", (load_request) ? "LOADING" : "UNLOADING",
						(long)cmd->dbuf_p->serialNo);

	media_state = rewind_tape(sam_stat);
	switch (get_tape_load_status()) {
	case TAPE_UNLOADED:
		if (load_request) {
			int load_state;

			/*
			 * media_state = 0 - Load OK -> Nothing to do
			 * media_state = 1 - Already loaded -> Nothing to do
			 */
			switch(media_state) {
			case 0:
				/*
				 * lu_priv->barcode indicates there is a tape in mouth
				 * media not mounted, and receive a mount request - attempt
				 * to load media
				 */
				if (!lu_priv->barcode) {
					sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
					return SAM_STAT_CHECK_CONDITION;
				}
				load_state = loadTape(lu_priv->barcode, sam_stat);
				if (load_state == 2) {
					sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
					return SAM_STAT_CHECK_CONDITION;
				} else if (load_state == 3) {
					sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
					return SAM_STAT_CHECK_CONDITION;
				}
				break;
			case 1:
				break;
			default:
				sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
				return SAM_STAT_CHECK_CONDITION;
				break;
			}
		} else {
			sam_not_ready(E_MEDIUM_NOT_PRESENT, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		set_lp11_medium_present(1);
		break;

	case TAPE_LOADED:
		if (!load_request)
			/* Send library an update status 'true' */
			unloadTape(TRUE, sam_stat);
		set_lp11_medium_present(1);
		break;

	default:
		sam_not_ready(E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}
	return SAM_STAT_GOOD;
}

uint8_t ssc_write_filemarks(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	int count;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	count = get_unaligned_be24(&cmd->scb[2]);

	MHVTL_DBG(1, "WRITE %d FILEMARKS (%ld) **", count,
						(long)cmd->dbuf_p->serialNo);
	if (!lu_priv->pm->check_restrictions(cmd)) {
		/* If restrictions & WORM media at block 0.. OK
		 * Otherwise return CHECK_CONDITION.
		 *	check_restrictions()
		 *	was nice enough to set correct sense status for us.
		 */
		if ((mam.MediumType == MEDIA_TYPE_WORM) &&
					(c_pos->blk_number == 0)) {
			MHVTL_DBG(1, "Erasing WORM media");
		} else
			return SAM_STAT_CHECK_CONDITION;
	}

	write_filemarks(count, sam_stat);
	if (count) {
		if (current_tape_offset() >=
				get_unaligned_be64(&mam.max_capacity)) {
			mam.remaining_capacity = 0L;
			MHVTL_DBG(2, "Setting EOM flag");
			sam_no_sense(SD_EOM, NO_ADDITIONAL_SENSE, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	return SAM_STAT_GOOD;
}

uint8_t ssc_pr_in(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;

	lu_priv = cmd->lu->lu_private;

	MHVTL_DBG(1, "PERSISTENT RESERVE IN (%ld) **",
						(long)cmd->dbuf_p->serialNo);
	if (lu_priv->I_am_SPC_2_Reserved)
		return SAM_STAT_RESERVATION_CONFLICT;
	else
		return resp_spc_pri(cmd->scb, cmd->dbuf_p);
}

static void update_tape_usage(struct TapeUsage *b,
				struct priv_lu_ssc *lu_ssc)
{
	uint64_t datasets = filemark_count(-1);
	uint64_t load_count;

	/* if we have more than 1 filemark,
	 * most apps write 2 filemarks to flag EOD
	 * So, lets subtract one from the filemark count to
	 * present a more accurate 'Data Set' count
	 */
	if (datasets > 1)
		datasets--;

	load_count = get_unaligned_be64(&lu_ssc->mamp->LoadCount);
	put_unaligned_be32(load_count, &b->volumeMounts);

	put_unaligned_be64(datasets, &b->volumeDatasetsWritten);
}

static void update_seq_access_counters(struct seqAccessDevice *sa,
				struct priv_lu_ssc *lu_ssc)
{
	put_unaligned_be64(lu_ssc->bytesWritten_I,
				&sa->writeDataB4Compression);
	put_unaligned_be64(lu_ssc->bytesWritten_M,
				&sa->writeDataAfCompression);
	put_unaligned_be64(lu_ssc->bytesRead_M,
				&sa->readDataB4Compression);
	put_unaligned_be64(lu_ssc->bytesRead_I,
				&sa->readDataAfCompression);

	/* Values in MBytes */
	if (get_tape_load_status() == TAPE_LOADED) {
		put_unaligned_be32(lu_ssc->max_capacity >> 20,
					&sa->capacity_bop_eod);
		put_unaligned_be32(lu_ssc->early_warning_position >> 20,
					&sa->capacity_bop_ew);
		put_unaligned_be32(lu_ssc->early_warning_sz >> 20,
					&sa->capacity_ew_leop);
		put_unaligned_be32(current_tape_offset() >> 20,
					&sa->capacity_bop_curr);
	} else {
		put_unaligned_be32(0xffffffff, &sa->capacity_bop_eod);
		put_unaligned_be32(0xffffffff, &sa->capacity_bop_ew);
		put_unaligned_be32(0xffffffff, &sa->capacity_ew_leop);
		put_unaligned_be32(0xffffffff, &sa->capacity_bop_curr);
	}

}

uint8_t ssc_log_sense(struct scsi_cmd *cmd)
{
	struct lu_phy_attr *lu;
	struct priv_lu_ssc *lu_ssc;
	uint8_t *b = cmd->dbuf_p->data;
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat;
	int retval;
	int i;
	uint16_t alloc_len;
	struct list_head *l_head;
	struct log_pg_list *l;
	char msg[64];
	struct s_sd sd;

	sprintf(msg, "LOG SENSE (%ld) ** : ", (long)cmd->dbuf_p->serialNo);

	alloc_len = get_unaligned_be16(&cdb[7]);
	cmd->dbuf_p->sz = alloc_len;

	lu = cmd->lu;
	lu_ssc = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;
	l_head = &lu->log_pg;
	retval = 0;

	switch (cdb[2] & 0x3f) {
	case 0:	/* Send supported pages */
		MHVTL_DBG(1, "%s %s", msg, "Sending supported pages");
		memset(b, 0, 4);	/* Clear first few (4) bytes */
		i = 4;
		b[i++] = 0;	/* b[0] is log page '0' (this one) */
		list_for_each_entry(l, l_head, siblings) {
			MHVTL_DBG(3, "found page 0x%02x", l->log_page_num);
			b[i] = l->log_page_num;
			i++;
		}
		put_unaligned_be16(i - 4, &b[2]);
		retval = i;
		break;
	case WRITE_ERROR_COUNTER:	/* Write error page */
		MHVTL_DBG(1, "%s %s", msg, "Write error page");
		l = lookup_log_pg(l_head, WRITE_ERROR_COUNTER);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	case READ_ERROR_COUNTER:	/* Read error page */
		MHVTL_DBG(1, "%s %s", msg, "Read error page");
		l = lookup_log_pg(&lu->log_pg, READ_ERROR_COUNTER);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	case SEQUENTIAL_ACCESS_DEVICE:
		MHVTL_DBG(1, "%s %s", msg, "Sequential Access Device Log page");
		l = lookup_log_pg(&lu->log_pg, SEQUENTIAL_ACCESS_DEVICE);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		update_seq_access_counters((struct seqAccessDevice *)b, lu_ssc);
		retval = l->size;
		break;
	case TEMPERATURE_PAGE:	/* Temperature page */
		MHVTL_DBG(1, "%s %s", msg, "Temperature page");
		l = lookup_log_pg(&lu->log_pg, TEMPERATURE_PAGE);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	case DEVICE_STATUS:
		MHVTL_DBG(1, "%s %s", msg, "VHF Device Status page");
		l = lookup_log_pg(&lu->log_pg, DEVICE_STATUS);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	case TAPE_ALERT:	/* TapeAlert page */
		MHVTL_DBG(1, "%s %s", msg, "TapeAlert page");

		l = lookup_log_pg(&lu->log_pg, TAPE_ALERT);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;

		/* Clear flags after value read. */
		if (alloc_len > 4)
			set_TapeAlert(lu, TA_NONE);
		else
			MHVTL_DBG(1, "TapeAlert : Alloc len short -"
				" Not clearing TapeAlert flags.");
		break;
	case TAPE_USAGE:	/* Tape Usage Log */
		MHVTL_DBG(1, "%s %s", msg, "Tape Usage page");
		l = lookup_log_pg(&lu->log_pg, TAPE_USAGE);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		update_tape_usage((struct TapeUsage *)b, lu_ssc);
		retval = l->size;
		break;
	case TAPE_CAPACITY: {	/* Tape Capacity page */
		MHVTL_DBG(1, "%s %s", msg, "Tape Capacity page");
		struct TapeCapacity *tp;

		l = lookup_log_pg(&lu->log_pg, TAPE_CAPACITY);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;

		/* Point the data structure to return data */
		tp = (struct TapeCapacity *)b;

		if (get_tape_load_status() == TAPE_LOADED) {
			uint64_t cap;

			cap = get_unaligned_be64(&mam.remaining_capacity);
			cap /= lu_ssc->capacity_unit;
			put_unaligned_be32(cap, &tp->partition0remaining);

			cap = get_unaligned_be64(&mam.max_capacity);
			cap /= lu_ssc->capacity_unit;
			put_unaligned_be32(cap, &tp->partition0maximum);
		} else {
			tp->partition0remaining = 0;
			tp->partition0maximum = 0;
		}
		}
		break;
	case DATA_COMPRESSION:	/* Data Compression page */
		MHVTL_DBG(1, "%s %s", msg, "Data Compression page");
		l = lookup_log_pg(&lu->log_pg, DATA_COMPRESSION);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	default:
		MHVTL_DBG(1, "%s Unknown code: 0x%x", msg, cdb[2] & 0x3f);
		goto log_page_not_found;
		break;
	}
	cmd->dbuf_p->sz = retval;

	return SAM_STAT_GOOD;

log_page_not_found:
	cmd->dbuf_p->sz = 0;
	sd.byte0 = SKSV | CD;
	sd.field_pointer = 2;
	sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}

uint8_t ssc_recv_diagnostics(struct scsi_cmd *cmd)
{
	struct s_sd sd;
	uint8_t *sam_stat;

	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "SSC RECEIVE DIAGNOSTICS (%ld) **", (long)cmd->dbuf_p->serialNo);

	cmd->dbuf_p->sz = 0;
	sd.byte0 = SKSV | CD;
	sd.field_pointer = 2;
	sam_illegal_request(E_INVALID_FIELD_IN_CDB, &sd, sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}

uint32_t GenerateRSCRC(uint32_t seed, int sz, const uint8_t *buf);
uint32_t crc32c(uint32_t seed, const uint8_t *buf, size_t sz);

uint8_t ssc_send_diagnostics(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat;
	int crc_check_failed = 0;

	const uint8_t block1[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43,
				47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127,
				131, 137, 139, 149, 151, 157};

	const uint8_t block2[] = {163, 167, 173, 179, 181, 191, 193, 197, 199, 211,
				223, 227, 229, 233, 239, 241, 251};


	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "SSC SEND DIAGNOSTICS (%ld) **", (long)cmd->dbuf_p->serialNo);

	/* Sanity check - the MAM should be 1024 bytes in size */
	if (sizeof(struct MAM) != 1024) {
		MHVTL_ERR("Structure of MAM should be 1024 bytes, but is %ld", (long)sizeof(struct MAM));
		sam_hardware_error(E_INTERNAL_TARGET_FAILURE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	/* Now check the CRC32C routines pass basic sanity check */
	uint32_t computedCRC1 = crc32c(0, block1, sizeof(block1));
	uint32_t computedCRC2 = crc32c(~computedCRC1, block2, sizeof(block2));
	uint32_t computedCRC3 = crc32c(~crc32c(0, block1, sizeof(block1)), block2, sizeof(block2));

	if (computedCRC1 != 0xE8174F48) {
		MHVTL_ERR("CRC32C #1 error");
		crc_check_failed = 1;
	}
	if (computedCRC2 != 0x56DAB0A6) {
		MHVTL_ERR("CRC32C #2 error");
		crc_check_failed = 1;
	}
	if (computedCRC3 != 0x56DAB0A6) {
		MHVTL_ERR("CRC32C #3 error");
		crc_check_failed = 1;
	}

	/* Now check the Reed/Solomon CRC routines pass basic sanity check */
	computedCRC1 = GenerateRSCRC(0, sizeof(block1), block1);
	computedCRC2 = GenerateRSCRC(computedCRC1, sizeof(block2), block2);
	computedCRC3 = GenerateRSCRC(GenerateRSCRC(0, sizeof(block1), block1), sizeof(block2), block2);

	if (computedCRC1 != 0x733D4DCA) {
		MHVTL_ERR("RS-CRC #1 error");
		crc_check_failed = 1;
	}
	if (computedCRC2 != 0x754ED37E) {
		MHVTL_ERR("RS-CRC #2 error");
		crc_check_failed = 1;
	}
	if (computedCRC3 != 0x754ED37E) {
		MHVTL_ERR("RS-CRC #3 error");
		crc_check_failed = 1;
	}

	if (crc_check_failed) {
		sam_hardware_error(E_INTERNAL_TARGET_FAILURE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	return SAM_STAT_GOOD;
}
