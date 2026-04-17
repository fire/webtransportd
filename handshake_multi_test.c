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
 * - Cycle 29 (this file): two concurrent clients against one daemon.
 *   Today's daemon (pre-29) keeps one active_cnx / one spawned child
 *   / one peer_session in server_ctx_t, so the second handshake
 *   overwrites the first. Each client's echo would end up going
 *   back out on whichever cnx was last seen, corrupting both.
 *
 *   The fix is a per-cnx `wtd_peer_t` keyed by picoquic_cnx_t*: the
 *   daemon spawns a dedicated child (and starts a dedicated
 *   peer_session reader) for each cnx on first stream_data or
 *   datagram, and routes echoes back through the peer that owns
 *   that cnx.
 *
 *   The test launches ./webtransportd --server --exec=/bin/cat,
 *   creates two picoquic clients on separate UDP sockets, sends
 *   "alpha" on one and "bravo" on the other, pumps both until each
 *   has received its own payload back, and asserts that neither
 *   client saw the other's bytes anywhere. Also asserts the daemon
 *   stdout log shows both outbound frames.
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

/* Cycle 33: pid-derived port, see handshake_socket_test.c banner. */
static uint16_t SERVER_PORT;

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
		(void)dup2(fds[1], STDERR_FILENO);
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
			(char *)"--exec=/bin/cat",
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

/* One logical client: UDP socket + quic context + cnx + the bytes we
 * sent and the bytes we've received back so far. */
typedef struct {
	int sock;
	struct sockaddr_in srv;
	picoquic_quic_t *quic;
	picoquic_cnx_t *cnx;
	const char *payload;
	size_t payload_len;
	uint8_t recv_buf[128];
	size_t recv_len;
	int sent;
} client_t;

static int client_stream_cb(picoquic_cnx_t *cnx, uint64_t stream_id,
		uint8_t *bytes, size_t length,
		picoquic_call_back_event_t event,
		void *callback_ctx, void *stream_ctx) {
	(void)cnx;
	(void)stream_id;
	(void)stream_ctx;
	client_t *c = (client_t *)callback_ctx;
	if (length == 0) {
		return 0;
	}
	if (event == picoquic_callback_stream_data
			|| event == picoquic_callback_stream_fin) {
		if (c->recv_len + length <= sizeof(c->recv_buf)) {
			memcpy(c->recv_buf + c->recv_len, bytes, length);
			c->recv_len += length;
		}
	}
	return 0;
}

static int client_init(client_t *c, uint16_t server_port,
		const char *payload) {
	memset(c, 0, sizeof(*c));
	c->payload = payload;
	c->payload_len = strlen(payload);

	c->sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (c->sock < 0) {
		return -1;
	}
	int fl = fcntl(c->sock, F_GETFL, 0);
	fcntl(c->sock, F_SETFL, fl | O_NONBLOCK);

	struct sockaddr_in me = { 0 };
	me.sin_family = AF_INET;
	me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	me.sin_port = 0;
	if (bind(c->sock, (struct sockaddr *)&me, sizeof(me)) != 0) {
		close(c->sock);
		return -1;
	}

	c->srv.sin_family = AF_INET;
	c->srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	c->srv.sin_port = htons(server_port);

	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = { 0 };
	c->quic = picoquic_create(
			4, NULL, NULL, NULL, "hq-test",
			NULL, NULL, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (c->quic == NULL) {
		close(c->sock);
		return -1;
	}
	c->cnx = picoquic_create_client_cnx(
			c->quic, (struct sockaddr *)&c->srv,
			picoquic_current_time(), 0, "test.example",
			"hq-test", client_stream_cb, c);
	if (c->cnx == NULL) {
		picoquic_free(c->quic);
		close(c->sock);
		return -1;
	}
	return 0;
}

static void client_pump(client_t *c) {
	uint8_t buf[2048];
	uint64_t now = picoquic_current_time();

	/* client -> server */
	size_t send_len = 0;
	struct sockaddr_storage sto, sfrom;
	int if_idx = 0;
	picoquic_connection_id_t log_cid;
	picoquic_cnx_t *last = NULL;
	int rc = picoquic_prepare_next_packet(c->quic, now, buf, sizeof(buf),
			&send_len, &sto, &sfrom, &if_idx, &log_cid, &last);
	if (rc == 0 && send_len > 0) {
		(void)sendto(c->sock, buf, send_len, 0,
				(struct sockaddr *)&c->srv, sizeof(c->srv));
	}

	/* server -> client (drain any pending) */
	for (int i = 0; i < 8; i++) {
		struct sockaddr_in from;
		socklen_t fromlen = sizeof(from);
		ssize_t n = recvfrom(c->sock, buf, sizeof(buf), 0,
				(struct sockaddr *)&from, &fromlen);
		if (n <= 0) {
			break;
		}
		struct sockaddr_in my = { 0 };
		my.sin_family = AF_INET;
		my.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t mylen = sizeof(my);
		getsockname(c->sock, (struct sockaddr *)&my, &mylen);
		picoquic_incoming_packet(c->quic, buf, (size_t)n,
				(struct sockaddr *)&from,
				(struct sockaddr *)&my,
				0, 0, picoquic_current_time());
	}

	/* Once the handshake finishes, send the payload on stream 0. */
	if (!c->sent
			&& picoquic_get_cnx_state(c->cnx) == picoquic_state_ready) {
		(void)picoquic_add_to_stream(c->cnx, 0,
				(const uint8_t *)c->payload, c->payload_len, 1);
		c->sent = 1;
	}
}

static void client_close(client_t *c) {
	if (c->quic != NULL) {
		picoquic_free(c->quic);
		c->quic = NULL;
	}
	if (c->sock >= 0) {
		close(c->sock);
		c->sock = -1;
	}
}

int main(void) {
	SERVER_PORT = (uint16_t)(20000 + (getpid() & 0x1fff));
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

	/* Single-letter payloads of distinct characters so the
	 * "none of the other's bytes leaked through" check is a
	 * clean byte-set comparison. Distinct sentinels also appear
	 * unambiguously in the daemon's outbound-frame log. */
	client_t a = { 0 }, b = { 0 };
	EXPECT(client_init(&a, SERVER_PORT, "aaaaa") == 0);
	EXPECT(client_init(&b, SERVER_PORT, "bbbbb") == 0);

	/* Pump until both clients have received their own payload back,
	 * or the wall-clock budget expires. */
	uint64_t deadline = picoquic_current_time() + 8ull * 1000 * 1000;
	while (picoquic_current_time() < deadline) {
		client_pump(&a);
		client_pump(&b);
		if (a.recv_len >= a.payload_len && b.recv_len >= b.payload_len) {
			break;
		}
		struct timespec ts = { 0, 2 * 1000 * 1000 };
		(void)nanosleep(&ts, NULL);
	}

	EXPECT(a.recv_len == a.payload_len);
	EXPECT(b.recv_len == b.payload_len);
	EXPECT(a.recv_len == a.payload_len
			&& memcmp(a.recv_buf, a.payload, a.payload_len) == 0);
	EXPECT(b.recv_len == b.payload_len
			&& memcmp(b.recv_buf, b.payload, b.payload_len) == 0);
	/* Neither client should have received any of the other's bytes. */
	EXPECT(memchr(a.recv_buf, 'b', a.recv_len) == NULL);
	EXPECT(memchr(b.recv_buf, 'a', b.recv_len) == NULL);

	char log[4096];
	size_t log_len = 0;
	drain_stdout(d.stdout_fd, log, sizeof(log), &log_len, 500);
	EXPECT(strstr(log, "outbound frame: flag=0 len=5 payload=aaaaa") != NULL);
	EXPECT(strstr(log, "outbound frame: flag=0 len=5 payload=bbbbb") != NULL);

	client_close(&a);
	client_close(&b);

	int status = 0;
	kill_and_reap(&d, &status);
	EXPECT(WIFEXITED(status));
	EXPECT(WEXITSTATUS(status) == 0);

	return failures == 0 ? 0 : 1;
}
#endif /* !_WIN32 */
