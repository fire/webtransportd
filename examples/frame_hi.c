/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * Tiny deterministic child used by handshake_socket_test:
 *  1. Writes "oops\n" to stderr (cycle 23: daemon forwards this via
 *     its stderr forwarder and the test asserts the sentinel).
 *  2. Writes one reliable frame with payload "hi" to stdout and
 *     exits (cycle 22b: daemon's peer_session reader decodes the
 *     frame; the test asserts the decoded payload appears).
 *
 * Wire format (frame.h):
 *   0x00       flag: bit 0 = reliable, bits 1-7 reserved zero
 *   0x02       varint(len=2) single-byte
 *   'h' 'i'    payload
 */

#include <unistd.h>

int main(void) {
	static const char err_line[] = "oops\n";
	(void)write(STDERR_FILENO, err_line, sizeof(err_line) - 1);

	static const unsigned char frame[] = { 0x00, 0x02, 'h', 'i' };
	(void)write(STDOUT_FILENO, frame, sizeof(frame));
	return 0;
}
