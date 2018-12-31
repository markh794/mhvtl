/*
 * mhvtl-device-conf-generator -- systemd generator for mhvtl
 *
 * This program read device.conf and creates the appropriate systemd
 * unit files and dependencies
 * 
 * NOTE: This program assumes the existance of systemd template
 * files for vtllibrary and vtltape
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

#define		MAX_LINE_WIDTH		1024

static int	debug_mode = 0;
static char	*device_conf = "/etc/mhvtl/device.conf";

struct vtl_info {
	int		num;
	struct vtl_info	*next;
};

static struct vtl_info our_tapes = {.num = -1, .next = NULL} ;
static struct vtl_info our_libraries = {.num = -1, .next = NULL} ;

static struct vtl_info *last_tape = &our_tapes;
static struct vtl_info *last_library = &our_libraries;

/*
 * return 1 if path is a writable directory, else return 0
 */
static int is_writable_dir(char *path)
{
	struct stat	sb;
	int		res = 0;	/* default to "no" */

	if (lstat(path, &sb) < 0)
		goto dun;
	if (!S_ISDIR(sb.st_mode))
		goto dun;
	res = (access(path, W_OK) == 0);
dun:
	if (debug_mode)
		(void) printf("DEBUG: %s is%s writable\n", path,
				res ? "" : " not");
	return res;
}

/*
 * scan arguments and return the "normal_dir" if all goes well,
 * else return NULL
 */
static char *get_working_dir(int argc, char **argv)
{
	char		*normal_dir;

	/* skip program name */
	argc--; argc++;

	if ((argc > 0) && !strcmp(argv[1], "-d")) {
		debug_mode++;
		argc--; argv++;
	}

	if (argc != 4) {
		if (debug_mode)
			(void) fprintf(stderr, "DEBUG: error: expected 3 arguments, got %d\n", argc);
		return NULL;
	}

	if (debug_mode)
		(void) printf("DEBUG: normal=%s, early=%s, late=%s\n",
				argv[1], argv[2], argv[3]);
	normal_dir = argv[1];

	/* we care about normal dir, so make sure it's real */
	if (!is_writable_dir(normal_dir)) {
		if (debug_mode)
			(void) fprintf(stderr, "DEBUG: error: not a writable dir: %s\n", normal_dir);
		return NULL;
	}

	return normal_dir;
}

/*
 * add to the end of the linked list of libraries
 *
 * return bool success (0 -> fail, else success)
 */
static int add_to_libraries(int lib_num)
{
	struct vtl_info	*info;

	if (debug_mode)
		(void) printf("DEBUG: add_to_libraries(%d): entering\n", lib_num);

	if (lib_num <= 0)
		return 0;

	info = malloc(sizeof(struct vtl_info));
	if (info == NULL) {
		if (debug_mode)
			(void) fprintf(stderr, "DEBUG: error: no memory\n");
		return 0;
	}

	info->num = lib_num;
	info->next = NULL;

	last_library->next = info;
	last_library = info;

	return 1;
}

/*
 * add to the end of the linked list of drives for a library
 *
 * return bool success (0 -> fail, else success)
 */
static int add_to_tapes(int tape_num)
{
	struct vtl_info	*info;

	if (debug_mode)
		(void) printf("DEBUG: add_to_tapes(%d): entering\n", tape_num);

	if (tape_num <= 0)
		return 0;

	info = malloc(sizeof(struct vtl_info));
	if (info == NULL) {
		if (debug_mode)
			(void) fprintf(stderr, "DEBUG: error: no memory\n");
		return 0;
	}

	info->num = tape_num;
	info->next = NULL;

	last_tape->next = info;
	last_tape = info;

	return 1;
}

/*
 * parse the device.conf configuration file into our_tape_library
 * 
 * This file has "Library" entries, and it has "Drive" entries. The
 * default configuration file has two library entries, and each
 * of those is followed by a number of drive entriess that go with
 * that library. But do NOT assume the file will be sorted in
 * this order. But DO assume that a library will not be referenced
 * before it is mentioned by a drive that uses it
 */
static void parse_config_file(char *path)
{
	FILE			*fp = NULL;
	char			line_buf[MAX_LINE_WIDTH+1];
	char			*cp = NULL;
	int			tape_num = -1;
	int			library_num = -1;

	if (debug_mode)
		(void) fprintf(stderr, "DEBUG: parsing config file: %s\n",
				path);
	if ((fp = fopen(path, "r")) == NULL) {
		if (debug_mode)
			(void) fprintf(stderr, "DEBUG: error: can't open: %s\n",
					path);
	}

	while ((cp = fgets(line_buf, MAX_LINE_WIDTH, fp)) != NULL) {
		if (*cp == '\n')
			continue;	// blank line
		if (*cp == '#')
			continue;	// comment line
		if (*cp == ' ')
			continue;	// the middle of some record
		if (strncmp(cp, "Library: ", 9) == 0) {
			/* found a Library entry */
			library_num = strtol(cp+9, NULL, 0);
			if (!add_to_libraries(library_num))
				goto dun;
		} else if (strncmp(cp, "Drive: ", 7) == 0) {
			/* found a Drive entry */
			tape_num = strtol(cp+7, NULL, 0);
			if (!add_to_tapes(tape_num))
				goto dun;
		}
	}

dun:
	(void) fclose(fp);
}

int main(int argc, char **argv)
{
	char		*working_dir;
	struct vtl_info	*ip;
	char		*dirs_to_create[] = {
				"multi-user.target.wants",
				"mhvtl.target.wants",
				NULL
			};
	char		**dirnamep;
	char		*path;


	working_dir = get_working_dir(argc, argv);
	if (!working_dir)
		exit(1);

	/*
	 * parse the config file /etc/mhvtl/device.conf 
	 *
	 * for each library found:
	 * 	- set up vtllibrary unit
	 * 	- for each tape that uses that library
	 * 		- set up vtltape for that tape unit
	 */

	/* put config in our_tape_library */
	parse_config_file(device_conf);

	if (debug_mode) {
		(void) printf("DEBUG: Libraries:\n");
		for (ip = our_libraries.next; ip != NULL; ip = ip->next)
			(void) printf("DEBUG:  %d\n", ip->num);

		(void) printf("DEBUG: Tapes:\n");
		for (ip = our_tapes.next; ip != NULL; ip = ip->next)
			(void) printf("DEBUG:  %d\n", ip->num);
	}

	for (dirnamep = dirs_to_create; *dirnamep != NULL; dirnamep++) {
		if (asprintf(&path, "%s/%s", working_dir, *dirnamep) < 0) {
			perror("Could not allocate memory (for directory path)");
			exit(1);
		}
		if (debug_mode)
			printf("DEBUG: creating dir: %s\n", path);
		if (mkdir(path, 0755) < 0) {
			if (debug_mode)
				(void) fprintf(stderr, "DEBUG: error: can't mkdir: %s\n",
						path);
			// clean up?
			exit(1);
		}
		free(path);

		if (debug_mode)
			printf("DEBUG: scanning libraries ...\n");
		for (ip = our_libraries.next; ip != NULL; ip = ip->next) {
			const char *to_path = "/usr/lib/systemd/system/vtllibrary@.service";
			if (asprintf(&path, "%s/%s/vtllibrary@%d.service", working_dir, *dirnamep, ip->num) < 0) {
				perror("Could not allocate memory (for vtllibrary template symlink)");
				exit(1);
			}
			if (debug_mode)
				(void) fprintf(stderr, "DEBUG: creating symlink: %s => %s\n", path, to_path);
			if (symlink(to_path, path) < 0) {
				if (debug_mode)
					(void) fprintf(stderr, "DEBUG: error: can't create symlink (%d): %s => %s\n",
							errno, path, to_path);
				// clean up?
				exit(1);
			}
			free(path);
		}
		if (debug_mode)
			printf("DEBUG: scanning tapes ...\n");
		for (ip = our_tapes.next; ip != NULL; ip = ip->next) {
			const char *to_path = "/usr/lib/systemd/system/vtltape@.service";
			if (asprintf(&path, "%s/%s/vtltape@%d.service", working_dir, *dirnamep, ip->num) < 0) {
				perror("Could not allocate memory (for vtltape template symlink)");
				exit(1);
			}
			if (debug_mode)
				(void) fprintf(stderr, "DEBUG: creating symlink: %s => %s\n", path, to_path);
			if (symlink(to_path, path) < 0) {
				if (debug_mode)
					(void) fprintf(stderr, "DEBUG: error: can't create symlink (%d): %s => %s\n",
							errno, path, to_path);
				// clean up?
				exit(1);
			}
			free(path);
		}

	}

	exit(0);
}
