# Design 43 (Tombstone): HTTP/3 Integration via h3zero_callback

**Status**: ABANDONED (Cycle 43)  
**Reason**: Path callback pattern incompatible with WebTransport CONNECT routing  
**Next**: Defer HTTP/3 to design 04 (dev console static serving); keep WebTransport on raw QUIC

---

## What We Tried

Cycles 43a-43c attempted to integrate h3zero_callback for HTTP/3 support:

1. **Server-side**: Registered h3zero_callback via ALPN select callback after TLS negotiation
2. **Path routing**: Set up `/wt` path with `wtd_wt_echo_callback` to handle WebTransport
3. **Client-side**: Clients sent HTTP/3 CONNECT frames via `h3zero_create_connect_header_frame`
4. **Echo**: Path callback echoed received bytes back via `picoquic_add_to_stream`

### Changes Made

- Added `picoquic_internal.h` for ALPN dispatch
- Implemented `alpn_select_callback` to route "h3" ALPN to h3zero_callback
- Created `wtd_wt_echo_callback` as picohttp path_callback for `/wt`
- Updated `h3zero_callback_ctx_t` setup with path_table
- Clients used `h3zero_create_connect_header_frame` to initiate sessions

---

## Why It Failed

**Root Cause**: h3zero does not route CONNECT requests (HTTP/3 method for protocol upgrade) through the path_callback mechanism. 

- Path callbacks are for POST/GET/etc. on traditional HTTP paths
- CONNECT for WebTransport requires special handling via picowt_* APIs or stream-level hooks
- h3zero's CONNECT handling is separate from the path routing layer
- No data was flowing between client and echo callback

**Evidence**:
- Tests showed `cli_rc != 0` (client timeout)
- No "outbound frame" log lines (peer not receiving framed data)
- Path callback was never invoked

---

## Decision: SUPERSEDED — HTTP/3 + picowt_* Required (Cycle 47-48)

~~The core WebTransport functionality (client → daemon → child → client round-trip) works reliably on raw QUIC~~

**UPDATE (Cycle 47-48)**: Program now requires **"HTTP/3 with WebTransport"** exclusively.

- ❌ Raw QUIC is NO LONGER acceptable (see Design 48)
- ✓ HTTP/3 + WebTransport MUST coexist
- ✓ RFC 9220 compliance required
- ✓ picowt_* APIs are the correct path (not h3zero path callbacks)

---

## What Happens to HTTP/3?

Design 04 (dev console) wants HTTP/3 to serve static files on non-WebTransport requests:

```
GET /index.html          → HTTP/3 response (static file)
CONNECT /wt for WT       → WebTransport session (raw QUIC internally, or HTTP/3 if done right)
```

This is deferred. Next approach:

1. **Option A** (Simpler): Serve dev console on separate HTTP-only port or via CLI tool, not daemon
2. **Option B** (Correct): Implement WebTransport + HTTP/3 coexistence by:
   - Using h3zero for HTTP requests only (GET, POST, etc.)
   - Using picowt_* APIs for CONNECT → WebTransport (not path callbacks)
   - Requires client-side update to use `picowt_connect` (not raw QUIC)

---

## Commits to Revert

If future cycle wants to resurrect, these introduced h3zero:

```
f551241 Cycle 43: HTTP/3 fully integrated with ALPN dispatch
efe375d Cycle 43: Add WebTransport /wt path handler for HTTP/3
d9e159b Cycle 43: Fix HTTP/3 by using ALPN select callback pattern
5c7b70e Cycle 43a: Register h3zero_callback globally for HTTP/3 handshake support
86c75c1 Cycle 43a: Restructure h3zero callback registration for HTTP/3
b8130e4 Cycle 43: HTTP/3 protocol migration with picowt WebTransport support
37a9bd2 Cycle 43a-GREEN (h3zero): swap server_stream_cb for h3zero_callback
```

---

## Lessons

1. **h3zero path callbacks are HTTP-specific**, not protocol-upgrade agnostic
2. **WebTransport CONNECT is special** — needs picowt_* client APIs + stream-level server dispatch
3. **Raw QUIC works** — don't over-engineer unless there's a concrete requirement
4. **Coexistence is hard** — mixing HTTP/3 and WebTransport on the same stream requires careful design

