/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
#include "frame.h"

#include <string.h>

/* QUIC-style varint (RFC 9000 §16). Top two bits of the first byte select
 * the encoded length: 0b00=1 byte, 0b01=2 bytes, 0b10=4 bytes, 0b11=8 bytes.
 * The remaining bits hold the value, big-endian. The encoder picks the
 * shortest form that fits v. Values >= 2^62 return 0 (QUIC varints do
 * not encode beyond 62 bits) — the caller (wtd_frame_encode) already
 * caps at WTD_FRAME_MAX_PAYLOAD so this branch is defensive. */
size_t wtd_frame_encode_varint(uint64_t v, uint8_t *out) {
	if (v < (1ull << 6)) {
		out[0] = (uint8_t)v;
		return 1;
	}
	if (v < (1ull << 14)) {
		/* 2-byte form: 14-bit value, prefix 0b01. */
		out[0] = (uint8_t)(0x40 | (v >> 8));
		out[1] = (uint8_t)(v & 0xff);
		return 2;
	}
	if (v < (1ull << 30)) {
		/* 4-byte form: 30-bit value, prefix 0b10. */
		out[0] = (uint8_t)(0x80 | (v >> 24));
		out[1] = (uint8_t)((v >> 16) & 0xff);
		out[2] = (uint8_t)((v >> 8) & 0xff);
		out[3] = (uint8_t)(v & 0xff);
		return 4;
	}
	if (v < (1ull << 62)) {
		/* 8-byte form: 62-bit value, prefix 0b11. Mirrors the decoder
		 * branch added in cycle 25. Never fires in production today
		 * (WTD_FRAME_MAX_PAYLOAD < 2^30) but covers interop with a
		 * peer that happens to emit a prefix-3 length. */
		out[0] = (uint8_t)(0xc0 | (v >> 56));
		out[1] = (uint8_t)((v >> 48) & 0xff);
		out[2] = (uint8_t)((v >> 40) & 0xff);
		out[3] = (uint8_t)((v >> 32) & 0xff);
		out[4] = (uint8_t)((v >> 24) & 0xff);
		out[5] = (uint8_t)((v >> 16) & 0xff);
		out[6] = (uint8_t)((v >> 8) & 0xff);
		out[7] = (uint8_t)(v & 0xff);
		return 8;
	}
	return 0;
}

/* Returns the number of bytes consumed (1, 2, 4, or 8), or 0 if `avail`
 * is too small to hold the indicated varint. Caller must check
 * `avail >= 1` before calling so we can examine the prefix safely. */
static size_t varint_decode(const uint8_t *buf, size_t avail, uint64_t *p_value) {
	uint8_t prefix = buf[0] >> 6;
	if (prefix == 0) {
		*p_value = buf[0];
		return 1;
	}
	if (prefix == 1) {
		if (avail < 2) {
			return 0;
		}
		*p_value = ((uint64_t)(buf[0] & 0x3f) << 8) | buf[1];
		return 2;
	}
	if (prefix == 2) {
		/* 4-byte form: 30-bit value. */
		if (avail < 4) {
			return 0;
		}
		*p_value = ((uint64_t)(buf[0] & 0x3f) << 24)
				| ((uint64_t)buf[1] << 16)
				| ((uint64_t)buf[2] << 8)
				| (uint64_t)buf[3];
		return 4;
	}
	/* prefix == 3: 8-byte form, 62-bit value. Our encoder always
	 * picks the shortest form so it never emits this, but peers
	 * might, and the previous code silently fell into the 4-byte
	 * branch which corrupts the decode. Accept the form here;
	 * wtd_frame_decode still enforces WTD_FRAME_MAX_PAYLOAD above it. */
	if (avail < 8) {
		return 0;
	}
	*p_value = ((uint64_t)(buf[0] & 0x3f) << 56)
			| ((uint64_t)buf[1] << 48)
			| ((uint64_t)buf[2] << 40)
			| ((uint64_t)buf[3] << 32)
			| ((uint64_t)buf[4] << 24)
			| ((uint64_t)buf[5] << 16)
			| ((uint64_t)buf[6] << 8)
			| (uint64_t)buf[7];
	return 8;
}

/* How many bytes a varint of value v will occupy (1, 2, 4, or 8).
 * Mirrors the branches in wtd_frame_encode_varint without writing
 * anything; lets us size-check the output buffer before touching it. */
static size_t varint_size(uint64_t v) {
	if (v < (1ull << 6)) return 1;
	if (v < (1ull << 14)) return 2;
	if (v < (1ull << 30)) return 4;
	return 8;
}

wtd_frame_status_t wtd_frame_encode(uint8_t flag,
		const uint8_t *payload, size_t payload_len,
		uint8_t *out, size_t out_size, size_t *p_out_len) {
	if ((flag & ~WTD_FRAME_FLAG_MASK) != 0) {
		return WTD_FRAME_ERR_RESERVED;
	}
	if (payload_len > WTD_FRAME_MAX_PAYLOAD) {
		return WTD_FRAME_ERR_TOO_BIG;
	}
	size_t needed = 1 + varint_size((uint64_t)payload_len) + payload_len;
	if (out_size < needed) {
		return WTD_FRAME_ERR_BUF_TOO_SMALL;
	}
	out[0] = flag;
	size_t vlen = wtd_frame_encode_varint((uint64_t)payload_len, out + 1);
	memcpy(out + 1 + vlen, payload, payload_len);
	*p_out_len = needed;
	return WTD_FRAME_OK;
}

wtd_frame_status_t wtd_frame_decode(const uint8_t *buf, size_t buf_len,
		size_t *p_consumed, uint8_t *p_flag,
		const uint8_t **p_payload, size_t *p_payload_len) {
	if (buf_len < 1) {
		return WTD_FRAME_INCOMPLETE; /* need flag byte */
	}
	if ((buf[0] & ~WTD_FRAME_FLAG_MASK) != 0) {
		return WTD_FRAME_ERR_RESERVED;
	}
	if (buf_len < 2) {
		return WTD_FRAME_INCOMPLETE; /* need at least 1 varint byte */
	}
	uint64_t plen = 0;
	size_t vlen = varint_decode(buf + 1, buf_len - 1, &plen);
	if (vlen == 0) {
		return WTD_FRAME_INCOMPLETE; /* multi-byte varint is truncated */
	}
	if (plen > WTD_FRAME_MAX_PAYLOAD) {
		return WTD_FRAME_ERR_TOO_BIG;
	}
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
