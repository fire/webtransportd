/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — frame.h
 *
 * Length-prefixed binary frame codec used on the child's stdin/stdout.
 * Both directions (daemon->child and child->daemon) use the same wire
 * format:
 *
 *   frame   = flag | len_varint | payload
 *   flag    = uint8_t
 *               bit 0 : 0=reliable (WT bidi stream), 1=unreliable (WT datagram)
 *               bits 1-7 : reserved, must be zero
 *   len     = QUIC-style varint (RFC 9000 section 16) of payload length
 *   payload = `len` bytes of opaque application data
 *
 * The codec is pure: no I/O, no allocation, no logging. It validates
 * every input byte and never reads or writes past the caller's buffer.
 */

#ifndef WEBTRANSPORTD_FRAME_H
#define WEBTRANSPORTD_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WTD_FRAME_FLAG_RELIABLE   0x00
#define WTD_FRAME_FLAG_UNRELIABLE 0x01
#define WTD_FRAME_FLAG_MASK       0x01 /* bit 0 only; bits 1-7 reserved */

/* Hard cap on payload size in a single frame. Bounds memory the daemon's
 * per-peer reader thread might allocate when it sees an attacker-crafted
 * length-varint. 16 MiB comfortably exceeds a typical HTTP/3 request body
 * or media keyframe while keeping per-session memory bounded. */
#define WTD_FRAME_MAX_PAYLOAD (1u << 24)

typedef enum wtd_frame_status {
	WTD_FRAME_OK = 0,
	WTD_FRAME_INCOMPLETE = 1,         /* decode: not enough bytes yet, retry */
	WTD_FRAME_ERR_RESERVED = -1,      /* flag has a reserved (>= bit 1) bit set */
	WTD_FRAME_ERR_TOO_BIG = -2,       /* payload past WTD_FRAME_MAX_PAYLOAD */
	WTD_FRAME_ERR_BUF_TOO_SMALL = -3, /* encode: out_size < encoded length */
} wtd_frame_status_t;

/* Encode a frame into out[0..out_size). On success writes the on-wire
 * bytes and stores the total written length in *p_out_len. Returns
 * WTD_FRAME_OK or one of the negative error codes; on error nothing is
 * written. */
wtd_frame_status_t wtd_frame_encode(uint8_t flag,
		const uint8_t *payload, size_t payload_len,
		uint8_t *out, size_t out_size, size_t *p_out_len);

/* Encode a QUIC-style varint into out (assumed large enough: up to 8
 * bytes). Returns the number of bytes written. The encoder always
 * picks the shortest form: 1 byte for v < 2^6, 2 bytes for < 2^14,
 * 4 bytes for < 2^30, 8 bytes for < 2^62. v >= 2^62 aborts via
 * assert/returns 0 (QUIC varints don't encode beyond 62 bits).
 *
 * This symbol is exported only so the unit test can probe the
 * prefix-3 (8-byte) branch without a 2^30+-byte payload. Production
 * callers should use wtd_frame_encode — which selects this helper
 * internally and enforces WTD_FRAME_MAX_PAYLOAD. */
size_t wtd_frame_encode_varint(uint64_t v, uint8_t *out);

/* Try to decode one frame from buf[0..buf_len). On WTD_FRAME_OK sets
 * *p_consumed to the number of bytes consumed (the caller should
 * advance by this much), *p_flag to the flag byte, *p_payload to a
 * pointer into buf (no copy), and *p_payload_len to its length.
 *
 * On WTD_FRAME_INCOMPLETE the caller should accumulate more bytes and
 * retry; on the negative codes the stream is malformed and the session
 * should be torn down. None of the out-pointers are modified on a
 * non-OK return. */
wtd_frame_status_t wtd_frame_decode(const uint8_t *buf, size_t buf_len,
		size_t *p_consumed, uint8_t *p_flag,
		const uint8_t **p_payload, size_t *p_payload_len);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_FRAME_H */
