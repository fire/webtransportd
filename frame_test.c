/* TDD log:
 * - Cycle 1: encode "hi" reliable, expect [0x00, 0x02, 'h', 'i'].
 * - Cycle 2: decode round-trips the same bytes back to flag + payload.
 * - Cycle 3: decode reports WTD_FRAME_INCOMPLETE for every short prefix of
 *   a valid frame; the full frame flips back to OK.
 * - Cycle 4: only bit 0 of the flag byte is meaningful. Encode rejects
 *   flags with any reserved bit set; decode rejects on-wire frames whose
 *   flag has any reserved bit set.
 * - Cycle 5: payload >= 64 bytes pushes the length past the 1-byte varint
 *   range. Round-trip must still work, the on-wire length prefix must be
 *   the 2-byte form (top two bits = 0b01), and `consumed` must account for
 *   the extra varint byte.
 * - Cycle 6: close the latent OOB-read door from cycle 5. ASAN-fenced
 *   exact-sized prefixes confirm decode never reads past the buffer.
 * - Cycle 7: payload >= 16384 bytes pushes the length past the 2-byte
 *   varint range. Round-trip must still work, the on-wire length prefix
 *   must be the 4-byte form (top two bits = 0b10), and the ASAN-fenced
 *   incomplete-prefix coverage extends to this size.
 * - Cycle 8: two frames packed back-to-back in the same buffer must each
 *   decode independently. `consumed` must report the exact length of the
 *   just-decoded frame (not the buffer), so the caller can advance and
 *   decode the next one.
 * - Cycle 9: payload past WTD_FRAME_MAX_PAYLOAD is rejected on encode,
 *   and decode refuses an on-wire frame whose length-varint claims more
 *   than the cap (defends against an attacker-crafted stream that would
 *   otherwise force a huge alloc on the daemon's reader thread).
 * - Cycle 10 (this addition): encode refuses if the caller's output
 *   buffer is smaller than the encoded frame would be — and does so
 *   *without* writing a single byte to that buffer (ASAN-fenced).
 */

#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

static void cycle1_encode_hi(void) {
	uint8_t out[8];
	size_t out_len = 0;
	uint8_t payload[] = { 'h', 'i' };
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE, payload, sizeof(payload),
				out, sizeof(out), &out_len) == WTD_FRAME_OK);
	uint8_t expected[] = { 0x00, 0x02, 'h', 'i' };
	EXPECT(out_len == sizeof(expected));
	EXPECT(memcmp(out, expected, out_len) == 0);
}

static void cycle2_decode_roundtrip(void) {
	uint8_t buf[8];
	size_t out_len = 0;
	uint8_t payload[] = { 'h', 'i' };
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE, payload, sizeof(payload),
				buf, sizeof(buf), &out_len) == WTD_FRAME_OK);

	size_t consumed = 99;
	uint8_t got_flag = 0xff;
	const uint8_t *got_payload = NULL;
	size_t got_payload_len = 99;
	EXPECT(wtd_frame_decode(buf, out_len,
				&consumed, &got_flag, &got_payload, &got_payload_len) == WTD_FRAME_OK);
	EXPECT(consumed == out_len);
	EXPECT(got_flag == WTD_FRAME_FLAG_RELIABLE);
	EXPECT(got_payload_len == sizeof(payload));
	EXPECT(memcmp(got_payload, payload, sizeof(payload)) == 0);
}

static void cycle3_incomplete_prefixes(void) {
	uint8_t buf[8];
	size_t out_len = 0;
	uint8_t payload[] = { 'h', 'i' };
	(void)wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE, payload, sizeof(payload),
			buf, sizeof(buf), &out_len);
	for (size_t i = 0; i < out_len; i++) {
		size_t consumed = 0;
		uint8_t flag = 0;
		const uint8_t *p = NULL;
		size_t plen = 0;
		EXPECT(wtd_frame_decode(buf, i, &consumed, &flag, &p, &plen) == WTD_FRAME_INCOMPLETE);
	}
	/* Full frame decodes OK. */
	size_t consumed = 0;
	uint8_t flag = 0;
	const uint8_t *p = NULL;
	size_t plen = 0;
	EXPECT(wtd_frame_decode(buf, out_len, &consumed, &flag, &p, &plen) == WTD_FRAME_OK);
	EXPECT(consumed == out_len);
}

static void cycle4_reserved_bits(void) {
	/* Encode side: any flag bit other than bit 0 is rejected. */
	uint8_t buf[8];
	size_t out_len = 0;
	uint8_t payload[] = { 'x' };
	EXPECT(wtd_frame_encode(0x02 /* reserved bit set */, payload, sizeof(payload),
				buf, sizeof(buf), &out_len) == WTD_FRAME_ERR_RESERVED);
	EXPECT(wtd_frame_encode(0x80, payload, sizeof(payload),
				buf, sizeof(buf), &out_len) == WTD_FRAME_ERR_RESERVED);

	/* Decode side: an attacker-crafted byte stream with reserved bits is rejected. */
	uint8_t bad[] = { 0x80 /* reserved bit */, 0x01, 'x' };
	size_t consumed = 0;
	uint8_t flag = 0;
	const uint8_t *p = NULL;
	size_t plen = 0;
	EXPECT(wtd_frame_decode(bad, sizeof(bad), &consumed, &flag, &p, &plen) == WTD_FRAME_ERR_RESERVED);

	/* Sanity: bit 0 alone (= unreliable) is still accepted on encode. */
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_UNRELIABLE, payload, sizeof(payload),
				buf, sizeof(buf), &out_len) == WTD_FRAME_OK);
	EXPECT(out_len == 3);
	EXPECT(buf[0] == 0x01);
}

static void cycle5_two_byte_varint(void) {
	/* 200-byte payload — first value past 1-byte varint range. */
	uint8_t payload[200];
	for (size_t i = 0; i < sizeof(payload); i++) {
		payload[i] = (uint8_t)(i & 0xff);
	}
	uint8_t buf[1 + 2 + sizeof(payload)];
	size_t out_len = 0;
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_UNRELIABLE, payload, sizeof(payload),
				buf, sizeof(buf), &out_len) == WTD_FRAME_OK);
	EXPECT(out_len == 1 + 2 + sizeof(payload));
	EXPECT(buf[0] == WTD_FRAME_FLAG_UNRELIABLE);
	/* Top two bits of first varint byte = 0b01 (2-byte form). */
	EXPECT((buf[1] & 0xc0) == 0x40);
	/* The 14-bit value must equal sizeof(payload). */
	uint16_t encoded_len = (uint16_t)((buf[1] & 0x3f) << 8) | buf[2];
	EXPECT(encoded_len == sizeof(payload));

	/* Round-trip. */
	size_t consumed = 0;
	uint8_t flag = 0;
	const uint8_t *p = NULL;
	size_t plen = 0;
	EXPECT(wtd_frame_decode(buf, out_len, &consumed, &flag, &p, &plen) == WTD_FRAME_OK);
	EXPECT(consumed == out_len);
	EXPECT(flag == WTD_FRAME_FLAG_UNRELIABLE);
	EXPECT(plen == sizeof(payload));
	EXPECT(memcmp(p, payload, sizeof(payload)) == 0);
}

static void cycle6_incomplete_two_byte_varint(void) {
	/* Encode a 200-byte payload (-> 2-byte varint). For every prefix shorter
	 * than the full frame, decode must report INCOMPLETE without reading
	 * past the buffer. We malloc each prefix into its own exact-sized
	 * allocation so ASAN flags any over-read as a heap-buffer-overflow. */
	uint8_t payload[200];
	for (size_t i = 0; i < sizeof(payload); i++) {
		payload[i] = (uint8_t)i;
	}
	uint8_t buf[1 + 2 + sizeof(payload)];
	size_t out_len = 0;
	(void)wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE, payload, sizeof(payload),
			buf, sizeof(buf), &out_len);
	for (size_t i = 0; i < out_len; i++) {
		uint8_t *prefix = (uint8_t *)malloc(i == 0 ? 1 : i);
		if (i > 0) {
			memcpy(prefix, buf, i);
		}
		size_t consumed = 0;
		uint8_t flag = 0;
		const uint8_t *p = NULL;
		size_t plen = 0;
		EXPECT(wtd_frame_decode(prefix, i, &consumed, &flag, &p, &plen) == WTD_FRAME_INCOMPLETE);
		free(prefix);
	}
	/* Full buffer still decodes OK. */
	size_t consumed = 0;
	uint8_t flag = 0;
	const uint8_t *p = NULL;
	size_t plen = 0;
	EXPECT(wtd_frame_decode(buf, out_len, &consumed, &flag, &p, &plen) == WTD_FRAME_OK);
	EXPECT(consumed == out_len);
}

static void cycle7_four_byte_varint(void) {
	/* 20000 bytes — first jump past the 14-bit (16383-byte) limit. */
	const size_t plen = 20000;
	uint8_t *payload = (uint8_t *)malloc(plen);
	for (size_t i = 0; i < plen; i++) {
		payload[i] = (uint8_t)((i * 31) & 0xff);
	}
	uint8_t *buf = (uint8_t *)malloc(1 + 4 + plen);
	size_t out_len = 0;
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE, payload, plen,
				buf, 1 + 4 + plen, &out_len) == WTD_FRAME_OK);
	EXPECT(out_len == 1 + 4 + plen);
	EXPECT(buf[0] == WTD_FRAME_FLAG_RELIABLE);
	/* Top two bits of first varint byte = 0b10 (4-byte form). */
	EXPECT((buf[1] & 0xc0) == 0x80);
	uint32_t encoded_len = (uint32_t)((buf[1] & 0x3f) << 24)
			| ((uint32_t)buf[2] << 16)
			| ((uint32_t)buf[3] << 8)
			| (uint32_t)buf[4];
	EXPECT(encoded_len == plen);

	/* Round-trip. */
	size_t consumed = 0;
	uint8_t flag = 0;
	const uint8_t *p = NULL;
	size_t got_len = 0;
	EXPECT(wtd_frame_decode(buf, out_len, &consumed, &flag, &p, &got_len) == WTD_FRAME_OK);
	EXPECT(consumed == out_len);
	EXPECT(flag == WTD_FRAME_FLAG_RELIABLE);
	EXPECT(got_len == plen);
	EXPECT(memcmp(p, payload, plen) == 0);

	/* ASAN-fenced incomplete-prefix coverage at the 4-byte varint boundary.
	 * We only test prefixes inside the header (1 + 4 = 5 bytes) plus a few
	 * just past it; covering every byte of a 20000-byte buffer is overkill. */
	for (size_t i = 0; i < 6; i++) {
		uint8_t *prefix = (uint8_t *)malloc(i == 0 ? 1 : i);
		if (i > 0) {
			memcpy(prefix, buf, i);
		}
		size_t c = 0;
		uint8_t f = 0;
		const uint8_t *pp = NULL;
		size_t pl = 0;
		EXPECT(wtd_frame_decode(prefix, i, &c, &f, &pp, &pl) == WTD_FRAME_INCOMPLETE);
		free(prefix);
	}

	free(payload);
	free(buf);
}

static void cycle8_two_frames_one_buffer(void) {
	/* Two distinct frames in one buffer: an unreliable 2-byte payload
	 * followed by a reliable 4-byte payload. Decode reads the first,
	 * caller advances by `consumed`, decodes the second. */
	uint8_t buf[64];
	size_t cursor = 0, w = 0;
	uint8_t a[] = { 0xCA, 0xFE };
	uint8_t b[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_UNRELIABLE, a, sizeof(a),
				buf + cursor, sizeof(buf) - cursor, &w) == WTD_FRAME_OK);
	cursor += w;
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE, b, sizeof(b),
				buf + cursor, sizeof(buf) - cursor, &w) == WTD_FRAME_OK);
	cursor += w;

	size_t pos = 0;
	for (int i = 0; i < 2; i++) {
		size_t consumed = 0;
		uint8_t flag = 0;
		const uint8_t *p = NULL;
		size_t plen = 0;
		EXPECT(wtd_frame_decode(buf + pos, cursor - pos,
					&consumed, &flag, &p, &plen) == WTD_FRAME_OK);
		if (i == 0) {
			EXPECT(flag == WTD_FRAME_FLAG_UNRELIABLE);
			EXPECT(plen == sizeof(a));
			EXPECT(memcmp(p, a, sizeof(a)) == 0);
		} else {
			EXPECT(flag == WTD_FRAME_FLAG_RELIABLE);
			EXPECT(plen == sizeof(b));
			EXPECT(memcmp(p, b, sizeof(b)) == 0);
		}
		EXPECT(consumed > 0);
		EXPECT(consumed <= cursor - pos);
		pos += consumed;
	}
	/* After both frames, the cursor must land exactly at the end. */
	EXPECT(pos == cursor);
}

static void cycle9_too_big(void) {
	/* Encode side: refuse a payload past the cap (nothing is written, no
	 * out_buf access — out_buf can be tiny / NULL-equivalent). */
	uint8_t tiny[1];
	size_t out_len = 99;
	wtd_frame_status_t s = wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE,
			tiny /* unused once we fail */, (size_t)WTD_FRAME_MAX_PAYLOAD + 1,
			tiny, sizeof(tiny), &out_len);
	EXPECT(s == WTD_FRAME_ERR_TOO_BIG);

	/* Decode side: hand-craft an on-wire 4-byte-varint frame whose declared
	 * length is past the cap. Decode must reject without trying to allocate
	 * or read the payload bytes. */
	uint8_t hdr[1 + 4];
	hdr[0] = WTD_FRAME_FLAG_RELIABLE;
	uint64_t big = (uint64_t)WTD_FRAME_MAX_PAYLOAD + 1;
	hdr[1] = (uint8_t)(0x80 | (big >> 24));
	hdr[2] = (uint8_t)((big >> 16) & 0xff);
	hdr[3] = (uint8_t)((big >> 8) & 0xff);
	hdr[4] = (uint8_t)(big & 0xff);
	size_t consumed = 0;
	uint8_t flag = 0;
	const uint8_t *p = NULL;
	size_t plen = 0;
	EXPECT(wtd_frame_decode(hdr, sizeof(hdr), &consumed, &flag, &p, &plen) == WTD_FRAME_ERR_TOO_BIG);
}

static void cycle10_buf_too_small(void) {
	/* Encoding "hi" reliable needs 4 bytes (flag + 1-byte varint + 2 payload).
	 * Any out_size < 4 must be rejected. We malloc each buffer at exactly
	 * the size we want to test so ASAN catches any over-write. */
	uint8_t payload[] = { 'h', 'i' };
	size_t needed = 4;
	for (size_t sz = 0; sz < needed; sz++) {
		uint8_t *out = (uint8_t *)malloc(sz == 0 ? 1 : sz);
		size_t out_len = 99;
		EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE, payload, sizeof(payload),
					out, sz, &out_len) == WTD_FRAME_ERR_BUF_TOO_SMALL);
		free(out);
	}
	/* Boundary: exactly `needed` bytes succeeds. */
	uint8_t *out = (uint8_t *)malloc(needed);
	size_t out_len = 0;
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE, payload, sizeof(payload),
				out, needed, &out_len) == WTD_FRAME_OK);
	EXPECT(out_len == needed);
	free(out);
}

int main(void) {
	cycle1_encode_hi();
	cycle2_decode_roundtrip();
	cycle3_incomplete_prefixes();
	cycle4_reserved_bits();
	cycle5_two_byte_varint();
	cycle6_incomplete_two_byte_varint();
	cycle7_four_byte_varint();
	cycle8_two_frames_one_buffer();
	cycle9_too_big();
	cycle10_buf_too_small();
	return failures == 0 ? 0 : 1;
}
