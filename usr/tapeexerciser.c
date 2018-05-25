/*
 * Put a tape device thru a set of exercises
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mtio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

static int space_forward_filemark(int fd, int count)
{
	struct mtop mtop;
	int err;

	printf("%s(%d)", __func__, count);
	mtop.mt_op = MTFSF;	/* Forward Space over count Filemarks */
	mtop.mt_count = count;
	err = ioctl(fd, MTIOCTOP, &mtop);
	if (err)
		printf(" %s", strerror(errno));
	printf("\n");
	return err;
}

static int space_back_filemark(int fd, int count)
{
	struct mtop mtop;
	int err;

	printf("%s(%d)", __func__, count);
	mtop.mt_op = MTBSF;	/* Back Space over count Filemarks */
	mtop.mt_count = count;
	err = ioctl(fd, MTIOCTOP, &mtop);
	if (err)
		printf(" %s", strerror(errno));
	printf("\n");
	return err;
}

static int space_forward_block(int fd, int count)
{
	struct mtop mtop;
	int err;

	printf("%s(%d)", __func__, count);
	mtop.mt_op = MTFSR;	/* Forward count block */
	mtop.mt_count = count;
	err = ioctl(fd, MTIOCTOP, &mtop);
	if (err)
		printf(" %s", strerror(errno));
	printf("\n");
	return err;
}

static int write_filemarks(int fd, int count)
{
	struct mtop mtop;
	int err;

	printf("%s(%d)", __func__, count);
	mtop.mt_op = MTWEOF;
	mtop.mt_count = count;
	err = ioctl(fd, MTIOCTOP, &mtop);
	if (err)
		printf(" %s", strerror(errno));
	printf("\n");
	return err;
}

static int space_back_block(int fd, int count)
{
	struct mtop mtop;
	int err;

	printf("%s(%d)", __func__, count);
	mtop.mt_op = MTBSR;	/* Back count block */
	mtop.mt_count = 1;
	err = ioctl(fd, MTIOCTOP, &mtop);
	if (err)
		printf(" %s", strerror(errno));
	printf("\n");
	return err;
}

static int set_compression(int fd, int state)
{
	struct mtop mtop;
	int err;

	printf("%s()", __func__);
	mtop.mt_op = MTCOMPRESSION; /* Set compression */
	mtop.mt_count = state;
	err = ioctl(fd, MTIOCTOP, &mtop);
	if (err)
		printf(" %s", strerror(errno));
	printf("\n");
	return err;
}

static int rewind_tape(int fd)
{
	struct mtop mtop;
	int err;

	printf("%s()", __func__);
	mtop.mt_op = MTREW;
	mtop.mt_count = 1;
	err = ioctl(fd, MTIOCTOP, &mtop);
	if (err)
		printf(" %s", strerror(errno));
	printf("\n");
	return err;
}

static int read_block_position(int fd)
{
	struct mtpos mtpos;
	int err;

	err = ioctl(fd, MTIOCPOS, &mtpos);
	if (err) {
		printf("%s: %s\n", __func__, strerror(errno));
		return -1;
	}

	return mtpos.mt_blkno;
}

static int read_block(int fd, int size)
{
	char *buf;
	int err;

	buf = malloc(size);
	if (!buf)
		return -ENOMEM;

	printf("%s(%d)", __func__, size);
	err = read(fd, buf, size);
	if (err < 0)
		printf(" %s", strerror(errno));
	printf("\n");
	free(buf);
	return err;
}

static int write_block(int fd, int size)
{
	char *buf;
	int err;

	buf = malloc(size);
	if (!buf)
		return -ENOMEM;

	memset(buf, 0x45, size);

	printf("%s(%d)", __func__, size);
	err = write(fd, buf, size);
	if (err < 0)
		printf(" %s", strerror(errno));
	printf("\n");
	free(buf);
	return err;
}

static void usage(char *arg)
{
	printf("Usage: %s -f tape_path\n", arg);
	printf("       WARNING: %s will overwrite the tape\n\n", arg);
}

void write_tape_pattern_1(int fd)
{
	rewind_tape(fd);

	write_block(fd, 8 * 1024);
	write_block(fd, 64 * 1024);
	write_filemarks(fd, 1);
	write_block(fd, 64 * 1024);
	write_block(fd, 64 * 1024);
	write_block(fd, 64 * 1024);
	write_block(fd, 64 * 1024);
	write_block(fd, 64 * 1024);
	write_filemarks(fd, 0);	/* Flush data */
	write_filemarks(fd, 1);
}

void write_tape_pattern_2(int fd)
{
	rewind_tape(fd);

	space_forward_filemark(fd, 1);
	space_forward_block(fd, 5); /* should be at end filemark */

	write_block(fd, 128 * 1024);
	write_block(fd, 128 * 1024);
	write_block(fd, 128 * 1024);
	write_filemarks(fd, 1);
	write_block(fd, 128 * 1024);
	write_filemarks(fd, 1);
	write_block(fd, 128 * 1024);
	write_filemarks(fd, 2);
}

int read_test_1(int fd)
{
	rewind_tape(fd);
	read_block(fd, 8 * 1024);
	printf("%s: Block %d\n", __func__, read_block_position(fd));
	space_forward_filemark(fd, 3);
	printf("%s: Block %d\n", __func__, read_block_position(fd));
	read_block(fd, 64 * 1024);
	printf("%s: Block %d\n", __func__, read_block_position(fd));
	space_back_block(fd, 1);
	printf("%s: Block %d\n", __func__, read_block_position(fd));
	read_block(fd, 32 * 1024);
	printf("%s: Block %d\n", __func__, read_block_position(fd));
	return 0;
}

int read_test_2(int fd)
{
	rewind_tape(fd);
	space_forward_filemark(fd, 3);
	read_block(fd, 64 * 1024);
	space_forward_block(fd, 1);
	read_block(fd, 64 * 1024);
	printf("%s: Did we error here ??\n", __func__);

	rewind_tape(fd);
	printf("%s: Block %d\n", __func__, read_block_position(fd));
	printf("%s: Note: following space_forward_filemark(100000)"
		" should error\n", __func__);
	space_forward_filemark(fd, 100000);
	printf("%s: Block %d\n", __func__, read_block_position(fd));
	space_back_filemark(fd, 2);
	printf("%s: Block %d\n", __func__, read_block_position(fd));
	space_back_block(fd, 1);
	printf("%s: Block %d\n", __func__, read_block_position(fd));
	read_block(fd, 64 * 1024);
	printf("%s: Block %d\n", __func__, read_block_position(fd));
	return 0;
}

int read_test_3(int fd)
{
	rewind_tape(fd);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	read_block(fd, 8 * 1024);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	space_forward_filemark(fd, 3);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	read_block(fd, 64 * 1024);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	space_back_block(fd, 1);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	read_block(fd, 64 * 1024);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	printf("%s: Note: following space_forward_block() should error\n",
				__func__);
	space_forward_block(fd, 1);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	return 0;
}

int read_test_4(int fd)
{
	rewind_tape(fd);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	space_forward_filemark(fd, 1);
	printf("*** Should be at block 3 ==> \t");
	printf("%s: Block %d\n", __func__ , read_block_position(fd));

	rewind_tape(fd);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	read_block(fd, 8 * 1024);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	space_forward_filemark(fd, 1);
	printf("*** Should be at block 3 ==> \t");
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	read_block(fd, 64 * 1024);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	space_back_block(fd, 1);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	read_block(fd, 64 * 1024);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	space_forward_block(fd, 1);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	space_forward_filemark(fd, 1);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	return 0;
}

int read_test_5(int fd)
{
	rewind_tape(fd);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	space_forward_filemark(fd, 1);
	printf("*** Should be at block 3 ==> \t");
	printf("%s: Block %d\n", __func__ , read_block_position(fd));

	rewind_tape(fd);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	read_block(fd, 8 * 1024);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));
	space_forward_filemark(fd, 1);
	printf("*** Should be at block 3 ==> \t");
	printf("%s: Block %d\n", __func__ , read_block_position(fd));

	printf("Only reading 32k of a 64k block\n");
	read_block(fd, 32 * 1024);
	printf("%s: Block %d\n", __func__ , read_block_position(fd));

	return 0;
}

int main(int argc, char *argv[])
{
	struct mtget mtstat;
	struct mtpos mtpos;
	char *dev = NULL;
	int tape_fd;
	int err;
	int count;

	if (argc != 3) {
		usage(argv[0]);
		exit(1);
	}

	/* checking several positions of -h/-help */
	for (count = 1; count < argc; count++) {
		if (!strcmp(argv[count], "-f")) {
			if (argc < count + 1) {
				usage(argv[0]);
				exit(1);
			}
			dev = argv[count + 1];
		}
	}
	if (!dev) {
		usage(argv[0]);
		exit(1);
	}

	memset(&mtstat, 0, sizeof(struct mtget));
	memset(&mtpos, 0, sizeof(struct mtpos));

	tape_fd = open(dev, O_RDWR);
	if (tape_fd < 0) {
		printf("Failed to open %s: %s\n",
				dev, strerror(errno));
		exit(-1);
	}

	/* Disable Compression */
	set_compression(tape_fd, 0);

	write_tape_pattern_1(tape_fd);
	write_tape_pattern_2(tape_fd);

	read_test_1(tape_fd);
	read_test_2(tape_fd);
	read_test_3(tape_fd);
	read_test_4(tape_fd);
	read_test_5(tape_fd);

	err = ioctl(tape_fd, MTIOCGET, &mtstat); /* Query device status */
	if (err) {
		printf("%s: %s\n", __func__, strerror(errno));
		exit(-1);
	}
	printf("  =======================\n");
	printf("    Querying type drive\n");
	printf("  =======================\n");
	printf("Status from tape device: type: %s\n",
		(mtstat.mt_type == MT_ISSCSI1) ? "SCSI-1" : "SCSI-2");
	printf("Position: fileno: %d, blockno: %d\n",
		mtstat.mt_fileno, mtstat.mt_blkno);
	printf("Block size: %d, density: 0x%02x\n",
		(unsigned int)mtstat.mt_dsreg & MT_ST_BLKSIZE_MASK,
		(unsigned int)mtstat.mt_dsreg >> MT_ST_DENSITY_SHIFT);
	printf("Drive is %sline\n", GMT_ONLINE(mtstat.mt_gstat) ? "On" : "Off");
	printf("Drive is EOF: %s\n", GMT_EOF(mtstat.mt_gstat) ? "Yes" : "No");
	printf("Drive is BOT: %s\n", GMT_BOT(mtstat.mt_gstat) ? "Yes" : "No");
	printf("Drive is EOT: %s\n", GMT_EOT(mtstat.mt_gstat) ? "Yes" : "No");
	printf("Drive is EOD: %s\n", GMT_EOD(mtstat.mt_gstat) ? "Yes" : "No");
	printf("Drive is SM: %s\n", GMT_SM(mtstat.mt_gstat) ? "Yes" : "No");
	printf("Drive is WRITE PROTECT: %s\n",
				GMT_WR_PROT(mtstat.mt_gstat) ? "Yes" : "No");
	printf("Tape is %sloaded in the drive\n",
				GMT_DR_OPEN(mtstat.mt_gstat) ? "not " : "");

	printf("  =======================\n\n");

	close(tape_fd);
	exit(0);
}

