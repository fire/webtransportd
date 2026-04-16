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

/* Number of fixed WEBTRANSPORT_* entries we always emit. */
#define WTD_ENV_FIXED_COUNT 5

/* Append `entry` to *p_envp, growing as needed. *p_envp must always be
 * NULL-terminated; *p_count is the number of non-NULL entries. Takes
 * ownership of `entry` on success; frees it on growth failure. */
static int env_append(char ***p_envp, size_t *p_count, size_t *p_cap, char *entry) {
	if (entry == NULL) {
		return -1;
	}
	if (*p_count + 1 >= *p_cap) {
		size_t new_cap = (*p_cap == 0) ? 8 : (*p_cap * 2);
		char **n = (char **)realloc(*p_envp, new_cap * sizeof(char *));
		if (n == NULL) {
			free(entry);
			return -1;
		}
		*p_envp = n;
		*p_cap = new_cap;
	}
	(*p_envp)[(*p_count)++] = entry;
	(*p_envp)[*p_count] = NULL;
	return 0;
}

/* Strip leading + trailing ASCII whitespace in-place. */
static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
		end--;
	}
	*end = '\0';
	return s;
}

char **wtd_env_build(const wtd_env_request_t *req, const char *passenv) {
	if (req == NULL) {
		return NULL;
	}
	char **envp = (char **)calloc(WTD_ENV_FIXED_COUNT + 1, sizeof(char *));
	if (envp == NULL) {
		return NULL;
	}
	size_t count = 0;
	size_t cap = WTD_ENV_FIXED_COUNT + 1;
	if (env_append(&envp, &count, &cap, kv_alloc("WEBTRANSPORT_REMOTE_ADDR", req->remote_addr)) != 0
			|| env_append(&envp, &count, &cap, kv_alloc("WEBTRANSPORT_REMOTE_PORT", req->remote_port)) != 0
			|| env_append(&envp, &count, &cap, kv_alloc("WEBTRANSPORT_PATH", req->path)) != 0
			|| env_append(&envp, &count, &cap, kv_alloc("WEBTRANSPORT_AUTHORITY", req->authority)) != 0
			|| env_append(&envp, &count, &cap, kv_alloc("WEBTRANSPORT_VERSION", req->version)) != 0) {
		wtd_env_free(envp);
		return NULL;
	}

	if (passenv != NULL && passenv[0] != '\0') {
		char *list = strdup(passenv);
		if (list == NULL) {
			wtd_env_free(envp);
			return NULL;
		}
		char *saveptr = NULL;
		for (char *tok = strtok_r(list, ",", &saveptr); tok != NULL;
				tok = strtok_r(NULL, ",", &saveptr)) {
			char *name = trim(tok);
			if (*name == '\0') {
				continue;
			}
			const char *value = getenv(name);
			if (value == NULL) {
				continue; /* whitelisted but not set on host: skip silently */
			}
			if (env_append(&envp, &count, &cap, kv_alloc(name, value)) != 0) {
				free(list);
				wtd_env_free(envp);
				return NULL;
			}
		}
		free(list);
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
