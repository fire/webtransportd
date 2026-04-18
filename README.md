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

Then in your browser (Chrome, tested):

```javascript
const transport = new WebTransport('https://localhost:4433/');
await transport.ready;

const {readable, writable} = await transport.createBidirectionalStream();
const writer = writable.getWriter();
const reader = readable.getReader();

await writer.write(new Uint8Array([0x48, 0x69])); // "Hi"
const {value} = await reader.read();
console.log(new TextDecoder().decode(value)); // "Hi" echoed back
```

Note: `--cert=auto` generates a self-signed certificate. Your browser will require the DevTools flag `chrome://flags/#unsafely-treat-insecure-origin-as-secure` set to `https://localhost:4433`, or a manual import of the certificate into your system trust store.

## How it works

webtransportd follows the same model as websocketd: for each accepted WebTransport session, it forks a child process and pipes bytes between the network and the child's stdin/stdout. The key difference from WebSocket is that WebTransport exposes two delivery modes — reliable streams and unreliable datagrams — so a thin framing layer tags each message with a reliability flag.

```
browser  ←─ QUIC/HTTP3 ─→  webtransportd  ←─ stdin/stdout ─→  your program
```

Every message on either side is wrapped in a three-field frame: `[flag | varint len | payload]`. Your child reads frames from stdin and writes frames to stdout. The daemon handles all TLS, QUIC, and HTTP/3 machinery.

## Framing

Data exchanged between the daemon and child is framed as:

```
[flag | varint len | payload]
```

Where:
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

### frame-helper.sh

`examples/frame-helper.sh` constructs a single frame on stdout, useful for piping to your child during development:

```bash
./examples/frame-helper.sh 0 "hello"   # reliable frame
./examples/frame-helper.sh 1 "ping"    # unreliable datagram frame
```

You can pipe directly into any child binary for offline testing:

```bash
./examples/frame-helper.sh 0 "hello world" | ./examples/echo | xxd
# 00000000: 000b 6865 6c6c 6f20 776f 726c 64   ..hello world

{ ./examples/frame-helper.sh 0 "ping"; \
  ./examples/frame-helper.sh 1 "dgram"; } | ./examples/echo | xxd
# 00000000: 0004 7069 6e67 0105 6467 7261 6d   ..ping..dgram
```

## Writing a child process

A child needs to read the frame stream from stdin and write responses to stdout. Below are three examples, from simplest to most capable.

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

`examples/echo.py` is a pure-Python drop-in for the C echo. It handles all four varint widths and works on any system with Python 3. Run it directly:

```bash
./webtransportd --server --cert=auto --port=4433 \
    --exec="python3 ./examples/echo.py"
```

`examples/uppercase.py` demonstrates real processing — it uppercases the payload text and preserves the reliability flag:

```bash
./webtransportd --server --cert=auto --port=4433 \
    --exec="python3 ./examples/uppercase.py"
```

Offline verification:

```bash
{ ./examples/frame-helper.sh 0 "hello world"; \
  ./examples/frame-helper.sh 1 "datagram too"; } \
  | python3 ./examples/uppercase.py | xxd
# flag=0 "HELLO WORLD", flag=1 "DATAGRAM TOO"
```

`examples/counter.py` shows per-session state — it prepends a sequence number to each reply. Because webtransportd forks a fresh child per connection, the counter resets to 1 for every new session:

```bash
./webtransportd --server --cert=auto --port=4433 \
    --exec="python3 ./examples/counter.py"
```

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

Any shell script that can produce correctly-framed bytes works. The simplest bridge uses `frame-helper.sh` to emit a single frame and exit — useful for smoke-testing or webhooks:

```bash
./webtransportd --server --cert=auto --port=4433 \
    --exec="sh -c './examples/frame-helper.sh 0 pong'"
```

## Static file serving

Pass `--dir=<path>` to serve ordinary HTTP/3 GET requests alongside WebTransport sessions. Non-WebTransport requests are resolved against the directory; `/wt` paths are reserved for the daemon.

```bash
mkdir -p ./public && echo '<h1>hello</h1>' > ./public/index.html
./webtransportd --server --cert=auto --port=4433 \
    --exec=./examples/echo --dir=./public
```

Browsers can then fetch `https://localhost:4433/index.html` and open a WebTransport session on the same port.

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

## Relationship to Godot

The vendored libraries in `thirdparty/` (picoquic, picotls, mbedtls) are **snapshots** pulled from Godot's tree to ensure compatibility. The daemon itself is intentionally independent — it requires no Godot toolchain and can run standalone on any POSIX system or Windows.

If you are using WebTransport inside the Godot engine, see [Godot's `modules/http3/`](https://github.com/godotengine/godot/tree/master/modules/http3) for the same stack integrated into the editor and runtime.

## License and Contact

This project is licensed under the BSD-2-Clause license. See [`LICENSE`](LICENSE) for details.

Contributors are listed in [`AUTHORS`](AUTHORS).

For changes and release notes, see [`CHANGES`](CHANGES).
