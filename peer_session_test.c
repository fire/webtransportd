/* TDD log:
 * - Cycle 17 (this file): peer_session.h exposes a tiny mutex-guarded
 *   work queue that the per-peer reader thread pushes onto and the
 *   network thread drains. Three pushes followed by one drain must
 *   return all three frames in FIFO order, with payloads + flags
 *   intact.
 *
 *   The reader thread itself comes in cycle 18; this cycle proves the
 *   queue primitive in isolation.
 */

#include "peer_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
	cycle17_fifo_drain();
	return failures == 0 ? 0 : 1;
}
