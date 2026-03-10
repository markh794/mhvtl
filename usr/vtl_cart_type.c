/*
 * Version 2 of tape format.
 *
 * Each media contains 3 files.
 *  - The .data file contains each block of data written to the media
 *  - The .indx file consists of an array of one raw_header structure per
 *    written tape block or filemark.
 *  - The .meta file consists of a MAM structure followed by a meta_header
 *    structure, followed by a variable-length array of filemark block numbers.
 *
 * Copyright (C) 2009 - 2010 Kevan Rehm
 *
 * This is a vtllibrary 'helper'. This contains a function to open the media
 * metadata file and return the media type (Data, WORM, Cleaning etc)
 * Required to fill in details of READ ELEMENT STATUS - instead of just guessing
 * based on barcode, open the actual media metadata and read 'from the horses
 * mouth'

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

#define _FILE_OFFSET_BITS 64

#define __STDC_FORMAT_MACROS /* for PRId64 */

/* for unistd.h pread/pwrite and fcntl.h posix_fadvise */
#define _XOPEN_SOURCE 600

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "logging.h"
#include "vtllib.h"
#include "vtlcart.h"

static char currentPCL[HOME_DIR_PATH_SZ + MAX_BARCODE_LEN + 3]; /* make room for home_dir plus some */

/*
 * Attempt to open PCL metadata and read cart type
 *
 * Returns valid SMC media type values (READ ELEMENT STATUS):
 * == 0 -> open failed or unknown cart type
 * == 1 -> Data Cart type
 * == 2 -> Cleaning
 * == 3 -> Diagnostics
 * == 4 -> WORM
 *
 * == 5 -> NULL media type (SMC specifies values > 4 as 'reserved')
 */

void update_home_dir(long lib_id) {
	if (strlen(home_directory) < 2) {
		find_media_home_directory(NULL, lib_id);
		MHVTL_DBG(3, "Setting home dir to %s", home_directory);
	}
}

int get_cart_type(const char *barcode) {
	char	   pcl[MAX_BARCODE_LEN + 1];
	char	   path[HOME_DIR_PATH_SZ + MAX_BARCODE_LEN + 4 + 10];
	int		   rc	   = 0;
	int		   mamfile = -1;
	struct MAM tmp_mam;

	/* init tmp_mam */
	init_mam(&tmp_mam);

	/* copy barcode to &pcl and terminate at either ' ' or '\0' */
	for (int i = 0; i < MAX_BARCODE_LEN; i++) {
		pcl[i] = barcode[i];
		if (pcl[i] == ' ' || pcl[i] == '\0') {
			pcl[i] = '\0';
			break;
		}
	}

	snprintf(currentPCL, ARRAY_SIZE(currentPCL), "%s/%s",
			 strlen(home_directory) ? home_directory : MHVTL_HOME_PATH,
			 pcl);

	/* Open mam file */
	snprintf(path, ARRAY_SIZE(path), "%s/mam", currentPCL);
	mamfile = open(path, O_RDWR | O_LARGEFILE);
	if (mamfile == -1) {
		MHVTL_ERR("open of file %s failed: %s", path, strerror(errno));
		rc = 0;
		goto failed;
	}

	/* Read in the MAM */
	read_mam(mamfile, -1, &tmp_mam);

	switch (tmp_mam.MediumType) {
	case MEDIA_TYPE_NULL:
		rc = 5; /* Reserved */
		break;
	case MEDIA_TYPE_DATA:
		rc = 1;
		break;
	case MEDIA_TYPE_CLEAN:
		rc = 2;
		break;
	case MEDIA_TYPE_DIAGNOSTIC:
		rc = 3;
		break;
	case MEDIA_TYPE_WORM:
		rc = 4;
		break;
	default:
		rc = 0;
		break;
	}

failed:
	if (mamfile >= 0)
		close(mamfile);

	MHVTL_DBG(3, "Opening media: %s (barcode %s), returning type %d",
			  currentPCL, barcode, rc);
	return rc;
}
