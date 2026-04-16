#ifndef WEBTRANSPORTD_FRAME_H
#define WEBTRANSPORTD_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define WTD_FRAME_FLAG_RELIABLE 0x00

typedef enum wtd_frame_status {
	WTD_FRAME_OK = 0,
	WTD_FRAME_INCOMPLETE = 1, /* not enough bytes yet; caller should retry */
} wtd_frame_status_t;

wtd_frame_status_t wtd_frame_encode(uint8_t flag,
		const uint8_t *payload, size_t payload_len,
		uint8_t *out, size_t out_size, size_t *p_out_len);

wtd_frame_status_t wtd_frame_decode(const uint8_t *buf, size_t buf_len,
		size_t *p_consumed, uint8_t *p_flag,
		const uint8_t **p_payload, size_t *p_payload_len);

#endif
