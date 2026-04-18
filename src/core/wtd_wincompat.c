/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/* Windows-only compatibility shim.
 *
 * picotls's wincompat.h has `#define gettimeofday wintimeofday` plus a
 * forward declaration, and ships a wintimeofday.c in its MSVC build
 * subdir. Linking that stock .c under mingw-w64 hits a
 * `conflicting types` diagnostic because mingw's <time.h> defines
 * `struct timezone` differently from wincompat.h's shape — we'd have
 * to patch upstream to fix it. Define wintimeofday ourselves here so
 * the picotls link resolves. Signature matches the one in
 * thirdparty/picotls/picotlsvs/picotls/wincompat.h exactly. POSIX
 * builds skip this entire TU.
 */

#ifdef _WIN32

#include <stdint.h>
/* Drag mingw's <time.h> in first so `struct timezone` exists as a
 * concrete type before wincompat.h's forward declaration would
 * otherwise make it a local incomplete tag. */
#include <time.h>
#include <windows.h>

/* Match the wincompat.h prototype. No header include here — inlining
 * the declaration sidesteps the picotls include pulling in picotls's
 * own inconsistent ordering. */
int wintimeofday(struct timeval *tv, struct timezone *tz);

int wintimeofday(struct timeval *tv, struct timezone *tz)
{
	(void)tz; /* picotls passes NULL; honour that. */
	FILETIME ft;
	GetSystemTimePreciseAsFileTime(&ft);
	/* FILETIME is 100-ns units since 1601-01-01; convert to Unix
	 * microseconds since 1970. Delta = 11644473600 seconds. */
	uint64_t now = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	now -= 116444736000000000ull;      /* 1601 → 1970 in 100 ns */
	uint64_t usec = now / 10ull;       /* 100 ns → 1 us */
	if (tv != NULL) {
		tv->tv_sec = (long)(usec / 1000000ull);
		tv->tv_usec = (long)(usec % 1000000ull);
	}
	return 0;
}

#endif /* _WIN32 */
