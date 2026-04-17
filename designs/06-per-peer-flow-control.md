# Design: Per-Peer Flow Control

## Goal
Prevent packet loop thread blocking when a child's stdin pipe fills; apply WT stream-level backpressure.

## Steps
- Detect partial writes in `write_all`.
- Stash remaining frame on peer object.
- Apply WebTransport stream-level backpressure instead of blocking loop.
- For datagrams: drop and metric on overflow (already unreliable).
- Last correctness ceiling under load.
