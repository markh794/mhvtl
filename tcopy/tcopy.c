
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/io.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/mtio.h>

/* Buffer size larger than any block we are expecting to read in one chunk */
#define BUF_SZ (1024 * 600)	/* 600K seems like a reasonable value */

char cBuf[BUF_SZ];

/*
 * Take error num and provide a more user friendly text error msg
 */
int iErr(int id)
{
	switch (id) {
	case EIO:
		fprintf(stderr, " The requested operation could not be completed.\n");
		goto fail;
		break;
	case ENOSPC:
		fprintf(stderr, " A write operation could not be completed because the tape reached end-of-medium.\n");
		goto fail;
		break;
	case EACCES:
		fprintf(stderr, " An attempt was made to write or erase a write-protected tape.\n");
		goto fail;
		break;
	case EFAULT:
		fprintf(stderr, " The command parameters point to memory not belonging to the calling process.\n");
		goto fail;
		break;
	case ENXIO:
		fprintf(stderr, "During opening, the tape device does not exist.\n");
		goto fail;
		break;
	case EBUSY:
		fprintf(stderr, "The device is already in use or the driver was unable to allocate a buffer.\n");
		goto fail;
		break;
	case EOVERFLOW:
		fprintf(stderr, "An attempt was made to read or write a variable-length block that is larger than the driver's internal buffer.\n");
		goto fail;
		break;
	case EINVAL:
		fprintf(stderr, "An ioctl had an illegal argument, or a requested block size was illegal.\n");
		goto fail;
		break;
	case ENOSYS:
		fprintf(stderr, "Unknown ioctl\n");
		break;
	case EROFS:
		fprintf(stderr, "Open is attempted with O_WRONLY or O_RDWR when the tape in the drive is write-protected.\n");
		goto fail;
		break;
	}
	return id;			/* set the return status */
fail:
	exit(-id);
}
int main(int argc, char **argv)
{
	int res, length;
	ssize_t idIn, idOut;
	int iFirst = 0, iLast = 0;
	int iOldByte = 0;
	int eof = 0, iFile = 1, iBlock = 0, iF = 0, iError = 0;
	struct mtop mt_cmd;

	mt_cmd.mt_op = MTWEOF;
	mt_cmd.mt_count = 1;

	fprintf(stderr, "*** Written by Odin Explorer programmer\n");

	if (argc == 1) {
		fprintf(stderr, "!!! Usage : %s input output\n", argv[0]);
		exit(1);
	}
	if (argc == 3) {
		idOut = open(argv[2], O_WRONLY);
		if (idOut == -1) {
			fprintf(stderr, "Can't open output file %s\n", argv[2]);
			exit(2);
		}
		/* Set variable block size on target tape drive */
		mt_cmd.mt_op    = MTSETBLK;
		mt_cmd.mt_count = 0;
		iErr(ioctl(idOut, MTIOCTOP, &mt_cmd));

		/* Set drive buffering per SCSI-2 on target tape drive */
		mt_cmd.mt_op    = MTSETDRVBUFFER;
		mt_cmd.mt_count = MT_ST_BOOLEANS | !MT_ST_ASYNC_WRITES;
		iErr(ioctl(idOut, MTIOCTOP, &mt_cmd));
	}
	idIn = open(argv[1], O_RDONLY);
	if (idIn == -1) {
		fprintf(stderr, "Can't open input file %s\n", argv[1]);
		exit(2);
	}

	/* Set variable block size of source tape drive */
	mt_cmd.mt_op    = MTSETBLK;
	mt_cmd.mt_count = 0;
	iErr(ioctl(idIn, MTIOCTOP, &mt_cmd));

	while (1) {
		if ((res = read(idIn, &cBuf, BUF_SZ)) == -1) {
			printf("Read Failed. %s (%d), Ret val: %d, Error count %d\n",
					strerror(errno), errno, res, iError);
			iError++;
			if (iError < 3) {
				sleep(1);
				continue;
			}
			exit(1);
		}
		iError = 0;
		if (!res) {
			if (eof) {
				break;
			} else {
				eof = 1;
				if (iFirst == iLast)
					fprintf(stderr, " tcopy: Tape File %d; Record %d; Size %d\n", iFile, iBlock, iOldByte);
				else
					fprintf(stderr, " tcopy: Tape File %d; Records: %d to %d; Size: %d\n", iFile, iFirst, iLast, iOldByte);
				fprintf(stderr, "************************************************************\n");
				iFile++;
				iBlock = 0;
				if (argc == 3) {
					/* Write 1 filemark to destination tape */
					mt_cmd.mt_op    = MTWEOF;
					mt_cmd.mt_count = 1;
					iErr(ioctl(idOut, MTIOCTOP, &mt_cmd));
				}
				continue;
			}
		}
		eof = 0;
		iBlock++;
		length = res;
		if (argc == 3) {
			res = write(idOut, &cBuf, length);
			if (res != length) {
				printf("Error writing to the file. res = %d length = %d\n", res, length);
				exit(1);
			}
		}

		if (iBlock == 1) {
			iOldByte = res;
			iFirst = 1;
			iLast = 1;
			continue;
		}
		if (res == iOldByte) {
			if (!iF) {
				iF = 1;
				iFirst = iBlock-1;
			}
			iLast = iBlock;
			continue;
		}
		if (iFirst == iLast)
			fprintf(stderr, " tcopy: Tape File %d; Record: %d; Size %d\n", iFile, iBlock-1, iOldByte);
		else {
			fprintf(stderr, " tcopy: Tape File %d; Records: %d to %d; Size: %d\n", iFile, iFirst, iLast, iOldByte);
			iFirst = iLast;
		}
		iOldByte = res;
		iF = 0;
	}
	fprintf(stderr, " tcopy: The end of tape is reached. Files on tape: %d\n", iFile-1);

	/* Eject source tape */
	mt_cmd.mt_op    = MTOFFL;
	mt_cmd.mt_count = 1;
	iErr(ioctl(idIn, MTIOCTOP, &mt_cmd));
	close(idIn);
	if (argc == 3) {
		/* Write 1 filemark to target */
		mt_cmd.mt_op    = MTWEOF;
		mt_cmd.mt_count = 1;
		iErr(ioctl(idOut, MTIOCTOP, &mt_cmd));

		/* eject target tape */
		mt_cmd.mt_op    = MTOFFL;
		mt_cmd.mt_count = 1;
		iErr(ioctl(idOut, MTIOCTOP, &mt_cmd));

		close(idOut);
	}
	exit(0);
}
