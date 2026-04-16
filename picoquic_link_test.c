/* TDD log:
 * - Cycle 21a: minimal linkage check against vendored picoquic.
 *   picoquic_error_name(PICOQUIC_TRANSPORT_INTERNAL_ERROR) must return
 *   a non-NULL string containing "internal". This proves the picoquic
 *   source tree's headers are reachable under our flags and at least
 *   one of its translation units (error_names.c) compiles + links into
 *   our binary.
 *
 *   Wider bring-up (the full picoquic + picotls + mbedtls compile, the
 *   real handshake, the packet loop, the WT path callback) lands in
 *   later narrow cycles once this smallest slice is green.
 */

#include "picoquic.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

int main(void) {
	const char *name = picoquic_error_name(PICOQUIC_TRANSPORT_INTERNAL_ERROR);
	EXPECT(name != NULL);
	EXPECT(name != NULL && strcmp(name, "internal") == 0);
	return failures == 0 ? 0 : 1;
}
