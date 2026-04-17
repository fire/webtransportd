/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * examples/echo.c — a minimal webtransportd-compatible child.
 *
 * Reads framed bytes from stdin (flag | varint len | payload),
 * decodes each complete frame with wtd_frame_decode, re-encodes
 * the payload with the same flag using wtd_frame_encode, and
 * writes the result to stdout.
 *
 * Behaviourally this is a pass-through: every byte the daemon
 * piped in comes back out unchanged (the daemon's encoder and
 * ours both pick the shortest varint form, so round-tripping is
 * byte-stable). Operators can use this as a starting point for
 * a real child — just replace the "re-encode same payload" block
 * with whatever processing the application needs.
 *
 * Exit codes:
 *   0  normal EOF on stdin
 *   1  malloc failed
 *   2  accumulator full (frame exceeds WTD_FRAME_MAX_PAYLOAD)
 *   3  read() returned a hard error
 *   4  wtd_frame_decode reported a malformed frame
 *   5  wtd_frame_encode failed (shouldn't happen — same input)
 *   6  write() returned a hard error
 */

#include "frame.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_CAP (1 + 4 + WTD_FRAME_MAX_PAYLOAD)

static int write_all(int fd, const uint8_t *buf, size_t len) {
	size_t done = 0;
	while (done < len) {
		ssize_t n = write(fd, buf + done, len - done);
		if (n > 0) {
			done += (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return -1;
		}
	}
	return 0;
}

int main(void) {
	uint8_t *in_buf = (uint8_t *)malloc(BUF_CAP);
	uint8_t *out_buf = (uint8_t *)malloc(BUF_CAP);
	if (in_buf == NULL || out_buf == NULL) {
		free(in_buf);
		free(out_buf);
		return 1;
	}
	size_t used = 0;

	for (;;) {
		if (used >= BUF_CAP) {
			free(in_buf);
			free(out_buf);
			return 2;
		}
		ssize_t n = read(STDIN_FILENO, in_buf + used, BUF_CAP - used);
		if (n == 0) {
			break; /* EOF */
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			free(in_buf);
			free(out_buf);
			return 3;
		}
		used += (size_t)n;

		for (;;) {
			size_t consumed = 0;
			uint8_t flag = 0;
			const uint8_t *payload = NULL;
			size_t plen = 0;
			wtd_frame_status_t st = wtd_frame_decode(in_buf, used,
					&consumed, &flag, &payload, &plen);
			if (st == WTD_FRAME_INCOMPLETE) {
				break;
			}
			if (st != WTD_FRAME_OK) {
				free(in_buf);
				free(out_buf);
				return 4;
			}
			size_t out_len = 0;
			wtd_frame_status_t est = wtd_frame_encode(flag, payload, plen,
					out_buf, BUF_CAP, &out_len);
			if (est != WTD_FRAME_OK) {
				free(in_buf);
				free(out_buf);
				return 5;
			}
			if (write_all(STDOUT_FILENO, out_buf, out_len) != 0) {
				free(in_buf);
				free(out_buf);
				return 6;
			}
			memmove(in_buf, in_buf + consumed, used - consumed);
			used -= consumed;
		}
	}
	free(in_buf);
	free(out_buf);
	return 0;
}
