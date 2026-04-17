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
| `frame`         | 1–11   | Length-prefixed codec: flag byte + 1/2/4-byte varint + payload       |
| `log`           | 12     | Level filter, thread-safe stderr emit                                |
| `env`           | 13–15  | `WEBTRANSPORT_*` CGI set + `--passenv` whitelist                     |
| `child_process` | 16     | `fork + execvp` with 3 pipes, SIGTERM + reap                         |
| `peer_session`  | 17–18  | Mutex-guarded FIFO work queue + reader thread that decodes frames    |
| `thirdparty/`   | 21a–c  | Vendored picoquic + picohttp + picotls + mbedtls compile and link    |
| `webtransportd` | 19–22d | `--version`, `--selftest`, `--server` (synchronous loop), `--exec`   |

All work lives in one directory under ASAN+UBSAN. 12 test binaries green:

```
$ make test
  RUN    ./child_process_test
  RUN    ./env_test
  RUN    ./frame_test
  RUN    ./handshake_echo_test
  RUN    ./handshake_socket_test
  RUN    ./handshake_test
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
- **22d** — **client-visible echo.** `drain_outbound` now also
  calls `picoquic_add_to_stream` on the first `(cnx, stream_id)`
  that fired the stream-data callback, so the decoded payload goes
  back to the client over QUIC. `handshake_echo_test` installs a
  client-side `client_stream_cb` that accumulates received bytes;
  after sending `"world"` with FIN, it pumps until
  `cctx.recv_len == 5`, then `memcmp`s against `PAYLOAD`. Client
  sees its own bytes come back — the v0.1 echo works end-to-end
  under mbedtls-backed QUIC + ASAN+UBSAN. Mutation-tested by
  flipping the expected payload to `"WORLD"` (the `memcmp` fires
  without touching the handshake or length checks).

## Next up

### Cycle 22e — datagrams

Symmetry: `flag=1` frames ↔ WebTransport datagrams.
`picoquic_queue_datagram_frame` on the daemon side; test opens an
unreliable flow and round-trips.

### Cycle N — Graceful shutdown on real multi-cnx load

21d.3's daemon handles a single connection and self-exits after 200 ms
of ready state. Production: accept many, handle SIGTERM cleanly with
every in-flight session drained and every child reaped, no ASAN leaks.

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

## After the daemon works

- **CI**: `.github/workflows/webtransportd.yml` — three-job matrix
  (`linux-gcc`, `macos-clang`, `windows-clang-cl`), `actions/checkout@v4`,
  only compiler toolchain installed (no TLS package), `make test`
  must pass, `./webtransportd --version` smoke, upload the built
  binary via `actions/upload-artifact@v4`. Treat warnings as errors
  (`-Werror` / `/WX`).
- **Windows parity**: `Makefile.win` + `sources.mk` so the POSIX and
  `nmake` paths share the same source list.
- **Docs**: top-level `README.md` (usage, framing spec, CLI flags,
  relationship to Godot), `LICENSE` (BSD-2 for the daemon we author;
  upstream libs keep their own licenses in `thirdparty/*/LICENSE*`),
  `AUTHORS`, `CHANGES`.
- **Examples**: `examples/echo.c` (smallest compliant child),
  `examples/echo.sh` + `examples/frame-helper.sh` (shell-friendly
  framing helpers). `examples/frame_hi.c` already exists as the
  cycle-22b test helper.

## Future cycles (once v0.1 ships)

- Fuzz harness for the frame codec (libFuzzer against
  `wtd_frame_decode`; must never crash or report OOB under ASAN).
- `--cert=auto` persistence — generated cert+key survive restart.
- Per-peer flow control: apply WT stream backpressure when the
  child's stdin pipe fills, instead of dropping unreliable frames.
- 8-byte varint support + configurable `WTD_FRAME_MAX_PAYLOAD` so
  large reliable payloads don't force session close.
- Real `child_process_win.c` (today's impl is POSIX-only).
- Forward child `stderr_fd` to `wtd_log` (currently opened but unread).
- `--dir` / `--staticdir` — serve static files on non-WT paths for
  devconsole, mirroring websocketd's `http.go`.

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
