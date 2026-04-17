/* TDD log:
 * - Cycle 21c: vendored picoquic + picohttp + picotls + mbedtls all
 *   compile and link together. picoquic_create(...) with a zero reset
 *   seed, no cert/key, no ticket file, and every other pointer NULL
 *   must return a non-NULL picoquic_quic_t (i.e. the TLS subsystem
 *   wired through picoquic_mbedtls initialises without crashing).
 *   picoquic_free must then tear it down without ASAN complaints.
 */

#include "picoquic.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

int main(void) {
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
	EXPECT(quic != NULL);
	if (quic != NULL) {
		picoquic_free(quic);
	}
	return failures == 0 ? 0 : 1;
}
