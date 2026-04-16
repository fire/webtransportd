/*
 * webtransportd — log.c
 *
 * Minimum log module driven by log_test.c. We deliberately keep this lean:
 * no timestamps, no level names, no rate-limiting until a test asks for it.
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

void wtd_log(wtd_log_level_t level, const char *fmt, ...) {
	if (level > s_level) {
		return;
	}
	pthread_mutex_lock(&s_mutex);
	va_list ap;
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
	(void)fflush(stderr);
	pthread_mutex_unlock(&s_mutex);
}
