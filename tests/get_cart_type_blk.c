/*
 * Helper for tests/test_raw_loopback.sh: checks that get_cart_type() reads the
 * media type of a raw cartridge through the symlink <home>/<barcode> -> block
 * device, the layout vtllibrary sees. Needs a real block device, so it is run
 * by the loopback script rather than by "make test".
 *
 * Usage: get_cart_type_blk <home> <barcode> <block device> <expected type>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vtllib.h"
#include "smc.h"

/* libvtlscsi expects each executable to supply this symbol. */
char mhvtl_driver_name[] = "get_cart_type_blk";

int main(int argc, char **argv)
{
	char link[HOME_DIR_PATH_SZ + MAX_BARCODE_LEN + 4];
	int got;

	if (argc != 5) {
		fprintf(stderr, "usage: %s home barcode blockdev expected\n", argv[0]);
		return 2;
	}
	snprintf(home_directory, sizeof(home_directory), "%s", argv[1]);
	snprintf(link, sizeof(link), "%s/%s", argv[1], argv[2]);
	if (symlink(argv[3], link) < 0) {
		perror("symlink");
		return 2;
	}
	got = get_cart_type(argv[2]);
	unlink(link);
	printf("get_cart_type(%s) = %d, want %s\n", argv[2], got, argv[4]);
	return got == atoi(argv[4]) ? 0 : 1;
}
