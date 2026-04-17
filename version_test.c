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
 * - Cycle 19-20 (this file): smoke test for the webtransportd binary.
 *   fork+exec ./webtransportd --version with stdout captured through a
 *   pipe. Assertions:
 *     * child exits 0 normally
 *     * stdout is non-empty
 *     * stdout contains WTD_VERSION (so the test breaks when the
 *       daemon forgets to print the version constant it was compiled
 *       with, not just anything non-empty)
 *
 *   The test links no module code — it shells the daemon. A Makefile
 *   prerequisite forces ./webtransportd to be built before this test
 *   is run.
 */

#include "version.h"

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

static void cycle19_version_smoke(void) {
	int fds[2] = { -1, -1 };
	EXPECT(pipe(fds) == 0);

	pid_t pid = fork();
	EXPECT(pid >= 0);
	if (pid == 0) {
		/* Child: wire pipe to stdout, exec the daemon. */
		(void)dup2(fds[1], STDOUT_FILENO);
		close(fds[0]);
		close(fds[1]);
		char *const argv[] = { (char *)"./webtransportd",
				(char *)"--version", NULL };
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
	EXPECT(total > 0);
	EXPECT(strstr(buf, WTD_VERSION) != NULL);
}

int main(void) {
	cycle19_version_smoke();
	return failures == 0 ? 0 : 1;
}
#endif /* !_WIN32 */
