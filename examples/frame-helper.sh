#!/bin/sh
# examples/frame-helper.sh — emit one webtransportd frame to stdout.
#
# usage:  frame-helper.sh <flag> <payload>
#   <flag>     0 = reliable (WT stream), 1 = unreliable (WT datagram)
#   <payload>  the bytes to wrap. Taken from argv[2] verbatim — no
#              trailing newline is added.
#
# Emits [flag | varint(len) | payload] per webtransportd's framing
# spec. Varint length uses the shortest QUIC-style encoding that
# fits (1, 2, 4, or 8 bytes). Delegates the byte math to python3 so
# all four widths are handled uniformly.
#
# Example:
#   ./examples/frame-helper.sh 0 hello | ./webtransportd --server ...

if [ "$#" -ne 2 ]; then
	echo "usage: $0 <flag:0|1> <payload>" >&2
	exit 2
fi

python3 - "$1" "$2" <<'EOF'
import sys
flag = int(sys.argv[1])
if flag not in (0, 1):
    sys.stderr.write("flag must be 0 or 1\n")
    sys.exit(2)
payload = sys.argv[2].encode()
n = len(payload)
if n < (1 << 6):
    varint = bytes([n])
elif n < (1 << 14):
    varint = bytes([0x40 | (n >> 8), n & 0xff])
elif n < (1 << 30):
    varint = bytes([
        0x80 | (n >> 24) & 0x3f,
        (n >> 16) & 0xff,
        (n >> 8) & 0xff,
        n & 0xff,
    ])
else:
    varint = bytes([
        0xc0 | ((n >> 56) & 0x3f),
        (n >> 48) & 0xff, (n >> 40) & 0xff, (n >> 32) & 0xff,
        (n >> 24) & 0xff, (n >> 16) & 0xff, (n >> 8) & 0xff,
        n & 0xff,
    ])
sys.stdout.buffer.write(bytes([flag]))
sys.stdout.buffer.write(varint)
sys.stdout.buffer.write(payload)
EOF
