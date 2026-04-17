# webtransportd — TODO

A standalone C WebTransport (HTTP/3) daemon that exec's a child process
per accepted session and pipes data between the WT session and the
child's stdin/stdout. Modeled on
[websocketd](https://github.com/joewalnes/websocketd).

Built strictly red-green-refactor: every feature is driven by a failing
test, committed when green, then any cleanup is done with the test
still green. ASAN + UBSAN run on every test. The source tree is
self-contained — mbedtls, picoquic, and picotls are all vendored under
`thirdparty/` so there is no system TLS package dependency.

## Status at a glance

| Module          | Cycles | Subject                                                              |
| --------------- | :----: | -------------------------------------------------------------------- |
| `frame`         | 1–11, 24–25, 34 | Length-prefixed codec: flag + 1/2/4/8-byte varint decode + payload; fuzz harness; shell framing helper |
| `log`           | 12, 28 | Level filter, thread-safe mutex-guarded stderr emit, `[LEVEL]` prefix |
| `env`           | 13–15  | `WEBTRANSPORT_*` CGI set + `--passenv` whitelist                     |
| `child_process` | 16, 37 | POSIX `fork + execvp` + Win32 `CreateProcessA` + `CreatePipe`; 3 pipes, SIGTERM / TerminateProcess + reap |
| `peer_session`  | 17–18  | Mutex-guarded FIFO work queue + reader thread that decodes frames    |
| `thirdparty/`   | 21a–c, 38 | Vendored picoquic + picohttp + picotls + mbedtls compile and link; Makefile ordering so CI builds from clean |
| `webtransportd` | 19–30, 33 | `--version`, `--selftest`, `--server` + `--exec` + `--log-level` + per-cnx `wtd_peer_t` list + richer `--help` + pid-derived port for test isolation |
| ship prep       | 31–36  | `LICENSE`, `examples/echo.c`, `examples/frame-helper.sh`, `AUTHORS`, `CHANGES`, CI workflow (linux/macOS/Windows) |

All work lives in one directory under ASAN+UBSAN. 16 test binaries green:

```
$ make test
  RUN    ./child_process_test
  RUN    ./env_test
  RUN    ./frame_fuzz_test
  RUN    ./frame_helper_test
  RUN    ./frame_test
  RUN    ./handshake_echo_test
  RUN    ./handshake_multi_test
  RUN    ./handshake_socket_test
  RUN    ./handshake_test
  RUN    ./help_test
  RUN    ./log_test
  RUN    ./peer_session_test
  RUN    ./picoquic_create_test
  RUN    ./picoquic_link_test
  RUN    ./selftest_test
  RUN    ./version_test
  OK     all tests passed
```

## Cycle log

Moved to [CHANGES](CHANGES) — one entry per cycle, grouped by phase.
Read there for the development history; this file tracks state and
what's next.


## Wanted (Pareto frontier by value/effort)

Nothing blocking the v0.1 echo — the daemon handshakes, round-trips
streams + datagrams through a child process, forwards child stderr,
and keeps concurrent clients isolated. The list below is the Pareto
frontier: for each item there is nothing else on the board that is
simultaneously higher-value **and** lower-effort. Pick what fits
your time budget; items above are increasing in effort as you go
down, and each one unlocks a distinct capability.

### Frontier

1. **Windows UTF-8 parity.** *Effort: medium. Value: required.*
   Windows is a first-class target, not best-effort — UTF-8 paths,
   argv, env, and child-stdin/stdout must round-trip identically to
   POSIX. Concrete pieces:
   - Embed an application manifest with
     `<activeCodePage>UTF-8</activeCodePage>` so the CRT's `main`
     argv, `getenv`, and `fopen` all treat bytes as UTF-8 (Windows
     10 1903+). On mingw, ship via `windres` + an `.rc` file; on
     `clang-cl` via the `/MANIFEST:EMBED` link flag.
   - Verify `CreateProcessA` in `child_process.c` now sees UTF-8
     correctly (with the manifest active it does). If an older
     Windows target ever becomes a requirement, switch to
     `CreateProcessW` with a built UTF-16 command line — but
     don't do that work speculatively.
   - `build_cmdline()` still doesn't escape embedded quotes; add
     the MSDN "Parsing C++ Command-Line Arguments" escape rules
     (backslash runs before quotes, quote-inside-quote doubling).
   - New `handshake_utf8_test` (or extend `handshake_echo_test`)
     exec's the child through a path containing `文档`/`日本語`
     and sends a UTF-8 payload; asserts round-trip byte equality
     both ways.
   - `--version` / `--help` output should also round-trip UTF-8
     sentinel bytes without mojibake on Windows.
2. **8-byte varint encode + configurable `WTD_FRAME_MAX_PAYLOAD`.**
   *Effort: low. Value: low–medium.* Cycle 25 already added the
   decode side — mirror it on the encoder and bump the max above
   `2^30` so large reliable payloads don't force a session close.
   Smallest behavioural test: encode a `2^30`-byte payload via a
   synthetic wrapper (no 1 GB buffer needed). On the frontier
   because nothing else comes close to this little effort.
3. **`--cert=auto`.** *Effort: medium. Value: high.* Generate a
   self-signed cert+key in-memory via mbedtls so the daemon boots
   without a PEM pair on disk — the single biggest UX win for
   first-time operators. Uses `mbedtls_x509write_crt_*` +
   `mbedtls_pk_write_key_der` + the existing
   `picoquic_set_tls_certificate_chain` / `picoquic_set_tls_key`
   pair. Opens the door to `--cert=auto` **persistence** as a
   follow-up (survives restart).
4. **`--dir=<path>` / `--staticdir=<path>`.** *Effort: medium.
   Value: high.* Serve static files on non-WT request paths,
   mirroring websocketd's `http.go`. Unlocks the devconsole story
   (ship a browser client alongside the daemon in one process).
   Independent hot path from `--cert=auto`, so the two don't trade
   off against each other.
5. **`README.md`.** *Effort: medium. Value: high.* Usage,
   framing spec, CLI flags, relationship to Godot, quickstart.
   The single biggest adoption lever once `--cert=auto` lands.
   (The harness rule deliberately blocks this from going first —
   write docs against the real behaviour, not plans.)
6. **Per-peer flow control.** *Effort: high. Value: high.* When a
   child's stdin pipe fills, the current `write_all` blocks the
   packet loop thread. Detect partial writes, stash the remaining
   frame on the peer, and apply WT stream-level backpressure
   instead of ever blocking the loop. For datagrams: drop + metric
   on overflow (already unreliable). On the frontier despite the
   effort because nothing else is both higher-value AND lower-
   effort — it's the last correctness ceiling under load.

### Dominated (skip or postpone)

Items that another item beats on both axes, or that existing work
already covers — worth keeping a note on so they don't sneak back
in without a reason:

- **libFuzzer-driven frame fuzz.** Cycle 24's deterministic harness
  already fuzzes 20,000 decodes + 2,000 round-trips under ASAN per
  run. Coverage-guided fuzz would find rare-prefix bugs faster, but
  cycle 24 has found none to date — marginal value until it starts
  missing things. Revisit if a wild-input bug sneaks past it.
- **Dedicated Win32 `child_process` unit test (cmd.exe + findstr).**
  The Win32 path (cycle 37) is exercised transitively by the
  handshake tests once those port to Windows. Adding a second
  Windows-only unit test before then is duplicated plumbing.
- **`examples/echo.sh`.** `examples/echo.c` is the reference child,
  and `examples/frame-helper.sh` is the shell-side framing demo —
  together they already cover both reasons an operator would want a
  shell echo.
- **`Makefile.win` + `sources.mk`.** The existing `Makefile` under
  MSYS2 already proves Windows builds in CI (cycle 36). A dedicated
  nmake path would duplicate the source list without enabling a new
  toolchain we can't already use.
- **Native `windows-clang-cl` / MSVC build.** MSYS2 + mingw-w64
  already ships a working Windows binary from CI (cycle 36); a
  second Windows toolchain costs matrix time without enabling
  anything the first one doesn't. Revisit if a user actually
  needs an MSVC-linked `.exe`.

### Done (moved out of Wanted)

- ✅ **Win32 `child_process.c`** (cycle 37) — `CreatePipe` ×3 with
  `SetHandleInformation` on the parent-side ends, `CreateProcessA`
  with `STARTF_USESTDHANDLES`, `_open_osfhandle` wraps the pipe
  handles so the daemon's existing read/write/close calls still
  work. `WTD_CHILD_PID_NONE` hides the pid_t vs HANDLE switch from
  callers.
- ✅ **Deterministic daemon-launch ports in tests** (cycle 33) —
  the three tests that fork/exec `./webtransportd` now derive their
  port from the test process's pid: `20000 + (getpid() & 0x1fff)`.
  A stale daemon from a previous failed run lands on a different
  port once its pid is reused; odds of two concurrent test runs
  colliding are ~1/8000. Could still be tightened by adding
  `--port=0` + printing the bound port from the daemon.

## Design notes

### The darwin-arm64 ASAN/pthread_create blocker

`picoquic_start_network_thread` trips a NULL-function-pointer crash
inside ASAN's `pthread_create` interceptor on darwin-arm64:

```
ERROR: AddressSanitizer: SEGV on unknown address (pc 0x0 T1)
    #0  0x0  (<unknown module>)
    #1  asan_thread_start+0x4c
    #2  _pthread_start+0x84
```

Narrowed with a minimal repro: the function pointer to
`picoquic_packet_loop_v3` reaches `pthread_create` **intact** when
called directly — the thread starts and crashes on NULL deref inside
the function body (expected; NULL arg). But the pointer becomes zero
when routed through picoquic's two-level indirection
(`picoquic_internal_thread_create` → `picoquic_create_thread` →
`pthread_create`). Suspected arm64 vs arm64e ABI / PAC interaction
with the ASAN runtime. Not blocking anything we need — the
synchronous loop pattern is cleaner anyway.

### Synchronous packet loop on main thread

Instead of `picoquic_start_network_thread`, the daemon builds a
`picoquic_network_thread_ctx_t` on the main-thread stack, wires its
`loop_callback` + `loop_callback_ctx`, and invokes
`picoquic_packet_loop_v3(&tctx)` directly. SIGTERM flips an atomic
`g_should_exit` flag that the loop callback checks on each call to
return `PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP`. No pthread, no
`picoquic_wake_up_network_thread`, no thread-join dance — one event
loop with one exit path.

## Guiding principles

- **RED first, always.** Before writing code, write a test that
  compiles (or add only the bare symbol to make it compile) and fails
  at runtime. Mutation-test characterization cycles (ones that pass
  on first run) by briefly breaking the code to prove the assertions
  are load-bearing.
- **Narrow the slice.** Each cycle is one public behavior. If a RED
  needs two implementation changes to turn green, split it.
- **ASAN is part of the contract.** `-fsanitize=address,undefined`
  runs on every `make test`; an ASAN finding is a RED, not a warning.
- **Commit every green.** One commit per cycle (or tightly-paired
  cycle), message starts with `Cycle N: …` so the TDD arc is visible
  in `git log`.
- **No SCsub, no `#include` from Godot.** Operational separation is
  the whole point. The vendored `thirdparty/` is a snapshot, not a
  symlink.
