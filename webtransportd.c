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
			"usage: webtransportd --version\n"
			"       webtransportd --selftest\n"
			"       webtransportd --server --cert=<pem> --key=<pem> --port=<N>\n"
			"                    [--exec=<bin>] [--log-level=<0..4>]\n");
	return 0;
}

static int cmd_selftest(void) {
	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = { 0 };
	picoquic_quic_t *quic = picoquic_create(
		8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		reset_seed, 0, NULL, NULL, NULL, 0);
	if (quic == NULL) {
		fprintf(stderr, "webtransportd: picoquic_create failed\n");
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
			buf[n] = '\0';
			fprintf(stderr, "child stderr: %.*s", (int)n, buf);
			fflush(stderr);
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

typedef struct {
	int client_reached_ready;
	const char *exec_path;
	wtd_child_t child;
	int child_spawned;
	wtd_peer_session_t peer;
	int peer_initialised;
	int reader_started;
	stderr_fwd_t stderr_fwd;
	atomic_int frames_pending;
	/* Cycle 22d: the (cnx, stream_id) pair the current payload came
	 * in on. Set by server_stream_cb, consumed by drain_outbound to
	 * echo decoded frames back. Supports one active stream for now
	 * (one client, one bidi stream); a future cycle replaces this
	 * with a per-stream peer_session_t map. */
	picoquic_cnx_t *active_cnx;
	uint64_t active_stream_id;
	int stream_seen;
} server_ctx_t;

static void on_outbound_ready(void *arg) {
	server_ctx_t *s = (server_ctx_t *)arg;
	atomic_fetch_add(&s->frames_pending, 1);
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

/* picoquic per-cnx callback. When the peer sends stream bytes, we
 * frame them (flag=0 reliable, varint len, payload) and write the
 * frame to the child's stdin_fd. Other events (stream_fin, close,
 * etc.) are ignored for now — 22c is inbound-data-only. */
static int server_stream_cb(picoquic_cnx_t *cnx, uint64_t stream_id,
		uint8_t *bytes, size_t length,
		picoquic_call_back_event_t event,
		void *callback_ctx, void *stream_ctx) {
	(void)cnx;
	(void)stream_id;
	(void)stream_ctx;
	server_ctx_t *ctx = (server_ctx_t *)callback_ctx;

	if (ctx == NULL) {
		return 0;
	}

	uint8_t flag;
	if (event == picoquic_callback_stream_data
			|| event == picoquic_callback_stream_fin) {
		/* Remember the cnx+stream regardless of payload length so
		 * even a lone FIN registers the stream echo target. */
		ctx->active_cnx = cnx;
		ctx->active_stream_id = stream_id;
		ctx->stream_seen = 1;
		flag = WTD_FRAME_FLAG_RELIABLE;
	} else if (event == picoquic_callback_datagram) {
		/* Datagrams don't carry a stream id, but we still need a
		 * cnx to echo back on via picoquic_queue_datagram_frame. */
		ctx->active_cnx = cnx;
		flag = WTD_FRAME_FLAG_UNRELIABLE;
	} else {
		return 0;
	}

	if (length == 0 || !ctx->child_spawned || ctx->child.stdin_fd < 0) {
		return 0;
	}
	uint8_t frame_buf[1 + 4 + 4096];
	if (length > sizeof(frame_buf) - 1 - 4) {
		/* Payload too large for the scratch buffer; drop for now.
		 * A production path would either stream via multiple frames
		 * or apply WT flow-control backpressure (future cycle). */
		return 0;
	}
	size_t out_len = 0;
	wtd_frame_status_t fs = wtd_frame_encode(flag,
			bytes, length, frame_buf, sizeof(frame_buf), &out_len);
	if (fs != WTD_FRAME_OK) {
		return 0;
	}
	(void)write_all(ctx->child.stdin_fd, frame_buf, out_len);
	return 0;
}

static void drain_outbound(server_ctx_t *ctx) {
	if (atomic_load(&ctx->frames_pending) == 0) {
		return;
	}
	atomic_store(&ctx->frames_pending, 0);
	wtd_outbound_frame_t *head = wtd_work_queue_drain(&ctx->peer.outbound);
	while (head != NULL) {
		wtd_outbound_frame_t *next = head->next;
		printf("outbound frame: flag=%u len=%zu payload=",
				(unsigned)head->flag, head->payload_len);
		(void)fwrite(head->payload, 1, head->payload_len, stdout);
		printf("\n");
		fflush(stdout);
		/* Cycle 22d/22e: echo the payload back on the channel it
		 * came in on. flag=0 -> same QUIC stream via
		 * picoquic_add_to_stream; flag=1 -> datagram frame via
		 * picoquic_queue_datagram_frame. set_fin=0 so further
		 * stream payloads still work; closing is the client's job. */
		if (ctx->active_cnx != NULL && head->payload_len > 0) {
			if (head->flag == WTD_FRAME_FLAG_UNRELIABLE) {
				(void)picoquic_queue_datagram_frame(
						ctx->active_cnx,
						head->payload_len, head->payload);
			} else if (ctx->stream_seen) {
				(void)picoquic_add_to_stream(
						ctx->active_cnx,
						ctx->active_stream_id,
						head->payload, head->payload_len, 0);
			}
		}
		free(head);
		head = next;
	}
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
	/* Walk connections; on the first one that reaches ready state,
	 * spawn the configured child and attach the peer_session reader
	 * + stderr forwarder. The daemon keeps running past that — only
	 * SIGTERM stops the loop (see the should_exit check above). */
	picoquic_cnx_t *cnx = picoquic_get_first_cnx(quic);
	while (cnx != NULL) {
		if (picoquic_get_cnx_state(cnx) == picoquic_state_ready) {
			if (!ctx->client_reached_ready) {
				ctx->client_reached_ready = 1;
				printf("client reached ready\n");
				fflush(stdout);
				if (ctx->exec_path != NULL && !ctx->child_spawned) {
					const char *argv[] = { ctx->exec_path, NULL };
					int rc = wtd_child_spawn(argv, NULL, &ctx->child);
					if (rc == 0) {
						ctx->child_spawned = 1;
						printf("child spawned pid=%ld\n",
								(long)ctx->child.pid);
						fflush(stdout);
						wtd_peer_session_init(&ctx->peer);
						ctx->peer_initialised = 1;
						int rr = wtd_peer_session_start_reader(
								&ctx->peer, ctx->child.stdout_fd,
								on_outbound_ready, ctx);
						if (rr == 0) {
							ctx->reader_started = 1;
						} else {
							fprintf(stderr,
									"webtransportd: start_reader rc=%d\n",
									rr);
						}
						/* Cycle 23: start stderr forwarder. */
						(void)stderr_fwd_start(&ctx->stderr_fwd,
								ctx->child.stderr_fd);
					} else {
						fprintf(stderr,
								"webtransportd: child_spawn(%s) rc=%d\n",
								ctx->exec_path, rc);
					}
				}
			}
			break;
		}
		cnx = picoquic_get_next_cnx(cnx);
	}
	drain_outbound(ctx);

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
	sctx.child.pid = -1;
	sctx.child.stdin_fd = -1;
	sctx.child.stdout_fd = -1;
	sctx.child.stderr_fd = -1;

	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = { 0 };
	/* default_callback_fn + default_callback_ctx are inherited by
	 * every new cnx picoquic accepts, so our stream_cb sees data
	 * from any client connection. */
	picoquic_quic_t *quic = picoquic_create(
			8, cert, key, NULL, "hq-test",
			server_stream_cb, &sctx, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (quic == NULL) {
		fprintf(stderr, "webtransportd: picoquic_create failed\n");
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

	/* Closing the child's stdin/stdout/stderr (via wtd_child_terminate)
	 * gives the reader + stderr-forwarder threads' blocking read()s
	 * an EOF so they exit; then the *_stop/_destroy calls join them
	 * and drain queues. Ordering: terminate child first so all three
	 * pipe fds close simultaneously, then tear down the consumers. */
	if (sctx.child_spawned) {
		wtd_child_terminate(&sctx.child);
	}
	stderr_fwd_stop(&sctx.stderr_fwd);
	if (sctx.peer_initialised) {
		drain_outbound(&sctx); /* last flush of anything buffered */
		wtd_peer_session_destroy(&sctx.peer);
	}
	picoquic_free(quic);

	/* SIGTERM interrupts the loop's recvmsg/poll with EINTR, which the
	 * packet loop surfaces as a non-zero rc. If we asked to exit, that
	 * is not an error — let the caller treat SIGTERM as a clean shutdown. */
	if (rc == PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP || rc == 0
			|| atomic_load(&g_should_exit)) {
		return 0;
	}
	fprintf(stderr, "webtransportd: packet loop exit rc=%d\n", rc);
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
		fprintf(stderr, "webtransportd: unknown argument: %s\n", argv[i]);
		(void)print_usage(stderr);
		return 2;
	}

	if (log_level_str != NULL) {
		long level = strtol(log_level_str, NULL, 10);
		if (level < WTD_LOG_QUIET || level > WTD_LOG_TRACE) {
			fprintf(stderr,
					"webtransportd: bad --log-level=%s (expected 0..4)\n",
					log_level_str);
			return 2;
		}
		wtd_log_set_level((wtd_log_level_t)level);
	}

	if (is_server) {
		if (cert == NULL || key == NULL || port_str == NULL) {
			fprintf(stderr,
					"webtransportd: --server requires --cert=, --key=, --port=\n");
			return 2;
		}
		long port = strtol(port_str, NULL, 10);
		if (port <= 0 || port > 65535) {
			fprintf(stderr, "webtransportd: bad --port=%s\n", port_str);
			return 2;
		}
		return cmd_server(cert, key, (uint16_t)port, exec_path);
	}

	(void)print_usage(stderr);
	return 2;
}
