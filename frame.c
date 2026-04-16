#include "frame.h"

#include <string.h>

/* Single-byte varint for now (covers values < 64). Future cycles will widen
 * this in lock-step with new failing tests for the larger ranges. */
static size_t varint_encode(uint64_t v, uint8_t *out) {
	out[0] = (uint8_t)v;
	return 1;
}

static size_t varint_decode(const uint8_t *buf, uint64_t *p_value) {
	*p_value = buf[0];
	return 1;
}

wtd_frame_status_t wtd_frame_encode(uint8_t flag,
		const uint8_t *payload, size_t payload_len,
		uint8_t *out, size_t out_size, size_t *p_out_len) {
	(void)out_size;
	out[0] = flag;
	size_t vlen = varint_encode((uint64_t)payload_len, out + 1);
	memcpy(out + 1 + vlen, payload, payload_len);
	*p_out_len = 1 + vlen + payload_len;
	return WTD_FRAME_OK;
}

wtd_frame_status_t wtd_frame_decode(const uint8_t *buf, size_t buf_len,
		size_t *p_consumed, uint8_t *p_flag,
		const uint8_t **p_payload, size_t *p_payload_len) {
	if (buf_len < 1) {
		return WTD_FRAME_INCOMPLETE; /* need flag byte */
	}
	if (buf_len < 2) {
		return WTD_FRAME_INCOMPLETE; /* need at least 1 varint byte */
	}
	uint64_t plen = 0;
	size_t vlen = varint_decode(buf + 1, &plen);
	size_t total = 1 + vlen + (size_t)plen;
	if (buf_len < total) {
		return WTD_FRAME_INCOMPLETE; /* payload not all here yet */
	}
	*p_flag = buf[0];
	*p_payload = buf + 1 + vlen;
	*p_payload_len = (size_t)plen;
	*p_consumed = total;
	return WTD_FRAME_OK;
}
