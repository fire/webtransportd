/* TDD log:
 * - Cycle 16 (this file): wtd_child_spawn forks/exec's a child program
 *   with three pipes (stdin/stdout/stderr). We use /bin/cat as the
 *   child: bytes we write to stdin_fd come back unchanged on stdout_fd.
 *   wtd_child_terminate then SIGTERMs and reaps cleanly.
 */

#include "child_process.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

/* Read exactly `want` bytes from fd, retrying short reads. Returns 0 on
 * full read or -1 on error/EOF. */
static int read_full(int fd, void *buf, size_t want) {
	uint8_t *p = (uint8_t *)buf;
	size_t got = 0;
	while (got < want) {
		ssize_t n = read(fd, p + got, want - got);
		if (n > 0) {
			got += (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return -1;
		}
	}
	return 0;
}

static void cycle16_cat_echo(void) {
	const char *argv[] = { "/bin/cat", NULL };
	wtd_child_t child;
	int rc = wtd_child_spawn(argv, NULL, &child);
	EXPECT(rc == 0);
	if (rc != 0) {
		return;
	}
	EXPECT(child.pid > 0);
	EXPECT(child.stdin_fd >= 0);
	EXPECT(child.stdout_fd >= 0);

	const uint8_t msg[] = "hello, child!\n";
	ssize_t w = write(child.stdin_fd, msg, sizeof(msg) - 1);
	EXPECT(w == (ssize_t)(sizeof(msg) - 1));

	uint8_t echo[sizeof(msg) - 1] = { 0 };
	EXPECT(read_full(child.stdout_fd, echo, sizeof(echo)) == 0);
	EXPECT(memcmp(echo, msg, sizeof(echo)) == 0);

	wtd_child_terminate(&child);
	EXPECT(child.pid == -1); /* terminate clears the pid */
}

int main(void) {
#ifdef _WIN32
	/* /bin/cat is POSIX-only. The Windows CreateProcessA path added
	 * in cycle 37 is covered indirectly by the handshake tests
	 * (examples/echo, which builds anywhere). A dedicated Win32
	 * unit test using a cmd.exe findstr pipe is a future cycle. */
	return 0;
#else
	cycle16_cat_echo();
	return failures == 0 ? 0 : 1;
#endif
}
