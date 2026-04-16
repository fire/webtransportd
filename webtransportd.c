/*
 * webtransportd — daemon entry point.
 *
 * Cycle 19-20: argv parsing + --version. The real picoquic-driven
 * session loop lands in cycle 21+; for now anything other than
 * --version prints a one-line usage hint to stderr and exits non-zero.
 */

#include "version.h"

#include <stdio.h>
#include <string.h>

static int print_usage(FILE *out) {
	fprintf(out,
			"usage: webtransportd --version\n"
			"       (more flags arrive in later cycles)\n");
	return 0;
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0) {
			printf("webtransportd %s\n", WTD_VERSION);
			return 0;
		}
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			(void)print_usage(stdout);
			return 0;
		}
		fprintf(stderr, "webtransportd: unknown argument: %s\n", argv[i]);
		(void)print_usage(stderr);
		return 2;
	}
	/* No args yet means "nothing to do" — the session loop arrives in
	 * cycle 21+. Until then, print usage and exit non-zero so scripts
	 * don't mistake us for a running daemon. */
	(void)print_usage(stderr);
	return 2;
}
