#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, K. S. Ernest (iFire) Lee
# examples/echo.py — minimal webtransportd child in Python.
#
# Reads frames from stdin, echoes each payload back with the same flag.
# Drop-in replacement for examples/echo (no compilation needed).
#
# Wire format: [flag:1][varint_len][payload]
#   flag 0 = reliable WebTransport stream
#   flag 1 = unreliable WebTransport datagram

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
    frame = bytes([flag]) + encode_varint(len(payload)) + payload
    out.write(frame)
    out.flush()
