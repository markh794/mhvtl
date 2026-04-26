/*
 * mhvtl kernel module transport backend
 *
 * Thin wrappers around the existing ioctl/chardev interface.
 * This is the default backend — no behavioral change from the
 * pre-transport-abstraction code.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "vtl_common.h"
#include "mhvtl_scsi.h"
#include "logging.h"
#include "vtllib.h"
#include "transport.h"

extern char mhvtl_driver_name[];

static int mhvtl_access(char *p, int len, char *entry)
{
	int fstat;
	struct stat km;
	char filename[256];

	snprintf(filename, sizeof(filename),
		 "/sys/bus/mhvtl/drivers/mhvtl/%s", entry);
	MHVTL_DBG(1, "Testing %s", filename);
	fstat = stat(filename, &km);
	if (fstat >= 0) {
		strncpy(p, filename, len);
		return 0;
	}
	snprintf(filename, sizeof(filename),
		 "/sys/bus/pseudo9/drivers/mhvtl/%s", entry);
	MHVTL_DBG(1, "Testing %s", filename);
	fstat = stat(filename, &km);
	if (fstat >= 0) {
		strncpy(p, filename, len);
		return 0;
	}
	snprintf(filename, sizeof(filename),
		 "/sys/bus/pseudo/drivers/mhvtl/%s", entry);
	MHVTL_DBG(1, "Testing %s", filename);
	fstat = stat(filename, &km);
	if (fstat >= 0) {
		strncpy(p, filename, len);
		return 0;
	}
	return -1;
}

static int chrdev_get_major(void)
{
	FILE *f;
	char filename[256];
	const char str[] = "Could not locate mhvtl kernel module";
	int rc = 0;
	int x;
	int majno;

	if (mhvtl_access(filename, sizeof(filename), "major") < 0) {
		MHVTL_ERR("%s: %s", mhvtl_driver_name, str);
		printf("%s: %s\n", mhvtl_driver_name, str);
		exit(EIO);
	}

	f = fopen(filename, "r");
	if (!f) {
		MHVTL_DBG(1, "Can't open %s: %s", filename, strerror(errno));
		return -ENOENT;
	}
	x = fscanf(f, "%d", &majno);
	if (!x) {
		MHVTL_DBG(1, "Cant identify major number for mhvtl");
		rc = -1;
	} else
		rc = majno;

	fclose(f);
	return rc;
}

static int chrdev_create(unsigned minor)
{
	int majno;
	int x;
	int ret = 0;
	dev_t dev;
	char pathname[64];

	snprintf(pathname, sizeof(pathname), "/dev/mhvtl%u", minor);

	majno = chrdev_get_major();
	if (majno == -ENOENT) {
		MHVTL_DBG(1, "** Incorrect version of kernel module loaded **");
		ret = -1;
		goto err;
	}

	dev = makedev(majno, minor);
	MHVTL_DBG(2, "Major number: %d, minor number: %u",
		   major(dev), minor(dev));
	MHVTL_DBG(3, "mknod(%s, %02o, major: %d minor: %d",
		   pathname, S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP,
		   major(dev), minor(dev));
	x = mknod(pathname, S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, dev);
	if (x < 0) {
		if (errno == EEXIST)
			return 0;

		MHVTL_DBG(1, "Error creating device node for mhvtl: %s",
			   strerror(errno));
		ret = -1;
	}

err:
	return ret;
}

static int chrdev_open(const char *name, unsigned minor)
{
	FILE *f;
	char devname[256];
	char buf[256];
	int devn;
	int ctlfd;

	f = fopen("/proc/devices", "r");
	if (!f) {
		printf("Cannot open control path to the driver: %s\n",
		       strerror(errno));
		return -1;
	}

	devn = 0;
	while (!feof(f)) {
		if (!fgets(buf, sizeof(buf), f))
			break;
		if (sscanf(buf, "%d %s", &devn, devname) != 2)
			continue;
		if (!strcmp(devname, name))
			break;
		devn = 0;
	}
	fclose(f);
	if (!devn) {
		printf("Cannot find %s in /proc/devices - "
		       "make sure the module is loaded\n",
		       name);
		return -1;
	}
	snprintf(devname, sizeof(devname), "/dev/%s%u", name, minor);
	ctlfd = open(devname, O_RDWR | O_NONBLOCK | O_EXCL);
	if (ctlfd < 0) {
		printf("Cannot open %s %s\n", devname, strerror(errno));
		fflush(NULL);
		printf("\n\n");
		return -1;
	}
	return ctlfd;
}

/*
 * open: Create device node and open the mhvtl char device.
 * Returns fd (>= 0) on success, -1 on error.
 */
static int mhvtl_open(unsigned minor, struct mhvtl_ctl *ctl)
{
	if (chrdev_create(minor)) {
		MHVTL_DBG(1, "Unable to create device node mhvtl%u", minor);
		return -1;
	}

	return chrdev_open("mhvtl", minor);
}

static int mhvtl_poll_cmd(int fd, struct mhvtl_header *hdr)
{
	return ioctl(fd, VTL_POLL_AND_GET_HEADER, hdr);
}

static int mhvtl_get_data(int fd, struct mhvtl_ds *ds)
{
	int err;

	MHVTL_DBG(3, "retrieving %d bytes from kernel", ds->sz);
	err = ioctl(fd, VTL_GET_DATA, ds);
	if (err < 0) {
		MHVTL_ERR("Failed retrieving data via ioctl(): %s",
			   strerror(errno));
		return 0;
	}
	return ds->sz;
}

static void mhvtl_put_data(int fd, struct mhvtl_ds *ds)
{
	uint8_t *s;

	ioctl(fd, VTL_PUT_DATA, ds);

	s = (uint8_t *)ds->sense_buf;

	if (ds->sam_stat == SAM_STAT_CHECK_CONDITION) {
		MHVTL_DBG(2, "s/n: (%ld), sz: %d, sam_status: %d"
			      " [%02x %02x %02x]",
			   (unsigned long)ds->serialNo,
			   ds->sz, ds->sam_stat,
			   s[2], s[12], s[13]);
	} else {
		MHVTL_DBG(2, "OP s/n: (%ld), sz: %d, sam_status: %d",
			   (unsigned long)ds->serialNo,
			   ds->sz, ds->sam_stat);
	}

	ds->sam_stat = 0;
}

/*
 * add_lu: Register a new logical unit with the kernel module.
 * Spawns a child process that writes to the kernel pseudo file.
 * Returns child pid on success, 0 on failure.
 */
static pid_t mhvtl_add_lu(unsigned minor, struct mhvtl_ctl *ctl)
{
	char str[1024];
	pid_t ppid, pid, mypid;
	ssize_t retval;
	FILE *pseudo;
	char pseudo_filename[256];
	char errmsg[512];

	sprintf(str, "add %u %d %d %d",
		minor, ctl->channel, ctl->id, ctl->lun);

	if (mhvtl_access(pseudo_filename, sizeof(pseudo_filename), "add_lu") < 0) {
		sprintf(str, "Could not find mhvtl kernel module");
		MHVTL_ERR("%s: %s", mhvtl_driver_name, str);
		printf("%s: %s\n", mhvtl_driver_name, str);
		exit(EIO);
	}

	ppid = getpid();

	switch (pid = fork()) {
	case 0: /* Child */
		mypid = getpid();
		pseudo = fopen(pseudo_filename, "w");
		if (!pseudo) {
			snprintf(errmsg, sizeof(errmsg),
				 "Could not open %s: %s", pseudo_filename, strerror(errno));
			MHVTL_ERR("Parent PID: %ld -> %s : %s", (long)ppid, errmsg, strerror(errno));
			perror("Could not open 'add_lu'");
			exit(-1);
		}

		retval = fprintf(pseudo, "%s\n", str);
		MHVTL_DBG(2, "Wrote '%s' (%d bytes) to %s",
			   str, (int)retval, pseudo_filename);

		fclose(pseudo);
		MHVTL_DBG(1, "Parent PID: [%ld] -> Child [%ld] anounces 'lu [%d:%d:%d] created'.",
			   (long)ppid, (long)mypid, ctl->channel, ctl->id, ctl->lun);
		exit(0);
		break;
	case -1:
		perror("Failed to fork()");
		MHVTL_ERR("Parent PID: %ld -> Fail to fork() %s", (long)ppid, strerror(errno));
		return 0;
		break;
	default: /* Parent */
		MHVTL_DBG(2, "[%ld] Child PID [%ld] will start logical unit [%d:%d:%d]",
			   (long)ppid, (long)pid, ctl->channel,
			   ctl->id, ctl->lun);

		return pid;
		break;
	}

	return 0;
}

static void mhvtl_remove_lu(int fd, struct mhvtl_ctl *ctl)
{
	ioctl(fd, VTL_REMOVE_LU, ctl);
}

static void mhvtl_close(int fd)
{
	close(fd);
}

static struct vtl_transport mhvtl_transport = {
	.name      = "mhvtl",
	.open      = mhvtl_open,
	.poll_cmd  = mhvtl_poll_cmd,
	.get_data  = mhvtl_get_data,
	.put_data  = mhvtl_put_data,
	.add_lu    = mhvtl_add_lu,
	.remove_lu = mhvtl_remove_lu,
	.close     = mhvtl_close,
};

struct vtl_transport *transport_mhvtl_init(void)
{
	return &mhvtl_transport;
}
