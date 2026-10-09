/*
 * Raw-disk bay map configuration parser. See include/raw_bay_conf.h.
 * Relies on _GNU_SOURCE (for getline) from the build flags, as the rest of usr/.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_bay_conf.h"

static const char BY_ID_PREFIX[] = "by-id=";

/* Trim leading and trailing ASCII whitespace in place; returns the start. */
static char *trim(char *s)
{
	char *end;

	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return s;
	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

static void set_err(char *errbuf, size_t errlen, int line, const char *why)
{
	if (errbuf && errlen)
		snprintf(errbuf, errlen, "mhvtl raw bay config line %d: %s",
			 line, why);
}

int raw_bay_conf_parse(const char *path, struct raw_bay **out, size_t *count,
		       char *errbuf, size_t errlen)
{
	FILE		*fp;
	char		*line = NULL;
	size_t		 cap = 0;
	ssize_t		 n;
	int		 lineno = 0;
	struct raw_bay	*bays = NULL;
	size_t		 nbays = 0, abays = 0;

	if (out)
		*out = NULL;
	if (count)
		*count = 0;
	if (!path || !out || !count) {
		set_err(errbuf, errlen, 0, "internal: null argument");
		return -1;
	}

	fp = fopen(path, "re");
	if (!fp) {
		if (errbuf && errlen)
			snprintf(errbuf, errlen, "cannot open %s", path);
		return -1;
	}

	while ((n = getline(&line, &cap, fp)) != -1) {
		char		 lib_s[64], slot_s[64], path_s[RAW_BAY_PATH_MAX];
		/* tok4 holds "by-id=" (6) + up to RAW_BAY_ID_MAX-1 value chars, so
		 * tok4+6 always fits by_id[RAW_BAY_ID_MAX] without truncation. */
		char		 tok4[RAW_BAY_ID_MAX + 6], tok5[16];
		char		*p, *endp;
		long		 library_id, map_slot;
		int		 fields;
		struct raw_bay	*b;

		lineno++;
		p = trim(line);
		if (*p == '\0' || *p == '#')
			continue;

		/* Required: library_id, map_slot, by-path. Optional: by-id token.
		 * A 5th token is unexpected trailing garbage. */
		fields = sscanf(p, "%63s %63s %1023s %261s %15s",
				lib_s, slot_s, path_s, tok4, tok5);
		if (fields < 3) {
			set_err(errbuf, errlen, lineno,
				"expected '<library_id> <map_slot> <by-path> [by-id=...]'");
			goto fail;
		}
		if (fields == 5) {
			set_err(errbuf, errlen, lineno, "unexpected extra field");
			goto fail;
		}

		library_id = strtol(lib_s, &endp, 10);
		if (*endp != '\0' || library_id < 0) {
			set_err(errbuf, errlen, lineno,
				"library_id must be a non-negative integer");
			goto fail;
		}
		map_slot = strtol(slot_s, &endp, 10);
		if (*endp != '\0' || map_slot < 1) {
			set_err(errbuf, errlen, lineno,
				"map_slot must be a positive integer");
			goto fail;
		}

		if (nbays == abays) {
			size_t		 na = abays ? abays * 2 : 8;
			struct raw_bay	*nb = realloc(bays, na * sizeof(*nb));

			if (!nb) {
				set_err(errbuf, errlen, lineno, "out of memory");
				goto fail;
			}
			bays = nb;
			abays = na;
		}
		b = &bays[nbays];
		memset(b, 0, sizeof(*b));
		b->library_id = (int)library_id;
		b->map_slot = (int)map_slot;
		snprintf(b->by_path, sizeof(b->by_path), "%s", path_s);

		if (fields == 4) {
			if (strncmp(tok4, BY_ID_PREFIX, strlen(BY_ID_PREFIX)) != 0) {
				set_err(errbuf, errlen, lineno,
					"unknown 4th field (expected by-id=...)");
				goto fail;
			}
			snprintf(b->by_id, sizeof(b->by_id), "%s",
				 tok4 + strlen(BY_ID_PREFIX));
		}
		nbays++;
	}

	free(line);
	fclose(fp);
	*out = bays;		/* NULL when the file had no data lines */
	*count = nbays;
	return 0;

fail:
	free(line);
	fclose(fp);
	free(bays);
	return -1;
}

void raw_bay_conf_free(struct raw_bay *bays)
{
	free(bays);
}
