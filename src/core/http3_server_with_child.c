/* SPDX-License-Identifier: BSD-2-Clause */
/* HTTP/3 server that spawns a child process and frames communication */

#include "picoquic.h"
#include "picoquic_packet_loop.h"
#include "h3zero.h"

#include "frame.h"
#include "child_process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>

static atomic_int should_exit = 0;

static void on_sigterm(int sig) {
	(void)sig;
	atomic_store(&should_exit, 1);
}

typedef struct {
	wtd_child_t child;
	uint8_t pending_buf[8192];
	size_t pending_len;
	size_t pending_off;
} server_state_t;

static server_state_t *g_state = NULL;

static void flush_pending(server_state_t *state) {
	if (state->pending_len == 0 || state->child.stdin_fd < 0) {
		return;
	}
	ssize_t written = write(state->child.stdin_fd,
			state->pending_buf + state->pending_off,
			state->pending_len - state->pending_off);
	if (written > 0) {
		state->pending_off += (size_t)written;
		if (state->pending_off >= state->pending_len) {
			state->pending_len = 0;
			state->pending_off = 0;
		}
	}
}

static int stream_callback(picoquic_cnx_t *cnx, uint64_t stream_id,
		uint8_t *bytes, size_t length,
		picoquic_call_back_event_t event,
		void *callback_ctx, void *stream_ctx) {
	(void)stream_ctx;
	(void)callback_ctx;

	server_state_t *state = g_state;
	if (!state) return 0;

	if (event == picoquic_callback_stream_data ||
			event == picoquic_callback_stream_fin) {
		if (length == 0 || state->child.stdin_fd < 0) {
			return 0;
		}

		/* Frame the data */
		uint8_t flag = WTD_FRAME_FLAG_RELIABLE;
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

		if (state->pending_len == 0) {
			memcpy(state->pending_buf, frame_buf, out_len);
			state->pending_len = out_len;
			state->pending_off = 0;
		}
		flush_pending(state);

		/* Also echo data back to client */
		picoquic_add_to_stream(cnx, stream_id, bytes, length, 0);
	}

	return 0;
}

int main(int argc, char **argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: %s <port> <cert> <key> <exec>\n", argv[0]);
		return 1;
	}

	uint16_t port = (uint16_t)atoi(argv[1]);
	const char *cert_file = argv[2];
	const char *key_file = argv[3];
	const char *exec_path = argv[4];

	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);

	/* Initialize server state */
	server_state_t state = {0};
	g_state = &state;

	/* Spawn child */
	const char *const child_argv[] = { exec_path, NULL };
	if (wtd_child_spawn(child_argv, NULL, &state.child) != 0) {
		fprintf(stderr, "Failed to spawn child\n");
		return 1;
	}

	/* Create QUIC */
	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = {0};
	picoquic_quic_t *quic = picoquic_create(
			8, cert_file, key_file, NULL, "h3",
			stream_callback, NULL, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (!quic) {
		fprintf(stderr, "Failed to create QUIC\n");
		wtd_child_terminate(&state.child);
		return 1;
	}

	printf("server ready\n");
	fflush(stdout);

	picoquic_packet_loop_param_t param = {0};
	param.local_af = AF_INET;
	param.local_port = port;

	picoquic_network_thread_ctx_t tctx = {0};
	tctx.quic = quic;
	tctx.param = &param;

	picoquic_packet_loop_v3(&tctx);

	picoquic_free(quic);
	wtd_child_terminate(&state.child);
	g_state = NULL;

	return 0;
}
