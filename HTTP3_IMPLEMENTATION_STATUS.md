# HTTP/3 Implementation Status

## Current State

The cycle 43 h3zero integration attempt failed because of a fundamental protocol mismatch:
- **Server**: Sends HTTP/3 response headers + data via `h3zero_callback`
- **Clients**: Raw QUIC callbacks expecting unframed stream data
- **Result**: Clients receive HTTP/3 frame data they can't parse → test failures

## Files Created (Cycle 44 - Fresh Approach)

### New Server Implementation
- **`webtransportd-http3.c`** (incomplete)
  - Uses `h3zero_callback` as connection handler
  - Integrates with existing peer session + child process infrastructure
  - **Issue**: Callback signature and integration with peer data flow needs refinement

### New Test Infrastructure
- **`handshake_http3_test.c`** (incomplete)
  - HTTP/3 client using h3zero APIs
  - Should connect, send CONNECT /wt, receive echo
  - **Issue**: Missing stream callback for receiving data

### Documentation
- **`designs/43-http3-tombstone.md`** — Why path callback approach failed
- **`designs/44-http3-client-server.md`** — Architecture for proper HTTP/3

## What's Needed to Complete

### Option A: Fix webtransportd-http3.c
Replace current `webtransportd.c` with the HTTP/3 implementation:
1. Properly integrate `h3zero_callback` with peer session data flow
2. Ensure framed data from child reaches client via HTTP/3 response frames
3. Handle WebTransport CONNECT request recognition

**Complexity**: Medium  
**Risk**: Breaks existing tests until all three test types are HTTP/3-ified

### Option B: Hybrid Approach (Not Recommended)
Keep both raw QUIC + HTTP/3 support in one daemon. Rejects "NO DUAL STACK" requirement.

### Option C: Minimal HTTP/3 Echo for Testing
Create a minimal HTTP/3 echo server just for testing, separate from main daemon.
**Complexity**: Low  
**Risk**: Doesn't solve the overall architecture problem

---

## Critical Design Decision

**How should h3zero route CONNECT /wt requests?**

Current attempt used path callbacks, but those are for GET/POST.

Proper options:
1. **h3zero handles CONNECT natively** — h3zero might have built-in WebTransport support we haven't enabled
2. **Custom stream-level handler** — Intercept CONNECT at stream level, not path level
3. **Use picowt_* server APIs** — If they exist; reference code used them on client side

---

## Recommended Next Step

1. **Research h3zero CONNECT handling** in picoquic source code
2. **Check if h3zero has WebTransport mode** via settings or context parameters  
3. **Review webtransport_test.c server setup** — see how reference implementation does it
4. **Implement minimal working version** with just echo, no existing infrastructure dependencies

---

## Why Pure HTTP/3 is Required

- User explicitly rejected dual-stack approach
- HTTP/3 + QUIC are now mandatory for WebTransport
- Clients and servers MUST speak same protocol
- Legacy raw QUIC tests become obsolete

