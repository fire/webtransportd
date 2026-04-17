#ifdef _WIN32
/* POSIX-only test (fork+exec / sys/wait / arpa/inet). Cross-
 * compilation on mingw would need CreateProcess + Winsock
 * ports of the harness. Until that cycle lands, skip on
 * Windows so the build is green. The test body is still
 * compiled and run on linux-gcc + macos-clang. */
#include <stdio.h>
int main(void) {
    fprintf(stderr, "SKIP: POSIX-only test on Windows\n");
    return 0;
}
#else
/* TDD log:
 * - Cycle 12: the log module emits a line on stderr when the message
 *   level is <= the configured filter level, and stays silent
 *   otherwise. We capture stderr to a temp file to verify both
 *   behaviours without mocking the FILE*.
 *
 * - Cycle 28: each emitted line is prefixed with its level tag —
 *   `[ERROR] `, `[WARN] `, `[INFO] `, `[TRACE] `. This lets a human
 *   reading the daemon's stderr distinguish routine INFO traffic
 *   (like forwarded child stderr) from genuine daemon errors,
 *   without having to track which lines came from which code path.
 */

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stdout, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

/* Returns the contents of stderr captured during cb(), as a heap string
 * the caller must free. Restores the original stderr fd before returning. */
static char *capture_stderr(void (*cb)(void)) {
	fflush(stderr);
	int saved = dup(STDERR_FILENO);
	if (saved < 0) {
		return NULL;
	}

	char tmpl[] = "/tmp/wtdlogXXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) {
		close(saved);
		return NULL;
	}
	(void)dup2(fd, STDERR_FILENO);

	cb();
	fflush(stderr);

	(void)dup2(saved, STDERR_FILENO);
	close(saved);

	off_t size = lseek(fd, 0, SEEK_END);
	(void)lseek(fd, 0, SEEK_SET);
	char *buf = (char *)malloc((size_t)size + 1);
	if (size > 0) {
		ssize_t n = read(fd, buf, (size_t)size);
		buf[n < 0 ? 0 : n] = '\0';
	} else {
		buf[0] = '\0';
	}
	close(fd);
	(void)unlink(tmpl);
	return buf;
}

static void emit_info(void) { wtd_log(WTD_LOG_INFO, "hello %d", 42); }
static void emit_error(void) { wtd_log(WTD_LOG_ERROR, "boom"); }

static void cycle12_level_filter(void) {
	wtd_log_set_level(WTD_LOG_INFO);
	char *out = capture_stderr(emit_info);
	EXPECT(out != NULL);
	EXPECT(strstr(out, "hello 42") != NULL); /* INFO at INFO level: shown */
	free(out);

	wtd_log_set_level(WTD_LOG_QUIET);
	out = capture_stderr(emit_error);
	EXPECT(out != NULL);
	EXPECT(out[0] == '\0'); /* ERROR at QUIET level: hidden */
	free(out);

	/* Restore default for any subsequent tests in the binary. */
	wtd_log_set_level(WTD_LOG_INFO);
}

static void emit_error_prefixed(void) {
	wtd_log(WTD_LOG_ERROR, "bad things happened: %d", 7);
}
static void emit_trace_prefixed(void) {
	wtd_log(WTD_LOG_TRACE, "detail level");
}

static void cycle28_level_prefix(void) {
	wtd_log_set_level(WTD_LOG_TRACE);
	char *out = capture_stderr(emit_error_prefixed);
	EXPECT(out != NULL);
	EXPECT(strstr(out, "[ERROR] bad things happened: 7") != NULL);
	free(out);

	out = capture_stderr(emit_trace_prefixed);
	EXPECT(out != NULL);
	EXPECT(strstr(out, "[TRACE] detail level") != NULL);
	free(out);

	out = capture_stderr(emit_info);
	EXPECT(out != NULL);
	EXPECT(strstr(out, "[INFO] hello 42") != NULL);
	free(out);

	wtd_log_set_level(WTD_LOG_INFO);
}

int main(void) {
	cycle12_level_filter();
	cycle28_level_prefix();
	return failures == 0 ? 0 : 1;
}
#endif /* !_WIN32 */
