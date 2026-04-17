# Design: Fix github actions CI/CD

## Goal

Drive all three matrix jobs — `linux-gcc` (ubuntu-latest),
`macos-clang` (macos-latest), `windows-mingw` (MSYS2 + mingw-w64) —
to green on every push to `main`. Cycle 40a's manifest work landed
but CI is still red on two jobs because of platform-specific build
issues that Apple clang didn't flag locally. This design lists the
concrete breakages and the targeted fixes.

## Current state (run 24548309500)

- ✅ `macos-clang` green.
- ❌ `linux-gcc`: `thirdparty/picoquic/picoquic/picosocks.c` fails
  with `invalid use of undefined type 'struct in_pktinfo'`.
- ❌ `windows-mingw`: `env_test.c` fails with
  `implicit declaration of function 'setenv' / 'unsetenv'`.

## Root causes

### Linux: VENDOR_CFLAGS is missing `-D_GNU_SOURCE`

`struct in_pktinfo` is a Linux extension declared in
`<linux/in.h>` (included via `<netinet/in.h>`). glibc gates it
behind `_GNU_SOURCE`. Our main `CFLAGS` got `-D_GNU_SOURCE` in
cycle 39, but `VENDOR_CFLAGS` (used to compile every vendored .c
under `thirdparty/`) never received it. On Apple's libc the struct
is visible without any feature-test macro, so macOS CI stayed
green and the bug hid behind the platform difference until the
cycle 40-pre/-40a fixes let linux-gcc get far enough to hit the
vendored picoquic compile.

### Windows: CFLAGS override in CI drops feature macros, and mingw has no POSIX setenv

Two compounding issues:

1. `.github/workflows/webtransportd.yml`'s `windows-mingw` step
   overrides the entire `CFLAGS` env var to disable sanitizers.
   That override doesn't include `-D_GNU_SOURCE -D_DARWIN_C_SOURCE`,
   so any POSIX feature-test macro gating on Windows is turned off
   relative to the Linux/macOS builds. We want the same
   feature-test macros on every platform.
2. `env_test.c` calls `setenv()` and `unsetenv()`. mingw-w64 ships
   neither — the POSIX-only API pair is not in its `<stdlib.h>`.
   mingw provides `_putenv_s` and `_putenv` instead (MSVCRT).

## Fix plan

The fixes all land together as cycle 40-pre2. Each is a minimal
platform shim; no feature work changes.

### Fix 1 — Propagate `-D_GNU_SOURCE` into vendor compilation

In [Makefile](Makefile), add `-D_GNU_SOURCE` to `VENDOR_CFLAGS`.
The macro is a no-op on non-glibc systems (Darwin, mingw), so
defining it unconditionally keeps the Makefile simple.

### Fix 2 — Preserve feature macros in the Windows CI CFLAGS override

In [.github/workflows/webtransportd.yml](.github/workflows/webtransportd.yml),
update the `windows-mingw` job's `CFLAGS` env to include
`-D_GNU_SOURCE -D_DARWIN_C_SOURCE`. That keeps parity with the
Makefile's default `CFLAGS` on the other two jobs. LDFLAGS stays
`-pthread`.

### Fix 3 — Add setenv/unsetenv shim for Windows in env_test.c

`env_test.c` is a test-only file that exercises env.c. mingw has
`_putenv_s(name, value)` (same semantics as `setenv(name, value, 1)`
because `_putenv_s` always overwrites) and `_putenv("NAME=")`
deletes the variable. Guard the POSIX calls with `#ifdef _WIN32`
and swap them for the mingw equivalents. Don't touch env.c — it
only uses `strdup` and `getenv`, both available on mingw.

### Fix 4 — (Deferred) Daemon syscall parity on mingw

webtransportd.c uses `read(2)`, `write(2)`, `sigaction(3)`,
`pthread_*`. These aren't all present on plain mingw without extra
includes. **This fix is out of scope for this design** — the
three-job CI matrix will go green after fixes 1–3 because:

- `windows-mingw` builds `env_test`, `frame_test`, `log_test`, etc.
  successfully — those don't touch the daemon's source.
- The Windows job's `webtransportd.exe` link step is still expected
  to fail, but that happens after `make test` already proved the
  module-level tests on Windows. The step in the workflow that
  smokes `./webtransportd.exe --version` will go red; we accept
  that as a known-deferred item for the follow-up cycle that
  actually ports the daemon syscalls.

Actually — `make test` also builds `webtransportd` as a dependency
of `handshake_*_test` and `selftest_test`. So deferring fix 4 does
not get Windows CI fully green. Revise: fix 4 is REQUIRED.

Revised fix 4: minimal daemon cross-compile fixes on mingw.

- Add `#include <unistd.h>` in webtransportd.c (mingw provides
  `read`/`write` via unistd.h).
- Guard the `sigaction`-based signal setup in `cmd_server` with
  `#ifdef _WIN32`, fall back to `signal(SIGTERM, on_sigterm);
  signal(SIGINT, on_sigterm);`. mingw has signal.h with `signal()`
  but no `struct sigaction`.
- Add Windows-only link libraries in the Makefile:
  `WINDOWS_LIBS := -lws2_32 -liphlpapi -lbcrypt` + the linker
  flag `-Wl,--allow-multiple-definition` for MinGW's duplicate
  IN6_* extern-inline definitions (recipe matches Godot's
  `modules/http3/SCsub` — `ws2_32` for winsock, `iphlpapi` for
  `GetAdaptersAddresses`, `bcrypt` for picotls's Windows RNG).
- If picoquic still fails to link for symbol reasons, exclude
  `picoquic_ptls_minicrypto.c` from the Windows build (same as
  Godot) — we use the mbedtls backend so minicrypto is dead code
  and dragging cifra/micro-ecc with it.

### Fix 5 — Keep POSIX tests green on every fix

After each individual fix, run `make clean && make test` locally
on macOS. If any POSIX test starts failing, the fix is wrong.

## Execution checklist

1. Apply Fix 1 (Makefile vendor CFLAGS).
2. Apply Fix 2 (workflow yml Windows CFLAGS).
3. Apply Fix 3 (env_test.c shim).
4. Apply Fix 4 (webtransportd.c unistd + signal shim; Makefile
   Windows link libs; try without minicrypto-exclude first).
5. `make clean && make test` locally to confirm POSIX still green.
6. Cross-build via mingw locally from macOS:
   ```
   make clean && make \
     CC=x86_64-w64-mingw32-gcc \
     WINDRES=x86_64-w64-mingw32-windres \
     OS=Windows_NT \
     CFLAGS="-O0 -g -Wall -Wextra -Werror -std=c11 \
             -D_GNU_SOURCE -D_DARWIN_C_SOURCE \
             -fno-omit-frame-pointer" \
     LDFLAGS="" \
     -j4 webtransportd
   ```
   Iterate on any remaining errors.
7. Commit each fix as a separate commit under the
   `Cycle 40-pre2: ...` umbrella, or as a single commit if they
   all land cleanly together.
8. Push, poll `gh run view --branch main` until all three jobs
   conclude. Both windows-mingw and linux-gcc must turn green.

## Verification

- `gh run list --limit 1 --branch main` shows `success` for the
  most recent run after the push.
- All three jobs inside that run report ✓, including the
  `daemon --version smoke` step for the Windows job.
- `make clean && make test` locally on macOS still green.

## Risk notes

- The `daemon --version smoke` step on Windows requires an
  actually-runnable `webtransportd.exe`. If the mingw cross-compile
  succeeds (object files + link) but the binary crashes at runtime
  due to some subtler POSIX/Win32 mismatch (pthread vs Win32
  threads, say), the CI will fail at the smoke step and we need to
  iterate. Budget for one or two extra CI round-trips.
- Fix 4's `--allow-multiple-definition` is a duct-tape fix for a
  MinGW toolchain bug; ideally upstream fixes it. In the
  meantime Godot's SCsub uses the same workaround, so it's a
  proven-enough approach for the short term.
