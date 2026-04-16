/* TDD log:
 * - Cycle 1: encode "hi" reliable, expect [0x00, 0x02, 'h', 'i'].
 * - Cycle 2: decode round-trips the same bytes back to flag + payload.
 * - Cycle 3: decode reports WTD_FRAME_INCOMPLETE for every short prefix of
 *   a valid frame; the full frame flips back to OK.
 * - Cycle 4 (this addition): only bit 0 of the flag byte is meaningful.
 *   Encode rejects flags with any reserved bit set; decode rejects frames
 *   on the wire that have any reserved bit set.
 */

#include "frame.h"

#include <stdio.h>
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

int main(void) {
	cycle1_encode_hi();
	cycle2_decode_roundtrip();
	cycle3_incomplete_prefixes();
	cycle4_reserved_bits();
	return failures == 0 ? 0 : 1;
}
