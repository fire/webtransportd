# Design 48 (Tombstone): Raw QUIC WebTransport Approach

**Status**: OFFICIALLY ABANDONED (Cycle 47/48 - 2026-04-17)  
**Reason**: Program requirement mandates HTTP/3 with WebTransport; raw QUIC is no longer an acceptable fallback  
**Commits that can be reverted if reconsideration needed**: 37a9bd2 and earlier (before HTTP/3 migration)

---

## What Raw QUIC Was

Cycles 22-42 implemented WebTransport on raw QUIC protocol:

- **Stream 0**: Direct WebTransport frame forwarding (no HTTP/3 layer)
- **Callback**: `server_stream_cb` handled raw QUIC stream events
- **Data flow**: `wtd_frame_encode` → child stdin, child stdout → `wtd_frame_decode` → datagram/stream echo
- **Status**: Functional and tested (Cycles 22-32 proved child spawning, multi-client, datagram echo)
- **Advantage**: Simple, no HTTP/3 parsing complexity
- **Disadvantage**: Non-standard protocol, not RFC 9220 compliant

### Evidence It Worked

- Cycle 22d: Client-visible stream echo confirmed
- Cycle 22e: Datagram round-trip working
- Cycle 29: Multi-client isolation proven
- Cycle 32: Child process echo working with frame codec

---

## Why It's Abandoned Now

**Program requirement**: "HTTP/3 with WebTransport"

This means:
1. ❌ Raw QUIC WebTransport is NOT acceptable
2. ❌ QUIC without HTTP/3 is NOT acceptable
3. ✓ HTTP/3 + WebTransport MUST coexist on same connection
4. ✓ RFC 9220 compliance required (HTTP/3 CONNECT → WebTransport)

---

## What This Means

### DO NOT:
- Revert to `server_stream_cb` as primary callback
- Use raw frame forwarding without HTTP/3 semantics
- Suggest "raw QUIC works, use that instead" as a fallback
- Implement WebTransport on non-HTTP/3 connections

### DO:
- Implement picowt_* APIs for HTTP/3 CONNECT → WebTransport upgrade
- Route all WebTransport through HTTP/3 CONNECT protocol
- Ensure client and server both speak HTTP/3
- Follow RFC 9220 (WebTransport over HTTP/3)

---

## Next Implementation Path

**ONLY valid approach going forward:**

1. **Server**: 
   - Use h3zero_callback for HTTP/3 (already in place)
   - Integrate picowt_* APIs to handle CONNECT upgrade
   - After CONNECT, switch stream to WebTransport framing
   - DO NOT use h3zero path callbacks (Design 47 proved they don't work)

2. **Client**:
   - Send HTTP/3 CONNECT to trigger WebTransport upgrade (test already does this)
   - After server accepts CONNECT, use WebTransport stream protocol
   - NOT raw stream data (must follow WebTransport frame semantics)

3. **Test**:
   - Update to use proper WebTransport client APIs (picowt_*) if available
   - OR use HTTP/3 CONNECT + frame-based WebTransport protocol
   - Continue validating with examples/echo child process

---

## Commits Affected

Raw QUIC implementation existed in commits before 37a9bd2:

```
37a9bd2 Cycle 43a-GREEN (h3zero): swap server_stream_cb for h3zero_callback
b8130e4 Cycle 43: HTTP/3 protocol migration with picowt WebTransport support
86c75c1 Cycle 43a: Restructure h3zero callback registration for HTTP/3
5c7b70e Cycle 43a: Register h3zero_callback globally for HTTP/3 handshake support
```

Anything before these was raw QUIC. **These are now dead commits — do not revisit.**

---

## Final Decision

**HTTP/3 + picowt_* is the ONLY acceptable implementation path.**

- Raw QUIC: ❌ CLOSED
- h3zero path callbacks: ❌ CLOSED (Design 47)
- Any non-HTTP/3 WebTransport: ❌ CLOSED

Next cycle must implement picowt_* integration for proper HTTP/3 CONNECT handling.
