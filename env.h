/*
 * webtransportd — env.h
 *
 * Build the environment block for a child process: WEBTRANSPORT_*
 * CGI-style variables describing the peer + path, plus any host env vars
 * whitelisted by --passenv (added in a later cycle).
 */

#ifndef WEBTRANSPORTD_ENV_H
#define WEBTRANSPORTD_ENV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wtd_env_request {
	const char *remote_addr; /* peer IP literal */
	const char *remote_port; /* peer UDP port as decimal string */
	const char *path;        /* :path of the CONNECT */
	const char *authority;   /* :authority (host[:port]) */
	const char *version;     /* webtransportd version string */
} wtd_env_request_t;

/* Allocate and return a NULL-terminated envp[] array (each entry is a
 * heap-allocated "KEY=VALUE" string) suitable for execve.
 * Release with wtd_env_free(). passenv may be NULL or empty. */
char **wtd_env_build(const wtd_env_request_t *req, const char *passenv);

void wtd_env_free(char **envp);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_ENV_H */
