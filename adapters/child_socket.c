/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — child_socket.c
 *
 * See child_socket.h for the rationale (a duplex-socket alternative to
 * child_process.h's three-pipe stdio wiring) and the platform-support
 * note (POSIX only for now).
 */

#include "child_socket.h"

#include <errno.h>

#ifdef _WIN32
/* ===================== Windows: not yet implemented =====================
 *
 * Winsock has no socketpair() equivalent, and wrapping an inherited
 * SOCKET as a CRT fd the way child_process.c wraps pipe HANDLEs via
 * _open_osfhandle does not work the same way for sockets — normally
 * you'd use send()/recv()/closesocket() directly against the SOCKET
 * rather than read()/write()/close() against a CRT fd, and getting
 * handle-inheritance plus WSADuplicateSocket-style handoff correct
 * needs dedicated Windows testing this change doesn't have.
 *
 * A reasonable follow-up path (documented here rather than guessed at
 * in code): emulate socketpair() via a loopback 127.0.0.1 TCP
 * listen+connect+accept, verify the accepted peer address is actually
 * loopback (reject anything else) before trusting it, then decide
 * whether callers get a SOCKET or an _open_osfhandle-wrapped fd
 * depending on how they intend to read/write it.
 */

int wtd_child_socket_spawn(const char *const *argv, const char *const *envp,
		wtd_child_socket_t *out) {
	(void)argv;
	(void)envp;
	if (out == NULL) {
		return -EINVAL;
	}
	out->pid = WTD_CHILD_SOCKET_PID_NONE;
	out->fd = -1;
	out->stderr_fd = -1;
	return -ENOSYS;
}

void wtd_child_socket_terminate(wtd_child_socket_t *child) {
	if (child == NULL) {
		return;
	}
	child->pid = WTD_CHILD_SOCKET_PID_NONE;
	child->fd = -1;
	child->stderr_fd = -1;
}

#else
/* ===================== POSIX implementation ===================== */

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void set_cloexec(int fd) {
	int flags = fcntl(fd, F_GETFD);
	if (flags >= 0) {
		(void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
	}
}

static void close_pair(int p[2]) {
	if (p[0] >= 0) {
		close(p[0]);
		p[0] = -1;
	}
	if (p[1] >= 0) {
		close(p[1]);
		p[1] = -1;
	}
}

int wtd_child_socket_spawn(const char *const *argv, const char *const *envp,
		wtd_child_socket_t *out) {
	if (out == NULL) {
		return -EINVAL;
	}
	out->pid = -1;
	out->fd = -1;
	out->stderr_fd = -1;
	if (argv == NULL || argv[0] == NULL) {
		return -EINVAL;
	}

	int sv[2] = { -1, -1 };       /* sv[0]: daemon side, sv[1]: child side */
	int err_pipe[2] = { -1, -1 };
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		return -errno;
	}
	if (pipe(err_pipe) != 0) {
		int e = errno;
		close_pair(sv);
		return -e;
	}

	pid_t pid = fork();
	if (pid < 0) {
		int e = errno;
		close_pair(sv);
		close_pair(err_pipe);
		return -e;
	}

	if (pid == 0) {
		/* Child: dup2 its end of the socket onto both stdin and
		 * stdout (a plain stdio-reading/writing program keeps
		 * working unmodified), stderr onto the log pipe, close
		 * everything else, exec. */
		(void)dup2(sv[1], STDIN_FILENO);
		(void)dup2(sv[1], STDOUT_FILENO);
		(void)dup2(err_pipe[1], STDERR_FILENO);
		close(sv[0]);
		close(sv[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);

		if (envp != NULL) {
			environ = (char **)envp;
		}
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}

	/* Parent: close the child-side socket fd and the pipe's write
	 * end; mark our fds FD_CLOEXEC so a subsequent spawn doesn't
	 * accidentally inherit them. */
	close(sv[1]);
	close(err_pipe[1]);
	set_cloexec(sv[0]);
	set_cloexec(err_pipe[0]);

	/* Unlike child_process.c's separate stdin-write pipe, this one fd
	 * carries both directions, so we deliberately do NOT set
	 * O_NONBLOCK here — fcntl's O_NONBLOCK applies to the whole file
	 * description, and would make reads non-blocking too, which is
	 * the reader's decision to make, not this function's. A caller
	 * wanting non-blocking sends without affecting reads can use
	 * send(fd, ..., MSG_DONTWAIT) per-call instead — a property this
	 * being a real socket (not a pipe) gives you for free. */

	out->pid = pid;
	out->fd = sv[0];
	out->stderr_fd = err_pipe[0];
	return 0;
}

static void close_fd(int *p) {
	if (*p >= 0) {
		close(*p);
		*p = -1;
	}
}

void wtd_child_socket_terminate(wtd_child_socket_t *child) {
	if (child == NULL) {
		return;
	}
	if (child->fd >= 0) {
		/* Half-close for write: signals EOF to the child's stdin
		 * side the same way child_process.c closing stdin_fd does,
		 * without losing any not-yet-read bytes on the stdout side. */
		shutdown(child->fd, SHUT_WR);
	}
	if (child->pid > 0) {
		(void)kill(child->pid, SIGTERM);
		/* Up to ~500 ms for the child to exit on its own. */
		for (int i = 0; i < 50 && child->pid > 0; i++) {
			int status = 0;
			pid_t r = waitpid(child->pid, &status, WNOHANG);
			if (r == child->pid) {
				child->pid = -1;
				break;
			}
			if (r < 0 && errno != EINTR) {
				break;
			}
			struct timespec ts = { 0, 10 * 1000 * 1000 };
			(void)nanosleep(&ts, NULL);
		}
		if (child->pid > 0) {
			(void)kill(child->pid, SIGKILL);
			int status = 0;
			(void)waitpid(child->pid, &status, 0);
			child->pid = -1;
		}
	}
	close_fd(&child->fd);
	close_fd(&child->stderr_fd);
}

#endif /* _WIN32 */
