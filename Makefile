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

# Cycle 19-20: webtransportd binary. 21d.1 links the full vendored
# object set for picoquic_create/--selftest; 22a adds child_process.c
# for the --exec=BIN spawn path; 22b adds peer_session.c + frame.c so
# the reader thread can decode the child's framed stdout into the
# outbound work queue. The -isystem keeps -Werror quiet on
# picoquic.h / picoquic_packet_loop.h.
webtransportd: webtransportd.c version.h \
               child_process.c child_process.h \
               peer_session.c peer_session.h \
               frame.c frame.h \
               $(VENDOR_ALL_OBJS)
	@echo "  CC     $@ (full vendored link + child_process + peer_session)"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ webtransportd.c child_process.c peer_session.c frame.c \
		$(VENDOR_ALL_OBJS) $(LDFLAGS)

# Cycle 22b: tiny helper child used by handshake_socket_test to prove
# the daemon's peer_session reader decodes frames off child stdout.
examples/frame_hi: examples/frame_hi.c
	@echo "  CC     $@"
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# version_test fork/execs ./webtransportd, so it needs that binary built
# first. The test compiles standalone (no matching version.c).
version_test: version_test.c version.h webtransportd
	@echo "  CC     $@ (smoke: execs ./webtransportd)"
	$(CC) $(CFLAGS) -o $@ version_test.c $(LDFLAGS)

# selftest_test fork/execs ./webtransportd --selftest, same pattern.
selftest_test: selftest_test.c webtransportd
	@echo "  CC     $@ (smoke: execs ./webtransportd --selftest)"
	$(CC) $(CFLAGS) -o $@ selftest_test.c $(LDFLAGS)

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
    -isystem thirdparty/picoquic/loglib \
    -isystem thirdparty/picotls/include \
    -isystem thirdparty/picotls/deps/cifra/src \
    -isystem thirdparty/picotls/deps/cifra/src/ext \
    -isystem thirdparty/picotls/deps/micro-ecc \
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

# Cycle 21c: picohttp + picotls (minus 5 files not compiled under our
# flags: brotli-backed certificate_compression, x86-only fusion, the
# broken upstream mbedtls_sign superseded by picoquic_mbedtls's copy,
# the openssl-dependent openssl.c, and the sha2-dependent uecc) +
# picoquic_mbedtls + mbedtls/library (minus Godot's platform shim for
# the legacy mbedtls config.h layout). Together these supply the TLS
# symbols picoquic_create needs.
PICOHTTP_SRCS := $(wildcard thirdparty/picoquic/picohttp/*.c)
PICOHTTP_OBJS := $(PICOHTTP_SRCS:.c=.o)

PICOTLS_EXCLUDE := \
    thirdparty/picotls/lib/certificate_compression.c \
    thirdparty/picotls/lib/fusion.c \
    thirdparty/picotls/lib/mbedtls.c \
    thirdparty/picotls/lib/mbedtls_sign.c \
    thirdparty/picotls/lib/openssl.c
PICOTLS_SRCS := $(filter-out $(PICOTLS_EXCLUDE), $(wildcard thirdparty/picotls/lib/*.c))
PICOTLS_OBJS := $(PICOTLS_SRCS:.c=.o)

# micro-ecc provides the secp256r1 curve arithmetic that picotls's uecc.c
# bridges to ptls_minicrypto_secp256r1 / _init_secp256r1sha256_sign_certificate.
MICROECC_OBJS := thirdparty/picotls/deps/micro-ecc/uECC.o

PICOQUIC_MBEDTLS_SRCS := $(wildcard thirdparty/picoquic/picoquic_mbedtls/*.c)
PICOQUIC_MBEDTLS_OBJS := $(PICOQUIC_MBEDTLS_SRCS:.c=.o)

MBEDTLS_EXCLUDE := thirdparty/mbedtls/library/godot_core_mbedtls_platform.c
MBEDTLS_SRCS := $(filter-out $(MBEDTLS_EXCLUDE), $(wildcard thirdparty/mbedtls/library/*.c))
MBEDTLS_OBJS := $(MBEDTLS_SRCS:.c=.o)

# loglib provides picoquic_set_qlog and friends (config.c references it).
LOGLIB_SRCS := $(wildcard thirdparty/picoquic/loglib/*.c)
LOGLIB_OBJS := $(LOGLIB_SRCS:.c=.o)

# picotls/lib/cifra provides ptls_minicrypto_* symbols. libaegis needs an
# external <aegis.h> not vendored here; skip it.
PICOTLS_CIFRA_EXCLUDE := thirdparty/picotls/lib/cifra/libaegis.c
PICOTLS_CIFRA_SRCS := $(filter-out $(PICOTLS_CIFRA_EXCLUDE), \
                       $(wildcard thirdparty/picotls/lib/cifra/*.c))
PICOTLS_CIFRA_OBJS := $(PICOTLS_CIFRA_SRCS:.c=.o)

# cifra internals. Exclude: all test drivers (test*.c) and the curve25519
# alt-implementations (curve25519.c wraps tweetnacl via #include; the other
# two are unused variants that would double-define symbols).
CIFRA_SRCS_ALL := $(wildcard thirdparty/picotls/deps/cifra/src/*.c)
CIFRA_EXCLUDE := $(wildcard thirdparty/picotls/deps/cifra/src/test*.c) \
                 thirdparty/picotls/deps/cifra/src/curve25519.donna.c \
                 thirdparty/picotls/deps/cifra/src/curve25519.naclref.c \
                 thirdparty/picotls/deps/cifra/src/curve25519.tweetnacl.c
CIFRA_SRCS := $(filter-out $(CIFRA_EXCLUDE), $(CIFRA_SRCS_ALL))
CIFRA_OBJS := $(CIFRA_SRCS:.c=.o)

VENDOR_ALL_OBJS := $(PICOQUIC_CORE_OBJS) $(PICOHTTP_OBJS) $(PICOTLS_OBJS) \
                   $(PICOTLS_CIFRA_OBJS) $(CIFRA_OBJS) $(MICROECC_OBJS) \
                   $(PICOQUIC_MBEDTLS_OBJS) $(MBEDTLS_OBJS) $(LOGLIB_OBJS)

thirdparty/picoquic/picoquic/%.o: thirdparty/picoquic/picoquic/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

thirdparty/picoquic/picohttp/%.o: thirdparty/picoquic/picohttp/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

thirdparty/picotls/lib/%.o: thirdparty/picotls/lib/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

thirdparty/picoquic/picoquic_mbedtls/%.o: thirdparty/picoquic/picoquic_mbedtls/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

thirdparty/mbedtls/library/%.o: thirdparty/mbedtls/library/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

thirdparty/picoquic/loglib/%.o: thirdparty/picoquic/loglib/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

thirdparty/picotls/lib/cifra/%.o: thirdparty/picotls/lib/cifra/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

thirdparty/picotls/deps/cifra/src/%.o: thirdparty/picotls/deps/cifra/src/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

thirdparty/picotls/deps/micro-ecc/%.o: thirdparty/picotls/deps/micro-ecc/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

# picoquic_link_test forces every PICOQUIC_CORE_OBJS build as a prereq;
# if any .c in picoquic/ regresses compile, `make test` goes red. The
# test only *links* error_names.o (cycle 21a slice).
picoquic_link_test: picoquic_link_test.c $(PICOQUIC_CORE_OBJS)
	@echo "  CC     $@"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ picoquic_link_test.c \
		thirdparty/picoquic/picoquic/error_names.o $(LDFLAGS)

# Cycle 21c: links the entire vendored object set so picoquic_create
# can exercise the real TLS initialisation path through mbedtls.
picoquic_create_test: picoquic_create_test.c $(VENDOR_ALL_OBJS)
	@echo "  CC     $@ (full vendored link)"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ picoquic_create_test.c $(VENDOR_ALL_OBJS) $(LDFLAGS)

# Cycle 21d.2: in-process client+server handshake pumped synchronously.
# Reads thirdparty/picoquic/certs/{cert,key}.pem at runtime, so `make test`
# must run from the project root (it does).
handshake_test: handshake_test.c $(VENDOR_ALL_OBJS)
	@echo "  CC     $@ (full vendored link)"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ handshake_test.c $(VENDOR_ALL_OBJS) $(LDFLAGS)

# Cycle 21d.3: real-socket handshake. Fork/execs ./webtransportd --server
# on a fixed loopback UDP port and drives a picoquic client against it
# over real sendto/recvfrom, so this test needs the daemon binary built.
handshake_socket_test: handshake_socket_test.c webtransportd examples/frame_hi $(VENDOR_ALL_OBJS)
	@echo "  CC     $@ (loopback UDP handshake)"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ handshake_socket_test.c $(VENDOR_ALL_OBJS) $(LDFLAGS)

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
	rm -f $(TESTS_BIN) webtransportd examples/frame_hi *.o
	rm -f thirdparty/picoquic/picoquic/*.o
	rm -f thirdparty/picoquic/picohttp/*.o
	rm -f thirdparty/picoquic/picoquic_mbedtls/*.o
	rm -f thirdparty/picoquic/loglib/*.o
	rm -f thirdparty/picotls/lib/*.o
	rm -f thirdparty/picotls/lib/cifra/*.o
	rm -f thirdparty/picotls/deps/cifra/src/*.o
	rm -f thirdparty/picotls/deps/micro-ecc/*.o
	rm -f thirdparty/mbedtls/library/*.o
