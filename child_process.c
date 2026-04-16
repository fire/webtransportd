/*
 * webtransportd — child_process.c
 *
 * fork()/execvp() with three pipes, plus tear-down. POSIX-only.
 */

#include "child_process.h"

#include <errno.h>
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
