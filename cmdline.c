/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
#include "cmdline.h"

#include <string.h>

/* Encode argv[] as a Windows command-line string per MSDN's
 * "Parsing C++ Command-Line Arguments" rules. The decoder is
 * CommandLineToArgvW / the CRT's argv parser — round-tripping
 * argv → cmdline → argv must preserve every byte of every arg.
 *
 * Algorithm, per MSDN:
 *   * An arg that contains whitespace or `"` must be wrapped in
 *     "..." (or the parser splits on the whitespace, or treats
 *     the raw `"` as a delimiter).
 *   * Inside a wrapped arg, a run of N backslashes followed by
 *     `"` encodes as `2N backslashes + \"`. A run of N backslashes
 *     at the end of the wrapped arg (right before the closing `"`)
 *     encodes as `2N backslashes`. Backslashes that aren't
 *     followed by a `"` pass through literally.
 *   * An arg with no whitespace and no `"` emits bare — backslashes
 *     inside it pass through unescaped (no decoder ambiguity in
 *     that state).
 *
 * We only read bytes, not code points, so UTF-8 multi-byte
 * sequences that don't collide with ASCII specials (` `, `\t`,
 * `"`, `\`) pass through unchanged. The activeCodePage=UTF-8
 * manifest on the receiving side (cycle 40a) makes that a
 * byte-for-byte round-trip.
 */
int wtd_build_cmdline(const char *const *argv, char *out, size_t cap) {
	if (argv == NULL || out == NULL) {
		return -1;
	}
	size_t pos = 0;
	/* Helper macro: write one byte, bail on overflow. Leaves room
	 * for the trailing NUL by checking against cap - 1. */
#define PUT(c) do { if (pos + 1 >= cap) return -1; out[pos++] = (char)(c); } while (0)

	for (size_t i = 0; argv[i] != NULL; i++) {
		if (i > 0) {
			PUT(' ');
		}
		const char *a = argv[i];
		int needs_quotes = (a[0] == '\0')
		                || strpbrk(a, " \t\"") != NULL;
		if (needs_quotes) {
			PUT('"');
		}
		const char *s = a;
		while (*s != '\0') {
			if (*s == '\\') {
				size_t bs = 0;
				while (*s == '\\') {
					bs++;
					s++;
				}
				if (*s == '\0') {
					/* End of arg. Inside quotes, double the run
					 * so the closing `"` isn't swallowed; bare
					 * args, emit as-is. */
					size_t emit = needs_quotes ? bs * 2 : bs;
					for (size_t j = 0; j < emit; j++) {
						PUT('\\');
					}
				} else if (*s == '"') {
					/* Escape: 2N backslashes + \". Advance past
					 * the `"` so the outer loop doesn't re-handle
					 * it. */
					for (size_t j = 0; j < bs * 2; j++) {
						PUT('\\');
					}
					PUT('\\');
					PUT('"');
					s++;
				} else {
					/* Backslashes followed by a non-quote non-NUL
					 * character: literal. */
					for (size_t j = 0; j < bs; j++) {
						PUT('\\');
					}
				}
			} else if (*s == '"') {
				/* Quote with no preceding backslashes. Escape as
				 * `\"`. */
				PUT('\\');
				PUT('"');
				s++;
			} else {
				PUT(*s);
				s++;
			}
		}
		if (needs_quotes) {
			PUT('"');
		}
	}
	if (pos >= cap) {
		return -1;
	}
	out[pos] = '\0';
	return 0;

#undef PUT
}
