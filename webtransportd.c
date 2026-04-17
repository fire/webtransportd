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
 */

#include "version.h"

#include "child_process.h"
#include "peer_session.h"
#include "picoquic.h"
#include "picoquic_packet_loop.h"

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
			"       webtransportd --server --cert=<pem> --key=<pem> --port=<N> [--exec=<bin>]\n");
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

typedef struct {
	int client_reached_ready;
	uint64_t ready_at_us;
	const char *exec_path;
	wtd_child_t child;
	int child_spawned;
	wtd_peer_session_t peer;
	int peer_initialised;
	int reader_started;
	atomic_int frames_pending;
} server_ctx_t;

static void on_outbound_ready(void *arg) {
	server_ctx_t *s = (server_ctx_t *)arg;
	atomic_fetch_add(&s->frames_pending, 1);
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
		return 0;
	}
	if (atomic_load(&g_should_exit)) {
		return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
	}
	/* Walk connections; note the first one that reaches ready state
	 * and exit ~200ms later so pending acks/handshake-done flush out. */
	picoquic_cnx_t *cnx = picoquic_get_first_cnx(quic);
	while (cnx != NULL) {
		if (picoquic_get_cnx_state(cnx) == picoquic_state_ready) {
			if (!ctx->client_reached_ready) {
				ctx->client_reached_ready = 1;
				ctx->ready_at_us = picoquic_get_quic_time(quic);
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

	if (ctx->client_reached_ready) {
		uint64_t now = picoquic_get_quic_time(quic);
		if (now - ctx->ready_at_us > 200 * 1000) {
			return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
		}
	}
	return 0;
}

static int cmd_server(const char *cert, const char *key, uint16_t port,
		const char *exec_path) {
	struct sigaction sa = { 0 };
	sa.sa_handler = on_sigterm;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = { 0 };
	picoquic_quic_t *quic = picoquic_create(
			8, cert, key, NULL, "hq-test",
			NULL, NULL, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (quic == NULL) {
		fprintf(stderr, "webtransportd: picoquic_create failed\n");
		return 1;
	}

	server_ctx_t sctx = { 0 };
	sctx.exec_path = exec_path;
	sctx.child.pid = -1;
	sctx.child.stdin_fd = -1;
	sctx.child.stdout_fd = -1;
	sctx.child.stderr_fd = -1;
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

	/* Closing the child's stdin/stdout side (via wtd_child_terminate)
	 * gives the reader thread's blocking read() an EOF so it exits;
	 * then wtd_peer_session_destroy joins it and drains the queue.
	 * Ordering: terminate child first, then destroy peer session. */
	if (sctx.child_spawned) {
		wtd_child_terminate(&sctx.child);
	}
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
		fprintf(stderr, "webtransportd: unknown argument: %s\n", argv[i]);
		(void)print_usage(stderr);
		return 2;
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
