/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — HTTP/3 WebTransport daemon
 *
 * Pure HTTP/3 with HTTP/3 WebTransport support.
 * Uses h3zero_callback for HTTP/3 and picowt_* for WebTransport.
 * Per-connection child process (--exec=BIN).
 * Reactor pattern via picoquic_packet_loop_v3.
 */

#include "version.h"

#include "autocert.h"
#include "child_process.h"
#include "frame.h"
#include "log.h"
#include "peer_session.h"
#include "picotls.h"
#include "picoquic.h"
#include "picoquic_packet_loop.h"
#include "h3zero.h"
#include "h3zero_common.h"
#include "pico_webtransport.h"

#include <errno.h>
#include <inttypes.h>
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

typedef struct server_ctx server_ctx_t;

typedef struct wtd_peer {
	picoquic_cnx_t *cnx;
	wtd_child_t child;
	wtd_peer_session_t session;
	struct wtd_peer *next;
	uint64_t control_stream_id;
	uint64_t data_stream_id;
	h3zero_callback_ctx_t *h3_ctx;
	server_ctx_t *sctx;
	uint8_t *pending_dgram;
	size_t pending_dgram_len;
} wtd_peer_t;

typedef struct server_ctx {
	const char *exec_path;
	const char *dir_path;
	wtd_peer_t *peers;
	picohttp_server_path_item_t *path_items;
} server_ctx_t;

static void on_sigterm(int sig) {
	(void)sig;
	atomic_store(&g_should_exit, 1);
}

static wtd_peer_t *peer_create(server_ctx_t *sctx, picoquic_cnx_t *cnx,
		h3zero_callback_ctx_t *h3_ctx) {
	wtd_peer_t *p = malloc(sizeof(wtd_peer_t));
	if (p == NULL) {
		return NULL;
	}
	memset(p, 0, sizeof(wtd_peer_t));
	p->cnx = cnx;
	p->h3_ctx = h3_ctx;
	p->sctx = sctx;
	p->control_stream_id = UINT64_MAX;
	p->data_stream_id = UINT64_MAX;

	wtd_peer_session_init(&p->session);

	const char *argv[] = { sctx->exec_path, NULL };
	if (wtd_child_spawn(argv, NULL, &p->child) != 0) {
		wtd_peer_session_destroy(&p->session);
		free(p);
		return NULL;
	}

	if (wtd_peer_session_start_reader(&p->session, p->child.stdout_fd,
			NULL, NULL) != 0) {
		wtd_log(WTD_LOG_ERROR, "peer_session_start_reader failed");
		wtd_child_terminate(&p->child);
		wtd_peer_session_destroy(&p->session);
		free(p);
		return NULL;
	}

	p->next = sctx->peers;
	sctx->peers = p;
	return p;
}


static void peer_remove(server_ctx_t *sctx, wtd_peer_t *p) {
	if (p == NULL) {
		return;
	}
	wtd_peer_t **pp = &sctx->peers;
	while (*pp != NULL && *pp != p) {
		pp = &(*pp)->next;
	}
	if (*pp == p) {
		*pp = p->next;
	}
	wtd_peer_session_stop_reader(&p->session);
	wtd_peer_session_destroy(&p->session);
	wtd_child_terminate(&p->child);
	if (p->pending_dgram != NULL) {
		free(p->pending_dgram);
	}
	free(p);
}


static void drain_outbound(wtd_peer_t *p) {
	if (p == NULL) {
		return;
	}

	wtd_outbound_frame_t *frame = wtd_work_queue_drain(&p->session.outbound);
	if (frame == NULL) {
		wtd_log(WTD_LOG_TRACE,
				"[Cycle 51] drain_outbound: no frames in queue");
		return;
	}

	wtd_log(WTD_LOG_TRACE, "[Cycle 51] drain_outbound: processing frames");
	while (frame != NULL) {
		wtd_outbound_frame_t *next = frame->next;

		if (frame->flag == 0) {
			if (p->data_stream_id != UINT64_MAX) {
				int add_ret = picoquic_add_to_stream(p->cnx,
						p->data_stream_id,
						frame->payload, frame->payload_len, 0);
				wtd_log(WTD_LOG_TRACE,
						"outbound frame: flag=%d len=%zu "
						"payload=%.*s (picoquic_add returned %d)",
						frame->flag, frame->payload_len,
						(int)frame->payload_len,
						frame->payload, add_ret);
			} else {
				wtd_log(WTD_LOG_WARN,
						"[Cycle 51] drain_outbound: "
						"data_stream_id not set for flag=0");
			}
		} else {
			if (p->pending_dgram != NULL) {
				free(p->pending_dgram);
			}
			p->pending_dgram = malloc(frame->payload_len);
			if (p->pending_dgram != NULL) {
				memcpy(p->pending_dgram, frame->payload,
						frame->payload_len);
				p->pending_dgram_len = frame->payload_len;
				h3zero_set_datagram_ready(p->cnx,
						p->control_stream_id);
				wtd_log(WTD_LOG_TRACE,
						"outbound frame: flag=%d len=%zu "
						"payload=%.*s",
						frame->flag, frame->payload_len,
						(int)frame->payload_len,
						frame->payload);
			}
		}
		free(frame);
		frame = next;
	}
}

static int wt_session_cb(picoquic_cnx_t *cnx, uint8_t *bytes, size_t length,
		picohttp_call_back_event_t event,
		h3zero_stream_ctx_t *stream_ctx, void *path_app_ctx) {
	int ret = 0;

	if (event == picohttp_callback_connect) {
		server_ctx_t *sctx = (server_ctx_t *)path_app_ctx;
		h3zero_callback_ctx_t *h3_ctx =
				(h3zero_callback_ctx_t *)picoquic_get_callback_context(cnx);
		wtd_log(WTD_LOG_TRACE,
				"[WebTransport] CONNECT received on stream %" PRIu64,
				stream_ctx->stream_id);
		wtd_peer_t *p = peer_create(sctx, cnx, h3_ctx);
		if (p == NULL) {
			wtd_log(WTD_LOG_ERROR, "peer_create failed");
			return -1;
		}
		p->control_stream_id = stream_ctx->stream_id;
		stream_ctx->path_callback_ctx = p;
		if (h3zero_declare_stream_prefix(h3_ctx,
				stream_ctx->stream_id, wt_session_cb, p) != 0) {
			wtd_log(WTD_LOG_ERROR,
					"h3zero_declare_stream_prefix failed");
			peer_remove(sctx, p);
			return -1;
		}
		wtd_log(WTD_LOG_TRACE,
				"[WebTransport] CONNECT accepted on stream %"
				PRIu64, p->control_stream_id);
		return 0;
	}

	wtd_peer_t *p = (wtd_peer_t *)path_app_ctx;
	int ret2 = 0;

	if (p != NULL) {
		wtd_log(WTD_LOG_TRACE,
				"[Cycle 51] wt_session_cb: event=%d stream=%" PRIu64
				" length=%zu control_stream=%" PRIu64,
				(int)event, stream_ctx->stream_id, length,
				p->control_stream_id);
	}

	switch (event) {
	case picohttp_callback_post_data:
	case picohttp_callback_post_fin: {
		if (p == NULL) {
			break;
		}
		if (stream_ctx->stream_id == p->control_stream_id) {
			wtd_log(WTD_LOG_TRACE,
					"[Cycle 51] post_data on control stream "
					"(capsule), skipping");
			break;
		}
		if (p->data_stream_id == UINT64_MAX) {
			p->data_stream_id = stream_ctx->stream_id;
			wtd_log(WTD_LOG_TRACE,
					"[Cycle 51] data_stream_id set to %" PRIu64,
					p->data_stream_id);
		}
		if (length == 0) {
			wtd_log(WTD_LOG_TRACE,
					"[Cycle 51] post_data: length==0, skipping");
			break;
		}
		uint8_t frame_buf[4096];
		size_t frame_len = 0;
		ret2 = wtd_frame_encode(0, bytes, length, frame_buf,
				sizeof(frame_buf), &frame_len);
		if (ret2 == 0) {
			ssize_t nwritten = write(p->child.stdin_fd,
					frame_buf, frame_len);
			wtd_log(WTD_LOG_TRACE,
					"[Cycle 51] wrote %zu bytes to "
					"child stdin (ret=%zd)",
					frame_len, nwritten);
		} else {
			wtd_log(WTD_LOG_ERROR,
					"[Cycle 51] wtd_frame_encode "
					"failed: %d", ret2);
		}
		break;
	}

	case picohttp_callback_post_datagram:
		if (p == NULL) {
			break;
		}
		if (length > 0) {
			uint8_t frame_buf[4096];
			size_t frame_len = 0;
			ret2 = wtd_frame_encode(1, bytes, length, frame_buf,
					sizeof(frame_buf), &frame_len);
			if (ret2 == 0) {
				ssize_t nwritten = write(p->child.stdin_fd,
						frame_buf, frame_len);
				wtd_log(WTD_LOG_TRACE,
						"[Cycle 51] datagram: wrote %zu "
						"bytes to child stdin (ret=%zd)",
						frame_len, nwritten);
			} else {
				wtd_log(WTD_LOG_ERROR,
						"[Cycle 51] datagram: "
						"wtd_frame_encode failed: %d",
						ret2);
			}
		}
		break;

	case picohttp_callback_provide_data:
		if (p != NULL) {
			drain_outbound(p);
		}
		break;

	case picohttp_callback_provide_datagram:
		if (p != NULL && p->pending_dgram != NULL) {
			uint8_t *buf = h3zero_provide_datagram_buffer(
					(void *)bytes, p->pending_dgram_len, 0);
			if (buf != NULL) {
				memcpy(buf, p->pending_dgram,
						p->pending_dgram_len);
			}
			free(p->pending_dgram);
			p->pending_dgram = NULL;
			p->pending_dgram_len = 0;
		}
		break;

	case picohttp_callback_deregister:
		if (p != NULL) {
			wtd_log(WTD_LOG_TRACE,
					"[WebTransport] session closed");
			peer_remove(p->sctx, p);
		}
		break;

	default:
		wtd_log(WTD_LOG_TRACE,
				"[Placeholder] unhandled callback event: %d",
				(int)event);
		break;
	}

	return ret;
}

/* Packet loop callback */
static int server_loop_cb(picoquic_quic_t *quic,
		picoquic_packet_loop_cb_enum cb,
		void *cb_ctx, void *cb_arg) {
	(void)quic;
	(void)cb_arg;

	if (cb == picoquic_packet_loop_ready) {
		printf("server ready\n");
		fflush(stdout);
		wtd_log(WTD_LOG_TRACE, "packet loop ready");
		return 0;
	}

	if (atomic_load(&g_should_exit)) {
		return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
	}

	server_ctx_t *sctx = (server_ctx_t *)cb_ctx;
	int peer_count = 0;
	for (wtd_peer_t *p = sctx->peers; p != NULL; p = p->next) {
		peer_count++;
		drain_outbound(p);
	}

	if (peer_count > 0) {
		wtd_log(WTD_LOG_TRACE,
				"[Cycle 51] server_loop_cb: drained %d peers",
				peer_count);
	}

	return 0;
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

	picohttp_server_path_item_t path_items[1] = {
		{ "/wt", 3, wt_session_cb, NULL }
	};

	picohttp_server_parameters_t server_param = { 0 };
	server_param.web_folder = dir_path;
	server_param.path_table = path_items;
	server_param.path_table_nb = 1;

	server_ctx_t sctx = { exec_path, dir_path, NULL, path_items };
	path_items[0].path_app_ctx = &sctx;

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

	picoquic_quic_t *quic = picoquic_create(
			8,
			use_autocert ? NULL : cert,
			use_autocert ? NULL : key,
			NULL, "h3",
			h3zero_callback, &server_param, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (quic == NULL) {
		wtd_log(WTD_LOG_ERROR,
				"webtransportd: picoquic_create failed");
		free(cert_der);
		free(key_der);
		return 1;
	}

	wtd_log(WTD_LOG_INFO, "webtransportd: HTTP/3 server on port %u", port);

	picowt_set_default_transport_parameters(quic);
	(void)picoquic_set_default_tp_value(quic,
			picoquic_tp_max_datagram_frame_size, 1500);

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

	picoquic_free(quic);

	if (rc == PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP || rc == 0 ||
			atomic_load(&g_should_exit)) {
		return 0;
	}
	wtd_log(WTD_LOG_ERROR, "webtransportd: packet loop rc=%d", rc);
	return 1;
}

static int cmd_selftest(void) {
	wtd_log(WTD_LOG_INFO, "selftest not implemented");
	return 0;
}

static int print_usage(FILE *out) {
	fprintf(out, "webtransportd: HTTP/3 WebTransport daemon\n");
	fprintf(out, "usage:\n");
	fprintf(out, "  --version          Print version and exit\n");
	fprintf(out, "  --server --cert=<pem> --key=<pem> --port=<N>\n");
	fprintf(out, "                      Start server\n");
	fprintf(out, "    --exec=<bin>      Spawn bin on connection\n");
	fprintf(out, "    --dir=<path>      Serve static files\n");
	fprintf(out, "    --log-level=<0-4> Set logging level\n");
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
		(void)print_usage(stderr);
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
			(void)print_usage(stderr);
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
