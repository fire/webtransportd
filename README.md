# webtransportd

A WebTransport/QUIC daemon that exec's a child process per session and pipes bytes between the WebTransport stream/datagram channel and the child's stdin/stdout. Like [websocketd](https://github.com/joewalnes/websocketd), but for WebTransport. Any programming language that can read stdin and write stdout can serve a WebTransport session without linking a QUIC library.

## Quickstart

```bash
git clone https://github.com/fire/webtransportd
cd webtransportd
make
./webtransportd --server --cert=auto --port=4433 \
    --exec=./examples/echo
```

The server prints a `cert-hash:` line on startup. Pass it to `serverCertificateHashes` so the browser accepts the self-signed cert without any flag or trust-store import:

```javascript
// Replace HASH_B64 with the cert-hash printed by the server on startup.
const HASH_B64 = "abc123...==";
const hashBytes = Uint8Array.from(atob(HASH_B64), c => c.charCodeAt(0));

const transport = new WebTransport('https://localhost:4433/wt', {
  serverCertificateHashes: [{ algorithm: "sha-256", value: hashBytes }]
});
await transport.ready;

const stream = await transport.createBidirectionalStream();
const writer = stream.writable.getWriter();
const reader = stream.readable.getReader();

await writer.write(new TextEncoder().encode("Hi"));
await writer.close();
const { value } = await reader.read();
console.log(new TextDecoder().decode(value)); // "Hi" echoed back
```

Works in Chrome and Firefox. The cert rotates every 13 days (required by the `serverCertificateHashes` spec); update the hash in your client when you restart the server.

## How it works

webtransportd follows the same model as websocketd: for each accepted WebTransport session, it forks a child process and pipes bytes between the network and the child's stdin/stdout. The key difference from WebSocket is that WebTransport exposes two delivery modes — reliable streams and unreliable datagrams — so a thin framing layer tags each message with a reliability flag.

```
browser  ←─ QUIC/HTTP3 ─→  webtransportd  ←─ stdin/stdout ─→  your program
```

Every message on either side is wrapped in a three-field frame: `[flag | varint len | payload]`. Your child reads frames from stdin and writes frames to stdout. The daemon handles all TLS, QUIC, and HTTP/3 machinery.

## CLI Flags

| Flag | Default | Meaning |
|------|---------|---------|
| `--version` | — | Print version string and exit. |
| `--selftest` | — | Exercise picoquic TLS initialization and exit. |
| `--server` | — | Run the daemon (required for normal operation). |
| `--cert=<pem\|auto>` | — | Path to PEM certificate file, or `auto` to generate self-signed in memory. |
| `--key=<pem>` | — | Path to PEM private key file (required unless `--cert=auto`). |
| `--port=<N>` | — | UDP port to listen on. |
| `--exec=<bin>` | — | Child process to spawn on each accepted connection. |
| `--dir=<path>` | — | Serve static files for HTTP/3 GET requests on non-WebTransport paths. |
| `--log-level=<0..4>` | `2` (WARN) | Log level: QUIET (0), ERROR (1), WARN (2), INFO (3), TRACE (4). |

## Building from Source

### Linux / macOS

```bash
make
```

Builds with ASAN + UBSAN in the test suite (`make test`) and a clean binary from the daemon target. To produce a portable binary without sanitizers:

```bash
make NO_SANITIZER=1
```

### Windows (MSYS2 + mingw-w64)

Install MSYS2, then in the MSYS2 shell:

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make
cd /path/to/webtransportd
make
```

The build includes an embedded UTF-8 manifest so command-line arguments and environment variables are interpreted as UTF-8.

### Cross-compile to Windows (from Linux/macOS)

```bash
CC=x86_64-w64-mingw32-gcc make
```

## Writing a child process

A child reads the frame stream from stdin and writes responses to stdout. Three examples follow, from simplest to most capable.

### Framing

Data exchanged between the daemon and child is framed as:

```
[flag | varint len | payload]
```

- **flag** (1 byte): bit 0 selects reliable (0, WebTransport stream) vs unreliable (1, WebTransport datagram). Bits 1–7 are reserved and must be zero.
- **varint len** (1–9 bytes): QUIC-style LEB128-encoded payload length (shortest form: 1 byte for values < 64, 2 for < 16384, 4 for < 1 073 741 824, 8 for larger).
- **payload** (0–16 MiB): raw bytes.

For example, a 5-byte reliable frame carrying `Hello`:

```
0x00           # flag: reliable stream
0x05           # varint: length = 5
0x48 0x65 0x6c 0x6c 0x6f  # "Hello"
```

See [`frame.h`](frame.h) for the encode/decode implementation.

`examples/frame-helper.sh` constructs a single frame on stdout, useful for offline testing:

```bash
./examples/frame-helper.sh 0 "hello"   # reliable frame
./examples/frame-helper.sh 1 "ping"    # unreliable datagram frame

./examples/frame-helper.sh 0 "hello world" | ./examples/echo | xxd
# 00000000: 000b 6865 6c6c 6f20 776f 726c 64   ..hello world
```

### C (compiled, zero-copy)

`examples/echo.c` re-encodes every incoming payload unchanged. It uses the same `wtd_frame_decode`/`wtd_frame_encode` helpers from `frame.h`:

```c
wtd_frame_status_t st = wtd_frame_decode(buf, used,
    &consumed, &flag, &payload, &plen);
if (st == WTD_FRAME_OK)
    wtd_frame_encode(flag, payload, plen, out_buf, BUF_CAP, &out_len);
```

Build and run:

```bash
make examples/echo
./webtransportd --server --cert=auto --port=4433 --exec=./examples/echo
```

### Python (no compilation)

`examples/echo.py` is a pure-Python drop-in for the C echo. It handles all four varint widths and works on any system with Python 3:

```bash
./webtransportd --server --cert=auto --port=4433 \
    --exec="python3 ./examples/echo.py"
```

`examples/uppercase.py` demonstrates real processing — it uppercases the payload text and preserves the reliability flag:

```bash
./webtransportd --server --cert=auto --port=4433 \
    --exec="python3 ./examples/uppercase.py"
```

`examples/counter.py` shows per-session state — it prepends a sequence number to each reply. Because webtransportd forks a fresh child per connection, the counter resets to 1 for every new session:

```bash
{ ./examples/frame-helper.sh 0 "first"; \
  ./examples/frame-helper.sh 0 "second"; \
  ./examples/frame-helper.sh 1 "third (datagram)"; } \
  | python3 ./examples/counter.py
# [1] first   (reliable)
# [2] second  (reliable)
# [3] third (datagram)  (unreliable)
```

### Shell (one-liner)

Any shell script that produces correctly-framed bytes works. The simplest bridge uses `frame-helper.sh` to emit a single frame and exit — useful for smoke-testing or webhooks:

```bash
./webtransportd --server --cert=auto --port=4433 \
    --exec="sh -c './examples/frame-helper.sh 0 pong'"
```

## Static file serving

Pass `--dir=<path>` to serve ordinary HTTP/3 GET requests alongside WebTransport sessions. Non-WebTransport requests resolve against the directory; `/wt` paths are reserved for the daemon.

```bash
mkdir -p ./public && echo '<h1>hello</h1>' > ./public/index.html
./webtransportd --server --cert=auto --port=4433 \
    --exec=./examples/echo --dir=./public
```

Browsers can then fetch `https://localhost:4433/index.html` and open a WebTransport session on the same port.

## Public Internet Exposure

WebTransport runs over QUIC (UDP), so TCP-only tunnels and proxies do not work. Services that support UDP/QUIC forwarding:

| Option | Type | Notes |
|--------|------|-------|
| **AWS Network Load Balancer** | UDP passthrough | QUIC Connection ID stickiness; production-ready |
| **GCP Network Load Balancer** | UDP passthrough | Works; no explicit WebTransport docs |
| **Fly.io** | UDP port | Requires dedicated IPv4; bind to `fly-global-services` |
| **frp** | Self-hosted UDP | Open source; runs on any VPS |

Services that do **not** work (TCP only, QUIC is dropped):

- Tailscale Funnel
- Cloudflare Tunnel (Argo)
- Railway public networking
- Most PaaS HTTP reverse proxies (nginx, Caddy, HAProxy cannot forward to a QUIC backend)

For local development across machines, Tailscale's mesh (node-to-node, not Funnel) works because the mesh itself supports UDP.

## Relationship to Godot

The vendored libraries in `thirdparty/` (picoquic, picotls, mbedtls) are **snapshots** pulled from Godot's tree to ensure compatibility. The daemon requires no Godot toolchain and runs standalone on any POSIX system or Windows.

If you are using WebTransport inside the Godot engine, see [Godot's `modules/http3/`](https://github.com/godotengine/godot/tree/master/modules/http3) for the same stack integrated into the editor and runtime.

## License and Contact

This project is licensed under the BSD-2-Clause license. See [`LICENSE`](LICENSE) for details.

Contributors are listed in [`AUTHORS`](AUTHORS).

For changes and release notes, see [`CHANGES`](CHANGES).
