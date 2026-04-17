/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
#ifdef _WIN32
/* POSIX-only test (fork+exec / sys/wait / arpa/inet). Cross-
 * compilation on mingw would need CreateProcess + Winsock
 * ports of the harness. Until that cycle lands, skip on
 * Windows so the build is green. The test body is still
 * compiled and run on linux-gcc + macos-clang. */
#include <stdio.h>
int main(void) {
    fprintf(stderr, "SKIP: POSIX-only test on Windows\n");
    return 0;
}
#else
/* TDD log:
 * - Cycle 21d.3: real-socket QUIC handshake. fork/exec ./webtransportd
 *   --server with the vendored test cert+key on a fixed loopback UDP
 *   port; drive a picoquic client through sendto/recvfrom until the
 *   connection reaches picoquic_state_ready; then SIGTERM + waitpid.
 *
 *   Deliberately uses the daemon's synchronous single-thread packet
 *   loop to bypass the darwin-arm64 ASAN/pthread_create crash
 *   documented in 21d.1.
 *
 * - Cycle 22a: same test passes --exec=<bin> and asserts the daemon
 *   prints "child spawned pid=<N>" after the first connection reaches
 *   ready. Proves child_process.c is wired into the real server
 *   pipeline (spawned on demand, reaped on shutdown, ASAN-clean).
 *
 * - Cycle 22b: --exec=examples/frame_hi — a tiny helper that writes
 *   one framed message (flag=0, payload="hi") and exits. Daemon's
 *   wtd_peer_session reader decodes it and the packet-loop callback
 *   prints "outbound frame: flag=0 len=2 payload=hi".
 *
 * - Cycle 23: frame_hi also writes "oops\n" to stderr before the
 *   frame. The daemon's stderr forwarder thread reads child.stderr_fd
 *   and emits "child stderr: oops" on the daemon's own stderr. The
 *   test dup2()s the daemon's stdout+stderr onto one pipe so one
 *   drain sees both channels and asserts the forwarded sentinel.
 *
 * - Cycle 27 (this commit): launch the daemon with --log-level=4
 *   (TRACE) and assert the sentinel "packet loop ready" — a
 *   TRACE-level wtd_log call in the server's packet_loop_ready
 *   callback that's filtered out at the default level. This tests
 *   both the --log-level flag parser and the log.c integration
 *   into the daemon binary.
 */

#include "picoquic.h"
#include "picoquic_utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

/* Cycle 33: derive the daemon's UDP port from the test's pid so
 * sequential or accidentally-overlapping runs don't collide on a
 * port the previous invocation hasn't finished releasing. Range
 * 20000-28191; odds of two runs picking the same port are ~1/8000. */
static uint16_t SERVER_PORT;

static int read_line(int fd, char *out, size_t cap, int timeout_ms) {
	size_t got = 0;
	while (got + 1 < cap) {
		struct timespec wait = { 0, 10 * 1000 * 1000 };
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		struct timeval tv = { 0, 10 * 1000 };
		int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (sel < 0 && errno == EINTR) {
			continue;
		}
		if (sel > 0) {
			char c = 0;
			ssize_t n = read(fd, &c, 1);
			if (n == 1) {
				out[got++] = c;
				if (c == '\n') {
					break;
				}
			} else if (n == 0) {
				break;
			}
		}
		(void)nanosleep(&wait, NULL);
		timeout_ms -= 10;
		if (timeout_ms <= 0) {
			break;
		}
	}
	out[got] = '\0';
	return (int)got;
}

typedef struct {
	pid_t pid;
	int stdout_fd;
} daemon_t;

static int spawn_daemon(daemon_t *out, uint16_t port, const char *exec_path) {
	int fds[2] = { -1, -1 };
	if (pipe(fds) != 0) {
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		/* Merge daemon stdout + stderr into the same pipe so the
		 * test's drain picks up both channels (cycle 23: the
		 * stderr forwarder emits to daemon stderr). */
		(void)dup2(fds[1], STDOUT_FILENO);
		(void)dup2(fds[1], STDERR_FILENO);
		close(fds[0]);
		close(fds[1]);
		char port_buf[32];
		char exec_buf[256];
		snprintf(port_buf, sizeof(port_buf), "--port=%u", (unsigned)port);
		snprintf(exec_buf, sizeof(exec_buf), "--exec=%s",
				exec_path != NULL ? exec_path : "");
		char *argv[9];
		int i = 0;
		argv[i++] = (char *)"./webtransportd";
		argv[i++] = (char *)"--server";
		argv[i++] = (char *)"--cert=thirdparty/picoquic/certs/cert.pem";
		argv[i++] = (char *)"--key=thirdparty/picoquic/certs/key.pem";
		argv[i++] = port_buf;
		argv[i++] = (char *)"--log-level=4"; /* cycle 27: TRACE */
		if (exec_path != NULL) {
			argv[i++] = exec_buf;
		}
		argv[i] = NULL;
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	out->pid = pid;
	out->stdout_fd = fds[0];
	return 0;
}

static int wait_for_ready(daemon_t *d, int timeout_ms) {
	char line[128];
	int remaining = timeout_ms;
	while (remaining > 0) {
		int n = read_line(d->stdout_fd, line, sizeof(line), 500);
		if (n > 0 && strstr(line, "server ready") != NULL) {
			return 0;
		}
		remaining -= 500;
	}
	return -1;
}

static void kill_and_reap(daemon_t *d, int *p_exit_status) {
	if (d->pid > 0) {
		(void)kill(d->pid, SIGTERM);
		for (int i = 0; i < 50; i++) {
			int st = 0;
			pid_t r = waitpid(d->pid, &st, WNOHANG);
			if (r == d->pid) {
				*p_exit_status = st;
				d->pid = -1;
				break;
			}
			struct timespec ts = { 0, 20 * 1000 * 1000 };
			(void)nanosleep(&ts, NULL);
		}
		if (d->pid > 0) {
			(void)kill(d->pid, SIGKILL);
			int st = 0;
			(void)waitpid(d->pid, &st, 0);
			*p_exit_status = st;
			d->pid = -1;
		}
	}
	if (d->stdout_fd >= 0) {
		close(d->stdout_fd);
		d->stdout_fd = -1;
	}
}

static int run_handshake(uint16_t server_port) {
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		return -1;
	}
	/* Non-blocking recv so the pump can check prepare/incoming each tick. */
	int fl = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, fl | O_NONBLOCK);

	struct sockaddr_in cli = { 0 };
	cli.sin_family = AF_INET;
	cli.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	cli.sin_port = 0;
	if (bind(sock, (struct sockaddr *)&cli, sizeof(cli)) != 0) {
		close(sock);
		return -1;
	}

	struct sockaddr_in srv = { 0 };
	srv.sin_family = AF_INET;
	srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	srv.sin_port = htons(server_port);

	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = { 0 };
	picoquic_quic_t *quic = picoquic_create(
			4, NULL, NULL, NULL, "hq-test",
			NULL, NULL, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (quic == NULL) {
		close(sock);
		return -1;
	}

	picoquic_cnx_t *cnx = picoquic_create_client_cnx(
			quic, (struct sockaddr *)&srv,
			picoquic_current_time(), 0, "test.example",
			"hq-test", NULL, NULL);
	if (cnx == NULL) {
		picoquic_free(quic);
		close(sock);
		return -1;
	}

	uint64_t deadline = picoquic_current_time() + 5ull * 1000 * 1000; /* 5s */
	int ready = 0;
	uint8_t buf[2048];
	while (picoquic_current_time() < deadline) {
		uint64_t now = picoquic_current_time();
		size_t send_len = 0;
		struct sockaddr_storage sto, sfrom;
		int if_idx = 0;
		picoquic_connection_id_t log_cid;
		picoquic_cnx_t *last = NULL;
		int rc = picoquic_prepare_next_packet(quic, now, buf,
				sizeof(buf), &send_len,
				&sto, &sfrom, &if_idx, &log_cid, &last);
		if (rc == 0 && send_len > 0) {
			(void)sendto(sock, buf, send_len, 0,
					(struct sockaddr *)&srv, sizeof(srv));
		}

		/* Drain any pending inbound packets. */
		for (int i = 0; i < 8; i++) {
			struct sockaddr_in from;
			socklen_t fromlen = sizeof(from);
			ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
					(struct sockaddr *)&from, &fromlen);
			if (n <= 0) {
				break;
			}
			struct sockaddr_in my = { 0 };
			my.sin_family = AF_INET;
			my.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			socklen_t mylen = sizeof(my);
			getsockname(sock, (struct sockaddr *)&my, &mylen);
			picoquic_incoming_packet(quic, buf, (size_t)n,
					(struct sockaddr *)&from,
					(struct sockaddr *)&my,
					0, 0, picoquic_current_time());
		}

		if (picoquic_get_cnx_state(cnx) == picoquic_state_ready) {
			ready = 1;
			break;
		}

		struct timespec ts = { 0, 5 * 1000 * 1000 };
		(void)nanosleep(&ts, NULL);
	}

	picoquic_free(quic);
	close(sock);
	return ready ? 0 : -1;
}

/* Drain anything the daemon has printed (appends to `buf`, up to cap),
 * until the timeout elapses with no further data. Non-blocking read
 * loop so we exit promptly once the daemon goes quiet. */
static void drain_stdout(int fd, char *buf, size_t cap, size_t *len,
		int quiet_ms) {
	int quiet = 0;
	while (quiet < quiet_ms && *len + 1 < cap) {
		struct timeval tv = { 0, 20 * 1000 };
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (sel > 0) {
			ssize_t n = read(fd, buf + *len, cap - 1 - *len);
			if (n > 0) {
				*len += (size_t)n;
				quiet = 0;
				continue;
			}
			if (n == 0) {
				break;
			}
		}
		quiet += 20;
	}
	buf[*len] = '\0';
}

int main(void) {
	SERVER_PORT = (uint16_t)(20000 + (getpid() & 0x1fff));
	daemon_t d = { -1, -1 };
	EXPECT(spawn_daemon(&d, SERVER_PORT, "./examples/frame_hi") == 0);
	if (d.pid < 0) {
		return 1;
	}

	int ready_rc = wait_for_ready(&d, 5000);
	EXPECT(ready_rc == 0);
	if (ready_rc != 0) {
		int st = 0;
		kill_and_reap(&d, &st);
		return 1;
	}

	int hs = run_handshake(SERVER_PORT);
	EXPECT(hs == 0);

	/* After handshake, daemon prints "client reached ready" and
	 * "child spawned pid=<N>" on its stdout (once) and then exits
	 * on its own ~200ms later. Drain until it goes quiet. */
	char log[1024];
	size_t log_len = 0;
	drain_stdout(d.stdout_fd, log, sizeof(log), &log_len, 500);
	EXPECT(strstr(log, "client reached ready") != NULL);
	EXPECT(strstr(log, "child spawned pid=") != NULL);
	EXPECT(strstr(log, "outbound frame: flag=0 len=2 payload=hi") != NULL);
	EXPECT(strstr(log, "child stderr: oops") != NULL);
	/* Cycle 27: TRACE log fired because we passed --log-level=4. */
	EXPECT(strstr(log, "packet loop ready") != NULL);

	int status = 0;
	kill_and_reap(&d, &status);
	EXPECT(WIFEXITED(status));
	EXPECT(WEXITSTATUS(status) == 0);

	return failures == 0 ? 0 : 1;
}
#endif /* !_WIN32 */
