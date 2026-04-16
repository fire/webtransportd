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
 * - Cycle 6 (this addition): close the latent OOB-read door from cycle 5.
 *   When a frame's varint is the 2-byte form, decode must still report
 *   INCOMPLETE on every short prefix (including the case where only the
 *   first varint byte is present) without reading past the buffer.
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

int main(void) {
	cycle1_encode_hi();
	cycle2_decode_roundtrip();
	cycle3_incomplete_prefixes();
	cycle4_reserved_bits();
	cycle5_two_byte_varint();
	cycle6_incomplete_two_byte_varint();
	return failures == 0 ? 0 : 1;
}
