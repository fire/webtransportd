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
| `frame`         | 1–11, 24–25 | Length-prefixed codec: flag + 1/2/4/8-byte varint decode + payload; fuzz harness |
| `log`           | 12, 28 | Level filter, thread-safe mutex-guarded stderr emit, `[LEVEL]` prefix |
| `env`           | 13–15  | `WEBTRANSPORT_*` CGI set + `--passenv` whitelist                     |
| `child_process` | 16     | `fork + execvp` with 3 pipes, SIGTERM + reap                         |
| `peer_session`  | 17–18  | Mutex-guarded FIFO work queue + reader thread that decodes frames    |
| `thirdparty/`   | 21a–c  | Vendored picoquic + picohttp + picotls + mbedtls compile and link    |
| `webtransportd` | 19–30  | `--version`, `--selftest`, `--server` + `--exec` + `--log-level` + per-cnx `wtd_peer_t` list + richer `--help` |

All work lives in one directory under ASAN+UBSAN. 15 test binaries green:

```
$ make test
  RUN    ./child_process_test
  RUN    ./env_test
  RUN    ./frame_fuzz_test
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

### Cycles 1–20 — module foundations

Frame codec (1–11), log (12), env (13–15), child_process (16),
peer_session (17–18), daemon entry with `--version` (19–20). All
isolated unit tests with deliberate RED-then-GREEN slices.

### Cycles 21a–c — vendored build bring-up

- **21a** — `picoquic_error_name` links. Minimal slice: `-isystem` on
  the vendored picoquic header dir + four Godot-style preprocessor
  defines (`PICOQUIC_WITH_MBEDTLS`, `PTLS_WITHOUT_OPENSSL`,
  `PTLS_WITHOUT_FUSION`, `DISABLE_DEBUG_PRINTF`). Vendored TUs compile
  under a softer `-w` profile so their warnings don't fight `-Werror`.
- **21b** — every `thirdparty/picoquic/picoquic/*.c` compiles (50 TUs,
  excluding Windows-only `winsockloop.c`). `picoquic_link_test`
  force-builds the full object set, so any regression in any TU
  turns `make test` red.
- **21c** — the full vendored build links (217 TUs total). Exclusions
  match Godot's SCsub: brotli-dependent `certificate_compression`,
  x86-only `fusion`, upstream-broken `mbedtls_sign`, openssl-backed
  `openssl.c`, two alternate curve25519 implementations, all cifra
  test drivers, and Godot's `godot_core_mbedtls_platform.c` shim for
  the legacy mbedtls config layout. `picoquic_create_test` calls
  `picoquic_create(...)` with all-NULL args + zero reset seed;
  `picoquic_free` tears it down under ASAN+UBSAN with no leaks.

### Cycles 21d.1–d.3, 22a–b — daemon wiring

- **21d.1** — `webtransportd --selftest` runs
  `picoquic_create` + `picoquic_free` from the daemon binary itself
  (no test harness). Proves the full mbedtls-backed TLS init path
  works when statically linked into our real executable. **Note:**
  starting the packet-loop via `picoquic_start_network_thread` trips
  an ASAN/pthread crash on darwin-arm64 (pc=0 at thread start). See
  [Design notes](#design-notes).
- **21d.2** — in-process client+server handshake reaches
  `picoquic_state_ready`. Both sides run in one test process,
  sharing a simulated clock; packets pump synchronously via
  `picoquic_prepare_next_packet` ↔ `picoquic_incoming_packet`. No
  sockets, no pthread — exercises the real mbedtls handshake path
  without the pthread-ASAN blocker.
- **21d.3** — real-socket handshake over loopback UDP. The daemon
  drives `picoquic_packet_loop_v3` directly on its main thread via a
  stack-allocated `picoquic_network_thread_ctx_t` (sidesteps the
  pthread blocker). `handshake_socket_test` launches the daemon on
  a fixed port, binds its own UDP socket, pumps a picoquic client
  through `sendto`/`recvfrom`, and asserts `picoquic_state_ready`
  within a 5s budget. SIGTERM + `waitpid` tears the daemon down
  cleanly.
- **22a** — `--exec=<bin>` spawns the configured child the first
  time a connection reaches ready state; the child is reaped when
  the daemon tears down. First integration of `child_process.c` into
  the real server pipeline. `handshake_socket_test` checks for
  `child spawned pid=<N>` in the daemon's stdout.
- **22b** — the `wtd_peer_session` reader thread attaches to the
  spawned child's `stdout_fd`. Every complete frame the child writes
  lands on the outbound work queue; the loop callback drains it each
  iteration and prints `outbound frame: flag=… len=… payload=…`.
  `examples/frame_hi` is a tiny deterministic child that writes one
  `flag=0 payload="hi"` frame and exits; the test asserts the
  decoded line appears. First integration of `peer_session.c` + the
  reader thread into the real server.
- **22c** — end-to-end daemon-internal echo. `server_stream_cb` is
  installed as the per-cnx default callback; on
  `picoquic_callback_stream_data` it calls `wtd_frame_encode` into a
  scratch buffer and writes the framed bytes into the child's
  `stdin_fd`. With `--exec=/bin/cat` the full round-trip works:
  client bytes → daemon frames → cat stdin → cat stdout → reader
  thread decodes → work queue → loop log.
- **22d** — **client-visible stream echo.** `drain_outbound`
  `picoquic_add_to_stream`s decoded payloads back onto the first
  `(cnx, stream_id)` the stream-data callback saw.
  `handshake_echo_test` accumulates received bytes with its own
  `client_stream_cb` and `memcmp`s against `"world"` — client sees
  its own bytes return over mbedtls-backed QUIC + ASAN.
- **22e** — **datagram round-trip.** Both sides set
  `picoquic_tp_max_datagram_frame_size` via
  `picoquic_set_default_tp_value` so datagram support negotiates.
  `server_stream_cb` handles `picoquic_callback_datagram`: frames
  bytes with `flag=1` and writes them to child stdin. The reader
  thread decodes a `flag=1` frame off cat's stdout and
  `drain_outbound` echoes via `picoquic_queue_datagram_frame`
  instead of `picoquic_add_to_stream`. The client's callback
  accumulates `picoquic_callback_datagram` bytes separately;
  `handshake_echo_test` sends both `"world"` on stream 0 and
  `"dgram"` as a datagram, then asserts both come back on their
  respective channels. Mutation-tested on the datagram memcmp.
- **23** — **child stderr forwarding.** A small dedicated pthread
  reads the child's `stderr_fd` and emits every chunk to the
  daemon's own stderr, prefixed `child stderr: `. Previously the
  fd was opened by `wtd_child_spawn` and left unread (documented
  under Future cycles). `examples/frame_hi` now also writes
  `oops\n` to stderr before the frame; `handshake_socket_test`
  `dup2`s the daemon's stdout+stderr into one pipe and asserts
  `child stderr: oops` shows up alongside the prior 22b frame
  sentinel. Mutation-tested on the stderr assertion.
- **24** — **frame codec fuzz harness.** `frame_fuzz_test` runs
  20,000 random-buffer decode iterations and 2,000 random encode-
  decode round-trips under a deterministic seed (`0xC0DE`). Every
  decode return must be a documented status code; any `OK` return
  must produce a `(consumed, payload, payload_len)` triple that
  stays inside the input buffer. ASAN is the teeth — if decode
  ever reads past its input on a malformed prefix, the harness
  trips. Mutation-tested by dropping the decoder's
  `buf_len < total` bounds check — the C-level assertions fire
  immediately (and ASAN would have caught an actual OOB on
  production-sized buffers).
- **25** — **QUIC 8-byte varint decode.** `varint_decode`
  previously silently routed prefix-3 inputs through the 4-byte
  branch (mask 0x3f ignored the high bits), corrupting consumed
  and payload-start. Our encoder never emits prefix-3, so no
  in-tree damage, but any peer that sends one would have
  de-synced the stream. Now decode handles the full
  `prefix ∈ {0,1,2,3}` range: 1 / 2 / 4 / 8 byte forms, with
  `WTD_FRAME_MAX_PAYLOAD` still bounding the length above. New
  frame_test cases cover a valid prefix-3 roundtrip, an
  oversized prefix-3 rejected as `ERR_TOO_BIG`, and a truncated
  prefix-3 returning `INCOMPLETE`. fuzz_test's random coverage
  keeps 1/4 of inputs on this path so a regression would surface
  there too. Encoder still always picks the shortest form —
  extending it to emit 8-byte when `WTD_FRAME_MAX_PAYLOAD` rises
  above 2^30 is a future cycle.
- **26** — **SIGTERM is the only shutdown.** Removed the "exit
  1 s after the first ready state" timer that was a testing
  convenience. The daemon's packet loop now runs until SIGTERM/
  SIGINT flips `g_should_exit`; tests were already sending
  SIGTERM at the end of their scenarios so nothing observable
  regressed. Clears one of the odder production behaviours
  (a daemon that self-terminated was surprising) and lets the
  same build serve multiple concurrent handshakes once the
  per-cnx peer_session split arrives.
- **27** — **`--log-level=<0..4>` wires log.c into the daemon.**
  The log module had existed since cycle 12 but the daemon was
  using `fprintf(stderr, ...)` directly. Now `webtransportd`
  links `log.c`, parses `--log-level=<n>`, calls
  `wtd_log_set_level`, and emits a TRACE-level `packet loop ready`
  sentinel inside the `picoquic_packet_loop_ready` callback.
  `handshake_socket_test` passes `--log-level=4` and asserts the
  sentinel appears in the daemon's combined stdout+stderr log.
  Mutation-tested by dropping the flag from the test's argv — the
  new assertion fires (confirming the flag is what gates the
  output). Default level is `WTD_LOG_INFO`, so operators who
  don't pass `--log-level` see the same output as before.
- **28** — **`[LEVEL]` log prefix + consistent wtd_log use.**
  `log.c` now prepends `[ERROR] `, `[WARN] `, `[INFO] `, or
  `[TRACE] ` to each line it emits, making the daemon's
  interleaved stderr readable at a glance. Cycle 28 added a
  cycle28_level_prefix case in `log_test` that exercises all four
  levels. Separately, every remaining `fprintf(stderr, ...)` call
  site in the daemon was routed through `wtd_log`: startup errors
  (`picoquic_create` / `start_reader` / `child_spawn` failures,
  unknown args, bad flags) emit at `WTD_LOG_ERROR`; the stderr
  forwarder thread emits forwarded child lines at `WTD_LOG_INFO`.
  The main observable benefit is thread safety — the forwarder
  thread's writes are now mutex-guarded and can't interleave
  mid-line with main-thread writes. Test assertions using
  `strstr("child stderr: oops", ...)` still match against
  `[INFO] child stderr: oops`, so nothing downstream broke.

- **29** — **per-cnx `wtd_peer_t` list.** `server_ctx_t` used to
  keep a single spawned child + `peer_session` + `active_cnx`, so
  two concurrent clients would trample each other's echo target.
  Each cnx now gets its own `wtd_peer_t` (child, peer_session,
  stderr forwarder, active_stream_id, frames_pending), prepended
  to a linked list on first observation. `server_stream_cb`
  `peer_find`s by cnx pointer and routes encoded frames to that
  peer's `child.stdin_fd`; `drain_all_peers` iterates every peer
  and echoes decoded frames back through the peer's own cnx.
  Cleanup tears every peer down in `peer_destroy_all` before
  `picoquic_free`. New `handshake_multi_test` launches two clients
  ("aaaaa" and "bbbbb") against one `--exec=/bin/cat` daemon,
  asserts each client receives its own five bytes and none of
  the other's, and that the daemon log shows both outbound frames.
- **30** — **operator-friendly `--help`.** `print_usage` now
  includes a one-line description, a subcommand-by-subcommand
  summary (`--version` / `--selftest` / `--server`), an indented
  options table, and a short note about the on-pipe framing
  format (`flag | varint len | payload`). `help_test` fork/execs
  `./webtransportd --help`, checks exit 0, and asserts on a
  handful of distinctive substrings (`WebTransport`, every
  supported flag, `framing:`, `varint`). Tolerant by design —
  passes as long as the right sections exist, doesn't break
  every time the prose is polished.

## Wanted (roughly prioritized)

Nothing blocking the v0.1 echo — the daemon handshakes, round-trips
streams + datagrams through a child process, forwards child stderr,
and keeps concurrent clients isolated. These are the next genuinely
useful slices, rough order of leverage-per-effort:

1. **`--cert=auto`** — generate a self-signed cert+key in-memory via
   mbedtls so the daemon boots without a PEM pair on disk. Uses
   `mbedtls_x509write_crt_*` + `mbedtls_pk_write_key_der` + the
   existing `picoquic_set_tls_certificate_chain` /
   `picoquic_set_tls_key` pair. Opens the door to `--cert=auto`
   **persistence** as a follow-up (survives restart).
2. **Per-peer flow control.** When a child's stdin pipe fills, the
   current `write_all` just blocks the packet loop thread. Detect
   partial writes, stash the remaining frame on the peer, and apply
   WT stream-level backpressure instead of ever blocking the loop.
   For datagrams: drop + metric on overflow (already unreliable).
3. **8-byte varint encode + configurable `WTD_FRAME_MAX_PAYLOAD`.**
   Cycle 25 already added the decode side. Extend `varint_encode`
   and bump MAX above 2^30 so large reliable payloads don't force
   a session close. Smallest behavioural test: encode a 2^30-byte
   payload via a synthetic wrapper (no 1 GB buffer needed).
4. **`child_process_win.c`.** Today `child_process.c` is POSIX
   (fork + execvp + three pipe()s). Windows needs `CreateProcessW`
   + three `CreatePipe`s + `SetHandleInformation` so they survive
   exec. Test shape mirrors cycle 16.
5. **`--dir=<path>` / `--staticdir=<path>`** — serve static files
   on non-WT request paths, mirroring websocketd's `http.go`.
   Needed for the devconsole story.
6. **libFuzzer-driven frame fuzz.** Cycle 24 is a deterministic
   random harness; coverage-guided fuzz catches the rare-prefix
   bug much faster. Needs a `LLVMFuzzerTestOneInput` shim and a
   seed corpus; build with `-fsanitize=fuzzer`.
7. ✅ **Deterministic daemon-launch in tests (cycle 33).** The
   three tests that fork/exec `./webtransportd` on a port
   (`handshake_socket_test`, `handshake_echo_test`,
   `handshake_multi_test`) now derive their port from the test
   process's pid: `20000 + (getpid() & 0x1fff)`. A stale daemon
   from a previous failed run lands on a different port once its
   pid is reused, and the odds of two concurrent test runs picking
   the same port are ~1/8000. Could still be tightened by adding
   `--port=0` + printing the bound port from the daemon.

## Ship prep

- **CI**: ✅ `.github/workflows/webtransportd.yml` shipped in
  cycle 36. Three jobs: `linux-gcc`, `macos-clang`,
  `windows-mingw` (MSYS2 + mingw-w64). POSIX jobs run `make test`
  under ASAN+UBSAN; Windows disables sanitizers via CFLAGS/LDFLAGS
  env overrides because mingw's ASAN is unreliable. All three
  smoke `--version` and upload the binary. `windows-clang-cl`
  stays wanted for when a native `child_process_win.c` lands.
- **Makefile.win** + **sources.mk** so POSIX `make` and Windows
  `nmake` share one source list.
- **Docs**: top-level `README.md` (usage, framing spec, CLI flags,
  relationship to Godot); ✅ `LICENSE` (cycle 31, BSD-2 for our
  code + vendored-library terms); ✅ `AUTHORS` and ✅ `CHANGES`
  (cycle 35).
- **Examples**: ✅ `examples/echo.c` shipped in cycle 32 (minimal
  C reference child — decode framed stdin, re-encode to framed
  stdout). ✅ `examples/frame-helper.sh` shipped in cycle 34
  (emit one framed envelope, delegate varint math to python3;
  `frame_helper_test` asserts the output decodes back to the
  original flag + payload). Still wanted: a shell-side `echo.sh`
  equivalent. `examples/frame_hi.c` already exists as the
  cycle-22b test helper.

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
