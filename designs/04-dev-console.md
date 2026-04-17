# Design: --dir=<path> / --staticdir=<path>

## Goal
Serve static files on non-WT request paths, mirroring websocketd's `http.go`.

## Steps
- Add `--dir=<path>` and `--staticdir=<path>` CLI flags.
- Serve static files for non-WebTransport request paths.
- Unlock devconsole: ship browser client alongside daemon in one process.
- Ensure this is independent from `--cert=auto` (no trade-off).
