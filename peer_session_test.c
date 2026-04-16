/* TDD log:
 * - Cycle 17: peer_session.h exposes a tiny mutex-guarded work queue
 *   that the per-peer reader thread pushes onto and the network thread
 *   drains. Three pushes followed by one drain must return all three
 *   frames in FIFO order, with payloads + flags intact.
 *
 * - Cycle 18 (added here): wtd_peer_session_start_reader spins a
 *   thread that reads a fd, decodes each complete frame with
 *   wtd_frame_decode, pushes it onto the outbound queue, and fires an
 *   on_outbound_ready callback. We write two encoded frames (one
 *   reliable, one unreliable, different payloads) into a pipe, start
 *   the reader, wait on a cv the callback signals until we've been
 *   notified twice, drain the queue, and verify FIFO order + flags +
 *   payload bytes. Closing the write end of the pipe lets the reader
 *   thread exit naturally via EOF so stop_reader can join cleanly.
 */

#include "peer_session.h"

#include "frame.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

static wtd_outbound_frame_t *make_frame(uint8_t flag, const char *s) {
	size_t n = strlen(s);
	wtd_outbound_frame_t *f = (wtd_outbound_frame_t *)malloc(
			sizeof(wtd_outbound_frame_t) + n);
	f->next = NULL;
	f->flag = flag;
	f->payload_len = n;
	memcpy(f->payload, s, n);
	return f;
}

static void cycle17_fifo_drain(void) {
	wtd_work_queue_t q;
	wtd_work_queue_init(&q);

	wtd_work_queue_push(&q, make_frame(0, "first"));
	wtd_work_queue_push(&q, make_frame(1, "second"));
	wtd_work_queue_push(&q, make_frame(0, "third"));

	wtd_outbound_frame_t *head = wtd_work_queue_drain(&q);
	EXPECT(head != NULL);

	const char *expected[] = { "first", "second", "third" };
	uint8_t expected_flag[] = { 0, 1, 0 };
	int i = 0;
	wtd_outbound_frame_t *cur = head;
	while (cur != NULL) {
		EXPECT(i < 3);
		if (i < 3) {
			size_t want = strlen(expected[i]);
			EXPECT(cur->payload_len == want);
			EXPECT(cur->flag == expected_flag[i]);
			EXPECT(memcmp(cur->payload, expected[i], want) == 0);
		}
		wtd_outbound_frame_t *next = cur->next;
		free(cur);
		cur = next;
		i++;
	}
	EXPECT(i == 3);

	/* Draining an empty queue returns NULL. */
	EXPECT(wtd_work_queue_drain(&q) == NULL);

	wtd_work_queue_destroy(&q);
}

typedef struct {
	pthread_mutex_t m;
	pthread_cond_t cv;
	int count;
} notify_t;

static void on_ready(void *ctx) {
	notify_t *n = (notify_t *)ctx;
	pthread_mutex_lock(&n->m);
	n->count++;
	pthread_cond_broadcast(&n->cv);
	pthread_mutex_unlock(&n->m);
}

static void cycle18_reader_parses_frames(void) {
	const uint8_t p1[] = "alpha";
	const uint8_t p2[] = "bravo-bravo";
	const size_t p1_len = sizeof(p1) - 1;
	const size_t p2_len = sizeof(p2) - 1;

	uint8_t wire[64];
	size_t n1 = 0, n2 = 0;
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE, p1, p1_len,
			wire, sizeof(wire), &n1) == WTD_FRAME_OK);
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_UNRELIABLE, p2, p2_len,
			wire + n1, sizeof(wire) - n1, &n2) == WTD_FRAME_OK);
	const size_t total = n1 + n2;

	int fds[2] = { -1, -1 };
	EXPECT(pipe(fds) == 0);

	ssize_t w = write(fds[1], wire, total);
	EXPECT(w == (ssize_t)total);
	/* Closing the write end lets the reader thread see EOF after it has
	 * consumed the two frames, so it exits on its own. */
	EXPECT(close(fds[1]) == 0);
	fds[1] = -1;

	notify_t n;
	pthread_mutex_init(&n.m, NULL);
	pthread_cond_init(&n.cv, NULL);
	n.count = 0;

	wtd_peer_session_t s;
	wtd_peer_session_init(&s);
	EXPECT(wtd_peer_session_start_reader(&s, fds[0], on_ready, &n) == 0);

	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 2;
	pthread_mutex_lock(&n.m);
	while (n.count < 2) {
		int rc = pthread_cond_timedwait(&n.cv, &n.m, &deadline);
		if (rc == ETIMEDOUT) {
			break;
		}
	}
	int got = n.count;
	pthread_mutex_unlock(&n.m);
	EXPECT(got >= 2);

	wtd_peer_session_stop_reader(&s);

	wtd_outbound_frame_t *head = wtd_work_queue_drain(&s.outbound);
	EXPECT(head != NULL);

	wtd_outbound_frame_t *cur = head;
	EXPECT(cur != NULL);
	if (cur != NULL) {
		EXPECT(cur->flag == WTD_FRAME_FLAG_RELIABLE);
		EXPECT(cur->payload_len == p1_len);
		EXPECT(memcmp(cur->payload, p1, p1_len) == 0);
		wtd_outbound_frame_t *next = cur->next;
		free(cur);
		cur = next;
	}
	EXPECT(cur != NULL);
	if (cur != NULL) {
		EXPECT(cur->flag == WTD_FRAME_FLAG_UNRELIABLE);
		EXPECT(cur->payload_len == p2_len);
		EXPECT(memcmp(cur->payload, p2, p2_len) == 0);
		wtd_outbound_frame_t *next = cur->next;
		free(cur);
		cur = next;
	}
	EXPECT(cur == NULL);

	wtd_peer_session_destroy(&s);
	if (fds[0] >= 0) {
		close(fds[0]);
	}
	pthread_cond_destroy(&n.cv);
	pthread_mutex_destroy(&n.m);
}

int main(void) {
	cycle17_fifo_drain();
	cycle18_reader_parses_frames();
	return failures == 0 ? 0 : 1;
}
