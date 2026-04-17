/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
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
 * - Cycle 21d.1 (this file): smoke test for the daemon's --selftest
 *   path. fork+exec ./webtransportd --selftest; the child must
 *   exit(0) with stdout containing "selftest ok". Proves the
 *   picoquic packet-loop thread can start (bind an OS-picked UDP
 *   port, become ready) and stop cleanly under ASAN+UBSAN from the
 *   daemon main().
 *
 *   Cycle 21d.2+ builds on this with an in-process client/server
 *   handshake, and 21d.3 drives a client against the running daemon
 *   over real loopback UDP.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

static void cycle21d1_selftest_smoke(void) {
	int fds[2] = { -1, -1 };
	EXPECT(pipe(fds) == 0);

	pid_t pid = fork();
	EXPECT(pid >= 0);
	if (pid == 0) {
		(void)dup2(fds[1], STDOUT_FILENO);
		close(fds[0]);
		close(fds[1]);
		char *const argv[] = { (char *)"./webtransportd",
				(char *)"--selftest", NULL };
		execvp(argv[0], argv);
		_exit(127);
	}

	close(fds[1]);
	char buf[256];
	size_t total = 0;
	for (;;) {
		ssize_t n = read(fds[0], buf + total, sizeof(buf) - 1 - total);
		if (n > 0) {
			total += (size_t)n;
			if (total >= sizeof(buf) - 1) {
				break;
			}
		} else if (n == 0) {
			break;
		} else if (errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
	buf[total] = '\0';
	close(fds[0]);

	int status = 0;
	EXPECT(waitpid(pid, &status, 0) == pid);
	EXPECT(WIFEXITED(status));
	EXPECT(WEXITSTATUS(status) == 0);
	EXPECT(strstr(buf, "selftest ok") != NULL);
}

int main(void) {
	cycle21d1_selftest_smoke();
	return failures == 0 ? 0 : 1;
}
#endif /* !_WIN32 */
