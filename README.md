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

## Framing

Data exchanged between the daemon and child is framed as:

```
[flag | varint len | payload]
```

Where:
- **flag** (1 byte): bit 0 selects reliable (0, WebTransport stream) vs unreliable (1, WebTransport datagram). Bits 1–7 are reserved and must be zero.
- **varint len** (1–9 bytes): LEB128-encoded payload length.
- **payload** (0–16 MiB): raw bytes.

For example, a 5-byte reliable frame:

```
0x00           # flag: reliable stream
0x05           # varint: length = 5
0x48 0x65 0x6c 0x6c 0x6f  # "Hello"
```

See [`frame.h`](frame.h) for the encoding/decoding implementation.

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
| `--log-level=<0..4>` | `2` (INFO) | Log level: QUIET (0), ERROR (1), WARN (2), INFO (3), TRACE (4). |

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
