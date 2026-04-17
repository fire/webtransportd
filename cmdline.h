/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — cmdline.h
 *
 * Encode a POSIX argv[] into a single Windows command-line string
 * that `CommandLineToArgvW` (and the CRT's own argv parser) will
 * decode back to the same sequence of arguments, byte-for-byte.
 * Rules are from MSDN's "Parsing C++ Command-Line Arguments":
 *
 *   * Arguments are space/tab-separated at the top level.
 *   * An argument containing whitespace or a double quote must be
 *     wrapped in "...".
 *   * Inside a wrapped argument, a run of N backslashes followed by
 *     a double quote encodes as 2N backslashes + `\"`. A run of N
 *     backslashes at the very end of the argument (just before the
 *     closing `"`) encodes as 2N backslashes. Backslashes not
 *     followed by a quote pass through unchanged.
 *
 * The encoder is pure C, no platform headers, no I/O — so it is
 * safe to compile and unit-test on POSIX even though the output is
 * consumed only on Windows. Returns 0 on success, -1 if `out` is
 * smaller than the encoded string (including the trailing NUL).
 */

#ifndef WEBTRANSPORTD_CMDLINE_H
#define WEBTRANSPORTD_CMDLINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int wtd_build_cmdline(const char *const *argv, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_CMDLINE_H */
