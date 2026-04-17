/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — child_process.c
 *
 * Two implementations of wtd_child_spawn / wtd_child_terminate,
 * picked by #ifdef _WIN32. Both satisfy the same contract:
 *
 *   * three anonymous pipes wired to the child's stdio
 *   * parent-side fds that behave like POSIX fds (read/write/close)
 *   * best-effort graceful exit (SIGTERM / TerminateProcess) with
 *     a short grace period, then forced kill / reap
 *
 * The POSIX path is the original cycle-16 code. The Win32 path
 * (cycle 37) uses CreateProcessA + CreatePipe and wraps the pipe
 * HANDLEs as CRT fds via _open_osfhandle, so the daemon's existing
 * read/write/close calls work unchanged on both platforms.
 */

#include "child_process.h"

#include "cmdline.h"

#include <errno.h>

#ifdef _WIN32
/* ===================== Win32 implementation ===================== */

#include <io.h>       /* _open_osfhandle, _close */
#include <fcntl.h>    /* _O_RDONLY, _O_WRONLY */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static void close_handle(HANDLE *h) {
	if (*h != NULL && *h != INVALID_HANDLE_VALUE) {
		CloseHandle(*h);
		*h = NULL;
	}
}

int wtd_child_spawn(const char *const *argv, const char *const *envp,
		wtd_child_t *out) {
	if (out == NULL) {
		return -EINVAL;
	}
	out->pid = NULL;
	out->stdin_fd = -1;
	out->stdout_fd = -1;
	out->stderr_fd = -1;
	if (argv == NULL || argv[0] == NULL) {
		return -EINVAL;
	}
	/* envp override is a nice-to-have; on Win32 we inherit the
	 * parent environment for now. A future cycle can plumb a
	 * UTF-16 environment block through CreateProcessW. */
	(void)envp;

	SECURITY_ATTRIBUTES sa = { 0 };
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	HANDLE in_rd = NULL, in_wr = NULL;
	HANDLE out_rd = NULL, out_wr = NULL;
	HANDLE err_rd = NULL, err_wr = NULL;

	if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) {
		return -EIO;
	}
	if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) {
		close_handle(&in_rd); close_handle(&in_wr);
		return -EIO;
	}
	if (!CreatePipe(&err_rd, &err_wr, &sa, 0)) {
		close_handle(&in_rd); close_handle(&in_wr);
		close_handle(&out_rd); close_handle(&out_wr);
		return -EIO;
	}

	/* Parent-side handles must not be inherited by the child. */
	SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);

	char cmdline[4096];
	if (wtd_build_cmdline(argv, cmdline, sizeof(cmdline)) != 0) {
		close_handle(&in_rd); close_handle(&in_wr);
		close_handle(&out_rd); close_handle(&out_wr);
		close_handle(&err_rd); close_handle(&err_wr);
		return -E2BIG;
	}

	STARTUPINFOA si = { 0 };
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = in_rd;
	si.hStdOutput = out_wr;
	si.hStdError = err_wr;

	PROCESS_INFORMATION pi = { 0 };
	BOOL ok = CreateProcessA(
			NULL,       /* lpApplicationName: look up via PATH */
			cmdline,    /* lpCommandLine: mutated by CreateProcess */
			NULL, NULL, /* process / thread security */
			TRUE,       /* bInheritHandles */
			0,          /* dwCreationFlags */
			NULL,       /* lpEnvironment: inherit */
			NULL,       /* lpCurrentDirectory: inherit */
			&si, &pi);

	/* Close child-side handles regardless of success. Parent-side
	 * ones stay open (we'll wrap them as CRT fds on success). */
	close_handle(&in_rd);
	close_handle(&out_wr);
	close_handle(&err_wr);

	if (!ok) {
		close_handle(&in_wr);
		close_handle(&out_rd);
		close_handle(&err_rd);
		return -EIO;
	}
	CloseHandle(pi.hThread); /* we don't hold the primary thread */

	int in_fd = _open_osfhandle((intptr_t)in_wr, _O_WRONLY);
	int out_fd = _open_osfhandle((intptr_t)out_rd, _O_RDONLY);
	int err_fd = _open_osfhandle((intptr_t)err_rd, _O_RDONLY);
	if (in_fd < 0 || out_fd < 0 || err_fd < 0) {
		if (in_fd >= 0) _close(in_fd); else close_handle(&in_wr);
		if (out_fd >= 0) _close(out_fd); else close_handle(&out_rd);
		if (err_fd >= 0) _close(err_fd); else close_handle(&err_rd);
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hProcess);
		return -EIO;
	}

	out->pid = pi.hProcess; /* HANDLE -> void* */
	out->stdin_fd = in_fd;
	out->stdout_fd = out_fd;
	out->stderr_fd = err_fd;
	return 0;
}

static void close_fd(int *p) {
	if (*p >= 0) {
		_close(*p);
		*p = -1;
	}
}

void wtd_child_terminate(wtd_child_t *child) {
	if (child == NULL) {
		return;
	}
	/* Close stdin first — most children exit on EOF. */
	close_fd(&child->stdin_fd);
	HANDLE h = (HANDLE)child->pid;
	if (h != NULL) {
		/* Give ~500 ms to exit on its own, then hard-kill. */
		DWORD wait = WaitForSingleObject(h, 500);
		if (wait != WAIT_OBJECT_0) {
			TerminateProcess(h, 1);
			WaitForSingleObject(h, INFINITE);
		}
		CloseHandle(h);
		child->pid = NULL;
	}
	close_fd(&child->stdout_fd);
	close_fd(&child->stderr_fd);
}

#else
/* ===================== POSIX implementation ===================== */

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
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

int wtd_child_spawn(const char *const *argv, const char *const *envp,
		wtd_child_t *out) {
	if (out == NULL) {
		return -EINVAL;
	}
	out->pid = -1;
	out->stdin_fd = -1;
	out->stdout_fd = -1;
	out->stderr_fd = -1;
	if (argv == NULL || argv[0] == NULL) {
		return -EINVAL;
	}

	int in_pipe[2] = { -1, -1 };
	int out_pipe[2] = { -1, -1 };
	int err_pipe[2] = { -1, -1 };
	if (pipe(in_pipe) != 0) {
		return -errno;
	}
	if (pipe(out_pipe) != 0) {
		int e = errno;
		close_pair(in_pipe);
		return -e;
	}
	if (pipe(err_pipe) != 0) {
		int e = errno;
		close_pair(in_pipe);
		close_pair(out_pipe);
		return -e;
	}

	pid_t pid = fork();
	if (pid < 0) {
		int e = errno;
		close_pair(in_pipe);
		close_pair(out_pipe);
		close_pair(err_pipe);
		return -e;
	}

	if (pid == 0) {
		/* Child: dup pipes onto stdio, close all parent-side ends, exec. */
		(void)dup2(in_pipe[0], STDIN_FILENO);
		(void)dup2(out_pipe[1], STDOUT_FILENO);
		(void)dup2(err_pipe[1], STDERR_FILENO);
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);

		if (envp != NULL) {
			environ = (char **)envp;
		}
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}

	/* Parent: close child-side ends; mark our ends FD_CLOEXEC so a
	 * subsequent spawn doesn't accidentally inherit them. */
	close(in_pipe[0]);
	close(out_pipe[1]);
	close(err_pipe[1]);
	set_cloexec(in_pipe[1]);
	set_cloexec(out_pipe[0]);
	set_cloexec(err_pipe[0]);
	/* Cycle 45: set non-blocking on parent stdin writer so
	 * server_stream_cb never blocks waiting for a slow child. */
	{
		int _fl = fcntl(in_pipe[1], F_GETFL);
		if (_fl >= 0)
			(void)fcntl(in_pipe[1], F_SETFL, _fl | O_NONBLOCK);
	}

	out->pid = pid;
	out->stdin_fd = in_pipe[1];
	out->stdout_fd = out_pipe[0];
	out->stderr_fd = err_pipe[0];
	return 0;
}

static void close_fd(int *p) {
	if (*p >= 0) {
		close(*p);
		*p = -1;
	}
}

void wtd_child_terminate(wtd_child_t *child) {
	if (child == NULL) {
		return;
	}
	close_fd(&child->stdin_fd); /* signals EOF; many children exit on it */
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
	close_fd(&child->stdout_fd);
	close_fd(&child->stderr_fd);
}

#endif /* _WIN32 */
