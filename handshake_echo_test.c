/* TDD log:
 * - Cycle 22c: daemon-internal echo visible on daemon stdout.
 * - Cycle 22d: client-visible stream echo.
 * - Cycle 22e: datagram round-trip. Client enables picoquic's
 *   datagram transport parameter, sends "dgram" via
 *   picoquic_queue_datagram_frame in addition to the stream
 *   "world", and the client callback accumulates stream bytes
 *   and datagram bytes separately. Server-side: a flag=1 frame
 *   on the peer_session work queue gets echoed via
 *   picoquic_queue_datagram_frame instead of
 *   picoquic_add_to_stream.
 *
 * - Cycle 32: child switched from /bin/cat to ./examples/echo, a
 *   real C reference child that decodes framed stdin with
 *   wtd_frame_decode and re-encodes the payload with the same
 *   flag via wtd_frame_encode. Output is byte-equivalent to
 *   /bin/cat (both encoders produce shortest-form varints) but
 *   the round trip now exercises the frame codec on the child
 *   side too, proving the published framing spec matches what
 *   the codec emits.
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

static const uint16_t SERVER_PORT = 24444;
static const char PAYLOAD[] = "world";
static const char DGRAM_PAYLOAD[] = "dgram";

typedef struct {
	pid_t pid;
	int stdout_fd;
} daemon_t;

static int read_line(int fd, char *out, size_t cap, int timeout_ms) {
	size_t got = 0;
	while (got + 1 < cap) {
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
		timeout_ms -= 10;
		if (timeout_ms <= 0) {
			break;
		}
	}
	out[got] = '\0';
	return (int)got;
}

static int spawn_daemon(daemon_t *out) {
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
		snprintf(port_buf, sizeof(port_buf),
				"--port=%u", (unsigned)SERVER_PORT);
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

static void kill_and_reap(daemon_t *d, int *p_status) {
	if (d->pid > 0) {
		(void)kill(d->pid, SIGTERM);
		for (int i = 0; i < 100; i++) {
			int st = 0;
			pid_t r = waitpid(d->pid, &st, WNOHANG);
			if (r == d->pid) {
				*p_status = st;
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
	uint8_t stream_buf[128];
	size_t stream_len;
	uint8_t dgram_buf[128];
	size_t dgram_len;
} client_ctx_t;

static int client_stream_cb(picoquic_cnx_t *cnx, uint64_t stream_id,
		uint8_t *bytes, size_t length,
		picoquic_call_back_event_t event,
		void *callback_ctx, void *stream_ctx) {
	(void)cnx;
	(void)stream_id;
	(void)stream_ctx;
	client_ctx_t *c = (client_ctx_t *)callback_ctx;
	if (length == 0) {
		return 0;
	}
	if (event == picoquic_callback_stream_data
			|| event == picoquic_callback_stream_fin) {
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

/* Open a UDP socket, handshake a picoquic client against the daemon,
 * send PAYLOAD on stream 0 with FIN, and keep pumping until the
 * client receives PAYLOAD back or the wall-clock budget expires. */
static int run_client(uint16_t server_port, client_ctx_t *cctx) {
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		return -1;
	}
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
	/* Cycle 22e: enable datagrams on the client side too. */
	(void)picoquic_set_default_tp_value(quic,
			picoquic_tp_max_datagram_frame_size, 1500);
	picoquic_cnx_t *cnx = picoquic_create_client_cnx(
			quic, (struct sockaddr *)&srv,
			picoquic_current_time(), 0, "test.example",
			"hq-test", client_stream_cb, cctx);
	if (cnx == NULL) {
		picoquic_free(quic);
		close(sock);
		return -1;
	}

	uint64_t deadline = picoquic_current_time() + 5ull * 1000 * 1000;
	int ready = 0;
	int sent = 0;
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
			if (!ready) {
				ready = 1;
			}
			if (!sent) {
				/* Bidi client-initiated stream 0. FIN=1 so the
				 * server sees stream_fin straight away. */
				(void)picoquic_add_to_stream(cnx, 0,
						(const uint8_t *)PAYLOAD,
						sizeof(PAYLOAD) - 1, 1);
				/* And a datagram in the same cnx. */
				(void)picoquic_queue_datagram_frame(cnx,
						sizeof(DGRAM_PAYLOAD) - 1,
						(const uint8_t *)DGRAM_PAYLOAD);
				sent = 1;
				/* Keep pumping ~800 ms after send so the server
				 * has time to receive both, pipe through cat,
				 * decode, and write echoes back on each channel. */
				deadline = now + 800ull * 1000;
			}
		}
		if (cctx->stream_len >= sizeof(PAYLOAD) - 1
				&& cctx->dgram_len >= sizeof(DGRAM_PAYLOAD) - 1) {
			break; /* both echoes came back */
		}

		struct timespec ts = { 0, 5 * 1000 * 1000 };
		(void)nanosleep(&ts, NULL);
	}

	picoquic_free(quic);
	close(sock);
	int ok = ready && sent
			&& cctx->stream_len >= sizeof(PAYLOAD) - 1
			&& cctx->dgram_len >= sizeof(DGRAM_PAYLOAD) - 1;
	return ok ? 0 : -1;
}

int main(void) {
	daemon_t d = { -1, -1 };
	EXPECT(spawn_daemon(&d) == 0);
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

	client_ctx_t cctx = { { 0 }, 0, { 0 }, 0 };
	int cli_rc = run_client(SERVER_PORT, &cctx);
	EXPECT(cli_rc == 0);
	/* Cycle 22d: client must see its own "world" come back on stream. */
	EXPECT(cctx.stream_len == sizeof(PAYLOAD) - 1);
	EXPECT(cctx.stream_len == sizeof(PAYLOAD) - 1
			&& memcmp(cctx.stream_buf, PAYLOAD, sizeof(PAYLOAD) - 1) == 0);
	/* Cycle 22e: and "dgram" comes back on the datagram channel. */
	EXPECT(cctx.dgram_len == sizeof(DGRAM_PAYLOAD) - 1);
	EXPECT(cctx.dgram_len == sizeof(DGRAM_PAYLOAD) - 1
			&& memcmp(cctx.dgram_buf, DGRAM_PAYLOAD,
					sizeof(DGRAM_PAYLOAD) - 1) == 0);

	char log[2048];
	size_t log_len = 0;
	drain_stdout(d.stdout_fd, log, sizeof(log), &log_len, 500);
	EXPECT(strstr(log, "outbound frame: flag=0 len=5 payload=world") != NULL);
	EXPECT(strstr(log, "outbound frame: flag=1 len=5 payload=dgram") != NULL);

	int status = 0;
	kill_and_reap(&d, &status);
	EXPECT(WIFEXITED(status));
	EXPECT(WEXITSTATUS(status) == 0);

	return failures == 0 ? 0 : 1;
}
