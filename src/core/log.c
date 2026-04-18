/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — log.c
 *
 * Minimum log module driven by log_test.c. Deliberately lean: no
 * timestamps, no rate-limiting, no file rotation. Cycle 28 added the
 * `[LEVEL] ` prefix so a human reading the daemon's stderr can pick
 * out genuine errors from routine INFO traffic (like forwarded child
 * stderr lines).
 */

#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

static wtd_log_level_t s_level = WTD_LOG_INFO;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

void wtd_log_set_level(wtd_log_level_t level) {
	s_level = level;
}

wtd_log_level_t wtd_log_get_level(void) {
	return s_level;
}

static const char *level_prefix(wtd_log_level_t level) {
	switch (level) {
		case WTD_LOG_ERROR: return "[ERROR] ";
		case WTD_LOG_WARN:  return "[WARN] ";
		case WTD_LOG_INFO:  return "[INFO] ";
		case WTD_LOG_TRACE: return "[TRACE] ";
		case WTD_LOG_QUIET: /* never emitted, but be explicit */
		default:            return "";
	}
}

void wtd_log(wtd_log_level_t level, const char *fmt, ...) {
	if (level > s_level) {
		return;
	}
	pthread_mutex_lock(&s_mutex);
	(void)fputs(level_prefix(level), stderr);
	va_list ap;
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
	pthread_mutex_unlock(&s_mutex);
}
