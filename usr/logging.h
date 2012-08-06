/*
 * logging macros
 *
 * Copyright (C) 2005-2012 Mark Harvey markh794@gmail.com
 *                                  mark_harvey@symantec.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _LOGGING_H_
#define _LOGGING_H_

#include <syslog.h>

#define MHVTL_OPT_NOISE 3

#ifdef MHVTL_DEBUG
extern char vtl_driver_name[];
extern int debug;
extern int verbose;

#define MHVTL_DBG_NO_FUNC(lvl, format, arg...) {		\
	if (debug)						\
		printf("%s: " format "\n",			\
			vtl_driver_name, ## arg);		\
	else if ((verbose & MHVTL_OPT_NOISE) >= (lvl))		\
		syslog(LOG_DAEMON|LOG_INFO, format, ## arg);	\
}

#define MHVTL_ERR(format, arg...) {				\
	if (debug) {						\
		printf("%s: ERROR: %s(): " format "\n",		\
			vtl_driver_name, __func__, ## arg);	\
		fflush(NULL);					\
	} else {						\
		syslog(LOG_DAEMON|LOG_ERR, "ERROR: %s(): " format,	\
			__func__, ## arg);			\
	}							\
}

#define MHVTL_LOG(format, arg...) {				\
	if (debug) {						\
		printf("%s: %s(): " format "\n",		\
			vtl_driver_name, __func__, ## arg);	\
		fflush(NULL);					\
	} else {						\
		syslog(LOG_DAEMON|LOG_ERR, "%s(): " format,	\
			__func__, ## arg);			\
	}							\
}

#define MHVTL_DBG(lvl, format, arg...) {			\
	if (debug)						\
		printf("%s: %s(): " format "\n",		\
			vtl_driver_name, __func__, ## arg);	\
	else if ((verbose & MHVTL_OPT_NOISE) >= (lvl))		\
		syslog(LOG_DAEMON|LOG_INFO, "%s(): " format,	\
			__func__, ## arg);			\
}

#define MHVTL_DBG_PRT_CDB(lvl, cmd) {				\
	if (debug) {						\
		mhvtl_prt_cdb((lvl), (cmd));			\
	} else if ((verbose & MHVTL_OPT_NOISE) >= (lvl)) {	\
		mhvtl_prt_cdb((lvl), (cmd));			\
	}							\
}

#else

#define MHVTL_DBG(lvl, s...)
#define MHVTL_DBG_NO_FUNC(lvl, s...)
#define MHVTL_DBG_PRT_CDB(lvl, cmd)

#define MHVTL_ERR(format, arg...) {			\
	syslog(LOG_DAEMON|LOG_ERR, "ERROR: %s(): " format,	\
		__func__, ## arg);			\
}

#define MHVTL_LOG(format, arg...) {			\
	syslog(LOG_DAEMON|LOG_ERR, "%s(): " format,	\
		__func__, ## arg);			\
}

#endif	/* MHVTL_DEBUG */
#endif /*  _LOGGING_H_ */
