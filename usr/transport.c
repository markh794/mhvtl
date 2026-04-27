/*
 * Transport backend selection
 *
 * Holds the global vtl_transport pointer and the selection logic.
 * Backend is chosen by:
 *   1. MHVTL_BACKEND env var ("mhvtl" or "tcmu")
 *   2. Compile-time default (mhvtl)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtl_common.h"
#include "logging.h"
#include "vtllib.h"
#include "transport.h"

struct vtl_transport *vtl_transport = NULL;

void transport_select(void)
{
	const char *env = getenv("MHVTL_BACKEND");

	if (!env)
		env = "mhvtl";

#ifdef MHVTL_TCMU_BACKEND
	if (strcmp(env, "tcmu") == 0) {
		vtl_transport = transport_tcmu_init();
		MHVTL_DBG(1, "Selected TCMU transport backend");
		return;
	}
#endif

#ifdef MHVTL_USERLAND_BACKEND
	if (strcmp(env, "userland") == 0) {
		vtl_transport = transport_userland_init();
		MHVTL_DBG(1, "Selected userland transport backend");
		return;
	}
#endif

	if (strcmp(env, "mhvtl") == 0 || !env[0]) {
		vtl_transport = transport_mhvtl_init();
		MHVTL_DBG(1, "Selected mhvtl kernel module transport backend");
		return;
	}

	fprintf(stderr, "error: Unknown MHVTL_BACKEND '%s'"
#ifndef MHVTL_TCMU_BACKEND
		" (tcmu support not compiled in, build with TCMU=1)"
#endif
		"\n", env);
	exit(1);
}
