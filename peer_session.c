/*
 * webtransportd — peer_session.c
 *
 * Currently only the work-queue primitive. Reader thread, child wiring,
 * and picoquic glue arrive in subsequent cycles when their tests demand
 * them.
 */

#include "peer_session.h"

#include <stdlib.h>

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
