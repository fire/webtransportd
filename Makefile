# webtransportd-2 — TDD-driven build.
#
# `make test` runs every `*_test.c` in this directory as its own binary,
# expecting exit code 0. The loop is intentionally minimal so the
# red-green-refactor cycle stays fast.

CC      ?= cc
CFLAGS  ?= -O0 -g -Wall -Wextra -Werror -std=c11
LDFLAGS ?=

TESTS_SRC := $(wildcard *_test.c)
TESTS_BIN := $(TESTS_SRC:_test.c=_test)

# Source files for the unit-under-test live next to the test that exercises
# them: e.g. frame_test.c links frame.c. We discover this by stripping `_test`
# from each test name and looking for a matching .c (and .h).
#
# Match recipe pairs the test with its .c + .h so that editing the
# implementation re-triggers a build. Fallback recipe handles tests that
# need no matching source (header-only or fully self-contained).
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
