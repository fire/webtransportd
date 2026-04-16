#ifndef WEBTRANSPORTD_FRAME_H
#define WEBTRANSPORTD_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define WTD_FRAME_FLAG_RELIABLE   0x00
#define WTD_FRAME_FLAG_UNRELIABLE 0x01
#define WTD_FRAME_FLAG_MASK       0x01 /* bit 0 only; bits 1-7 reserved */

/* Hard cap on payload size in a single frame. Bounds memory the daemon's
 * per-peer reader thread might allocate when it sees an attacker-crafted
 * length-varint. 1 MiB comfortably exceeds typical UDP framing while
 * keeping per-session memory bounded. */
#define WTD_FRAME_MAX_PAYLOAD (1u << 20)

typedef enum wtd_frame_status {
	WTD_FRAME_OK = 0,
	WTD_FRAME_INCOMPLETE = 1,    /* not enough bytes yet; caller should retry */
	WTD_FRAME_ERR_RESERVED = -1, /* a reserved flag bit was set */
	WTD_FRAME_ERR_TOO_BIG = -2,  /* payload past WTD_FRAME_MAX_PAYLOAD */
} wtd_frame_status_t;

wtd_frame_status_t wtd_frame_encode(uint8_t flag,
		const uint8_t *payload, size_t payload_len,
		uint8_t *out, size_t out_size, size_t *p_out_len);

wtd_frame_status_t wtd_frame_decode(const uint8_t *buf, size_t buf_len,
		size_t *p_consumed, uint8_t *p_flag,
		const uint8_t **p_payload, size_t *p_payload_len);

#endif
