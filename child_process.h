/*
 * webtransportd — child_process.h
 *
 * Spawn a child program with three pipes: stdin, stdout, stderr. The
 * daemon writes framed bytes destined for the peer to stdin_fd; reads
 * framed bytes destined for the daemon from stdout_fd; and forwards the
 * child's stderr_fd to the daemon's log.
 *
 * POSIX-only (fork + execvp). Future cycles add a Windows implementation
 * driven by a parallel test.
 */

#ifndef WEBTRANSPORTD_CHILD_PROCESS_H
#define WEBTRANSPORTD_CHILD_PROCESS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wtd_child {
	pid_t pid;
	int stdin_fd;  /* daemon writes here; child reads on FD 0 */
	int stdout_fd; /* daemon reads here; child writes on FD 1 */
	int stderr_fd; /* daemon reads here; forwarded to log */
} wtd_child_t;

/* Spawn argv[0] with the given environment. argv must be NULL-terminated.
 * envp must be NULL-terminated, or NULL to inherit nothing. On success
 * fills *out and returns 0; on failure returns -errno (and out is left
 * with all FDs == -1 / pid == -1). */
int wtd_child_spawn(const char *const *argv, const char *const *envp,
		wtd_child_t *out);

/* SIGTERM the child, wait briefly, then SIGKILL on timeout. Closes the
 * three FDs and reaps the child. Sets out->pid to -1. Safe to call once
 * per spawn; safe to call on a partially-initialised wtd_child_t. */
void wtd_child_terminate(wtd_child_t *child);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_CHILD_PROCESS_H */
