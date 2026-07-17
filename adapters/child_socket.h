/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — adapters/child_socket.h
 *
 * Alternative to child_process.h's three-pipe stdio wiring: spawn a
 * child program connected to the daemon over a single full-duplex
 * AF_UNIX socket (via socketpair(2)) instead of separate stdin/stdout
 * pipes. The child side of the socket is dup2'd onto the child's own
 * fd 0 and fd 1, so a child program that just does plain stdio
 * read()/write() calls works completely unchanged — the daemon-side
 * difference is invisible to the child.
 *
 * Why this exists alongside child_process.h, not instead of it: a
 * consumer whose own I/O primitives are natively socket-shaped (e.g. an
 * actor runtime built around socket/connection abstractions rather
 * than raw pipe reads) can reuse those primitives directly against the
 * daemon-side fd this returns, instead of writing a bespoke pipe-fd
 * reader. child_process.h's stdio wiring is unaffected and remains the
 * default; this is an additional, separately-tested adapter satisfying
 * the same general shape (spawn / terminate), not a replacement.
 *
 * Platform status: POSIX (socketpair) is implemented. Windows has no
 * socketpair() equivalent in Winsock, and wrapping an inherited SOCKET
 * as a CRT fd usable with read()/write()/close() the way child_process.c
 * wraps pipe HANDLEs via _open_osfhandle is a materially different (and
 * more fragile) problem — getting it right needs dedicated Windows
 * testing this change doesn't have. wtd_child_socket_spawn returns
 * -ENOSYS on Windows until that's done; see the tracking note in
 * child_socket.c.
 *
 * Stderr is still piped separately and forwarded to the daemon's log,
 * same as child_process.h — only the data-plane (stdin+stdout) pipes
 * are replaced by the single duplex socket.
 */

#ifndef WEBTRANSPORTD_CHILD_SOCKET_H
#define WEBTRANSPORTD_CHILD_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
typedef struct wtd_child_socket {
	void *pid;
	int fd;         /* always -1 on Windows for now; see file comment */
	int stderr_fd;
} wtd_child_socket_t;
#define WTD_CHILD_SOCKET_PID_NONE ((void *)0)
#else
#include <sys/types.h>
typedef struct wtd_child_socket {
	pid_t pid;
	int fd;         /* daemon's end of the duplex socket: read + write + close like a POSIX fd */
	int stderr_fd;  /* daemon reads here; forwarded to log, same as child_process.h */
} wtd_child_socket_t;
#define WTD_CHILD_SOCKET_PID_NONE ((pid_t)-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn argv[0] with the given environment, connected to the daemon
 * over a single full-duplex socket dup2'd onto the child's fd 0 and
 * fd 1. argv must be NULL-terminated. envp must be NULL-terminated, or
 * NULL to inherit the parent's env. On success fills *out and returns
 * 0; on failure returns -errno (POSIX) or -ENOSYS (Windows, not yet
 * implemented), and out is left with fd == -1 / pid == the platform's
 * NONE sentinel. */
int wtd_child_socket_spawn(const char *const *argv, const char *const *envp,
		wtd_child_socket_t *out);

/* Request the child exit (SIGTERM on POSIX after half-closing the
 * socket for write, which many children treat as EOF the same way
 * they would stdin EOF), wait for it, then close the socket and
 * stderr fds. Sets pid back to the NONE sentinel. Safe to call once
 * per spawn; safe on a partially-initialised wtd_child_socket_t. */
void wtd_child_socket_terminate(wtd_child_socket_t *child);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_CHILD_SOCKET_H */
