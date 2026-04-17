/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — peer_session.h
 *
 * State for a single accepted WebTransport session:
 *   - picoquic cnx pointer (added in a later cycle)
 *   - the spawned child process
 *   - an outbound work queue: frames the child wrote to its stdout,
 *     parsed by the per-peer reader thread, awaiting flush onto the
 *     cnx by the picoquic network thread.
 *
 * Cycle 18 adds the reader thread: wtd_peer_session_start_reader
 * spawns a pthread that read()s the supplied fd, feeds bytes through
 * wtd_frame_decode, pushes each complete frame onto the work queue, and
 * fires an on_outbound_ready callback so the network thread can wake
 * up. The thread exits on EOF (read returns 0) or on a decode error.
 *
 * fd ownership stays with the caller: to shut the thread down, the
 * caller must cause EOF on the fd (typically by closing the peer's
 * write end), then call wtd_peer_session_stop_reader to join.
 */

#ifndef WEBTRANSPORTD_PEER_SESSION_H
#define WEBTRANSPORTD_PEER_SESSION_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wtd_outbound_frame {
	struct wtd_outbound_frame *next;
	uint8_t flag;
	size_t payload_len;
	uint8_t payload[]; /* flexible array member */
} wtd_outbound_frame_t;

typedef struct wtd_work_queue {
	pthread_mutex_t mutex;
	wtd_outbound_frame_t *head;
	wtd_outbound_frame_t *tail;
} wtd_work_queue_t;

void wtd_work_queue_init(wtd_work_queue_t *q);
void wtd_work_queue_destroy(wtd_work_queue_t *q);

/* Takes ownership of f. Thread-safe. */
void wtd_work_queue_push(wtd_work_queue_t *q, wtd_outbound_frame_t *f);

/* Atomically detach the whole list; caller owns every node and must
 * free() each one. Returns NULL if empty. */
wtd_outbound_frame_t *wtd_work_queue_drain(wtd_work_queue_t *q);

typedef void (*wtd_on_outbound_ready_fn)(void *ctx);

typedef struct wtd_peer_session {
	wtd_work_queue_t outbound;
	pthread_t reader_thread;
	int reader_thread_started;
	int reader_fd;
	wtd_on_outbound_ready_fn on_ready;
	void *on_ready_ctx;
} wtd_peer_session_t;

void wtd_peer_session_init(wtd_peer_session_t *s);

/* destroy() joins the reader thread if it is still running. The caller
 * must first close the fd's producer side (or otherwise EOF it) or the
 * thread will block forever in read(). */
void wtd_peer_session_destroy(wtd_peer_session_t *s);

/* Spawn the reader thread. Returns 0 on success, -errno on failure.
 * fd is not closed by the session; the caller retains ownership. */
int wtd_peer_session_start_reader(wtd_peer_session_t *s, int fd,
		wtd_on_outbound_ready_fn on_ready, void *ctx);

/* Join the reader thread. Caller must have already caused EOF on the
 * fd (typically by close()ing the writer end) so the read() returns. */
void wtd_peer_session_stop_reader(wtd_peer_session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_PEER_SESSION_H */
