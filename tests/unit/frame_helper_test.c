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
 * - Cycle 34 (this file): examples/frame-helper.sh is a shell-level
 *   framing encoder that operators can pipe text through. This test
 *   fork/execs the script with a (flag, payload) pair, captures the
 *   raw bytes it emits to stdout, feeds them through wtd_frame_decode,
 *   and asserts the decoded triple matches the inputs.
 *
 *   Three cases exercise the 1-byte and 2-byte varint forms (4-byte
 *   and 8-byte would require arg values the shell arg list can't
 *   realistically carry in a test, so those are covered by the C
 *   codec tests instead).
 */

#include "frame.h"

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

/* Run ./examples/frame-helper.sh flag payload, capture its stdout
 * into out[0..cap), return the number of bytes captured on success
 * or -1 on any error (including a non-zero exit from the script). */
static ssize_t run_helper(const char *flag, const char *payload,
		uint8_t *out, size_t cap) {
	int fds[2] = { -1, -1 };
	if (pipe(fds) != 0) {
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		(void)dup2(fds[1], STDOUT_FILENO);
		close(fds[0]);
		close(fds[1]);
		char *const argv[] = {
			(char *)"./examples/frame-helper.sh",
			(char *)flag,
			(char *)payload,
			NULL,
		};
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	size_t total = 0;
	for (;;) {
		ssize_t n = read(fds[0], out + total, cap - total);
		if (n > 0) {
			total += (size_t)n;
			if (total >= cap) {
				break;
			}
		} else if (n == 0) {
			break;
		} else if (errno == EINTR) {
			continue;
		} else {
			close(fds[0]);
			return -1;
		}
	}
	close(fds[0]);
	int st = 0;
	if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)
			|| WEXITSTATUS(st) != 0) {
		return -1;
	}
	return (ssize_t)total;
}

static void one_case(const char *flag, uint8_t want_flag,
		const char *payload) {
	size_t want_len = strlen(payload);
	uint8_t buf[16384 + 8];
	ssize_t got = run_helper(flag, payload, buf, sizeof(buf));
	EXPECT(got > 0);
	if (got <= 0) {
		return;
	}
	size_t consumed = 0;
	uint8_t out_flag = 0xff;
	const uint8_t *out_payload = NULL;
	size_t out_payload_len = 0;
	EXPECT(wtd_frame_decode(buf, (size_t)got,
				&consumed, &out_flag, &out_payload, &out_payload_len)
			== WTD_FRAME_OK);
	EXPECT((size_t)got == consumed);
	EXPECT(out_flag == want_flag);
	EXPECT(out_payload_len == want_len);
	if (out_payload_len == want_len) {
		EXPECT(memcmp(out_payload, payload, want_len) == 0);
	}
}

int main(void) {
	/* 1-byte varint (len <= 63). */
	one_case("0", 0, "hi");
	one_case("1", 1, "dgram");
	one_case("0", 0, "");

	/* 2-byte varint (64 <= len < 16384). A 100-byte payload crosses
	 * the threshold; anything larger than ~8 KB tends to blow up the
	 * shell's argv size so we stay modest. */
	char med[1024];
	memset(med, 'x', sizeof(med) - 1);
	med[sizeof(med) - 1] = '\0';
	one_case("1", 1, med);

	return failures == 0 ? 0 : 1;
}
#endif /* !_WIN32 */
