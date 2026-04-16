/* TDD log:
 * - Cycle 13 (this file): env_build returns a NULL-terminated envp[]
 *   where each entry is "KEY=VALUE". At minimum it must contain
 *   WEBTRANSPORT_REMOTE_ADDR with the string we supplied. We don't pin
 *   the ordering or the exact set of WEBTRANSPORT_* vars yet — later
 *   cycles add more.
 */

#include "env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

static int envp_has(char **envp, const char *kv) {
	if (envp == NULL) {
		return 0;
	}
	for (size_t i = 0; envp[i] != NULL; i++) {
		if (strcmp(envp[i], kv) == 0) {
			return 1;
		}
	}
	return 0;
}

static void cycle13_remote_addr(void) {
	wtd_env_request_t req = {
		.remote_addr = "127.0.0.1",
		.remote_port = "40001",
		.path = "/echo",
		.authority = "localhost:4433",
		.version = "0.1.0",
	};
	char **envp = wtd_env_build(&req, NULL /* no passenv */);
	EXPECT(envp != NULL);
	EXPECT(envp_has(envp, "WEBTRANSPORT_REMOTE_ADDR=127.0.0.1"));
	wtd_env_free(envp);
}

int main(void) {
	cycle13_remote_addr();
	return failures == 0 ? 0 : 1;
}
