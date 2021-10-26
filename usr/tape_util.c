/*
 *	Dump headers of 'tape' datafile
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
 *                                mark.harvey at nutanix dot com
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <inttypes.h>
#include <zlib.h>
#include "minilzo.h"
#include "be_byteshift.h"
#include "mhvtl_scsi.h"
#include "mhvtl_list.h"
#include "vtl_common.h"
#include "vtllib.h"
#include "vtltape.h"
#include "q.h"
#include "ssc.h"
#include "ccan/crc32c/crc32c.h"

#define MEDIA_WRITABLE 0
#define MEDIA_READONLY 1

char mhvtl_driver_name[] = "tape_util";
int dump_tape = 0;	/* dual personality - dump_tape & preload_tape */
int verbose = 0;
int debug = 0;
long my_id = 0;
int lib_id;
struct priv_lu_ssc lu_ssc;
struct lu_phy_attr lunit;
struct encryption encryption;

extern char home_directory[HOME_DIR_PATH_SZ + 1];

static char *progname;

static void print_mam_info(void)
{
	uint64_t size;
	uint64_t remaining;
	char size_mul;		/* K/M/G/T/P multiplier */
	char remain_mul;	/* K/M/G/T/P multiplier */
	int a;
	static const char mul[] = " KMGT";

	size = get_unaligned_be64(&mam.max_capacity);
	remaining = get_unaligned_be64(&mam.remaining_capacity);

	size_mul = remain_mul = ' ';
	for (a = 0; a < 4; a++) {
		if (size > 5121) {
			size >>= 10;	/* divide by 1024 */
			size_mul = mul[a+1];
		}
	}
	for (a = 0; a < 4; a++) {
		if (remaining > 5121) {
			remaining >>= 10;	/* divide by 1024 */
			remain_mul = mul[a+1];
		}
	}

	printf("Media density code: 0x%02x\n", mam.MediumDensityCode);
	printf("Media type code   : 0x%02x\n", mam.MediaType);
	printf("Media description : %s\n", mam.media_info.description);
	printf("Tape Capacity     : %" PRId64 " (%" PRId64 " %cBytes)\n",
					get_unaligned_be64(&mam.max_capacity),
					size, size_mul);
	switch (mam.MediumType) {
	case MEDIA_TYPE_CLEAN:
		printf("Media type        : Cleaning media\n");
		break;
	case MEDIA_TYPE_DATA:
		printf("Media type        : Normal data\n");
		break;
	case MEDIA_TYPE_NULL:
		printf("Media type        : NULL - Use with caution !!\n");
		break;
	case MEDIA_TYPE_WORM:
		printf("Media type        : Write Once Read Many (WORM)\n");
		break;
	default:
		printf("Media type        : Unknown - please report !!\n");
		break;
	}

	printf("Media             : %s\n",
				(mam.Flags & MAM_FLAGS_MEDIA_WRITE_PROTECT) ?
					"Write-protected" : "read-write");
	printf("Remaining Tape Capacity : %" PRId64 " (%" PRId64 " %cBytes)\n",
				get_unaligned_be64(&mam.remaining_capacity),
				remaining, remain_mul);
}

static void init_lunit(struct lu_phy_attr *lu, struct priv_lu_ssc *priv_lu)
{
	if (debug)
		printf("Entering %s() +++\n", __func__);
	memset(lu, 0, sizeof(struct lu_phy_attr));

	lu->lu_private = priv_lu;
	lu->sense_p = sense;
	strncpy(lu->lu_serial_no, "ABC123", 7);
	strncpy(lu->vendor_id, "TAPE_UTIL", 10);
	strncpy(lu->product_id, "xyzz", 5);
	INIT_LIST_HEAD(&lu->den_list);
	INIT_LIST_HEAD(&lu->mode_pg);
	INIT_LIST_HEAD(&lu->log_pg);
}

static void init_lu_ssc(struct priv_lu_ssc *lu_priv)
{
	if (debug)
		printf("Entering %s() +++\n", __func__);
	memset(lu_priv, 0, sizeof(struct priv_lu_ssc));

	lu_priv->bufsize = 2 * 1024 * 1024;
	lu_priv->load_status = TAPE_UNLOADED;
	lu_priv->inLibrary = 0;
	lu_priv->sam_status = SAM_STAT_GOOD;
	lu_priv->MediaWriteProtect = MEDIA_WRITABLE;
	lu_priv->capacity_unit = 1;
	lu_priv->configCompressionFactor = Z_BEST_SPEED;
	lu_priv->bytesRead_I = 0;
	lu_priv->bytesRead_M = 0;
	lu_priv->bytesWritten_I = 0;
	lu_priv->bytesWritten_M = 0;
	lu_priv->c_pos = c_pos;
	lu_priv->KEY_INSTANCE_COUNTER = 0;
	lu_priv->DECRYPT_MODE = 0;
	lu_priv->ENCRYPT_MODE = 0;
	lu_priv->encr = &encryption;
	lu_priv->OK_2_write = &OK_to_write;
	lu_priv->mamp = &mam;
	INIT_LIST_HEAD(&lu_priv->supported_media_list);
	lu_priv->pm = NULL;
	lu_priv->state_msg = NULL;
	lu_priv->delay_load = 0;
	lu_priv->delay_unload = 0;
	lu_priv->delay_thread = 0;
	lu_priv->delay_position = 0;
	lu_priv->delay_rewind = 0;
}

/* Fix me - make sure it's not a WORM, cleaning cart etc */
uint8_t check_restrictions(struct scsi_cmd *cmd)
{
	if (debug)
		printf("Entering %s() +++\n", __func__);
	*lu_ssc.OK_2_write = 1;
	return 1;
}

uint8_t valid_encryption_blk(struct scsi_cmd *cmd)
{
	if (debug)
		printf("Entering %s() +++\n", __func__);
	return TRUE;
}

void register_ops(struct lu_phy_attr *lu, int op, void *f, void *g, void *h)
{
	if (debug)
		printf("Entering %s() +++\n", __func__);
}

void ssc_personality_module_register(struct ssc_personality_template *pm)
{
	if (debug)
		printf("Entering %s() +++\n", __func__);
	lu_ssc.pm = pm;
	if (debug)
		printf("Exiting %s() ++\n", __func__);
}

static struct media_details *check_media_can_load(struct list_head *mdl, int mt)
{
	struct media_details *m_detail;
	if (debug)
		printf("Entering %s() +++\n", __func__);

	if (debug)
		printf("Looking for media_type: 0x%02x\n", mt);

	list_for_each_entry(m_detail, mdl, siblings) {
		if (debug)
			printf("testing against m_detail->media_type (0x%02x)\n",
						m_detail->media_type);
		if (m_detail->media_type == (unsigned int)mt)
			return m_detail;
	}
	return NULL;
}

static int lookup_media_int(struct name_to_media_info *media_info, char *s)
{
	unsigned int i;
	if (debug)
		printf("Entering %s() +++\n", __func__);

	if (debug)
		printf("looking for media type %s\n", s);

	for (i = 0; media_info[i].media_density != 0; i++)
		if (!strcmp(media_info[i].name, s))
			return media_info[i].media_type;

	return Media_undefined;
}

int add_drive_media_list(struct lu_phy_attr *lu, int status, char *s)
{
	struct priv_lu_ssc *lu_tape;
	struct media_details *m_detail;
	struct list_head *den_list;
	int media_type;

	if (debug)
		printf("Entering %s() +++\n", __func__);
	lu_tape = (struct priv_lu_ssc *)lu->lu_private;
	den_list = &lu_tape->supported_media_list;

	if (debug)
		printf("Adding %s, status: 0x%02x\n", s, status);
	media_type = lookup_media_int(lu_tape->pm->media_handling, s);
	m_detail = check_media_can_load(den_list, media_type);

	if (m_detail) {
		if (debug)
			printf("Existing status for %s, Load Capability: 0x%02x\n",
					s, m_detail->load_capability);
		m_detail->load_capability |= status;
		if (debug)
			printf("Updating entry for %s, new Load Capability: 0x%02x\n",
					s, m_detail->load_capability);
	} else {
		if (debug)
			printf("Adding new entry for %s\n", s);
		m_detail = zalloc(sizeof(struct media_details));
		if (!m_detail) {
			printf("Failed to allocate %d bytes\n",
						(int)sizeof(m_detail));
			return -ENOMEM;
		}
		m_detail->media_type = media_type;
		m_detail->load_capability = status;
		list_add_tail(&m_detail->siblings, den_list);
	}

	return 0;
}

static void set_compression(struct priv_lu_ssc *lu_priv, char *compression)
{
	if (!strcasecmp(compression, "LZO")) {
		if (verbose)
			printf("Setting compression to LZO\n");
		lu_priv->compressionType = LZO;
	} else if (!strcasecmp(compression, "ZLIB")) {
		if (verbose)
			printf("Setting compression to ZLIB\n");
		lu_priv->compressionType = ZLIB;
	} else {
		if (verbose)
			printf("Setting compression to NONE\n");
		lu_priv->compressionType = 0;
	}
}

static int write_tape(char *source_file, uint32_t block_size, char *compression, uint8_t *sam_stat)
{
	uint8_t *b;
	int fd = -1;
	int rc = 0;
	int retval;
	size_t count = 1;
	struct scsi_cmd cmd;
	struct mhvtl_ds ds;
	void (*drive_init)(struct lu_phy_attr *) = init_default_ssc;

	if (debug)
		printf("Entering %s() +++\n", __func__);

	/* fake scsi_cmd & lu_private structures for write operations */
	memset(&cmd, 0, sizeof(cmd));
	memset(&ds, 0, sizeof(ds));

	cmd.lu = &lunit;
	cmd.dbuf_p = &ds;
	ds.sense_buf = sense;

	lu_ssc.max_capacity = get_unaligned_be64(&mam.max_capacity);

	drive_init(&lunit);

	init_default_ssc(&lunit);

	if (verbose) {
		printf("Opening %s for reading\n", source_file);
		printf("Block size: %d\n", block_size);
		printf("Compression : %s\n", compression);

		printf("Tape max capacity is %ld\n", lu_ssc.max_capacity);
	}

	b = malloc(block_size);
	if (!b)
		return -ENOMEM;
	if (debug)
		printf("malloc(%d) successfully\n", block_size);
	ds.data = b;
	ds.sz = block_size;

	set_compression(lunit.lu_private, compression);

	fd = open(source_file, O_LARGEFILE);
	if (fd < 0) {
		printf("Could not open file '%s', returned error: %s\n", source_file, strerror(errno));
		rc = -EBADF;
		goto abort;
	}
	while(count > 0) {
		count = read(fd, b, block_size);
		if (count > 0) {
			if (count < block_size) {
				printf("zeroing out remaining block: %ld\n", block_size - count);
				memset(b + count, 0, block_size - count);	/* Zero out remaining block */
			}
			retval = writeBlock(&cmd, count);
			if (retval < count) {
				printf("Tried to write %d, only succeeded in writing %d, SAM status: 0x%02x 0x%02x 0x%02x\n",
							block_size, retval, sense[2], sense[12], sense[13]);
				break;
			}
		}
	}
	write_filemarks(1, sam_stat);

abort:
	close(fd);
	free(b);
	return rc;
}

static int read_data(uint8_t *sam_stat)
{
	uint8_t *p;
	uint32_t ret;

	printf("c_pos->blk_size: %d\n", c_pos->blk_size);

	if (c_pos->blk_size <= 0) {
		printf("Data size: %d - skipping read\n", c_pos->blk_size);
		return 0;
	}
	p = malloc(c_pos->blk_size);
	if (!p) {
		fprintf(stderr, "Unable to allocate %d bytes\n", c_pos->blk_size);
		return -ENOMEM;
	}
	ret = readBlock(p, c_pos->blk_size, 1, sam_stat);
	if (ret != c_pos->blk_size) {
		printf("Requested %d bytes, received %d\n",
				c_pos->blk_size, ret);
	}
	free(p);
	puts("\n");
	return ret;
}

void find_media_home_directory(char *config_directory, char *home_directory, long lib_id);

static void usage(char *errmsg)
{
	if (errmsg)
		printf("%s\n", errmsg);
	printf("Usage: %s OPTIONS\n", progname);
	printf("Where OPTIONS are from:\n");
	printf("  -h               Print this message and exit\n");
	printf("  -d               Enable debugging\n");
	printf("  -v               Be verbose\n");
	if (dump_tape == 1) {
		printf("  -D               Dump data\n");
		printf("  -l lib_no        Look in specified library\n");
		printf("  -f pcl           Look for specified PCL\n");
	} else if (dump_tape == 2) {
		printf("  -l lib_no        Look in specified library\n");
		printf("  -f pcl           Look for specified PCL\n");
		printf("  -b <block size>  tape block size\n");
		printf("  -c <compression> Compression type (NONE|LZO|ZLIB)\n");
		printf("  -F <inputfile>   Filename to read data from\n");
	} else {
		printf("\n\nNot sure of my personality (dump_tape or preload_tape)\n");
	}
	exit(errmsg ? 1 : 0);
}

static void dump_tape_metadata(int dump_data, uint8_t *sam_stat)
{

	if (lzo_init() != LZO_E_OK) {
		fprintf(stderr, "internal error - lzo_init() failed !!!\n");
		fprintf(stderr,
			"(this usually indicates a compiler bug - try recompiling\nwithout optimizations, and enable '-DLZO_DEBUG' for diagnostics)\n");
		exit(3);
	}
	print_mam_info();

	print_filemark_count();
	if (verbose) {
		printf("Dumping filemark meta info\n");
		print_metadata();
	}

	while (c_pos->blk_type != B_EOD) {
		print_raw_header();
		if (dump_data) {
			read_data(sam_stat);
		}
		position_blocks_forw(1, sam_stat);
	}
	print_raw_header();
	unload_tape(sam_stat);
}

int main(int argc, char *argv[])
{
	uint8_t sam_stat;
	char *pcl = NULL;
	int rc;
	int libno = 0;
	int indx;
	int block_size = 0;
	int dump_data = FALSE;
	char *config = MHVTL_CONFIG_PATH"/device.conf";
	char *source_file = NULL;
	char *compression = NULL;
	FILE *conf;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */

	progname = argv[0];
	if (strcasestr(progname, "dump_tape")) {
		dump_tape = 1;
	} else if (strcasestr(progname, "preload_tape")) {
		dump_tape = 2;
	} else {
		usage("program name not correct");
	}

	if (argc < 2)
		usage("Not enough arguments");

	while (argc > 1) {
		if (argv[1][0] == '-') {
			switch (argv[1][1]) {
			case 'h':
				usage(NULL);
				break;
			case 'd':
				debug++;
				verbose = 9;	/* If debug, make verbose... */
				break;
			case 'f':
				if (argc > 1)
					pcl = argv[2];
				else
					usage("More args needed for -f");
				break;
			case 'l':
				if (argc > 1)
					libno = atoi(argv[1]);
				else
					usage("More args needed for -l");
				break;
			case 'D':
				dump_data = TRUE;
				break;
			case 'v':
				verbose++;
				break;
			case 'b':	/* block size */
				if (dump_tape == 2) {
					if (argc > 1) {
						block_size = atoi(argv[2]);
					} else {
						usage("-b requires additional block size");
					}
				} else {
					usage("-b is not a supported option");
				}
				break;
			case 'c':	/* compression type (NONE/LZO/ZLIB) */
				if (dump_tape == 2) {
					if (argc > 1) {
						compression = argv[2];
					} else {
						usage("-c requires additional compression type");
					}
				} else {
					usage("-c is not a supported option");
				}
				break;
			case 'F':	/* File to read from */
				if (dump_tape == 2) {
					if (argc > 1) {
						source_file = argv[2];
					} else {
						usage("-F requires additional filename");
					}
				} else {
					usage("-F is not a supported option");
				}
				break;
			default:
				usage("Unknown option");
				break;
			}
		}
		argv++;
		argc--;
	}

	if (pcl == NULL)
		usage("No PCL number supplied");
	if (dump_tape == 2) {
		if (source_file == NULL)
			usage("Need to specify the filename to read data from");
		if (compression == NULL)
			usage("Need to specify the compression type NONE|LZO|ZLIB");
		if (block_size == 0)
			usage("Need to specify a block size");
	}

#ifdef __x86_64__
	if (verbose) {
		if (__builtin_cpu_supports("sse4.2")) {
			printf("crc32c using Intel sse4.2 hardware optimization\n");
		} else {
			printf("crc32c not using Intel sse4.2 optimization\n");
		}
	}
#endif

	init_lu_ssc(&lu_ssc);
	init_lunit(&lunit, &lu_ssc);


	conf = fopen(config , "r");
	if (!conf) {
		fprintf(stderr, "Cannot open config file %s: %s\n", config,
					strerror(errno));
		exit(1);
	}
	s = malloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = malloc(MALLOC_SZ);
	if (!b) {
		perror("Could not allocate memory");
		exit(1);
	}

	rc = ENOENT;

	if (libno) {
		printf("Looking for PCL: %s in library %d\n", pcl, libno);
		find_media_home_directory(NULL, home_directory, libno);
		rc = load_tape(pcl, &sam_stat);
	} else { /* Walk thru all defined libraries looking for media */
		while (readline(b, MALLOC_SZ, conf) != NULL) {
			if (b[0] == '#')	/* Ignore comments */
				continue;
			/* If found a library: Attempt to load media
			 * Break out of loop if found. Otherwise try next lib.
			 */
			if (sscanf(b, "Library: %d CHANNEL:", &indx)) {
				find_media_home_directory(NULL, home_directory, indx);
				rc = load_tape(pcl, &sam_stat);
				if (!rc)
					break;
			}
		}
	}

	fclose(conf);
	free(s);
	free(b);

	if (rc) {
		fprintf(stderr, "PCL %s cannot be opened, "
				"load_tape() returned %d\n",
					pcl, rc);
		exit(1);
	}

	if (dump_tape == 1) {
		dump_tape_metadata(dump_data, &sam_stat);
	} else if (dump_tape == 2) {
		write_tape(source_file, block_size, compression, &sam_stat);
	}

	return 0;
}
