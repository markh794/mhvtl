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
 * Required to fill in details of READ ELEMENT STATUS - instead of just gussing
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

#define __STDC_FORMAT_MACROS	/* for PRId64 */

/* for unistd.h pread/pwrite and fcntl.h posix_fadvise */
#define _XOPEN_SOURCE 600

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logging.h"
#include "mhvtl_list.h"
#include "vtltape.h"

/* The .meta file consists of a MAM structure followed by a meta_header
   structure, followed by a variable-length array of filemark block numbers.
   Both the MAM and meta_header structures also contain padding to allow
   for future expansion with backwards compatibility.
*/

struct	meta_header {
	uint32_t filemark_count;
	char pad[512 - sizeof(uint32_t)];
};

static char currentPCL[HOME_DIR_PATH_SZ * 2];	/* make room for home_dir plus some */
static struct meta_header meta;

static char home_directory[HOME_DIR_PATH_SZ + 1];

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

void update_home_dir(long lib_id)
{
	if (strlen(home_directory) < 2) {
		find_media_home_directory(NULL, home_directory, lib_id);
		MHVTL_DBG(3, "Setting home dir to %s", home_directory);
	}
}

int get_cart_type(const char *barcode)
{
	char *pcl_meta = NULL;
	char pcl[MAX_BARCODE_LEN + 1];
	struct stat meta_stat;
	uint64_t exp_size;
	loff_t nread;
	int rc = 0;
	int i;
	int metafile;
	struct MAM tmp_mam;

	/* copy barcode to &pcl and terminate at either ' ' or '\0' */
	for (i = 0; i < MAX_BARCODE_LEN; i++) {
		pcl[i] = barcode[i];
		if (pcl[i] == ' ' || pcl[i] == '\0') {
			pcl[i] = '\0';
			break;
		}
	}

	if (strlen(home_directory))
		snprintf(currentPCL, ARRAY_SIZE(currentPCL), "%s/%s",
						home_directory, pcl);
	else
		snprintf(currentPCL, ARRAY_SIZE(currentPCL), "%s/%s",
						MHVTL_HOME_PATH, pcl);

	if (asprintf(&pcl_meta, "%s/meta", currentPCL) < 0) {
		perror("Could not allocate memory");
		exit(1);
	}

	if (stat(pcl_meta, &meta_stat) == -1) {
		MHVTL_DBG(2, "Couldn't find %s, trying previous default: %s/%s",
				pcl_meta, MHVTL_HOME_PATH, pcl);
		snprintf(currentPCL, ARRAY_SIZE(currentPCL), "%s/%s",
						MHVTL_HOME_PATH, pcl);
		free(pcl_meta);
		if (asprintf(&pcl_meta, "%s/meta", currentPCL) < 0) {
			perror("Could not allocate memory");
			exit(1);
		}
	}

	metafile = open(pcl_meta, O_RDONLY);
	if (metafile == -1) {
		MHVTL_ERR("open of pcl %s file %s failed, %s", pcl,
			pcl_meta, strerror(errno));
		rc = 0;
		goto failed;
	}

	if (fstat(metafile, &meta_stat) < 0) {
		MHVTL_ERR("stat of pcl %s file %s failed: %s", pcl,
			pcl_meta, strerror(errno));
		rc = 0;
		goto failed;
	}

	/* Verify that the metafile size is at least reasonable. */

	exp_size = sizeof(tmp_mam) + sizeof(meta);
	if ((uint32_t)meta_stat.st_size < exp_size) {
		MHVTL_ERR("pcl %s file %s is not the correct length, "
			"expected at least %" PRId64 ", actual %" PRId64,
			pcl, pcl_meta, exp_size, meta_stat.st_size);
		rc = 0;
		goto failed;
	}

	/* Read in the MAM and sanity-check it. */
	nread = read(metafile, &tmp_mam, sizeof(tmp_mam));
	if (nread < 0) {
		MHVTL_ERR("Error reading pcl %s MAM from metafile: %s",
			pcl, strerror(errno));
		rc = 0;
		goto failed;
	} else if (nread != sizeof(tmp_mam)) {
		MHVTL_ERR("Error reading pcl %s MAM from metafile: "
			"unexpected read length", pcl);
		rc = 0;
		goto failed;
	}

	switch (tmp_mam.MediumType) {
	case MEDIA_TYPE_NULL:
		rc = 5;	/* Reserved */
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
	close(metafile);
	MHVTL_DBG(3, "Opening media: %s (barcode %s), returning type %d",
			currentPCL, barcode, rc);
	if (pcl_meta)
		free(pcl_meta);
	return rc;
}
