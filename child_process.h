/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — child_process.h
 *
 * Spawn a child program with three pipes: stdin, stdout, stderr. The
 * daemon writes framed bytes destined for the peer to stdin_fd; reads
 * framed bytes destined for the daemon from stdout_fd; and forwards
 * the child's stderr_fd to the daemon's log.
 *
 * Two implementations live behind this header:
 *
 *   * POSIX (fork + execvp + pipe):
 *       pid is a pid_t; the _fds are kernel fds callable with
 *       read(2) / write(2) / close(2).
 *
 *   * Win32 (CreateProcessA + CreatePipe):
 *       pid is a process HANDLE (stored as void* so <windows.h>
 *       doesn't leak into every consumer). stdin/stdout/stderr_fd
 *       are still ints — _open_osfhandle wraps the pipe handles
 *       as CRT file descriptors so the daemon's existing
 *       read/write/close calls work unchanged.
 *
 * WTD_CHILD_PID_NONE is the "no process" sentinel; pick it over a
 * raw -1 / NULL so the switch stays invisible to callers.
 */

#ifndef WEBTRANSPORTD_CHILD_PROCESS_H
#define WEBTRANSPORTD_CHILD_PROCESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
/* HANDLE is void*; we store it as void* so callers don't need windows.h. */
typedef struct wtd_child {
	void *pid;
	int stdin_fd;
	int stdout_fd;
	int stderr_fd;
} wtd_child_t;
#define WTD_CHILD_PID_NONE ((void *)0)
#else
#include <sys/types.h>
typedef struct wtd_child {
	pid_t pid;
	int stdin_fd;  /* daemon writes here; child reads on FD 0 */
	int stdout_fd; /* daemon reads here; child writes on FD 1 */
	int stderr_fd; /* daemon reads here; forwarded to log */
} wtd_child_t;
#define WTD_CHILD_PID_NONE ((pid_t)-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn argv[0] with the given environment. argv must be NULL-terminated.
 * envp must be NULL-terminated, or NULL to inherit the parent's env. On
 * success fills *out and returns 0; on failure returns -errno (and out is
 * left with all FDs == -1 / pid == WTD_CHILD_PID_NONE). */
int wtd_child_spawn(const char *const *argv, const char *const *envp,
		wtd_child_t *out);

/* Request the child exit (SIGTERM on POSIX, TerminateProcess on
 * Win32 after a brief grace period), wait for it, then close the
 * three fds. Sets pid back to WTD_CHILD_PID_NONE. Safe to call
 * once per spawn; safe on a partially-initialised wtd_child_t. */
void wtd_child_terminate(wtd_child_t *child);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_CHILD_PROCESS_H */
