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
 * - Cycle 30 (this file): `./webtransportd --help` prints an
 *   operator-friendly summary to stdout and exits 0. Beyond the
 *   bare "usage:" line, it must include a one-line description of
 *   what the daemon does, a line per subcommand, a line per
 *   server-mode option, and a note about the framing on the
 *   child's pipes. We assert a handful of distinctive substrings
 *   so the test fails loudly if any section is accidentally
 *   deleted but doesn't need updating every time the prose is
 *   rewritten.
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

static void cycle30_help_smoke(void) {
	int fds[2] = { -1, -1 };
	EXPECT(pipe(fds) == 0);

	pid_t pid = fork();
	EXPECT(pid >= 0);
	if (pid == 0) {
		(void)dup2(fds[1], STDOUT_FILENO);
		close(fds[0]);
		close(fds[1]);
		char *const argv[] = {
			(char *)"./webtransportd",
			(char *)"--help",
			NULL,
		};
		execvp(argv[0], argv);
		_exit(127);
	}

	close(fds[1]);
	char buf[2048];
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

	/* One-line description — should mention what this is. */
	EXPECT(strstr(buf, "WebTransport") != NULL);
	/* Every documented subcommand appears. */
	EXPECT(strstr(buf, "--version") != NULL);
	EXPECT(strstr(buf, "--selftest") != NULL);
	EXPECT(strstr(buf, "--server") != NULL);
	/* Every server-mode option. */
	EXPECT(strstr(buf, "--cert=") != NULL);
	EXPECT(strstr(buf, "--key=") != NULL);
	EXPECT(strstr(buf, "--port=") != NULL);
	EXPECT(strstr(buf, "--exec=") != NULL);
	EXPECT(strstr(buf, "--log-level=") != NULL);
	/* A framing-spec summary so operators know what bytes their
	 * child will see on its stdin and must produce on stdout. */
	EXPECT(strstr(buf, "framing:") != NULL);
	EXPECT(strstr(buf, "flag") != NULL);
	EXPECT(strstr(buf, "varint") != NULL);
}

int main(void) {
	cycle30_help_smoke();
	return failures == 0 ? 0 : 1;
}
#endif /* !_WIN32 */
