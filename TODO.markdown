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

## Status

| Done | Module          | Cycles / subject                                                                                                                                    |     Sanitizer      |
| :--: | --------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- | :----------------: |
|  ✅  | `frame`         | 1-11: encode/decode, INCOMPLETE on prefix, reserved bits, 1/2/4-byte varints, MAX_PAYLOAD, BUF_TOO_SMALL, two-frames-one-buffer, public API settled |     ASAN+UBSAN     |
|  ✅  | `log`           | 12: level filter, thread-safe stderr emit                                                                                                           |     ASAN+UBSAN     |
|  ✅  | `env`           | 13-15: WEBTRANSPORT_REMOTE_ADDR → full CGI set → --passenv whitelist                                                                                |     ASAN+UBSAN     |
|  ✅  | `child_process` | 16: fork+execvp with 3 pipes, /bin/cat round-trip, SIGTERM+reap                                                                                     |     ASAN+UBSAN     |
|  ✅  | `peer_session`  | 17-18: mutex-guarded FIFO work queue + per-peer reader thread that decodes child stdout into frames and fires on_outbound_ready                     |     ASAN+UBSAN     |
|  ✅  | `webtransportd` | 19-20: `main()` + argv parsing + `--version`; 21d.1: `--selftest` exercises full mbedtls-backed `picoquic_create` + `picoquic_free` path from the daemon binary. Fork/exec smoke tests assert both (`version_test`, `selftest_test`) | ASAN+UBSAN |
|  ✅  | `thirdparty/`   | 21a-c: full vendored build — picoquic (50) + picohttp (13) + picotls/lib (9) + cifra adapters (5) + cifra internals (24) + micro-ecc (1) + picoquic_mbedtls (2) + mbedtls/library (103) + loglib (10) all compile + link; `picoquic_create(...)` returns non-NULL under mbedtls-backed TLS | ASAN+UBSAN |

Ten test binaries, all green:

```
$ make test
  RUN    ./child_process_test
  RUN    ./env_test
  RUN    ./frame_test
  RUN    ./handshake_test
  RUN    ./log_test
  RUN    ./peer_session_test
  RUN    ./picoquic_create_test
  RUN    ./picoquic_link_test
  RUN    ./selftest_test
  RUN    ./version_test
  OK     all tests passed
```

## Next up

### Cycle 21+ — Real handshake, picoquic bootstrap, end-to-end echo

Cycle 21 from the original plan was too fat. Split into sub-cycles,
each driven by one failing test:

- ✅ **21a (done)**: vendored `error_names.c` compiles with
  `-isystem thirdparty/picoquic/picoquic` and the four Godot-style
  defines (`PICOQUIC_WITH_MBEDTLS`, `PTLS_WITHOUT_OPENSSL`,
  `PTLS_WITHOUT_FUSION`, `DISABLE_DEBUG_PRINTF`). Vendored TUs use
  `-w` (sanitizers still on) so their warnings don't stop our
  `-Werror` build.
- ✅ **21b (done)**: every `thirdparty/picoquic/picoquic/*.c` (50 TUs,
  minus Windows-only `winsockloop.c`) compiles under VENDOR_CFLAGS
  with the wider include path (`picotls/include`, `mbedtls/include`,
  `picoquic_mbedtls`, `picohttp`). `picoquic_link_test` force-builds
  the entire object set as a prereq so any compile regression in any
  picoquic TU turns `make test` red.
- ✅ **21c (done)**: full vendored build + link. picohttp, picotls/lib
  (minus certificate_compression/fusion/mbedtls/mbedtls_sign/openssl),
  picotls/lib/cifra (minus libaegis — needs external `<aegis.h>`),
  cifra internals (minus test drivers + curve25519.donna/.naclref;
  curve25519.c wraps tweetnacl inline), micro-ecc, picoquic_mbedtls,
  mbedtls/library (minus godot_core_mbedtls_platform's legacy
  config.h layout), and loglib all compile + link. `picoquic_create_test`
  calls `picoquic_create(8, NULL-cert, NULL-key, ..., zero_reset_seed,
  ...)` — asserts non-NULL return, then `picoquic_free` tears it down
  under ASAN+UBSAN without leaks.
- ✅ **21d.1 (done)**: `webtransportd --selftest` exercises
  `picoquic_create` + `picoquic_free` end-to-end from the daemon
  binary (full mbedtls TLS init path) under ASAN+UBSAN. The
  daemon now links the entire vendored object set. `selftest_test`
  fork/execs the binary and asserts exit 0 + stdout contains
  `selftest ok`. NOTE: starting the picoquic packet-loop thread
  (`picoquic_start_network_thread`) currently trips an ASAN
  thread-start crash (pc=0) on darwin-arm64; investigation deferred
  to a focused cycle because 21d.2 doesn't need the pthread path.
- ✅ **21d.2 (done)**: `handshake_test` creates two in-process
  `picoquic_quic_t` contexts sharing a simulated clock (server
  loads `thirdparty/picoquic/certs/{cert,key}.pem`, both set
  default ALPN `"hq-test"`). `picoquic_create_client_cnx` +
  synchronous packet pumping via `picoquic_prepare_next_packet` ↔
  `picoquic_incoming_packet` (no sockets, no pthread) drives both
  sides to `picoquic_state_ready` well within the 200-iteration
  budget. Mutation-tested the client-ready assertion to prove the
  handshake is observably complete.
- **21d.3 (real-socket handshake)**: `handshake_test` launches
  `./webtransportd` as a subprocess with a server cert+key,
  opens a real loopback UDP socket with its own picoquic client,
  and asserts a WebTransport CONNECT reaches
  `picoquic_state_ready`.
  Blocked by the ASAN pthread issue: the function-pointer to
  `picoquic_packet_loop_v3` gets zeroed when it routes through
  picoquic's two-level indirection
  (`picoquic_internal_thread_create` -> `picoquic_create_thread` ->
  `pthread_create`) but reaches the function fine when
  pthread_create is called directly. Reproduced with a minimal
  standalone binary linking the full vendored objs; suspected
  arm64 vs arm64e ABI/PAC interaction with the ASAN runtime.
  Design workaround for this slice: skip `picoquic_start_network_thread`
  entirely and call `picoquic_packet_loop_v3` synchronously on the
  daemon's main thread (single-threaded loop, `should_exit` flag
  returns `PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP`).
- **21e (bootstrap)**: `picoquic_create` with server cert+key (or
  `--cert=auto` self-signed), `picoquic_set_alpn_select_fn_v2`,
  `picowt_set_default_transport_parameters`,
  `picoquic_start_network_thread`.
- **21f (path callback)**: register `_wt_session_path_callback` that
  on CONNECT exec's the configured child, allocates a `peer_session_t`,
  stores it in `path_callback_ctx`, and wires incoming stream/datagram
  frames to the child's stdin via the framing codec.
- **21g (echo)**: end-to-end echo test — the client sends a datagram
  + a stream message; the daemon pipes both through `/bin/cat` (or a
  small echo helper); the client receives identical bytes back in the
  same mode (unreliable ↔ unreliable, reliable ↔ reliable). GREEN
  drains the per-peer outbound work queue from the picoquic loop
  callback on `picoquic_packet_loop_wake_up` and dispatches to
  `picoquic_add_to_stream` / `picoquic_queue_datagram_frame`.

### Cycle N — Graceful shutdown

- **RED**: `SIGTERM` the daemon; all in-flight sessions close, all
  child processes are reaped, daemon exits 0, no ASAN leaks.
- **GREEN**: install `sigaction(SIGTERM, …)` that sets an atomic
  `should_exit` flag and calls `picoquic_wake_up_network_thread`; the
  loop callback checks the flag and returns
  `PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP`. After the loop thread
  joins, free all `peer_session_t`s (each one SIGTERMs its child +
  closes the work queue) before `picoquic_free`.

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
  framing helpers).

## Future cycles (once v0.1 ships)

- **RED**: fuzz harness for the frame codec (libFuzzer against
  `wtd_frame_decode`; should never crash or report
  heap-buffer-overflow under ASAN).
- **RED**: `--cert=auto` persistence — generated cert+key survive a
  daemon restart.
- **RED**: per-peer flow control — if the child's stdin pipe is full,
  apply WT stream flow-control backpressure instead of dropping
  unreliable frames.
- **RED**: 8-byte varint support + a configurable
  `WTD_FRAME_MAX_PAYLOAD` so large reliable payloads don't force a
  session close.
- **RED**: a real Windows `child_process_win.c` (today's
  implementation is POSIX-only; Windows ships the daemon binary but
  not the child-plumbing path yet).
- **RED**: stderr-from-child forwarding to `wtd_log` (currently the
  `stderr_fd` pipe is opened but unread).
- **RED**: `--dir` / `--staticdir` equivalents — serve static files
  on non-WT paths for the devconsole case, mirroring
  websocketd's `http.go`.

## Guiding principles (keep revisiting)

- **RED first, always.** Before writing code, write a test that
  compiles (or add only the bare symbol to make it compile) and
  fails at runtime. Mutation-test characterization cycles
  (ones that pass on first run) by briefly breaking the code to
  prove the assertions are load-bearing.
- **Narrow the slice.** Each cycle should be one public behavior.
  If a RED needs two implementation changes to turn green, split it.
- **ASAN is part of the contract.** `-fsanitize=address,undefined`
  runs every `make test` — an ASAN finding is a RED, not a warning.
- **Commit every green.** One commit per cycle (or tightly-paired
  cycle), message starts with `Cycle N: …` so the TDD arc is visible
  in `git log`.
- **No SCsub, no `#include` from Godot.** Operational separation is
  the whole point. The vendored `thirdparty/` is a snapshot, not a
  symlink.
