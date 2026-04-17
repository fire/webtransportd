# Design: Per-Peer Flow Control

## Goal

Remove the one remaining correctness ceiling in `webtransportd`'s
data path: the packet-loop thread must never block on a single
peer's slow child. Today's `write_all` (in [webtransportd.c](../webtransportd.c))
is a blocking loop; if any peer's child leaves its stdin
unread, every other peer on the daemon stops progressing — no
ACKs get sent, no datagrams go out, no new connections accept.
Last cycle on the Wanted frontier before v0.1 ships.

## Problem sketch

The current data path, from [webtransportd.c:340](../webtransportd.c#L340)
and [webtransportd.c:359](../webtransportd.c#L359):

```
server_stream_cb (on packet-loop thread)
  └── peer_find / peer_create
  └── wtd_frame_encode into a 4 KiB stack buffer
  └── write_all(p->child.stdin_fd, frame_buf, out_len)
        └── while (done < len) { write(...) }   ← BLOCKS
```

`p->child.stdin_fd` was opened in blocking mode by
`wtd_child_spawn` (see [child_process.c](../child_process.c)).
As long as the child reads promptly, `write(2)` completes in
nanoseconds and nobody notices. As soon as the child pauses
(GC, disk I/O, operator attached a debugger), the kernel pipe
buffer fills (~64 KiB on Linux, ~16 KiB on macOS), and the
next `write` on the packet-loop thread blocks indefinitely.

The consequences are worse than "that one peer stalls":

- `picoquic_packet_loop_v3` doesn't get back control → no other
  peer gets a packet processed → all connections stall together.
- Congestion-control ACKs aren't sent → the remote peer
  eventually times out and tears down, losing the session.
- Datagrams queued on OTHER peers get dropped because they
  never see their "send turn."

One misbehaving child shouldn't be able to DoS every other peer
on the daemon.

## Shape of the fix — non-blocking writes + per-peer buffering

Three coordinated changes. All landing in one commit keeps the
invariant "the loop never blocks" atomic.

### 1. Open `stdin_fd` non-blocking at spawn time

In [child_process.c](../child_process.c)'s POSIX branch, after
`pipe(in_pipe)` succeeds and before `set_cloexec(in_pipe[1])`:
set `O_NONBLOCK` on `in_pipe[1]` (the parent-side writer). The
child-side reader stays blocking — it's the parent's job to
avoid blocking, not the child's. On Win32, mark the stdin HANDLE
with `PIPE_NOWAIT` via `SetNamedPipeHandleState` (best-effort;
if that fails we fall back to the current blocking behaviour and
document that flow control is Linux/macOS-only for the moment).

### 2. Replace `write_all` with a partial-write helper

New helper in `webtransportd.c`:

```c
/* Returns 0 on full write, 1 on EAGAIN/partial (wrote *p_done),
 * -errno on real error. Never blocks. */
static int write_partial(int fd, const uint8_t *buf, size_t len,
                         size_t *p_done);
```

`*p_done` accumulates bytes actually written across calls.
Callers use the pattern `while (done < len) { int r =
write_partial(...); if (r == 1) break; }`.

### 3. Per-peer pending buffer + flush on each loop tick

Extend `wtd_peer_t` (see [webtransportd.c:195](../webtransportd.c#L195))
with:

```c
uint8_t pending_buf[WTD_PEER_PENDING_CAP];
size_t pending_len;   /* bytes queued but not yet written */
size_t pending_off;   /* byte offset of the next write attempt */
```

`WTD_PEER_PENDING_CAP` = `1 + 9 + WTD_FRAME_MAX_PAYLOAD` =
~16 MiB + 10 bytes (frame header + varint + max payload from
cycle 41). At most one in-flight encoded frame per peer — if a
second frame arrives before the first drains, see §4.

New helper:

```c
static void flush_pending(wtd_peer_t *p); /* partial-write attempt */
```

Called from `server_loop_cb` on every tick (see
[webtransportd.c:399](../webtransportd.c#L399)) for every peer
BEFORE the existing `drain_all_peers` call — outbound bytes from
the child only matter if inbound bytes to the child have drained.

### 4. Server-stream callback backpressure policy

`server_stream_cb` today encodes bytes and writes them directly.
After this change:

- **Stream data (reliable)**: if `p->pending_len != 0`, the
  child's stdin is already full. Return
  `PICOQUIC_ERROR_FLOW_CONTROL_DATA_BEYOND_WINDOW` (or whatever
  picoquic's public API for "don't give me more right now"
  ends up being — fallback is to simply drop the bytes and
  rely on picoquic's receive window to throttle the peer).
  Look up the exact primitive during implementation — the two
  candidates are `picoquic_stream_flow_control_stop_sending`
  (hostile) and `picoquic_mark_active_stream` with a
  stop-sending flag (cooperative). Prefer the cooperative
  form; document the hostile fallback if the cooperative one
  isn't available.
- **Datagrams (unreliable)**: drop silently + increment a
  per-peer `uint64_t dgrams_dropped` counter. A TRACE-level log
  line on first drop per peer is useful for operators
  diagnosing a slow child, but not on every drop (hot path).

### 5. Thread-safety

All writes and the pending-buffer state live **on the
packet-loop thread** — the per-peer reader thread only writes
to its outbound queue. No new mutex required; the invariant is
"only the loop thread touches `pending_*`." Document this as a
comment on the struct field so a future contributor doesn't
add a reader-thread access site.

## Test plan (RED-first)

New file `flow_control_test.c`. Spawns the daemon via the
existing `handshake_*_test` harness pattern, with a child that
deliberately pauses before draining stdin. Two choices for the
slow child:

- **Option A**: `sh -c 'sleep 1; cat'`. Pure POSIX, no new C
  code. Works with the daemon's `--exec=sh -c '...'` argv.
- **Option B**: `examples/slow_cat.c` that reads stdin in a
  nanosleep-throttled loop. More deterministic under ASAN —
  `sleep 1` on an overloaded CI runner might not actually pause
  for the full second.

Prefer Option A for the first RED; swap in Option B if CI
flakes on the timing. Write the file once so future adjustments
don't mean rewriting the test.

Assertions:

1. Send two back-to-back 8 KiB frames. Before the slow child's
   sleep expires, the daemon's log shows `pending_len != 0` on
   the peer (via a TRACE-level "[TRACE] peer N: backpressured"
   log line added as part of this cycle).
2. The daemon's main thread is still responsive — a second
   client connects successfully during the first child's
   pause. (This is the core invariant: one slow peer does not
   block another.)
3. After the slow child drains, both frames eventually echo
   back to the first client intact.
4. The `dgrams_dropped` counter is 0 when the client sent
   only reliable stream data.
5. Mutation-test by reverting `O_NONBLOCK` on the pipe: the
   second client's handshake should time out (blocked behind
   the first peer's stuck `write_all`). Test would then
   fail → the `O_NONBLOCK` flip is load-bearing.

## Critical files

- [webtransportd.c](../webtransportd.c) — `write_all` at line 340,
  `server_stream_cb` at line 359, `server_loop_cb` at line
  399, `wtd_peer_t` struct at line 195. All three need edits.
- [child_process.c](../child_process.c) — `O_NONBLOCK` flag on
  `in_pipe[1]` in the POSIX branch.
- [child_process.h](../child_process.h) — no change; the struct
  stays opaque to callers.
- New: `flow_control_test.c` + possibly
  `examples/slow_cat.c`.

## Out of scope for this cycle

- **Per-peer datagram queue.** Today drop-on-overflow matches
  datagram semantics. A bounded FIFO with metrics is a
  follow-up if operators ask for it.
- **Multi-frame pipelining.** One in-flight frame per peer
  keeps `pending_buf` sized predictably; two or three would
  improve throughput but adds head-of-line-blocking scenarios
  and a real queue. Defer until a benchmark shows the single
  frame is the bottleneck.
- **Windows flow control.** `PIPE_NOWAIT` on Win32 pipes has
  known quirks. Ship this cycle with POSIX non-blocking and
  Win32 blocking (current behaviour), document that a Win32
  child that pauses will stall the daemon, and open a follow-up
  for overlapped I/O if needed.

## Verification

- `flow_control_test` passes on linux-gcc + macos-clang under
  ASAN+UBSAN.
- `handshake_socket_test` + `handshake_echo_test` + `handshake_
  multi_test` all stay green (this change must not alter the
  happy path for a cooperating child).
- `pgrep -fl webtransportd` prints nothing after the test
  suite exits — the slow child is reaped.
- Mutation: remove the `O_NONBLOCK` set in `child_process.c` —
  `flow_control_test` times out waiting for the second
  client's handshake.

## Commit shape

One commit. The three changes (non-blocking pipe, partial-write
helper, per-peer buffer + loop-tick flush) are interdependent
and landing them together keeps the "loop never blocks" invariant
continuously true.

```
Cycle 45: per-peer flow control via O_NONBLOCK + pending buffer

Stops one slow child from blocking the whole packet loop.
Opens every peer's child stdin in non-blocking mode, adds a
per-peer in-flight-frame buffer, and flushes it on each loop
tick. server_stream_cb applies stream-level backpressure when
the buffer is non-empty; datagrams drop + count on overflow
(already unreliable). Win32 stays with the current blocking
pipe — a follow-up cycle will wire overlapped I/O.
```

## Risk notes

- **Subtle EAGAIN handling bugs.** The partial-write state
  machine is easy to get wrong in ways that don't fail under
  a cooperative child. Mutation-testing the "two frames during
  one pause" scenario specifically guards against this.
- **picoquic backpressure API.** If picoquic doesn't expose a
  cooperative "don't feed me stream data yet" primitive, the
  fallback (drop bytes and trust the receive window) is a
  correctness regression for reliable data. Research this
  BEFORE writing the GREEN — if no primitive exists, split the
  cycle: 45a ships the datagram-drop + log-warning path only;
  45b adds a picoquic patch upstream once the API exists.
- **CI timing flake for the slow-child test.** `sleep 1` in the
  child is reliable on dev laptops, less so on
  `windows-mingw`-style shared runners. Option B
  (`examples/slow_cat.c` with a nanosleep pause) is the
  fallback; budget one CI round-trip to confirm flakiness.
