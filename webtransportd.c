/*
 * webtransportd — daemon entry point.
 *
 * Cycle 19-20: argv parsing + --version.
 * Cycle 21d.1: --selftest exercises full picoquic init/teardown.
 * Cycle 21d.3: --server drives picoquic_packet_loop_v3 synchronously
 * on the main thread (no background pthread — sidesteps the
 * darwin-arm64 ASAN/pthread_create crash documented in 21d.1). A
 * SIGTERM handler flips an atomic `should_exit` flag that the loop
 * callback checks to return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP.
 * Cycle 22a: --exec=BIN spawns the configured child the first time
 * a connection reaches ready state; the child is reaped when the
 * daemon tears down. Proves child_process.c works from inside the
 * real server pipeline (not just a unit test harness).
 * Cycle 22b: wtd_peer_session reader thread attaches to the spawned
 * child's stdout_fd; every complete frame the child writes lands on
 * the work queue, and the picoquic loop callback drains the queue
 * on each iteration, printing "outbound frame: <payload>" lines.
 * Cycle 22c: server_stream_cb frames picoquic_callback_stream_data
 * bytes and writes them into child.stdin_fd. With `--exec=/bin/cat`
 * this completes the full daemon-internal round-trip: client bytes
 * -> daemon frames -> child stdin -> cat echoes -> daemon reader
 * decodes -> work queue -> loop log.
 * Cycle 22d: drain_outbound now also calls picoquic_add_to_stream
 * on the first cnx/stream that arrived, so the decoded payload
 * goes back to the client on the same QUIC stream. The round-trip
 * is now visible to the client.
 * Cycle 22e: datagram symmetry. max_datagram_frame_size is set on
 * the quic context so datagrams are negotiated; picoquic_callback_
 * datagram frames bytes with flag=1 and writes them to the child;
 * drain_outbound routes flag=1 frames to picoquic_queue_datagram_
 * frame instead of picoquic_add_to_stream.
 * Cycle 23: a small forwarder thread reads the child's stderr_fd
 * and emits each chunk to the daemon's own stderr, prefixed with
 * "child stderr: ". Previously stderr_fd was opened by
 * wtd_child_spawn but never read.
 * Cycle 26: removed the "exit 1s after first ready" timer that was
 * a testing convenience. The daemon now runs until SIGTERM/SIGINT
 * flips g_should_exit; tests were already sending SIGTERM at the
 * end of their scenarios so nothing observable changed for them.
 * Cycle 27: --log-level=<0..4> wires log.c into the daemon.
 * Cycle 28: every fprintf(stderr, ...) error site and the stderr
 * forwarder now go through wtd_log. Gives us the mutex protection
 * against log-line interleaving from the forwarder thread, and
 * picks up cycle 28's [LEVEL] prefix so a human reading stderr can
 * pick out [ERROR] lines from the routine [INFO] "child stderr:"
 * stream.
 * Cycle 29: per-cnx state split. server_ctx_t holds a linked list
 * of wtd_peer_t; each cnx gets its own spawned child, peer_session
 * reader, and stderr forwarder. server_stream_cb looks up (or
 * creates) the peer for the inbound cnx by pointer identity and
 * routes echoes back through that peer only. Two concurrent clients
 * no longer share state; they each see their own bytes come back.
 */

#include "version.h"

#include "child_process.h"
#include "frame.h"
#include "log.h"
#include "peer_session.h"
#include "picoquic.h"
#include "picoquic_packet_loop.h"

#include <errno.h>
#include <pthread.h>

#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static atomic_int g_should_exit = 0;

static void on_sigterm(int sig) {
	(void)sig;
	atomic_store(&g_should_exit, 1);
}

static int print_usage(FILE *out) {
	fprintf(out,
			"webtransportd: a WebTransport/QUIC daemon that exec's a child\n"
			"  process per accepted session and pipes data between the WT\n"
			"  session and the child's stdin/stdout. Modeled on websocketd.\n"
			"\n"
			"usage:\n"
			"  webtransportd --version\n"
			"      Print version string and exit.\n"
			"  webtransportd --selftest\n"
			"      Exercise the picoquic + mbedtls TLS init path and exit.\n"
			"  webtransportd --server --cert=<pem> --key=<pem> --port=<N> [options]\n"
			"      Listen for QUIC connections on UDP port <N>, TLS via\n"
			"      <cert>+<key>. Runs until SIGTERM/SIGINT.\n"
			"      options:\n"
			"        --exec=<bin>        Spawn <bin> on each accepted\n"
			"                            connection; frame its stdin/stdout.\n"
			"        --log-level=<0..4>  QUIET / ERROR / WARN / INFO / TRACE\n"
			"                            (default INFO).\n"
			"\n"
			"framing: bytes on the child's stdin and stdout are\n"
			"  [flag | varint len | payload]. flag bit 0 selects reliable\n"
			"  (0, WT stream) vs unreliable (1, WT datagram); bits 1-7 are\n"
			"  reserved and must be zero.\n");
	return 0;
}

static int cmd_selftest(void) {
	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = { 0 };
	picoquic_quic_t *quic = picoquic_create(
		8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		reset_seed, 0, NULL, NULL, NULL, 0);
	if (quic == NULL) {
		wtd_log(WTD_LOG_ERROR, "webtransportd: picoquic_create failed");
		return 1;
	}
	picoquic_free(quic);
	printf("selftest ok\n");
	return 0;
}

/* Cycle 23: child-stderr forwarder. A dedicated pthread reads from
 * child.stderr_fd and emits each chunk to the daemon's own stderr.
 * Exits on read() returning 0 (EOF, fired when wtd_child_terminate
 * closes the fd) or an error; stderr_fwd_stop joins it afterwards. */
typedef struct {
	pthread_t thread;
	int fd;
	int started;
} stderr_fwd_t;

static void *stderr_fwd_loop(void *arg) {
	stderr_fwd_t *f = (stderr_fwd_t *)arg;
	char buf[512];
	for (;;) {
		ssize_t n = read(f->fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			/* Strip one trailing newline so wtd_log's own "\n" on the
			 * line terminator doesn't turn every child log line into
			 * two lines. Any earlier newlines in the chunk stay. */
			if (buf[n - 1] == '\n') {
				n--;
			}
			buf[n] = '\0';
			/* wtd_log is mutex-guarded; fprintf from multiple threads
			 * can interleave. This is the thread-safety payoff for
			 * cycle 28's refactor. */
			wtd_log(WTD_LOG_INFO, "child stderr: %s", buf);
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			break; /* EOF or hard error */
		}
	}
	return NULL;
}

static int stderr_fwd_start(stderr_fwd_t *f, int fd) {
	f->fd = fd;
	f->started = 0;
	int rc = pthread_create(&f->thread, NULL, stderr_fwd_loop, f);
	if (rc != 0) {
		return -rc;
	}
	f->started = 1;
	return 0;
}

static void stderr_fwd_stop(stderr_fwd_t *f) {
	if (f->started) {
		(void)pthread_join(f->thread, NULL);
		f->started = 0;
	}
}

/* Cycle 29: per-cnx state. The daemon holds a linked list of these
 * in server_ctx_t.peers; each cnx the server accepts gets its own
 * spawned child, peer_session, stderr forwarder, and echo target. */
typedef struct wtd_peer {
	struct wtd_peer *next;
	picoquic_cnx_t *cnx; /* key — compared by pointer identity */
	wtd_child_t child;
	wtd_peer_session_t peer_session;
	stderr_fwd_t stderr_fwd;
	uint64_t active_stream_id;
	atomic_int frames_pending;
	int child_spawned;
	int peer_initialised;
	int reader_started;
	int stream_seen;
} wtd_peer_t;

typedef struct {
	int client_reached_ready;
	const char *exec_path;
	wtd_peer_t *peers;
} server_ctx_t;

static void on_outbound_ready(void *arg) {
	wtd_peer_t *p = (wtd_peer_t *)arg;
	atomic_fetch_add(&p->frames_pending, 1);
}

static wtd_peer_t *peer_find(server_ctx_t *s, picoquic_cnx_t *cnx) {
	for (wtd_peer_t *p = s->peers; p != NULL; p = p->next) {
		if (p->cnx == cnx) {
			return p;
		}
	}
	return NULL;
}

/* Allocate a new peer, spawn its child (if exec_path is set), wire
 * the reader thread and stderr forwarder, and prepend to the list.
 * Returns NULL only on OOM; spawn failures log ERROR and still
 * return a peer so future stream data at least has a lookup target. */
static wtd_peer_t *peer_create(server_ctx_t *s, picoquic_cnx_t *cnx) {
	wtd_peer_t *p = (wtd_peer_t *)calloc(1, sizeof(*p));
	if (p == NULL) {
		return NULL;
	}
	p->cnx = cnx;
	p->child.pid = WTD_CHILD_PID_NONE;
	p->child.stdin_fd = -1;
	p->child.stdout_fd = -1;
	p->child.stderr_fd = -1;

	if (s->exec_path != NULL) {
		const char *argv[] = { s->exec_path, NULL };
		int rc = wtd_child_spawn(argv, NULL, &p->child);
		if (rc == 0) {
			p->child_spawned = 1;
			/* pid is pid_t on POSIX and a HANDLE (void*) on Win32;
			 * printing as an intptr_t-cast long long works for both
			 * and keeps the test sentinel ("child spawned pid=...")
			 * byte-for-byte stable. */
			printf("child spawned pid=%lld\n",
					(long long)(intptr_t)p->child.pid);
			fflush(stdout);
			wtd_peer_session_init(&p->peer_session);
			p->peer_initialised = 1;
			int rr = wtd_peer_session_start_reader(
					&p->peer_session, p->child.stdout_fd,
					on_outbound_ready, p);
			if (rr == 0) {
				p->reader_started = 1;
			} else {
				wtd_log(WTD_LOG_ERROR,
						"webtransportd: start_reader rc=%d", rr);
			}
			(void)stderr_fwd_start(&p->stderr_fwd, p->child.stderr_fd);
		} else {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: child_spawn(%s) rc=%d",
					s->exec_path, rc);
		}
	}
	p->next = s->peers;
	s->peers = p;
	return p;
}

static void drain_peer(wtd_peer_t *p) {
	if (atomic_load(&p->frames_pending) == 0) {
		return;
	}
	atomic_store(&p->frames_pending, 0);
	wtd_outbound_frame_t *head = wtd_work_queue_drain(&p->peer_session.outbound);
	while (head != NULL) {
		wtd_outbound_frame_t *next = head->next;
		printf("outbound frame: flag=%u len=%zu payload=",
				(unsigned)head->flag, head->payload_len);
		(void)fwrite(head->payload, 1, head->payload_len, stdout);
		printf("\n");
		fflush(stdout);
		if (p->cnx != NULL && head->payload_len > 0) {
			if (head->flag == WTD_FRAME_FLAG_UNRELIABLE) {
				(void)picoquic_queue_datagram_frame(
						p->cnx, head->payload_len, head->payload);
			} else if (p->stream_seen) {
				(void)picoquic_add_to_stream(
						p->cnx, p->active_stream_id,
						head->payload, head->payload_len, 0);
			}
		}
		free(head);
		head = next;
	}
}

static void drain_all_peers(server_ctx_t *s) {
	for (wtd_peer_t *p = s->peers; p != NULL; p = p->next) {
		drain_peer(p);
	}
}

/* Tear down one peer: terminate child (closes all 3 pipe fds, which
 * wakes the reader + stderr forwarder to EOF), stop the forwarder,
 * drain+destroy the peer_session, free the struct. */
static void peer_destroy(wtd_peer_t *p) {
	if (p->child_spawned) {
		wtd_child_terminate(&p->child);
	}
	stderr_fwd_stop(&p->stderr_fwd);
	if (p->peer_initialised) {
		drain_peer(p); /* flush anything buffered */
		wtd_peer_session_destroy(&p->peer_session);
	}
	free(p);
}

static void peer_destroy_all(server_ctx_t *s) {
	wtd_peer_t *p = s->peers;
	while (p != NULL) {
		wtd_peer_t *next = p->next;
		peer_destroy(p);
		p = next;
	}
	s->peers = NULL;
}

/* Write every byte in buf[0..len) to fd, retrying short writes and
 * EINTR. Returns 0 on success, -errno on hard failure. */
static int write_all(int fd, const uint8_t *buf, size_t len) {
	size_t done = 0;
	while (done < len) {
		ssize_t n = write(fd, buf + done, len - done);
		if (n > 0) {
			done += (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return -errno;
		}
	}
	return 0;
}

/* picoquic per-cnx callback. Finds (or creates) the wtd_peer_t for
 * this cnx, frames the incoming bytes and writes them to that peer's
 * child.stdin_fd. Each cnx gets its own child, so echoes can never
 * be misrouted between concurrent clients. */
static int server_stream_cb(picoquic_cnx_t *cnx, uint64_t stream_id,
		uint8_t *bytes, size_t length,
		picoquic_call_back_event_t event,
		void *callback_ctx, void *stream_ctx) {
	(void)stream_ctx;
	server_ctx_t *ctx = (server_ctx_t *)callback_ctx;
	if (ctx == NULL) {
		return 0;
	}

	uint8_t flag;
	if (event == picoquic_callback_stream_data
			|| event == picoquic_callback_stream_fin) {
		flag = WTD_FRAME_FLAG_RELIABLE;
	} else if (event == picoquic_callback_datagram) {
		flag = WTD_FRAME_FLAG_UNRELIABLE;
	} else {
		return 0;
	}

	wtd_peer_t *p = peer_find(ctx, cnx);
	if (p == NULL) {
		/* Very early data can arrive before server_loop_cb has noticed
		 * the cnx reached ready (both run on the same thread, but the
		 * loop callback can fire for unrelated events first). Create
		 * the peer on demand so inbound data is never silently lost. */
		p = peer_create(ctx, cnx);
		if (p == NULL) {
			return 0;
		}
	}

	if (flag == WTD_FRAME_FLAG_RELIABLE) {
		p->active_stream_id = stream_id;
		p->stream_seen = 1;
	}

	if (length == 0 || !p->child_spawned || p->child.stdin_fd < 0) {
		return 0;
	}
	uint8_t frame_buf[1 + 4 + 4096];
	if (length > sizeof(frame_buf) - 1 - 4) {
		return 0;
	}
	size_t out_len = 0;
	wtd_frame_status_t fs = wtd_frame_encode(flag, bytes, length,
			frame_buf, sizeof(frame_buf), &out_len);
	if (fs != WTD_FRAME_OK) {
		return 0;
	}
	(void)write_all(p->child.stdin_fd, frame_buf, out_len);
	return 0;
}

static int server_loop_cb(picoquic_quic_t *quic,
		picoquic_packet_loop_cb_enum cb,
		void *cb_ctx, void *cb_arg) {
	(void)cb_arg;
	server_ctx_t *ctx = (server_ctx_t *)cb_ctx;

	if (cb == picoquic_packet_loop_ready) {
		printf("server ready\n");
		fflush(stdout);
		/* Cycle 27: TRACE-level detail only emits when the operator
		 * asks for it via --log-level=4. */
		wtd_log(WTD_LOG_TRACE, "packet loop ready");
		return 0;
	}
	if (atomic_load(&g_should_exit)) {
		return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
	}
	/* Walk every connection: for each that has reached ready, ensure
	 * a wtd_peer_t exists (spawning a child + starting the reader on
	 * first sighting). The daemon keeps running past the first ready
	 * — only SIGTERM stops the loop. */
	picoquic_cnx_t *cnx = picoquic_get_first_cnx(quic);
	while (cnx != NULL) {
		if (picoquic_get_cnx_state(cnx) == picoquic_state_ready
				&& peer_find(ctx, cnx) == NULL) {
			(void)peer_create(ctx, cnx);
			if (!ctx->client_reached_ready) {
				ctx->client_reached_ready = 1;
				printf("client reached ready\n");
				fflush(stdout);
			}
		}
		cnx = picoquic_get_next_cnx(cnx);
	}
	drain_all_peers(ctx);

	return 0;
}

static int cmd_server(const char *cert, const char *key, uint16_t port,
		const char *exec_path) {
	struct sigaction sa = { 0 };
	sa.sa_handler = on_sigterm;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	server_ctx_t sctx = { 0 };
	sctx.exec_path = exec_path;

	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = { 0 };
	/* default_callback_fn + default_callback_ctx are inherited by
	 * every new cnx picoquic accepts, so our stream_cb sees data
	 * from any client connection. */
	picoquic_quic_t *quic = picoquic_create(
			8, cert, key, NULL, "hq-test",
			server_stream_cb, &sctx, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (quic == NULL) {
		wtd_log(WTD_LOG_ERROR, "webtransportd: picoquic_create failed");
		return 1;
	}
	/* Cycle 22e: enable QUIC datagrams. 1500 ≈ ethernet MTU — any
	 * non-zero value advertises support in the transport params. */
	(void)picoquic_set_default_tp_value(quic,
			picoquic_tp_max_datagram_frame_size, 1500);
	picoquic_packet_loop_param_t param = { 0 };
	param.local_af = AF_INET;
	param.local_port = port;

	picoquic_network_thread_ctx_t tctx = { 0 };
	tctx.quic = quic;
	tctx.param = &param;
	tctx.loop_callback = server_loop_cb;
	tctx.loop_callback_ctx = &sctx;

	(void)picoquic_packet_loop_v3(&tctx);
	int rc = tctx.return_code;

	/* Tear down every peer: each terminate closes the child's pipes,
	 * reader + stderr forwarder see EOF, threads join, final queue
	 * drain echoes any buffered frames (best-effort), free the
	 * wtd_peer_t. Order matters: destroy peers before picoquic_free
	 * because peer_destroy may call picoquic_add_to_stream on their
	 * cnx, which must still be attached to the quic context. */
	peer_destroy_all(&sctx);
	picoquic_free(quic);

	/* SIGTERM interrupts the loop's recvmsg/poll with EINTR, which the
	 * packet loop surfaces as a non-zero rc. If we asked to exit, that
	 * is not an error — let the caller treat SIGTERM as a clean shutdown. */
	if (rc == PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP || rc == 0
			|| atomic_load(&g_should_exit)) {
		return 0;
	}
	wtd_log(WTD_LOG_ERROR, "webtransportd: packet loop exit rc=%d", rc);
	return 1;
}

static int parse_arg_value(const char *arg, const char *prefix,
		const char **out) {
	size_t plen = strlen(prefix);
	if (strncmp(arg, prefix, plen) == 0) {
		*out = arg + plen;
		return 1;
	}
	return 0;
}

int main(int argc, char **argv) {
	int is_server = 0;
	const char *cert = NULL;
	const char *key = NULL;
	const char *port_str = NULL;
	const char *exec_path = NULL;
	const char *log_level_str = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0) {
			printf("webtransportd %s\n", WTD_VERSION);
			return 0;
		}
		if (strcmp(argv[i], "--selftest") == 0) {
			return cmd_selftest();
		}
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			(void)print_usage(stdout);
			return 0;
		}
		if (strcmp(argv[i], "--server") == 0) {
			is_server = 1;
			continue;
		}
		if (parse_arg_value(argv[i], "--cert=", &cert)) {
			continue;
		}
		if (parse_arg_value(argv[i], "--key=", &key)) {
			continue;
		}
		if (parse_arg_value(argv[i], "--port=", &port_str)) {
			continue;
		}
		if (parse_arg_value(argv[i], "--exec=", &exec_path)) {
			continue;
		}
		if (parse_arg_value(argv[i], "--log-level=", &log_level_str)) {
			continue;
		}
		wtd_log(WTD_LOG_ERROR,
				"webtransportd: unknown argument: %s", argv[i]);
		(void)print_usage(stderr);
		return 2;
	}

	if (log_level_str != NULL) {
		long level = strtol(log_level_str, NULL, 10);
		if (level < WTD_LOG_QUIET || level > WTD_LOG_TRACE) {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: bad --log-level=%s (expected 0..4)",
					log_level_str);
			return 2;
		}
		wtd_log_set_level((wtd_log_level_t)level);
	}

	if (is_server) {
		if (cert == NULL || key == NULL || port_str == NULL) {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: --server requires --cert=, --key=, --port=");
			return 2;
		}
		long port = strtol(port_str, NULL, 10);
		if (port <= 0 || port > 65535) {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: bad --port=%s", port_str);
			return 2;
		}
		return cmd_server(cert, key, (uint16_t)port, exec_path);
	}

	(void)print_usage(stderr);
	return 2;
}
