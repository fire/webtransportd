# webtransportd — TDD-driven build.
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

# Cycle 19-20: webtransportd binary. Only our own .c files — picoquic /
# mbedtls / picotls wiring arrives in cycle 21+.
webtransportd: webtransportd.c version.h
	@echo "  CC     $@"
	$(CC) $(CFLAGS) -o $@ webtransportd.c $(LDFLAGS)

# version_test fork/execs ./webtransportd, so it needs that binary built
# first. The test compiles standalone (no matching version.c).
version_test: version_test.c version.h webtransportd
	@echo "  CC     $@ (smoke: execs ./webtransportd)"
	$(CC) $(CFLAGS) -o $@ version_test.c $(LDFLAGS)

# Cycles 21a-b: vendored picoquic bring-up. The include paths use
# -isystem so our -Werror doesn't trip on third-party headers; vendored
# .c files compile under VENDOR_CFLAGS which keeps sanitizers on but
# drops -Werror (they are not our source to clean). Include paths cover
# picoquic's own transitive header needs (picotls for ech.c / tls_api.c
# and the crypto bridges, mbedtls for the picoquic_mbedtls.c bridge).
PICOQUIC_ISYSTEM := -isystem thirdparty/picoquic/picoquic
PICOQUIC_DEFS := \
    -DPICOQUIC_WITH_MBEDTLS=1 \
    -DPTLS_WITHOUT_OPENSSL=1 \
    -DPTLS_WITHOUT_FUSION=1 \
    -DDISABLE_DEBUG_PRINTF=1
VENDOR_ISYSTEM := \
    -isystem thirdparty/picoquic/picoquic \
    -isystem thirdparty/picoquic/picohttp \
    -isystem thirdparty/picoquic/picoquic_mbedtls \
    -isystem thirdparty/picotls/include \
    -isystem thirdparty/mbedtls/include
VENDOR_CFLAGS := -O0 -g -std=c11 -w -pthread \
                 -fsanitize=address,undefined -fno-omit-frame-pointer \
                 $(VENDOR_ISYSTEM) $(PICOQUIC_DEFS)

# Cycle 21b: every file in picoquic/ must compile under VENDOR_CFLAGS.
# winsockloop.c is Windows-only and drops out on POSIX builds. The
# generic pattern writes .o next to its .c so `make clean` can blow the
# whole third-party build away with one glob.
PICOQUIC_CORE_SRCS := $(filter-out thirdparty/picoquic/picoquic/winsockloop.c, \
                       $(wildcard thirdparty/picoquic/picoquic/*.c))
PICOQUIC_CORE_OBJS := $(PICOQUIC_CORE_SRCS:.c=.o)

thirdparty/picoquic/picoquic/%.o: thirdparty/picoquic/picoquic/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

# picoquic_link_test forces every PICOQUIC_CORE_OBJS build as a prereq;
# if any .c in picoquic/ regresses compile, `make test` goes red. The
# test still only *links* error_names.o (cycle 21a slice); wider link
# arrives with the picotls/mbedtls slices.
picoquic_link_test: picoquic_link_test.c $(PICOQUIC_CORE_OBJS)
	@echo "  CC     $@"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ picoquic_link_test.c \
		thirdparty/picoquic/picoquic/error_names.o $(LDFLAGS)

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
	rm -f $(TESTS_BIN) webtransportd *.o
	rm -f thirdparty/picoquic/picoquic/*.o
