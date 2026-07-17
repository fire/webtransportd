/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/* wtd_child_socket_spawn forks/execs a child program connected over a
 * single duplex AF_UNIX socket (dup2'd onto the child's stdin and
 * stdout) instead of child_process.h's two separate pipes. The test
 * uses /bin/cat as the child: bytes written to fd come back unchanged
 * on the same fd, and wtd_child_socket_terminate then half-closes,
 * SIGTERMs, and reaps it cleanly.
 */

#include "child_socket.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
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

static void cycle16_cat_echo_over_socket(void) {
	const char *argv[] = { "/bin/cat", NULL };
	wtd_child_socket_t child;
	int rc = wtd_child_socket_spawn(argv, NULL, &child);
	EXPECT(rc == 0);
	if (rc != 0) {
		return;
	}
	EXPECT(child.pid > 0);
	EXPECT(child.fd >= 0);
	EXPECT(child.stderr_fd >= 0);

	const uint8_t msg[] = "hello, child!\n";
	ssize_t w = write(child.fd, msg, sizeof(msg) - 1);
	EXPECT(w == (ssize_t)(sizeof(msg) - 1));

	uint8_t echo[sizeof(msg) - 1] = { 0 };
	EXPECT(read_full(child.fd, echo, sizeof(echo)) == 0);
	EXPECT(memcmp(echo, msg, sizeof(echo)) == 0);

	wtd_child_socket_terminate(&child);
	EXPECT(child.pid == -1); /* terminate clears the pid */
	EXPECT(child.fd == -1);  /* and closes the socket fd */
}
#endif /* !_WIN32 */

int main(void) {
#ifdef _WIN32
	/* wtd_child_socket_spawn returns -ENOSYS on Windows for now; see
	 * child_socket.c's file comment. Nothing to test here yet. */
	return 0;
#else
	cycle16_cat_echo_over_socket();
	return failures == 0 ? 0 : 1;
#endif
}
