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
 * Only the work queue is wired up in this cycle; the rest grows as the
 * next tests demand it.
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

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_PEER_SESSION_H */
