/*
 * webtransportd — daemon entry point.
 *
 * Cycle 19-20: argv parsing + --version.
 * Cycle 21d.1: --selftest exercises the full mbedtls-backed picoquic
 * init/teardown path from inside the daemon binary: picoquic_create
 * with all-NULL args + picoquic_free under ASAN+UBSAN.
 *
 * Note: picoquic_start_network_thread trips a NULL-function-pointer
 * crash inside ASAN's pthread_create interceptor on darwin-arm64
 * (thread creates, pc=0 on entry). Not reproduced by our other
 * pthread test (peer_session_test) so it is specific to the path
 * picoquic takes. 21d.2 sidesteps it with a synchronous in-process
 * handshake via sim_link; 21d.3 will revisit the thread in the
 * context of a real-socket server and investigate there.
 */

#include "version.h"

#include "picoquic.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int print_usage(FILE *out) {
	fprintf(out,
			"usage: webtransportd --version\n"
			"       webtransportd --selftest\n"
			"       (more flags arrive in later cycles)\n");
	return 0;
}

static int cmd_selftest(void) {
	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = { 0 };
	picoquic_quic_t *quic = picoquic_create(
		/* max_nb_connections */ 8,
		/* cert_file_name */ NULL,
		/* key_file_name */ NULL,
		/* cert_root_file_name */ NULL,
		/* default_alpn */ NULL,
		/* default_callback_fn */ NULL,
		/* default_callback_ctx */ NULL,
		/* cnx_id_callback */ NULL,
		/* cnx_id_callback_data */ NULL,
		reset_seed,
		/* current_time */ 0,
		/* p_simulated_time */ NULL,
		/* ticket_file_name */ NULL,
		/* ticket_encryption_key */ NULL,
		/* ticket_encryption_key_length */ 0);
	if (quic == NULL) {
		fprintf(stderr, "webtransportd: picoquic_create failed\n");
		return 1;
	}

	/* Cycle 21d.1 slice: prove the daemon can stand up the picoquic
	 * quic_t context (exercising the full mbedtls TLS init path) and
	 * tear it down cleanly. The packet-loop thread itself trips an
	 * ASAN/pthread_create interaction on darwin arm64 that is worth
	 * tracking down in its own cycle; spawning it here would block
	 * 21d.2 (in-process sim handshake via sim_link, which drives the
	 * loop state machine synchronously without pthread). */
	picoquic_free(quic);

	printf("selftest ok\n");
	return 0;
}

int main(int argc, char **argv) {
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
		fprintf(stderr, "webtransportd: unknown argument: %s\n", argv[i]);
		(void)print_usage(stderr);
		return 2;
	}
	(void)print_usage(stderr);
	return 2;
}
