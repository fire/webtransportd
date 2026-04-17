/* SPDX-License-Identifier: BSD-2-Clause */
/* Minimal HTTP/3 server for echo testing */

#include "picoquic.h"
#include "picoquic_packet_loop.h"
#include "h3zero.h"

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

/* Simple echo callback - receive data on stream, send it back */
static int echo_callback(picoquic_cnx_t *cnx, uint64_t stream_id,
		uint8_t *bytes, size_t length,
		picoquic_call_back_event_t event,
		void *callback_ctx, void *stream_ctx) {
	(void)callback_ctx;
	(void)stream_ctx;

	if (event == picoquic_callback_stream_data ||
			event == picoquic_callback_stream_fin) {
		if (length > 0) {
			printf("echo: received %zu bytes on stream %llu\n",
					length, (unsigned long long)stream_id);
			picoquic_add_to_stream(cnx, stream_id, bytes, length, 0);
		}
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s <port> <cert.pem> <key.pem>\n",
				argv[0]);
		return 1;
	}

	uint16_t port = (uint16_t)atoi(argv[1]);
	const char *cert_file = argv[2];
	const char *key_file = argv[3];

	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);

	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = {0};
	picoquic_quic_t *quic = picoquic_create(
			8, cert_file, key_file, NULL, "h3",
			echo_callback, NULL, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (!quic) {
		fprintf(stderr, "Failed to create QUIC context\n");
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
	return 0;
}
