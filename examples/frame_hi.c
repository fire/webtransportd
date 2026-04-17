/*
 * Tiny deterministic child used by handshake_socket_test to verify the
 * daemon's peer_session reader thread decodes frames off the child's
 * stdout: writes one reliable frame with payload "hi" and exits.
 *
 * Wire format (frame.h):
 *   0x00   flag: bit 0 = reliable, bits 1-7 reserved zero
 *   0x02   varint(len=2) single-byte
 *   'h' 'i'  payload
 */

#include <unistd.h>

int main(void) {
	static const unsigned char frame[] = { 0x00, 0x02, 'h', 'i' };
	(void)write(STDOUT_FILENO, frame, sizeof(frame));
	return 0;
}
