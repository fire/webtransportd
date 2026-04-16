/*
 * webtransportd — env.c
 *
 * Minimum implementation: emit only WEBTRANSPORT_REMOTE_ADDR for now.
 * The next cycle's RED test will demand WEBTRANSPORT_PATH, etc.
 */

#include "env.h"

#include <stdlib.h>
#include <string.h>

static char *kv_alloc(const char *key, const char *value) {
	if (value == NULL) {
		value = "";
	}
	size_t klen = strlen(key);
	size_t vlen = strlen(value);
	char *s = (char *)malloc(klen + 1 + vlen + 1);
	if (s == NULL) {
		return NULL;
	}
	memcpy(s, key, klen);
	s[klen] = '=';
	memcpy(s + klen + 1, value, vlen);
	s[klen + 1 + vlen] = '\0';
	return s;
}

char **wtd_env_build(const wtd_env_request_t *req, const char *passenv) {
	(void)passenv;
	if (req == NULL) {
		return NULL;
	}
	char **envp = (char **)calloc(2, sizeof(char *));
	if (envp == NULL) {
		return NULL;
	}
	envp[0] = kv_alloc("WEBTRANSPORT_REMOTE_ADDR", req->remote_addr);
	envp[1] = NULL;
	if (envp[0] == NULL) {
		free(envp);
		return NULL;
	}
	return envp;
}

void wtd_env_free(char **envp) {
	if (envp == NULL) {
		return;
	}
	for (size_t i = 0; envp[i] != NULL; i++) {
		free(envp[i]);
	}
	free(envp);
}
