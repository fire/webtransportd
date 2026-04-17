# Contributing

A standalone C WebTransport (HTTP/3) daemon that exec's a child process
per accepted session and pipes data between the WT session and the
child's stdin/stdout. Modeled on
[websocketd](https://github.com/joewalnes/websocketd).

Built strictly red-green-refactor: every feature is driven by a failing
test, committed when green, then any cleanup is done with the test
still green. ASAN + UBSAN run on every test. The source tree is
self-contained — mbedtls, picoquic, and picotls are all vendored under
`thirdparty/` so there is no system TLS package dependency.

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
