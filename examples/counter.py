#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, K. S. Ernest (iFire) Lee
# examples/counter.py — stateful webtransportd child that counts messages.
#
# Each incoming frame gets a sequence number prepended to its payload
# before being echoed back. Demonstrates per-session state: the counter
# resets for every new WebTransport connection (new child process).
#
# Run with:
#   ./webtransportd --server --cert=auto --port=4433 \
#       --exec=./examples/counter.py

import sys

inp = sys.stdin.buffer
out = sys.stdout.buffer

def read_exact(n):
    buf = b''
    while len(buf) < n:
        chunk = inp.read(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf

def read_varint():
    b = inp.read(1)
    if not b:
        return None
    first = b[0]
    prefix = first >> 6
    if prefix == 0:
        return first & 0x3f
    elif prefix == 1:
        rest = inp.read(1)
        return None if not rest else ((first & 0x3f) << 8) | rest[0]
    elif prefix == 2:
        rest = read_exact(3)
        if not rest:
            return None
        return ((first & 0x3f) << 24) | (rest[0] << 16) | (rest[1] << 8) | rest[2]
    else:
        rest = read_exact(7)
        if not rest:
            return None
        val = first & 0x3f
        for byte in rest:
            val = (val << 8) | byte
        return val

def encode_varint(n):
    if n < (1 << 6):
        return bytes([n])
    elif n < (1 << 14):
        return bytes([0x40 | (n >> 8), n & 0xff])
    elif n < (1 << 30):
        return bytes([0x80 | ((n >> 24) & 0x3f), (n >> 16) & 0xff,
                      (n >> 8) & 0xff, n & 0xff])
    else:
        return bytes([0xc0 | ((n >> 56) & 0x3f),
                      (n >> 48) & 0xff, (n >> 40) & 0xff, (n >> 32) & 0xff,
                      (n >> 24) & 0xff, (n >> 16) & 0xff,
                      (n >> 8) & 0xff, n & 0xff])

def write_frame(flag, payload):
    frame = bytes([flag]) + encode_varint(len(payload)) + payload
    out.write(frame)
    out.flush()

count = 0
while True:
    flag_byte = inp.read(1)
    if not flag_byte:
        break
    flag = flag_byte[0]
    length = read_varint()
    if length is None:
        break
    payload = read_exact(length)
    if payload is None:
        break
    count += 1
    try:
        response = f'[{count}] {payload.decode("utf-8")}'.encode('utf-8')
    except UnicodeDecodeError:
        response = f'[{count}] <binary {length} bytes>'.encode('utf-8')
    write_frame(flag, response)
