/*
 * Replay of LIO's saved APTPL persistent reservation state; see
 * include/tcmu_aptpl.h.
 *
 * core_scsi3_update_aptpl_buf() in drivers/target/target_core_pr.c builds
 * the file, one block per registration, and
 * __core_scsi3_write_aptpl_to_file() writes it with a trailing NUL:
 *
 *   PR_REG_START: 0
 *   initiator_fabric=iSCSI
 *   initiator_node=iqn.1993-08.org.debian:01:5c6e2e6c8ba6
 *   initiator_sid=0x23d000000         (only if given at registration)
 *   sa_res_key=1234
 *   res_holder=1
 *   res_type=08                       (only for the holder, as %02x)
 *   res_scope=00                      (only for the holder, as %02x)
 *   res_all_tg_pt=0
 *   mapped_lun=1
 *   target_fabric=iSCSI
 *   target_node=iqn.2003-01.org.linux-iscsi.host:mhvtl:lib10
 *   tpgt=1
 *   port_rtpi=1
 *   target_lun=1
 *   PR_REG_END: 0
 *
 * target_pr_res_aptpl_metadata_store() in target_core_configfs.c takes one
 * registration per write, the same keys separated by commas.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/target/target_core_pr.c#L1856-L1988
 * https://github.com/torvalds/linux/blob/v6.12/drivers/target/target_core_configfs.c#L2059
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "tcmu_aptpl.h"

/* Two 512-byte snprintf() blocks in core_scsi3_update_aptpl_buf(), plus commas */
#define APTPL_RECORD_MAX	2048

static int append(char *rec, size_t *len, const char *s, size_t n)
{
	if (*len + n + 2 > APTPL_RECORD_MAX)
		return -1;
	if (*len)
		rec[(*len)++] = ',';
	memcpy(rec + *len, s, n);
	*len += n;
	rec[*len] = '\0';
	return 0;
}

/* With fn NULL, only check the format and count the registrations */
static int parse(const char *buf, aptpl_record_fn fn, void *arg)
{
	char rec[APTPL_RECORD_MAX];
	size_t len = 0;
	int in_rec = 0, applied = 0;
	const char *line = buf, *end;

	if (!strncmp(buf, "No Registrations or Reservations", 32))
		return 0;

	for (; *line; line = *end ? end + 1 : end) {
		size_t n;

		end = strchr(line, '\n');
		if (!end)
			end = line + strlen(line);
		n = end - line;
		if (n && line[n - 1] == '\r')
			n--;
		if (!n)
			continue;

		if (!strncmp(line, "PR_REG_START:", 13)) {
			if (in_rec)
				return -1;
			in_rec = 1;
			len = 0;
			rec[0] = '\0';
		} else if (!strncmp(line, "PR_REG_END:", 11)) {
			if (!in_rec)
				return -1;
			in_rec = 0;
			if (!fn || fn(rec, arg) == 0)
				applied++;
		} else if (!in_rec || !memchr(line, '=', n)) {
			return -1;
		} else if ((n > 9 && !strncmp(line, "res_type=", 9)) ||
			   (n > 10 && !strncmp(line, "res_scope=", 10))) {
			/*
			 * Written as %02x, but match_token() matches %d with
			 * base 0: in "08" (Exclusive Access - All Registrants)
			 * the 8 is left over, res_type= is ignored, and the
			 * holder's type 0 makes the store return -EINVAL.
			 * Pass the value in decimal.
			 * https://github.com/torvalds/linux/blob/v6.12/lib/parser.c#L78
			 */
			const char *eq = memchr(line, '=', n);
			char hex[16], out[32];
			size_t hl = n - (eq + 1 - line);
			char *e;
			unsigned long v;

			if (!hl || hl >= sizeof(hex))
				return -1;
			memcpy(hex, eq + 1, hl);
			hex[hl] = '\0';
			v = strtoul(hex, &e, 16);
			if (*e || !isxdigit((unsigned char)hex[0]) || v > 0xff)
				return -1;
			snprintf(out, sizeof(out), "%.*s%lu",
				 (int)(eq + 1 - line), line, v);
			if (append(rec, &len, out, strlen(out)) < 0)
				return -1;
		} else if (append(rec, &len, line, n) < 0) {
			return -1;
		}
	}
	return in_rec ? -1 : applied;
}

int aptpl_replay(const char *buf, aptpl_record_fn fn, void *arg)
{
	/* Nothing is applied from a file that is malformed anywhere */
	if (parse(buf, NULL, NULL) < 0)
		return -1;
	return parse(buf, fn, arg);
}
