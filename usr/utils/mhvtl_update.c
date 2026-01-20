/*
 * Functions to update the tape format and mam format to new versions
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
 */

#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "logging.h"
#include "vtlcart.h"
#include "mhvtl_update.h"
#include "vtllib.h"

struct MAM_tapeFmtV3 {
	uint32_t tape_fmt_version;
	uint32_t mam_fmt_version;

	uint64_t remaining_capacity;
	uint64_t max_capacity;
	uint64_t TapeAlert;
	uint64_t LoadCount;
	uint64_t MAMSpaceRemaining;
	uint8_t	 AssigningOrganization_1[8];
	uint8_t	 InitializationCount[2];
	uint8_t	 DevMakeSerialLastLoad[40];
	uint8_t	 DevMakeSerialLastLoad1[40];
	uint8_t	 DevMakeSerialLastLoad2[40];
	uint8_t	 DevMakeSerialLastLoad3[40];
	uint64_t WrittenInMediumLife;
	uint64_t ReadInMediumLife;
	uint64_t WrittenInLastLoad;
	uint64_t ReadInLastLoad;

	uint8_t	 MediumManufacturer[8];
	uint8_t	 MediumSerialNumber[32];
	uint32_t MediumLength;
	uint32_t MediumWidth;
	uint8_t	 AssigningOrganization_2[8];
	uint8_t	 MediumManufactureDate[12];
	uint8_t	 FormattedDensityCode;
	uint8_t	 MediumDensityCode;
	uint8_t	 MediumType; /* 0 -> Data, 1 -> WORM, 6 -> Clean */
	uint8_t	 MediaType;	 /* LTO1, LTO2, AIT etc (Media_Type_list) */
	uint64_t MAMCapacity;
	uint16_t MediumTypeInformation; /* If Clean, max mount */

	uint8_t ApplicationVendor[8];
	uint8_t ApplicationName[32];
	uint8_t ApplicationVersion[8];
	uint8_t UserMediumTextLabel[160];
	uint8_t DateTimeLastWritten[12];
	uint8_t LocalizationIdentifier;
	uint8_t Barcode[32];
	uint8_t OwningHostTextualName[80];
	uint8_t MediaPool[160];

	uint8_t	 record_dirty; /* 0 = Record clean, non-zero umount failed. */
	uint16_t Flags;

	struct uniq_media_info_tapeFmtV3 {
		uint32_t bits_per_mm;
		uint16_t tracks;
		char	 density_name[8];
		char	 description[32];
	} media_info;
	uint8_t max_partitions;
	uint8_t num_partitions;

	/* Pad to keep MAM to 1024 bytes */
	uint8_t pad[1024 - 878];
} __attribute__((packed));

/*
 * Assuming mam.tape_fmt_version == 3,
 * extract mam from "meta" and create separate "mam" file
 * Sets mam.tape_fmt_version to 4
 *
 * Returns:
 * == 0 -> Successfully extracted mam
 * == 1 -> Failed to extract mam from meta file
 * == 2 -> could not find meta file : format corrupt
 */
int try_extract_mam(char *currentPCL) {
	struct MAM_tapeFmtV3 mam_v3;
	char				 meta_path[1024];
	char				 mam_path[1024];
	char				 tmp_path[1024];
	int					 metafile = -1;
	int					 mamfile  = -1;
	int					 tmpfile  = -1;
	int					 rc		  = 1; /* default: failed to extract */

	snprintf(meta_path, sizeof(meta_path), "%s/meta", currentPCL);
	snprintf(mam_path, sizeof(mam_path), "%s/mam", currentPCL);
	snprintf(tmp_path, sizeof(tmp_path), "%s/meta.new", currentPCL);

	metafile = open(meta_path, O_RDWR | O_LARGEFILE);
	if (metafile < 0) {
		MHVTL_ERR("open of file %s failed: %s", meta_path, strerror(errno));
		return 2;
	}

	if (read(metafile, &mam_v3, sizeof(struct MAM_tapeFmtV3)) != sizeof(struct MAM_tapeFmtV3)) {
		MHVTL_ERR("Error reading pcl %s MAM from meta file: %s",
				  currentPCL, strerror(errno));
		goto cleanup;
	}

	/* Checking Tape Format Version */
	if (mam_v3.tape_fmt_version != 3) {
		MHVTL_ERR("Error : Tape Format Version : %d , expected 3.\
					\nCannot handle conversion of %s tape format to version 4",
				  mam_v3.tape_fmt_version, currentPCL);
		goto cleanup;
	}

	/* create mam file */
	mamfile = open(mam_path, O_CREAT | O_EXCL | O_WRONLY,
				   S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (mamfile < 0) {
		MHVTL_ERR("Failed to create file %s: %s", mam_path, strerror(errno));
		goto cleanup;
	}

	/* Update Tape Format Version and write mam in "mam" file */
	mam_v3.tape_fmt_version = 4;
	if (write(mamfile, &mam_v3, sizeof(struct MAM_tapeFmtV3)) != sizeof(struct MAM_tapeFmtV3)) {
		MHVTL_ERR("Failed to initialize file %s: %s", mam_path,
				  strerror(errno));
		goto cleanup;
	}

	/* Rewrite meta without mam : writing content to meta.tmp, then renaming to meta */
	{
		size_t remaining_meta;
		char   buf[4096];

		remaining_meta = lseek(metafile, 0, SEEK_END) - sizeof(struct MAM_tapeFmtV3);
		if (remaining_meta < 0) {
			MHVTL_ERR("Error : lseek failed on %s: %s", meta_path, strerror(errno));
			goto cleanup;
		}

		/* Positioning at the beginning of the meta data */
		if (lseek(metafile, sizeof(struct MAM_tapeFmtV3), SEEK_SET) < 0) {
			MHVTL_ERR("Error : lseek failed on %s: %s", meta_path, strerror(errno));
			goto cleanup;
		}

		tmpfile = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY,
					   S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
		if (tmpfile < 0) {
			MHVTL_ERR("Failed to create temp meta file %s: %s",
					  tmp_path, strerror(errno));
			goto cleanup;
		}

		while (remaining_meta > 0) {
			ssize_t nread = read(metafile, buf,
								 remaining_meta > sizeof(buf) ? sizeof(buf) : remaining_meta);
			if (nread <= 0) {
				MHVTL_ERR("Error reading meta file %s: %s",
						  meta_path, strerror(errno));
				goto cleanup;
			}
			if (write(tmpfile, buf, nread) != nread) {
				MHVTL_ERR("Error writing temp meta file %s: %s",
						  tmp_path, strerror(errno));
				goto cleanup;
			}
			remaining_meta -= nread;
		}

		if (fsync(tmpfile) < 0) {
			MHVTL_ERR("Error doing fsync of temp meta file %s: %s", tmp_path, strerror(errno));
			goto cleanup;
		}

		if (rename(tmp_path, meta_path) < 0) {
			MHVTL_ERR("rename %s -> %s failed: %s",
					  tmp_path, meta_path, strerror(errno));
			goto cleanup;
		}
	}

	rc = 0; /* success */

cleanup:
	if (mamfile >= 0) close(mamfile);
	if (metafile >= 0) close(metafile);
	if (tmpfile >= 0) close(tmpfile);

	if (rc != 0) {
		unlink(mam_path);
		unlink(tmp_path);
	}

	return rc;
}

/*
 * Assuming mam.mam_fmt_version == 3,
 * Update mam from mam_fmt_version 3 to 4
 * Splitting mam into mam/mhvtl_data : updating tape_fmt_version from version 4 to 5
 * turning mam into an auto-descriptive format
 *
 * Returns:
 * == 0 -> Successfully updated mam
 * == 1 -> Failed to update mam
 * == 2 -> could not find mam file : format corrupt
 */
int try_update_mam(char *currentPCL) {
	struct MAM_tapeFmtV3 mam_v3;
	char				 mam_path[1024];
	char				 mhvtl_data_path[1024];
	char				 tmp_path[1024];
	int					 mamfile   = -1;
	int					 mhvtlfile = -1;
	int					 tmpfile   = -1;
	ssize_t				 nread;
	int					 rc = 1;

	snprintf(mam_path, sizeof(mam_path), "%s/mam", currentPCL);
	snprintf(mhvtl_data_path, sizeof(mhvtl_data_path), "%s/mhvtl_data", currentPCL);
	snprintf(tmp_path, sizeof(tmp_path), "%s/mam.tmp", currentPCL);

	mamfile = open(mam_path, O_RDWR | O_LARGEFILE);
	if (mamfile < 0) {
		MHVTL_ERR("open of file %s failed: %s", mam_path, strerror(errno));
		return 2;
	}

	nread = read(mamfile, &mam_v3, sizeof(struct MAM_tapeFmtV3));
	if (nread != sizeof(struct MAM_tapeFmtV3)) {
		MHVTL_ERR("Error reading pcl %s MAM from mam file: %s",
				  currentPCL, strerror(errno));
		goto cleanup;
	}

	/* Checking MAM Format Version */
	if (mam_v3.mam_fmt_version != 3) {
		MHVTL_ERR("Error : MAM Format Version : %d , expected 3.\
					\nCannot handle conversion of %s MAM format to version 4",
				  mam_v3.mam_fmt_version, currentPCL);
		goto cleanup;
	}

	mam.mam_fmt_version	 = MAM_VERSION;
	mam.tape_fmt_version = 5;

	/* Copying attributes to new format */

	mam.remaining_capacity = mam_v3.remaining_capacity;
	mam.max_capacity	   = mam_v3.max_capacity;
	mam.TapeAlert		   = mam_v3.TapeAlert;
	mam.LoadCount		   = mam_v3.LoadCount;
	mam.MAMSpaceRemaining  = mam_v3.MAMSpaceRemaining;
	memcpy(mam.AssigningOrganization_1, mam_v3.AssigningOrganization_1, sizeof(mam.AssigningOrganization_1));
	memcpy(mam.InitializationCount, mam_v3.InitializationCount, sizeof(mam.InitializationCount));
	memcpy(mam.DevMakeSerialLastLoad, mam_v3.DevMakeSerialLastLoad, sizeof(mam.DevMakeSerialLastLoad));
	memcpy(mam.DevMakeSerialLastLoad1, mam_v3.DevMakeSerialLastLoad1, sizeof(mam.DevMakeSerialLastLoad1));
	memcpy(mam.DevMakeSerialLastLoad2, mam_v3.DevMakeSerialLastLoad2, sizeof(mam.DevMakeSerialLastLoad2));
	memcpy(mam.DevMakeSerialLastLoad3, mam_v3.DevMakeSerialLastLoad3, sizeof(mam.DevMakeSerialLastLoad3));
	mam.WrittenInMediumLife = mam_v3.WrittenInMediumLife;
	mam.ReadInMediumLife	= mam_v3.ReadInMediumLife;
	mam.WrittenInLastLoad	= mam_v3.WrittenInLastLoad;
	mam.ReadInLastLoad		= mam_v3.ReadInLastLoad;

	memcpy(mam.MediumManufacturer, mam_v3.MediumManufacturer, sizeof(mam.MediumManufacturer));
	memcpy(mam.MediumSerialNumber, mam_v3.MediumSerialNumber, sizeof(mam.MediumSerialNumber));
	mam.MediumLength = mam_v3.MediumLength;
	mam.MediumWidth	 = mam_v3.MediumWidth;
	memcpy(mam.AssigningOrganization_2, mam_v3.AssigningOrganization_2, sizeof(mam.AssigningOrganization_2));
	memcpy(mam.MediumManufactureDate, mam_v3.MediumManufactureDate, sizeof(mam.MediumManufactureDate));
	mam.FormattedDensityCode  = mam_v3.FormattedDensityCode;
	mam.MediumDensityCode	  = mam_v3.MediumDensityCode;
	mam.MediumType			  = mam_v3.MediumType;
	mam.MediaType			  = mam_v3.MediaType;
	mam.MAMCapacity			  = mam_v3.MAMCapacity;
	mam.MediumTypeInformation = mam_v3.MediumTypeInformation;

	memcpy(mam.ApplicationVendor, mam_v3.ApplicationVendor, sizeof(mam.ApplicationVendor));
	memcpy(mam.ApplicationName, mam_v3.ApplicationName, sizeof(mam.ApplicationName));
	memcpy(mam.ApplicationVersion, mam_v3.ApplicationVersion, sizeof(mam.ApplicationVersion));
	memcpy(mam.UserMediumTextLabel, mam_v3.UserMediumTextLabel, sizeof(mam.UserMediumTextLabel));
	memcpy(mam.DateTimeLastWritten, mam_v3.DateTimeLastWritten, sizeof(mam.DateTimeLastWritten));
	mam.LocalizationIdentifier = mam_v3.LocalizationIdentifier;
	memcpy(mam.Barcode, mam_v3.Barcode, sizeof(mam.Barcode));
	memcpy(mam.OwningHostTextualName, mam_v3.OwningHostTextualName, sizeof(mam.OwningHostTextualName));
	memcpy(mam.MediaPool, mam_v3.MediaPool, sizeof(mam.MediaPool));

	mam.record_dirty = mam_v3.record_dirty;
	mam.Flags		 = mam_v3.Flags;

	mam.media_info.bits_per_mm = mam_v3.media_info.bits_per_mm;
	mam.media_info.tracks	   = mam_v3.media_info.tracks;
	memcpy(mam.media_info.density_name, mam_v3.media_info.density_name, sizeof(mam.media_info.density_name));
	memcpy(mam.media_info.description, mam_v3.media_info.description, sizeof(mam.media_info.description));

	mam.max_partitions = mam_v3.max_partitions;
	mam.num_partitions = mam_v3.num_partitions;

	/* create mhvtl_data file */
	mhvtlfile = open(mhvtl_data_path, O_CREAT | O_EXCL | O_WRONLY,
					 S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (mhvtlfile < 0) {
		MHVTL_ERR("Failed to create mhvtl_data file %s: %s", mhvtl_data_path, strerror(errno));
		goto cleanup;
	}

	/* Updating mam :
	 * - writing content to mam.tmp, then renaming to mam
	 * - filling mhvtl_data */
	{
		tmpfile = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY,
					   S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
		if (tmpfile < 0) {
			MHVTL_ERR("Failed to create temp mam file %s: %s",
					  tmp_path, strerror(errno));
			goto cleanup;
		}

		write_mam(tmpfile, mhvtlfile);

		if (fsync(tmpfile) < 0) {
			MHVTL_ERR("Error doing fsync of temp mhvtl_data file %s: %s", mhvtl_data_path, strerror(errno));
			goto cleanup;
		}

		if (fsync(tmpfile) < 0) {
			MHVTL_ERR("Error doing fsync of temp mam file %s: %s", tmp_path, strerror(errno));
			goto cleanup;
		}

		if (rename(tmp_path, mam_path) < 0) {
			MHVTL_ERR("rename %s -> %s failed: %s",
					  tmp_path, mam_path, strerror(errno));
			goto cleanup;
		}
	}

	rc = 0; /* success */

cleanup:
	if (mamfile >= 0) close(mamfile);
	if (mhvtlfile >= 0) close(mhvtlfile);
	if (tmpfile >= 0) close(tmpfile);

	if (rc != 0) {
		unlink(tmp_path);
		unlink(mhvtl_data_path);
	}

	return rc;
}
