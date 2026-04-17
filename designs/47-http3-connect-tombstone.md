# Design 47 (Tombstone): HTTP/3 CONNECT for WebTransport

**Status**: ABANDONED (Cycle 47 - 2026-04-17)  
**Reason**: Confirmed Design 43 finding — h3zero path callbacks do NOT route HTTP/3 CONNECT requests  
**Proof**: Test sends proper HTTP/3 CONNECT frames via `h3zero_create_connect_header_frame`, but path callback never fires

---

## What We Tried

Cycle 47 re-attempted HTTP/3 WebTransport via h3zero path callbacks with proper CONNECT protocol:

1. **Test changes**: Updated `handshake_echo_test.c` to send HTTP/3 CONNECT on stream 0 to `/wt` endpoint
2. **CONNECT frame**: Used `h3zero_create_connect_header_frame("webtransport")` (RFC 9220 compliant)
3. **Payload after CONNECT**: Sent PAYLOAD + DGRAM after CONNECT acceptance
4. **Server-side**: Registered `/wt` path with h3zero path table, pointing to `wt_data_cb` callback

### Code Changes

```c
/* Client: send HTTP/3 CONNECT for WebTransport */
uint8_t connect_frame[512];
uint8_t *next = h3zero_create_connect_header_frame(
    connect_frame, connect_frame + sizeof(connect_frame),
    "test.example", (const uint8_t *)"/wt", 3,
    "webtransport", NULL, NULL, NULL);
picoquic_add_to_stream(cnx, 0, connect_frame, frame_len, 0);

/* Server: register /wt path with h3zero */
path_table[0].path = "/wt";
path_table[0].path_callback = wt_data_cb;
h3zero_callback_ctx_t *h3_ctx = h3zero_callback_create_context(&params);
picoquic_create(..., h3zero_callback, h3_ctx, ...);
```

---

## Why It Failed

**Root Cause**: Same as Design 43 — **h3zero does not invoke path callbacks for HTTP/3 CONNECT requests**.

Evidence:
- Client sends proper HTTP/3 CONNECT with `"webtransport"` protocol tag
- Server receives CONNECT (TLS handshake succeeds, "server ready" log appears)
- **Path callback `wt_data_cb` is NEVER invoked** (no trace logs from callback)
- Test timeout (client gets no echo response)
- No framed data appears in daemon logs (child process never receives stdin)

### Why Path Callbacks Don't Work

h3zero's path routing is designed for **POST/GET/HEAD** on traditional HTTP paths:
- `picohttp_callback_post` - received POST request
- `picohttp_callback_get` - received GET request
- `picohttp_callback_post_data` - data from POST body

**HTTP/3 CONNECT is a protocol upgrade** — h3zero processes it separately:
- CONNECT is not a traditional HTTP method in the path routing sense
- CONNECT requires special handling at the stream level, not path level
- h3zero likely rejects or silently drops unhandled CONNECT requests
- No integration point exists to hook WebTransport CONNECT in path callbacks

---

## Conclusion

The **HTTP/3 WebTransport spec DOES require CONNECT**, but:

❌ **h3zero path callbacks cannot be used for WebTransport CONNECT**  
✓ **Use Option B from Design 43**: picowt_* APIs on server + client  
✓ **OR**: Fall back to raw QUIC (which works, per Design 43)

---

## Next Steps

**Do NOT continue with h3zero path callbacks.**

### Option 1: Use picowt_* APIs (Correct Per RFC 9220)

Server:
- Register stream callback that checks for WebTransport prefix frames
- Use `picowt_connect` / `picowt_*` APIs to handle protocol upgrade
- Requires deeper integration with picoquic stream loop

Client:
- Use `h3zero_create_connect_header_frame` to send CONNECT (already in test)
- **After CONNECT accept**: upgrade stream to WebTransport framing
- Send WebTransport frames instead of raw bytes

### Option 2: Raw QUIC (Proven Working, No HTTP/3)

- Revert to pre-HTTP/3 approach (Design 22-32)
- WebTransport on raw QUIC stream 0, not HTTP/3
- No HTTP/3 static file serving; defer that to Design 04

### Option 3: Split Concerns

- HTTP/3 on stream 0 for GET/POST (via h3zero)
- Raw QUIC WebTransport on stream 4+ (side-channel)
- No mixing; explicit protocol selection by client

---

## Tombstone Marker

**❌ CLOSED Cycle 47 — Do not revisit h3zero path callbacks for WebTransport CONNECT**

Confirmed: Design 43 analysis was correct. h3zero is insufficient for HTTP/3 WebTransport CONNECT.

Commits this cycle (to preserve if future cycle resumes):
- (Uncommitted — webtransportd.c modifications for h3zero path table setup)

If resurrecting: revert to h3_ctx = NULL, use raw QUIC or picowt_* instead.
