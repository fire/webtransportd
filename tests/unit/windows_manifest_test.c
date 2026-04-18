/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/* Cycle 40a: verify that webtransportd's Windows build has the
 * UTF-8 activeCodePage manifest embedded. `GetACP()` returns the
 * process's active ANSI code page; with our manifest it reports
 * 65001 (CP_UTF8), which is what makes CreateProcessA, main()'s
 * argv, getenv, and fopen treat their byte strings as UTF-8 on
 * Windows 10 1903+.
 *
 * The test itself is the daemon's own runtime environment, not a
 * second process — it's linked alone (no picoquic) and simply
 * asks the system about the calling process's active code page.
 * On POSIX there is nothing to check (the manifest is Windows-
 * only), so main returns 0.
 */

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>

int main(void) {
	UINT acp = GetACP();
	if (acp != CP_UTF8) {
		fprintf(stderr,
			"FAIL: GetACP()=%u, expected 65001 (CP_UTF8). "
			"The UTF-8 manifest is not being picked up by the CRT.\n",
			(unsigned)acp);
		return 1;
	}
	return 0;
}
#else
int main(void) {
	/* POSIX has no concept of an ANSI code page — argv/env/fopen
	 * are already byte-transparent, so UTF-8 parity is automatic.
	 * Pass trivially; the Windows job is what keeps us honest. */
	return 0;
}
#endif
