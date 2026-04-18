/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — peer_session.c
 *
 * Work-queue primitive (cycle 17) + per-peer reader thread (cycle 18).
 * Picoquic glue lands when its test demands it.
 */

#include "peer_session.h"

#include "frame.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void wtd_work_queue_init(wtd_work_queue_t *q) {
	pthread_mutex_init(&q->mutex, NULL);
	q->head = NULL;
	q->tail = NULL;
}

void wtd_work_queue_destroy(wtd_work_queue_t *q) {
	wtd_outbound_frame_t *cur = wtd_work_queue_drain(q);
	while (cur != NULL) {
		wtd_outbound_frame_t *next = cur->next;
		free(cur);
		cur = next;
	}
	pthread_mutex_destroy(&q->mutex);
}

void wtd_work_queue_push(wtd_work_queue_t *q, wtd_outbound_frame_t *f) {
	if (f == NULL) {
		return;
	}
	pthread_mutex_lock(&q->mutex);
	f->next = NULL;
	if (q->tail != NULL) {
		q->tail->next = f;
	} else {
		q->head = f;
	}
	q->tail = f;
	pthread_mutex_unlock(&q->mutex);
}

wtd_outbound_frame_t *wtd_work_queue_drain(wtd_work_queue_t *q) {
	pthread_mutex_lock(&q->mutex);
	wtd_outbound_frame_t *h = q->head;
	q->head = NULL;
	q->tail = NULL;
	pthread_mutex_unlock(&q->mutex);
	return h;
}

void wtd_peer_session_init(wtd_peer_session_t *s) {
	wtd_work_queue_init(&s->outbound);
	s->reader_thread_started = 0;
	s->reader_fd = -1;
	s->on_ready = NULL;
	s->on_ready_ctx = NULL;
	atomic_store(&s->reader_done, 0);
}

/* Large enough to hold one full-size frame (flag + 4-byte varint + max
 * payload) without forcing a second read round-trip. */
#define WTD_READ_BUF_SIZE (1 + 4 + WTD_FRAME_MAX_PAYLOAD)

static void *reader_main(void *arg) {
	wtd_peer_session_t *s = (wtd_peer_session_t *)arg;
	uint8_t *buf = (uint8_t *)malloc(WTD_READ_BUF_SIZE);
	if (buf == NULL) {
		return NULL;
	}
	size_t used = 0;

	for (;;) {
		if (used >= WTD_READ_BUF_SIZE) {
			/* No space to make progress — either a pathological
			 * peer or a decode bug. Stop cleanly. */
			break;
		}
		ssize_t n = read(s->reader_fd, buf + used, WTD_READ_BUF_SIZE - used);
		if (n == 0) {
			break; /* EOF */
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		used += (size_t)n;

		for (;;) {
			size_t consumed = 0;
			uint8_t flag = 0;
			const uint8_t *payload = NULL;
			size_t plen = 0;
			wtd_frame_status_t st = wtd_frame_decode(buf, used,
					&consumed, &flag, &payload, &plen);
			if (st == WTD_FRAME_INCOMPLETE) {
				break;
			}
			if (st != WTD_FRAME_OK) {
				goto done;
			}
			wtd_outbound_frame_t *f = (wtd_outbound_frame_t *)malloc(
					sizeof(wtd_outbound_frame_t) + plen);
			if (f == NULL) {
				goto done;
			}
			f->next = NULL;
			f->flag = flag;
			f->payload_len = plen;
			if (plen > 0) {
				memcpy(f->payload, payload, plen);
			}
			wtd_work_queue_push(&s->outbound, f);
			if (s->on_ready != NULL) {
				s->on_ready(s->on_ready_ctx);
			}
			memmove(buf, buf + consumed, used - consumed);
			used -= consumed;
		}
	}

done:
	atomic_store(&s->reader_done, 1);
	if (s->on_ready != NULL) {
		s->on_ready(s->on_ready_ctx);
	}
	free(buf);
	return NULL;
}

int wtd_peer_session_start_reader(wtd_peer_session_t *s, int fd,
		wtd_on_outbound_ready_fn on_ready, void *ctx) {
	if (s == NULL || fd < 0) {
		return -EINVAL;
	}
	if (s->reader_thread_started) {
		return -EALREADY;
	}
	s->reader_fd = fd;
	s->on_ready = on_ready;
	s->on_ready_ctx = ctx;
	int rc = pthread_create(&s->reader_thread, NULL, reader_main, s);
	if (rc != 0) {
		return -rc;
	}
	s->reader_thread_started = 1;
	return 0;
}

void wtd_peer_session_stop_reader(wtd_peer_session_t *s) {
	if (s == NULL || !s->reader_thread_started) {
		return;
	}
	(void)pthread_join(s->reader_thread, NULL);
	s->reader_thread_started = 0;
}

void wtd_peer_session_destroy(wtd_peer_session_t *s) {
	if (s == NULL) {
		return;
	}
	wtd_peer_session_stop_reader(s);
	wtd_work_queue_destroy(&s->outbound);
}
