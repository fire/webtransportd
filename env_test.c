/* TDD log:
 * - Cycle 13: env_build returns a NULL-terminated envp[] containing
 *   WEBTRANSPORT_REMOTE_ADDR.
 * - Cycle 14: the full CGI-style set — REMOTE_ADDR, REMOTE_PORT, PATH,
 *   AUTHORITY, VERSION — must all be present.
 * - Cycle 15 (this addition): a comma-separated --passenv whitelist
 *   forwards selected host env vars. Vars not in the whitelist are
 *   omitted; vars in the whitelist that aren't set on the host are
 *   silently skipped.
 */

#include "env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* mingw-w64 ships neither POSIX setenv nor unsetenv — it provides
 * _putenv_s ("NAME", "VALUE") and _putenv("NAME=") (empty-string
 * value deletes the entry in MSVCRT). Shim both so the test body
 * below can stay POSIX-flavoured. Not needed in env.c itself —
 * the module only uses strdup and getenv, both present on mingw. */
#ifdef _WIN32
#include <stdlib.h>
static int setenv(const char *name, const char *value, int overwrite) {
	(void)overwrite; /* _putenv_s always overwrites; matches overwrite==1 */
	return _putenv_s(name, value);
}
static int unsetenv(const char *name) {
	size_t n = strlen(name) + 2;
	char *buf = (char *)malloc(n);
	if (buf == NULL) {
		return -1;
	}
	snprintf(buf, n, "%s=", name);
	int rc = _putenv(buf);
	free(buf);
	return rc;
}
#endif

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

static void cycle14_full_cgi_set(void) {
	wtd_env_request_t req = {
		.remote_addr = "203.0.113.7",
		.remote_port = "55001",
		.path = "/echo",
		.authority = "example.test:443",
		.version = "0.1.0",
	};
	char **envp = wtd_env_build(&req, NULL);
	EXPECT(envp != NULL);
	EXPECT(envp_has(envp, "WEBTRANSPORT_REMOTE_ADDR=203.0.113.7"));
	EXPECT(envp_has(envp, "WEBTRANSPORT_REMOTE_PORT=55001"));
	EXPECT(envp_has(envp, "WEBTRANSPORT_PATH=/echo"));
	EXPECT(envp_has(envp, "WEBTRANSPORT_AUTHORITY=example.test:443"));
	EXPECT(envp_has(envp, "WEBTRANSPORT_VERSION=0.1.0"));
	wtd_env_free(envp);
}

static int envp_has_key(char **envp, const char *key_eq) {
	size_t n = strlen(key_eq);
	for (size_t i = 0; envp != NULL && envp[i] != NULL; i++) {
		if (strncmp(envp[i], key_eq, n) == 0) {
			return 1;
		}
	}
	return 0;
}

static void cycle15_passenv_whitelist(void) {
	/* Set two distinct host vars: one we want forwarded, one we don't. */
	setenv("WTD_TEST_FORWARD", "yes-please", 1);
	setenv("WTD_TEST_SECRET", "hunter2", 1);
	/* And one on the whitelist that isn't set on the host — must be skipped. */
	unsetenv("WTD_TEST_ABSENT");

	wtd_env_request_t req = {
		.remote_addr = "127.0.0.1", .remote_port = "1",
		.path = "/", .authority = "localhost:443", .version = "0.1.0",
	};
	char **envp = wtd_env_build(&req, "WTD_TEST_FORWARD, WTD_TEST_ABSENT");
	EXPECT(envp != NULL);
	EXPECT(envp_has(envp, "WTD_TEST_FORWARD=yes-please"));
	EXPECT(!envp_has_key(envp, "WTD_TEST_SECRET=")); /* not whitelisted */
	EXPECT(!envp_has_key(envp, "WTD_TEST_ABSENT=")); /* whitelisted but unset */
	/* CGI vars still present. */
	EXPECT(envp_has(envp, "WEBTRANSPORT_REMOTE_ADDR=127.0.0.1"));
	wtd_env_free(envp);

	unsetenv("WTD_TEST_FORWARD");
	unsetenv("WTD_TEST_SECRET");
}

int main(void) {
	cycle13_remote_addr();
	cycle14_full_cgi_set();
	cycle15_passenv_whitelist();
	return failures == 0 ? 0 : 1;
}
