# webtransportd — TDD-driven build.
#
# `make test` runs every `*_test.c` in this directory as its own binary,
# expecting exit code 0. The loop is intentionally minimal so the
# red-green-refactor cycle stays fast.

CC      ?= cc
# ASAN catches the latent OOB read that an "INCOMPLETE" decode could otherwise
# get away with by accident. -fno-omit-frame-pointer keeps stack traces clean.
#
# _GNU_SOURCE exposes strdup/setenv/unsetenv/environ from glibc headers under
# -std=c11 (without it, glibc hides POSIX-2008 symbols and -Werror turns the
# implicit decls into hard errors — macOS/clang doesn't enforce this, which
# hid the bug locally until the linux-gcc CI job caught it). _DARWIN_C_SOURCE
# re-enables BSD extensions on Darwin when _POSIX_C_SOURCE-style macros are
# active; it's a no-op elsewhere. Both macros are harmless on mingw.
CFLAGS  ?= -O0 -g -Wall -Wextra -Werror -std=c11 \
           -D_GNU_SOURCE -D_DARWIN_C_SOURCE \
           -fsanitize=address,undefined -fno-omit-frame-pointer -pthread
LDFLAGS ?= -fsanitize=address,undefined -pthread

# Cycle 40a: on Windows, embed a side-by-side assembly manifest that
# pins the process's active code page to UTF-8 (Windows 10 1903+).
# With this embedded, main()'s argv, getenv(), CreateProcessA(), and
# fopen() all interpret byte strings as UTF-8 — which is what lets
# --exec=<utf8-path> and friends just work. windres compiles the .rc
# into a COFF .o that the linker wraps into the .exe's resource
# section.
WINDRES ?= windres
ifeq ($(OS),Windows_NT)
WINRES_OBJ := webtransportd_rc.o
# Godot's modules/http3/SCsub establishes the proven recipe for this
# stack on mingw: link ws2_32 (winsock), iphlpapi (GetAdaptersAddresses
# for picosocks' local-IP enumeration), and bcrypt (Windows CNG RNG
# used by picotls's Windows path). --allow-multiple-definition papers
# over a MinGW 14+ header bug where IN6_* helpers are emitted as
# extern inline in every TU; the linker merges the duplicates.
WINDOWS_LIBS := -lws2_32 -liphlpapi -lbcrypt
WINDOWS_LDEXTRA := -Wl,--allow-multiple-definition
else
WINRES_OBJ :=
WINDOWS_LIBS :=
WINDOWS_LDEXTRA :=
endif

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

# Cycle 24: deterministic fuzz harness for the frame codec. Links frame.c
# directly (the %_test pattern would look for frame_fuzz.c which does
# not exist).
frame_fuzz_test: frame_fuzz_test.c frame.c frame.h
	@echo "  CC     $@ (frame codec fuzz)"
	$(CC) $(CFLAGS) -o $@ frame.c frame_fuzz_test.c $(LDFLAGS)

# Cycle 34: exec's the shell framing helper and decodes its output
# with wtd_frame_decode. Depends on both frame.c and the script
# (the script is not a build product — listing it here just makes
# the dependency explicit; `make clean` doesn't touch it).
frame_helper_test: frame_helper_test.c frame.c frame.h examples/frame-helper.sh
	@echo "  CC     $@ (frame-helper.sh round-trip)"
	$(CC) $(CFLAGS) -o $@ frame.c frame_helper_test.c $(LDFLAGS)

# Cycles 21a-b: vendored picoquic bring-up. The include paths use
# -isystem so our -Werror doesn't trip on third-party headers; vendored
# .c files compile under VENDOR_CFLAGS which keeps sanitizers on but
# drops -Werror (they are not our source to clean). Include paths cover
# picoquic's own transitive header needs (picotls for ech.c / tls_api.c
# and the crypto bridges, mbedtls for the picoquic_mbedtls.c bridge).
#
# NOTE: these definitions MUST live above the `webtransportd:` target
# because GNU make expands prerequisite lists when a rule is read, not
# when it fires. Defining VENDOR_ALL_OBJS later meant the prerequisite
# silently expanded to the empty string, so CI (starting from a clean
# tree) tried to link .o files that had never been scheduled to build.
PICOQUIC_ISYSTEM := -isystem thirdparty/picoquic/picoquic
PICOQUIC_DEFS := \
    -DPICOQUIC_WITH_MBEDTLS=1 \
    -DPTLS_WITHOUT_OPENSSL=1 \
    -DPTLS_WITHOUT_FUSION=1 \
    -DDISABLE_DEBUG_PRINTF=1

# picoquic's headers gate the Win32 socket include block on
# `_WINDOWS` (MSVC convention), not `_WIN32`. MSYS2/mingw doesn't
# auto-define `_WINDOWS`, so we have to do it ourselves on Windows
# builds — otherwise picoquic.h pulls in <arpa/inet.h> and the
# whole vendored tree fails to compile.
ifeq ($(OS),Windows_NT)
PICOQUIC_DEFS += -D_WINDOWS
endif
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

# On Windows, picotls.c does `#include "wincompat.h"` under _WINDOWS.
# That shim lives in the Visual Studio build subdir, not on a standard
# include path, so we have to add it explicitly. It must come *before*
# `thirdparty/picoquic/picoquic` in the -isystem order: picoquic ships
# its own `wincompat.h` that picotls's sources would otherwise pick up
# (picoquic's version is missing <ws2tcpip.h>, so inet_pton is unknown
# and `struct sockaddr_in6` is incomplete). picoquic's own .c files
# find their wincompat.h via the source-directory quoted-include
# search, so moving picotls first doesn't break them.
ifeq ($(OS),Windows_NT)
VENDOR_ISYSTEM := -isystem thirdparty/picotls/picotlsvs/picotls $(VENDOR_ISYSTEM)
endif
# Sanitizer flags for the vendored TU pile. mingw-w64 doesn't ship
# libasan so we turn them off on Windows; POSIX keeps them on under
# the same Makefile contract as our own sources. `-w` silences
# upstream-only diagnostics so -Werror on our code isn't contaminated
# by them. `-Wno-error=incompatible-pointer-types` un-promotes gcc
# 14+'s default-error for cifra's `_BitScanReverse(&uint32_t, ...)`
# call — `unsigned long *` vs `uint32_t *` are the same size on
# LLP64 but differently-typed (harmless at the ABI level).
ifeq ($(OS),Windows_NT)
VENDOR_SAN :=
VENDOR_WARN_SUPPRESS := -Wno-error=incompatible-pointer-types \
                        -Wno-error=implicit-function-declaration \
                        -Wno-error=int-conversion
else
VENDOR_SAN := -fsanitize=address,undefined
VENDOR_WARN_SUPPRESS :=
endif
VENDOR_CFLAGS := -O0 -g -std=c11 -w $(VENDOR_WARN_SUPPRESS) -pthread \
                 -D_GNU_SOURCE \
                 $(VENDOR_SAN) -fno-omit-frame-pointer \
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

# picotls.c on Windows calls `wintimeofday` (via wincompat.h's
# `#define gettimeofday wintimeofday`). Upstream ships wintimeofday.c
# in the MSVC build subdir but it triggers a `conflicting types`
# diagnostic under mingw's <time.h> `struct timezone` shape. We
# provide our own tiny shim in wtd_wincompat.c instead.
ifeq ($(OS),Windows_NT)
PICOTLS_OBJS += wtd_wincompat.o
endif

# Cycle 40-pre2: our own Windows-only gettimeofday shim. POSIX just
# skips this file — wtd_wincompat.o is only referenced inside
# `ifeq ($(OS),Windows_NT)` above.
wtd_wincompat.o: wtd_wincompat.c
	@echo "  CC     $@ (Windows gettimeofday shim)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

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

# Windows-only wintimeofday shim (picotls.c calls it under _WINDOWS).
thirdparty/picotls/picotlsvs/picotls/%.o: thirdparty/picotls/picotlsvs/picotls/%.c
	@echo "  CC     $@ (vendored, -w)"
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

# Cycle 19-20: webtransportd binary. 21d.1 links the full vendored
# object set for picoquic_create/--selftest; 22a adds child_process.c
# for the --exec=BIN spawn path; 22b adds peer_session.c + frame.c so
# the reader thread can decode the child's framed stdout; 27 adds
# log.c for --log-level. The -isystem keeps -Werror quiet on
# picoquic.h / picoquic_packet_loop.h.
webtransportd: webtransportd.c version.h \
               child_process.c child_process.h \
               peer_session.c peer_session.h \
               frame.c frame.h \
               log.c log.h \
               $(VENDOR_ALL_OBJS) $(WINRES_OBJ)
	@echo "  CC     $@ (full vendored link + child_process + peer_session + log)"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ webtransportd.c child_process.c peer_session.c \
		frame.c log.c \
		$(VENDOR_ALL_OBJS) $(WINRES_OBJ) \
		$(WINDOWS_LDEXTRA) $(WINDOWS_LIBS) $(LDFLAGS)

# Cycle 40a: Windows resource object. windres turns the .rc script
# (which just points at webtransportd.exe.manifest) into a COFF .o
# the mingw linker wraps into the .exe's resource section. Omitted
# from POSIX builds (WINRES_OBJ is empty there) so the rule just
# never fires.
webtransportd_rc.o: webtransportd.rc webtransportd.exe.manifest
	@echo "  RC     $@ (UTF-8 manifest)"
	$(WINDRES) -O coff -o $@ $<

# Cycle 22b: tiny helper child used by handshake_socket_test to prove
# the daemon's peer_session reader decodes frames off child stdout.
examples/frame_hi: examples/frame_hi.c
	@echo "  CC     $@"
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Cycle 32: reference C child — reads framed stdin, re-encodes
# payload with same flag, writes framed stdout. Links frame.c for
# the codec so operators can use it as a starting point.
examples/echo: examples/echo.c frame.c frame.h
	@echo "  CC     $@ (reference child + frame.c)"
	$(CC) $(CFLAGS) -I . -o $@ examples/echo.c frame.c $(LDFLAGS)

# version_test fork/execs ./webtransportd, so it needs that binary built
# first. The test compiles standalone (no matching version.c).
version_test: version_test.c version.h webtransportd
	@echo "  CC     $@ (smoke: execs ./webtransportd)"
	$(CC) $(CFLAGS) -o $@ version_test.c $(LDFLAGS)

# selftest_test fork/execs ./webtransportd --selftest, same pattern.
selftest_test: selftest_test.c webtransportd
	@echo "  CC     $@ (smoke: execs ./webtransportd --selftest)"
	$(CC) $(CFLAGS) -o $@ selftest_test.c $(LDFLAGS)

# Cycle 30: --help prints an operator-friendly summary. Fork/execs
# the daemon and asserts on a handful of distinctive substrings.
help_test: help_test.c webtransportd
	@echo "  CC     $@ (smoke: execs ./webtransportd --help)"
	$(CC) $(CFLAGS) -o $@ help_test.c $(LDFLAGS)

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
		-o $@ picoquic_create_test.c $(VENDOR_ALL_OBJS) $(WINDOWS_LDEXTRA) $(WINDOWS_LIBS) $(LDFLAGS)

# Cycle 21d.2: in-process client+server handshake pumped synchronously.
# Reads thirdparty/picoquic/certs/{cert,key}.pem at runtime, so `make test`
# must run from the project root (it does).
handshake_test: handshake_test.c $(VENDOR_ALL_OBJS)
	@echo "  CC     $@ (full vendored link)"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ handshake_test.c $(VENDOR_ALL_OBJS) $(WINDOWS_LDEXTRA) $(WINDOWS_LIBS) $(LDFLAGS)

# Cycle 21d.3: real-socket handshake. Fork/execs ./webtransportd --server
# on a fixed loopback UDP port and drives a picoquic client against it
# over real sendto/recvfrom, so this test needs the daemon binary built.
handshake_socket_test: handshake_socket_test.c webtransportd examples/frame_hi $(VENDOR_ALL_OBJS)
	@echo "  CC     $@ (loopback UDP handshake)"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ handshake_socket_test.c $(VENDOR_ALL_OBJS) $(WINDOWS_LDEXTRA) $(WINDOWS_LIBS) $(LDFLAGS)

# Cycle 22c: end-to-end daemon-internal echo. Client sends bytes on a
# QUIC stream; daemon frames them into the child's stdin; the reader
# thread reads the child's stdout, decodes the frame, and logs the
# payload. Cycle 32 switched the child from /bin/cat to the new
# reference child at examples/echo (byte-equivalent output, but
# actually exercises the frame codec on the child side too).
handshake_echo_test: handshake_echo_test.c webtransportd examples/echo $(VENDOR_ALL_OBJS)
	@echo "  CC     $@ (loopback UDP echo via examples/echo)"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ handshake_echo_test.c $(VENDOR_ALL_OBJS) $(WINDOWS_LDEXTRA) $(WINDOWS_LIBS) $(LDFLAGS)

# Cycle 29: two concurrent clients against one daemon. Asserts each
# sees its own echo, not the other's — exercises the per-cnx
# wtd_peer_t split.
handshake_multi_test: handshake_multi_test.c webtransportd $(VENDOR_ALL_OBJS)
	@echo "  CC     $@ (two concurrent loopback clients)"
	$(CC) $(CFLAGS) $(PICOQUIC_ISYSTEM) $(PICOQUIC_DEFS) \
		-o $@ handshake_multi_test.c $(VENDOR_ALL_OBJS) $(WINDOWS_LDEXTRA) $(WINDOWS_LIBS) $(LDFLAGS)

# Cycle 40a: the manifest test checks GetACP() inside its own
# process, so it needs the manifest linked into the test binary
# too — not just into webtransportd.exe. The .rc dependency is
# empty on POSIX ($(WINRES_OBJ) unset), so this rule collapses to
# a plain self-contained compile there.
windows_manifest_test: windows_manifest_test.c $(WINRES_OBJ)
	@echo "  CC     $@ (GetACP() smoke)"
	$(CC) $(CFLAGS) -o $@ windows_manifest_test.c $(WINRES_OBJ) $(LDFLAGS)

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
	rm -f $(TESTS_BIN) webtransportd \
		examples/frame_hi examples/echo \
		*.o
	rm -f thirdparty/picoquic/picoquic/*.o
	rm -f thirdparty/picoquic/picohttp/*.o
	rm -f thirdparty/picoquic/picoquic_mbedtls/*.o
	rm -f thirdparty/picoquic/loglib/*.o
	rm -f thirdparty/picotls/lib/*.o
	rm -f thirdparty/picotls/lib/cifra/*.o
	rm -f thirdparty/picotls/deps/cifra/src/*.o
	rm -f thirdparty/picotls/deps/micro-ecc/*.o
	rm -f thirdparty/mbedtls/library/*.o
