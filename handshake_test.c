/* TDD log:
 * - Cycle 21d.2 (this file): in-process QUIC handshake. Two picoquic
 *   quic contexts (client + server, server with cert+key, both with
 *   matching default ALPN) share a simulated clock and exchange
 *   packets synchronously via picoquic_prepare_next_packet /
 *   picoquic_incoming_packet. We pump in a loop, advancing
 *   simulated_time each iteration, and assert both sides reach
 *   picoquic_state_ready before the iteration budget runs out.
 *
 *   No sockets, no pthread — so this sidesteps the ASAN/thread-start
 *   crash noted in cycle 21d.1. Real-socket handshake lives in 21d.3.
 */

#include "picoquic.h"
#include "picoquic_utils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

static void sockaddr_v4(struct sockaddr_in *a, uint32_t ip, uint16_t port) {
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	a->sin_addr.s_addr = htonl(ip);
	a->sin_port = htons(port);
}

/* Pump one direction: prepare from `src_quic`, and if a packet comes
 * out, feed it to `dst_quic` with swapped addresses. */
static int pump_one(picoquic_quic_t *src_quic, picoquic_quic_t *dst_quic,
		struct sockaddr *src_addr, struct sockaddr *dst_addr,
		uint64_t simulated_time) {
	uint8_t buf[2048];
	size_t send_len = 0;
	struct sockaddr_storage sto, sfrom;
	int if_idx = 0;
	picoquic_connection_id_t log_cid;
	picoquic_cnx_t *last = NULL;
	int rc = picoquic_prepare_next_packet(src_quic, simulated_time,
			buf, sizeof(buf), &send_len,
			&sto, &sfrom, &if_idx, &log_cid, &last);
	if (rc != 0) {
		return rc;
	}
	if (send_len == 0) {
		return 0;
	}
	return picoquic_incoming_packet(dst_quic, buf, send_len,
			src_addr, dst_addr, 0, 0, simulated_time);
}

int main(void) {
	const char *alpn = "hq-test";
	const char *cert_path = "thirdparty/picoquic/certs/cert.pem";
	const char *key_path = "thirdparty/picoquic/certs/key.pem";

	uint8_t reset_seed_s[PICOQUIC_RESET_SECRET_SIZE] = { 0 };
	uint8_t reset_seed_c[PICOQUIC_RESET_SECRET_SIZE] = { 0 };
	uint64_t simulated_time = 0;

	picoquic_quic_t *server = picoquic_create(
			8, cert_path, key_path, NULL, alpn,
			NULL, NULL, NULL, NULL, reset_seed_s,
			simulated_time, &simulated_time, NULL, NULL, 0);
	EXPECT(server != NULL);
	if (server == NULL) {
		return 1;
	}

	picoquic_quic_t *client = picoquic_create(
			8, NULL, NULL, NULL, alpn,
			NULL, NULL, NULL, NULL, reset_seed_c,
			simulated_time, &simulated_time, NULL, NULL, 0);
	EXPECT(client != NULL);
	if (client == NULL) {
		picoquic_free(server);
		return 1;
	}

	struct sockaddr_in server_addr, client_addr;
	sockaddr_v4(&server_addr, 0x7f000001, 54321);
	sockaddr_v4(&client_addr, 0x7f000001, 12345);

	picoquic_cnx_t *cnx = picoquic_create_client_cnx(
			client, (struct sockaddr *)&server_addr,
			simulated_time, 0, "test.example", alpn,
			NULL, NULL);
	EXPECT(cnx != NULL);
	if (cnx == NULL) {
		picoquic_free(client);
		picoquic_free(server);
		return 1;
	}

	int client_ready = 0;
	int server_ready = 0;
	for (int iter = 0; iter < 200; iter++) {
		(void)pump_one(client, server,
				(struct sockaddr *)&client_addr,
				(struct sockaddr *)&server_addr,
				simulated_time);
		(void)pump_one(server, client,
				(struct sockaddr *)&server_addr,
				(struct sockaddr *)&client_addr,
				simulated_time);

		simulated_time += 1000;

		if (picoquic_get_cnx_state(cnx) == picoquic_state_ready) {
			client_ready = 1;
		}
		picoquic_cnx_t *scnx = picoquic_get_first_cnx(server);
		if (scnx != NULL
				&& picoquic_get_cnx_state(scnx) == picoquic_state_ready) {
			server_ready = 1;
		}
		if (client_ready && server_ready) {
			break;
		}
	}
	EXPECT(client_ready);
	EXPECT(server_ready);

	picoquic_free(client);
	picoquic_free(server);
	return failures == 0 ? 0 : 1;
}
