/*
 * webtransportd — log.h
 *
 * Tiny leveled logger writing to stderr. Thread-safe via a single mutex;
 * callable from any thread (picoquic packet loop, per-peer reader threads,
 * the daemon's signal handler).
 */

#ifndef WEBTRANSPORTD_LOG_H
#define WEBTRANSPORTD_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum wtd_log_level {
	WTD_LOG_QUIET = 0,
	WTD_LOG_ERROR = 1,
	WTD_LOG_WARN = 2,
	WTD_LOG_INFO = 3,
	WTD_LOG_TRACE = 4,
} wtd_log_level_t;

void wtd_log_set_level(wtd_log_level_t level);
wtd_log_level_t wtd_log_get_level(void);

void wtd_log(wtd_log_level_t level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
		__attribute__((format(printf, 2, 3)))
#endif
		;

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_LOG_H */
