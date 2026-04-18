/* SPDX-License-Identifier: BSD-2-Clause */
/* HTTP/3 version of echo test */

#ifdef _WIN32
#include <stdio.h>
int main(void) {
	fprintf(stderr, "SKIP: POSIX-only test on Windows\n");
	return 0;
}
#else

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

#define PAYLOAD "world"
#define DGRAM_PAYLOAD "dgram"

static uint16_t SERVER_PORT;

typedef struct {
	pid_t pid;
	int stdout_fd;
} daemon_t;

static int read_line(int fd, char *out, size_t cap, int timeout_ms) {
	size_t got = 0;
	while (got + 1 < cap && timeout_ms > 0) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		struct timeval tv = { 0, 10 * 1000 };
		int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (sel < 0 && errno == EINTR) continue;
		if (sel > 0) {
			char c;
			ssize_t n = read(fd, &c, 1);
			if (n == 1) {
				out[got++] = c;
				if (c == '\n') break;
			} else if (n == 0) {
				break;
			}
		}
		timeout_ms -= 10;
	}
	out[got] = '\0';
	return (int)got;
}

static int spawn_daemon(daemon_t *out) {
	int fds[2];
	if (pipe(fds) != 0) return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		close(fds[0]);
		close(fds[1]);
		char port_buf[32];
		snprintf(port_buf, sizeof(port_buf), "--port=%u",
				(unsigned)SERVER_PORT);
		char *const argv[] = {
			(char *)"./webtransportd",
			(char *)"--server",
			(char *)"--cert=thirdparty/picoquic/certs/cert.pem",
			(char *)"--key=thirdparty/picoquic/certs/key.pem",
			port_buf,
			(char *)"--exec=./examples/echo",
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
	while (timeout_ms > 0) {
		int n = read_line(d->stdout_fd, line, sizeof(line), 500);
		if (n > 0 && strstr(line, "server ready")) return 0;
		timeout_ms -= 500;
	}
	return -1;
}

static void kill_and_reap(daemon_t *d, int *p_status) {
	if (d->pid > 0) {
		kill(d->pid, SIGTERM);
		for (int i = 0; i < 100; i++) {
			int st = 0;
			pid_t r = waitpid(d->pid, &st, WNOHANG);
			if (r == d->pid) {
				*p_status = st;
				d->pid = -1;
				break;
			}
			usleep(20000);
		}
		if (d->pid > 0) {
			kill(d->pid, SIGKILL);
			int st = 0;
			waitpid(d->pid, &st, 0);
			*p_status = st;
			d->pid = -1;
		}
	}
	if (d->stdout_fd >= 0) {
		close(d->stdout_fd);
		d->stdout_fd = -1;
	}
}

typedef struct {
	uint8_t stream_buf[256];
	size_t stream_len;
	uint8_t dgram_buf[256];
	size_t dgram_len;
} client_ctx_t;

static int client_callback(picoquic_cnx_t *cnx, uint64_t stream_id,
		uint8_t *bytes, size_t length,
		picoquic_call_back_event_t event,
		void *callback_ctx, void *stream_ctx) {
	(void)cnx;
	(void)stream_id;
	(void)stream_ctx;
	client_ctx_t *c = (client_ctx_t *)callback_ctx;
	if (!c || length == 0) return 0;

	if (event == picoquic_callback_stream_data ||
			event == picoquic_callback_stream_fin) {
		if (c->stream_len + length <= sizeof(c->stream_buf)) {
			memcpy(c->stream_buf + c->stream_len, bytes, length);
			c->stream_len += length;
		}
	} else if (event == picoquic_callback_datagram) {
		if (c->dgram_len + length <= sizeof(c->dgram_buf)) {
			memcpy(c->dgram_buf + c->dgram_len, bytes, length);
			c->dgram_len += length;
		}
	}
	return 0;
}

static int run_client(uint16_t port, client_ctx_t *cctx) {
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) return -1;
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);

	struct sockaddr_in cli = {0};
	cli.sin_family = AF_INET;
	cli.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	cli.sin_port = 0;
	if (bind(sock, (struct sockaddr *)&cli, sizeof(cli)) != 0) {
		close(sock);
		return -1;
	}

	struct sockaddr_in srv = {0};
	srv.sin_family = AF_INET;
	srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	srv.sin_port = htons(port);

	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = {0};
	picoquic_quic_t *quic = picoquic_create(
			4, NULL, NULL, NULL, "h3",
			NULL, NULL, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (!quic) {
		close(sock);
		return -1;
	}

	picoquic_set_default_tp_value(quic,
			picoquic_tp_max_datagram_frame_size, 1500);

	picoquic_cnx_t *cnx = picoquic_create_client_cnx(
			quic, (struct sockaddr *)&srv,
			picoquic_current_time(), 0, "test.example",
			"h3", client_callback, cctx);
	if (!cnx) {
		picoquic_free(quic);
		close(sock);
		return -1;
	}

	int ready = 0, sent = 0;
	uint64_t deadline = picoquic_current_time() + 8ull * 1000 * 1000;
	uint8_t buf[2048];

	while (picoquic_current_time() < deadline) {
		uint64_t now = picoquic_current_time();
		size_t len = 0;
		struct sockaddr_storage addr_to, addr_from;
		int if_idx = 0;
		picoquic_connection_id_t log_cid;
		picoquic_cnx_t *last = NULL;

		picoquic_prepare_next_packet(quic, now, buf, sizeof(buf),
				&len, &addr_to, &addr_from, &if_idx, &log_cid, &last);
		if (len > 0) {
			sendto(sock, buf, len, 0,
					(struct sockaddr *)&srv, sizeof(srv));
		}

		for (int i = 0; i < 8; i++) {
			struct sockaddr_in from;
			socklen_t flen = sizeof(from);
			ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
					(struct sockaddr *)&from, &flen);
			if (n <= 0) break;

			struct sockaddr_in me = {0};
			me.sin_family = AF_INET;
			me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			socklen_t mlen = sizeof(me);
			getsockname(sock, (struct sockaddr *)&me, &mlen);

			picoquic_incoming_packet(quic, buf, (size_t)n,
					(struct sockaddr *)&from,
					(struct sockaddr *)&me,
					0, 0, picoquic_current_time());
		}

		if (picoquic_get_cnx_state(cnx) == picoquic_state_ready) {
			ready = 1;
			if (!sent) {
				picoquic_add_to_stream(cnx, 0,
						(uint8_t *)PAYLOAD,
						strlen(PAYLOAD), 0);
				picoquic_queue_datagram_frame(cnx,
						strlen(DGRAM_PAYLOAD),
						(uint8_t *)DGRAM_PAYLOAD);
				sent = 1;
			}
		}

		if (sent && cctx->stream_len >= strlen(PAYLOAD) &&
				cctx->dgram_len >= strlen(DGRAM_PAYLOAD)) {
			break;
		}

		usleep(5000);
	}

	picoquic_free(quic);
	close(sock);

	return (ready && sent && cctx->stream_len >= strlen(PAYLOAD) &&
			cctx->dgram_len >= strlen(DGRAM_PAYLOAD)) ? 0 : -1;
}

int main(void) {
	SERVER_PORT = 20000 + (getpid() & 0x1fff);
	daemon_t d = {-1, -1};

	EXPECT(spawn_daemon(&d) == 0);
	if (d.pid < 0) return 1;

	EXPECT(wait_for_ready(&d, 5000) == 0);
	if (wait_for_ready(&d, 5000) != 0) {
		int st = 0;
		kill_and_reap(&d, &st);
		return 1;
	}

	client_ctx_t cctx = {0};
	int cli_rc = run_client(SERVER_PORT, &cctx);
	EXPECT(cli_rc == 0);
	EXPECT(cctx.stream_len >= strlen(PAYLOAD));
	EXPECT(memcmp(cctx.stream_buf, PAYLOAD, strlen(PAYLOAD)) == 0);
	EXPECT(cctx.dgram_len >= strlen(DGRAM_PAYLOAD));
	EXPECT(memcmp(cctx.dgram_buf, DGRAM_PAYLOAD, strlen(DGRAM_PAYLOAD)) == 0);

	int status = 0;
	kill_and_reap(&d, &status);
	EXPECT(WIFEXITED(status));
	EXPECT(WEXITSTATUS(status) == 0);

	return failures == 0 ? 0 : 1;
}

#endif
