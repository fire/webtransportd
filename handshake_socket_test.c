/* TDD log:
 * - Cycle 21d.3 (this file): real-socket WebTransport-style QUIC
 *   handshake. fork/exec ./webtransportd --server with the vendored
 *   test cert+key on a fixed loopback UDP port; read the "server
 *   ready" sentinel from the daemon's stdout; open our own UDP
 *   socket, drive a picoquic client against 127.0.0.1:<port> by
 *   pumping packets through sendto/recvfrom synchronously; assert
 *   the client connection reaches picoquic_state_ready before the
 *   wall-clock budget expires, then SIGTERM the daemon and
 *   waitpid for a clean exit.
 *
 *   Deliberately uses the synchronous single-thread packet loop on
 *   the daemon (cycle 21d.3 design note — bypasses the darwin-arm64
 *   ASAN/pthread_create crash on picoquic_start_network_thread).
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

static const uint16_t SERVER_PORT = 24443;

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

static int spawn_daemon(daemon_t *out, uint16_t port) {
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
		(void)dup2(fds[1], STDOUT_FILENO);
		close(fds[0]);
		close(fds[1]);
		char port_buf[32];
		snprintf(port_buf, sizeof(port_buf), "--port=%u", (unsigned)port);
		char *const argv[] = {
			(char *)"./webtransportd",
			(char *)"--server",
			(char *)"--cert=thirdparty/picoquic/certs/cert.pem",
			(char *)"--key=thirdparty/picoquic/certs/key.pem",
			port_buf,
			NULL,
		};
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

int main(void) {
	daemon_t d = { -1, -1 };
	EXPECT(spawn_daemon(&d, SERVER_PORT) == 0);
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

	int status = 0;
	kill_and_reap(&d, &status);
	EXPECT(WIFEXITED(status));
	EXPECT(WEXITSTATUS(status) == 0);

	return failures == 0 ? 0 : 1;
}
