# Design 44: Pure HTTP/3 WebTransport (Fresh Implementation)

**Status**: IN PROGRESS (Cycle 44)  
**Goal**: HTTP/3-only architecture where both server and clients speak HTTP/3 properly  
**Approach**: Rebuild client-server interaction using h3zero + picowt APIs correctly

---

## Problem Statement

Cycle 43 attempted to use h3zero path callbacks for `/wt` CONNECT routing. This failed because:

1. **Path callbacks are HTTP-specific** — they route GET/POST/etc. on paths, not protocol upgrades
2. **CONNECT is a protocol upgrade** — h3zero handles it separately from regular HTTP requests
3. **Clients were raw QUIC** — they couldn't parse HTTP/3 response frames
4. **Mismatch between server and client protocols** — one spoke HTTP/3, the other raw QUIC

---

## Solution: Proper HTTP/3 Implementation

### Server (`webtransportd-http3.c`)

- Uses `h3zero_callback` as the default connection callback
- h3zero handles all HTTP/3 frame parsing and routing
- WebTransport CONNECT requests are passed to h3zero's built-in handling
- After successful CONNECT, stream data is passed to peer session reader (existing)
- Child process interaction via `wtd_peer_session` (unchanged)

**Key insight**: Don't override h3zero's CONNECT handling. Let h3zero do what it's designed to do.

### Clients (`handshake_http3_test.c`)

- Use `h3zero_callback_create_context` to set up HTTP/3 context
- Use `h3zero_create_connect_header_frame` to send CONNECT request
- Use h3zero's response parsing to receive data
- Proper HTTP/3 framing and protocol semantics

---

## Implementation Steps

1. **Fix `webtransportd-http3.c`**:
   - Remove custom `server_h3_callback` (not needed)
   - Use h3zero_callback directly
   - Ensure peer session integration works
   - Test with simple echo

2. **Complete `handshake_http3_test.c`**:
   - Add proper h3zero stream callback for receiving data
   - Parse HTTP/3 response headers and data
   - Verify payload round-trip

3. **Update other tests**:
   - `handshake_multi_test.c` → `handshake_multi_http3_test.c`
   - `handshake_echo_test.c` → replace with HTTP/3 version

4. **Makefile**:
   - Add rule to compile `webtransportd` from `webtransportd-http3.c` (replace current)
   - Add rules for HTTP/3 test binaries

---

## Key References

- **picowt_* APIs**: Client-side WebTransport session initiation
- **h3zero_callback**: Core HTTP/3 stream handling in picoquic
- **h3zero_callback_ctx_t**: HTTP/3 context with path tables and parameters
- **picohttp_post_data_cb_fn**: Custom handlers for specific paths (if needed for GET/etc.)

---

## Success Criteria

- [ ] Server compiles and starts with `./webtransportd-http3`
- [ ] `handshake_http3_test` connects and receives echo
- [ ] Multi-client test works (two concurrent HTTP/3 clients)
- [ ] Datagrams echo (flag=1 frames)
- [ ] Child stderr forwarding works
- [ ] All tests green under ASAN+UBSAN

---

## Future: Dev Console (Design 04)

Once HTTP/3 is solid, add static file serving:

```c
picohttp_server_path_item_t path_table[2];
path_table[0].path = "/index.html";
path_table[0].path_callback = serve_static_callback;
path_table[1].path = "/wt";
path_table[1].path_callback = NULL;  /* h3zero handles CONNECT */
```

This way, HTTP/3 GET requests serve static files, and HTTP/3 CONNECT initializes WebTransport.

