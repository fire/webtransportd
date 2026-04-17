/* SPDX-License-Identifier: BSD-2-Clause */
/* Minimal HTTP/3 client for echo testing */

#include "picoquic.h"
#include "picoquic_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define PAYLOAD "hello"

typedef struct {
	uint8_t buf[256];
	size_t len;
} recv_buf_t;

static int client_callback(picoquic_cnx_t *cnx, uint64_t stream_id,
		uint8_t *bytes, size_t length,
		picoquic_call_back_event_t event,
		void *callback_ctx, void *stream_ctx) {
	(void)cnx;
	(void)stream_id;
	(void)stream_ctx;

	recv_buf_t *buf = (recv_buf_t *)callback_ctx;
	if (!buf) return 0;

	if (event == picoquic_callback_stream_data ||
			event == picoquic_callback_stream_fin) {
		if (length > 0 && buf->len + length <= sizeof(buf->buf)) {
			memcpy(buf->buf + buf->len, bytes, length);
			buf->len += length;
			printf("client: received %zu bytes\n", length);
		}
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		return 1;
	}

	uint16_t port = (uint16_t)atoi(argv[1]);

	/* Create UDP socket */
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("socket");
		return 1;
	}
	int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(sock);
		return 1;
	}

	/* Create QUIC connection */
	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = {0};
	picoquic_quic_t *quic = picoquic_create(
			4, NULL, NULL, NULL, "h3",
			NULL, NULL, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (!quic) {
		fprintf(stderr, "Failed to create QUIC\n");
		close(sock);
		return 1;
	}

	struct sockaddr_in srv = {0};
	srv.sin_family = AF_INET;
	srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	srv.sin_port = htons(port);

	recv_buf_t recv_buf = {0};
	picoquic_cnx_t *cnx = picoquic_create_client_cnx(
			quic, (struct sockaddr *)&srv,
			picoquic_current_time(), 0, "localhost",
			"h3", client_callback, &recv_buf);
	if (!cnx) {
		fprintf(stderr, "Failed to create client connection\n");
		picoquic_free(quic);
		close(sock);
		return 1;
	}

	/* Handshake and send */
	int ready = 0;
	int sent = 0;
	uint64_t deadline = picoquic_current_time() + 10 * 1000 * 1000;

	while (picoquic_current_time() < deadline) {
		uint64_t now = picoquic_current_time();
		uint8_t buf[2048];
		size_t len = 0;
		struct sockaddr_storage addr_to, addr_from;
		int if_idx = 0;
		picoquic_connection_id_t log_cid;
		picoquic_cnx_t *last = NULL;

		/* Prepare outgoing packet */
		int rc = picoquic_prepare_next_packet(quic, now, buf, sizeof(buf),
				&len, &addr_to, &addr_from, &if_idx, &log_cid, &last);
		if (rc == 0 && len > 0) {
			sendto(sock, buf, len, 0, (struct sockaddr *)&srv, sizeof(srv));
		}

		/* Receive packets */
		for (int i = 0; i < 10; i++) {
			struct sockaddr_in from;
			socklen_t fromlen = sizeof(from);
			ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
					(struct sockaddr *)&from, &fromlen);
			if (n <= 0) break;

			struct sockaddr_in local = {0};
			local.sin_family = AF_INET;
			local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			socklen_t local_len = sizeof(local);
			getsockname(sock, (struct sockaddr *)&local, &local_len);

			picoquic_incoming_packet(quic, buf, (size_t)n,
					(struct sockaddr *)&from,
					(struct sockaddr *)&local,
					0, 0, picoquic_current_time());
		}

		/* Send payload when ready */
		if (picoquic_get_cnx_state(cnx) == picoquic_state_ready) {
			ready = 1;
			if (!sent) {
				picoquic_add_to_stream(cnx, 0,
						(uint8_t *)PAYLOAD,
						strlen(PAYLOAD), 1);
				sent = 1;
				printf("client: sent payload\n");
			}
		}

		/* Check if received */
		if (sent && recv_buf.len >= strlen(PAYLOAD)) {
			printf("client: received echo\n");
			break;
		}

		usleep(5000);
	}

	int success = (recv_buf.len >= strlen(PAYLOAD) &&
			memcmp(recv_buf.buf, PAYLOAD, strlen(PAYLOAD)) == 0);

	picoquic_free(quic);
	close(sock);

	if (success) {
		printf("SUCCESS\n");
		return 0;
	}
	printf("FAILED\n");
	return 1;
}
