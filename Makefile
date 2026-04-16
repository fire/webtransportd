# webtransportd-2 — TDD-driven build.
#
# `make test` runs every `*_test.c` in this directory as its own binary,
# expecting exit code 0. The loop is intentionally minimal so the
# red-green-refactor cycle stays fast.

CC      ?= cc
# ASAN catches the latent OOB read that an "INCOMPLETE" decode could otherwise
# get away with by accident. -fno-omit-frame-pointer keeps stack traces clean.
CFLAGS  ?= -O0 -g -Wall -Wextra -Werror -std=c11 \
           -fsanitize=address,undefined -fno-omit-frame-pointer -pthread
LDFLAGS ?= -fsanitize=address,undefined -pthread

TESTS_SRC := $(wildcard *_test.c)
TESTS_BIN := $(TESTS_SRC:_test.c=_test)

# Source files for the unit-under-test live next to the test that exercises
# them: e.g. frame_test.c links frame.c. We discover this by stripping `_test`
# from each test name and looking for a matching .c (and .h).
#
# Match recipe pairs the test with its .c + .h so that editing the
# implementation re-triggers a build. Fallback recipe handles tests that
# need no matching source (header-only or fully self-contained).

# peer_session links frame.c because the reader thread decodes wire frames
# with wtd_frame_decode, and the test encodes the wire bytes it writes into
# the pipe with wtd_frame_encode. Explicit rule wins over the %_test pattern.
peer_session_test: peer_session_test.c peer_session.c peer_session.h frame.c frame.h
	@echo "  CC     $@ (peer_session.c + frame.c + $<)"
	$(CC) $(CFLAGS) -o $@ peer_session.c frame.c $< $(LDFLAGS)

%_test: %_test.c %.c %.h
	@echo "  CC     $@ ($*.c + $<)"
	$(CC) $(CFLAGS) -o $@ $*.c $< $(LDFLAGS)

%_test: %_test.c
	@echo "  CC     $@ (self-contained)"
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: test clean
test: $(TESTS_BIN)
	@for t in $(TESTS_BIN); do \
		echo "  RUN    ./$$t"; \
		./$$t || exit 1; \
	done
	@echo "  OK     all tests passed"

clean:
	rm -f $(TESTS_BIN) *.o
