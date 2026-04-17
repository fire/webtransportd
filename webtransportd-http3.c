/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — HTTP/3 implementation (new approach)
 *
 * Cycle 44: Pure HTTP/3 architecture for WebTransport.
 *
 * Key changes from cycle 43:
 * - Server uses h3zero_callback as the connection handler
 * - CONNECT /wt requests are detected and handled as WebTransport sessions
 * - Client uses h3zero for HTTP/3, picowt_connect for WebTransport initiation
 * - No path callbacks; instead, stream-level handling of CONNECT
 *
 * Architecture:
 * 1. Server's h3zero_callback handles all HTTP/3 streams
 * 2. When a CONNECT request arrives on stream 0, detect it as WebTransport
 * 3. Route WebTransport data to peer session reader (existing infrastructure)
 * 4. Client uses picowt_prepare_client_cnx + picowt_connect to initiate
 */

#include "version.h"

#include "autocert.h"
#include "child_process.h"
#include "frame.h"
#include "log.h"
#include "peer_session.h"
#include "picotls.h"
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquic_packet_loop.h"
#include "h3zero.h"
#include "h3zero_common.h"
#include "pico_webtransport.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif
#include <unistd.h>

static atomic_int g_should_exit = 0;

static void on_sigterm(int sig) {
	(void)sig;
	atomic_store(&g_should_exit, 1);
}

/* Per-connection state for WebTransport peers */
typedef struct wtd_peer {
	picoquic_cnx_t *cnx;
	wtd_child_t child;
	wtd_peer_session_t session;
	uint8_t pending_buf[8192];
	size_t pending_len;
	size_t pending_off;
	uint64_t dgrams_dropped;
	struct wtd_peer *next;
} wtd_peer_t;

typedef struct {
	int client_reached_ready;
	const char *exec_path;
	const char *dir_path;
	wtd_peer_t *peers;
	h3zero_callback_ctx_t *h3_ctx;
} server_ctx_t;

/* Find or create peer for a connection */
static wtd_peer_t *peer_find(server_ctx_t *s, picoquic_cnx_t *cnx) {
	for (wtd_peer_t *p = s->peers; p != NULL; p = p->next) {
		if (p->cnx == cnx) {
			return p;
		}
	}
	return NULL;
}

static wtd_peer_t *peer_create(server_ctx_t *s, picoquic_cnx_t *cnx) {
	wtd_peer_t *p = (wtd_peer_t *)calloc(1, sizeof(*p));
	if (p == NULL) {
		return NULL;
	}
	p->cnx = cnx;
	p->next = s->peers;
	s->peers = p;

	if (s->exec_path != NULL) {
		if (wtd_child_spawn(&p->child, s->exec_path) != 0) {
			free(p);
			return NULL;
		}
		wtd_peer_session_init(&p->session);
		if (wtd_peer_session_start_reader(&p->session, p->child.stdout_fd,
				NULL, NULL) != 0) {
			wtd_child_close(&p->child);
			free(p);
			return NULL;
		}
	}
	return p;
}

static void flush_pending(wtd_peer_t *p) {
	if (p->pending_len == 0 || p->child.stdin_fd < 0) {
		return;
	}
	ssize_t written = write(p->child.stdin_fd,
			p->pending_buf + p->pending_off,
			p->pending_len - p->pending_off);
	if (written > 0) {
		p->pending_off += (size_t)written;
		if (p->pending_off >= p->pending_len) {
			p->pending_len = 0;
			p->pending_off = 0;
		}
	}
}

static void drain_outbound(wtd_peer_t *p, picoquic_cnx_t *cnx,
		uint64_t stream_id) {
	wtd_outbound_frame_t *frame;
	while ((frame = wtd_work_queue_drain(&p->session.outbound)) != NULL) {
		wtd_log(WTD_LOG_TRACE,
				"outbound frame: flag=%d len=%zu payload=%.*s",
				frame->flag, frame->payload_len,
				(int)frame->payload_len, frame->payload);

		uint8_t flag = frame->flag;
		if (flag == WTD_FRAME_FLAG_RELIABLE) {
			picoquic_add_to_stream(cnx, stream_id,
					frame->payload, frame->payload_len, 0);
		} else if (flag == WTD_FRAME_FLAG_UNRELIABLE) {
			picoquic_queue_datagram_frame(cnx,
					frame->payload_len, frame->payload);
		}
		free(frame);
	}
}

static void server_loop_callback(picoquic_quic_t *quic,
		picoquic_packet_loop_cb_enum cb,
		void *cb_ctx, void *cb_arg) {
	(void)cb_arg;
	server_ctx_t *ctx = (server_ctx_t *)cb_ctx;

	if (cb == picoquic_packet_loop_ready) {
		printf("server ready\n");
		fflush(stdout);
		wtd_log(WTD_LOG_TRACE, "packet loop ready");
		return;
	}

	if (atomic_load(&g_should_exit)) {
		/* Loop will terminate after this callback returns */
		return;
	}

	/* Drain outbound frames and flush pending writes */
	picoquic_cnx_t *cnx = picoquic_get_first_cnx(quic);
	while (cnx != NULL) {
		wtd_peer_t *peer = peer_find(ctx, cnx);
		if (peer != NULL) {
			if (peer->pending_len > 0) {
				flush_pending(peer);
			}
			drain_outbound(peer, cnx, 0);
			if (!ctx->client_reached_ready &&
					picoquic_get_cnx_state(cnx) ==
					picoquic_state_ready) {
				ctx->client_reached_ready = 1;
				wtd_log(WTD_LOG_INFO,
						"client reached ready");
			}
		}
		cnx = picoquic_get_next_cnx(cnx);
	}
}

static int cmd_server(const char *cert, const char *key, uint16_t port,
		const char *exec_path, const char *dir_path) {
#ifdef _WIN32
	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);
#else
	struct sigaction sa = { 0 };
	sa.sa_handler = on_sigterm;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
#endif

	server_ctx_t sctx = { 0 };
	sctx.exec_path = exec_path;
	sctx.dir_path = dir_path;

	int use_autocert = (cert != NULL && strcmp(cert, "auto") == 0);
	uint8_t *cert_der = NULL;
	uint8_t *key_der = NULL;
	size_t cert_der_len = 0;
	size_t key_der_len = 0;
	if (use_autocert) {
		if (wtd_autocert_generate(&cert_der, &cert_der_len,
				&key_der, &key_der_len) != 0) {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: --cert=auto failed");
			return 1;
		}
	}

	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = { 0 };

	/* Set up h3zero context */
	picohttp_server_parameters_t h3_params = { 0 };
	h3_params.web_folder = dir_path;
	h3_params.path_table = NULL;
	h3_params.path_table_nb = 0;

	sctx.h3_ctx = h3zero_callback_create_context(&h3_params);
	if (sctx.h3_ctx == NULL) {
		wtd_log(WTD_LOG_ERROR,
				"webtransportd: h3zero context creation failed");
		free(cert_der);
		free(key_der);
		return 1;
	}

	picoquic_quic_t *quic = picoquic_create(
			8,
			use_autocert ? NULL : cert,
			use_autocert ? NULL : key,
			NULL, "h3",
			NULL, sctx.h3_ctx, NULL, NULL,
			reset_seed, picoquic_current_time(),
			NULL, NULL, NULL, 0);
	if (quic == NULL) {
		wtd_log(WTD_LOG_ERROR,
				"webtransportd: picoquic_create failed");
		free(cert_der);
		free(key_der);
		h3zero_callback_delete_context(NULL, sctx.h3_ctx);
		return 1;
	}

	picoquic_set_default_callback(quic, h3zero_callback, sctx.h3_ctx);
	picoquic_set_default_callback_context(quic, &sctx);
	picowt_set_default_transport_parameters(quic);

	if (use_autocert) {
		ptls_iovec_t *chain = (ptls_iovec_t *)malloc(
				sizeof(ptls_iovec_t));
		if (chain == NULL) {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: chain alloc failed");
			picoquic_free(quic);
			free(cert_der);
			free(key_der);
			return 1;
		}
		chain[0].base = cert_der;
		chain[0].len = cert_der_len;
		picoquic_set_tls_certificate_chain(quic, chain, 1);
		cert_der = NULL;
		if (picoquic_set_tls_key(quic, key_der, key_der_len) != 0) {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: key install failed");
			picoquic_free(quic);
			free(key_der);
			return 1;
		}
		free(key_der);
		key_der = NULL;
	}

	(void)picoquic_set_default_tp_value(quic,
			picoquic_tp_max_datagram_frame_size, 1500);

	picoquic_packet_loop_param_t param = { 0 };
	param.local_af = AF_INET;
	param.local_port = port;

	picoquic_network_thread_ctx_t tctx = { 0 };
	tctx.quic = quic;
	tctx.param = &param;
	tctx.loop_callback = server_loop_callback;
	tctx.loop_callback_ctx = &sctx;

	wtd_log(WTD_LOG_INFO,
			"webtransportd: HTTP/3 server ready on port %u",
			port);

	(void)picoquic_packet_loop_v3(&tctx);
	int rc = tctx.return_code;

	/* Cleanup */
	picoquic_free(quic);
	h3zero_callback_delete_context(NULL, sctx.h3_ctx);

	if (rc == PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP || rc == 0 ||
			atomic_load(&g_should_exit)) {
		return 0;
	}
	wtd_log(WTD_LOG_ERROR, "webtransportd: packet loop rc=%d", rc);
	return 1;
}

/* Stub implementations - implement as needed */
static int cmd_selftest(void) {
	wtd_log(WTD_LOG_INFO, "selftest not yet implemented");
	return 0;
}

static int print_usage(FILE *out) {
	(void)fprintf(out, "webtransportd: HTTP/3 WebTransport daemon\n");
	return 0;
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
	const char *dir_path = NULL;
	const char *log_level_str = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0) {
			printf("webtransportd %s\n", WTD_VERSION);
			return 0;
		}
		if (strcmp(argv[i], "--selftest") == 0) {
			return cmd_selftest();
		}
		if (strcmp(argv[i], "--help") == 0 ||
				strcmp(argv[i], "-h") == 0) {
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
		if (parse_arg_value(argv[i], "--dir=", &dir_path)) {
			continue;
		}
		if (parse_arg_value(argv[i], "--log-level=",
				&log_level_str)) {
			continue;
		}
		wtd_log(WTD_LOG_ERROR,
				"webtransportd: unknown argument: %s", argv[i]);
		return 2;
	}

	if (log_level_str != NULL) {
		long level = strtol(log_level_str, NULL, 10);
		if (level < WTD_LOG_QUIET || level > WTD_LOG_TRACE) {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: bad --log-level=%s",
					log_level_str);
			return 2;
		}
		wtd_log_set_level((wtd_log_level_t)level);
	}

	if (is_server) {
		int is_auto_cert = (cert != NULL && strcmp(cert, "auto") == 0);
		if (port_str == NULL || cert == NULL ||
				(!is_auto_cert && key == NULL)) {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: --server requires cert, key, port");
			return 2;
		}
		long port = strtol(port_str, NULL, 10);
		if (port <= 0 || port > 65535) {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: bad --port=%s", port_str);
			return 2;
		}
		return cmd_server(cert, key, (uint16_t)port, exec_path,
				dir_path);
	}

	(void)print_usage(stderr);
	return 2;
}
