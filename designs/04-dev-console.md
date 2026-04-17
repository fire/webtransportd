# Design: `--dir=<path>` static file serving for the devconsole

## Goal

Let an operator run `webtransportd` with a `--dir=./site/` flag
and have the daemon serve files under that directory on any
non-WebTransport HTTP/3 request — without giving up the current
WebTransport session behaviour. That's the last piece the
"ship a browser client alongside the daemon in one process"
story needs, matching how
[websocketd](https://github.com/joewalnes/websocketd)'s
`http.go` serves its console alongside the WS endpoint.

## Why this is cheap — picohttp already implements it

Key finding from reading `thirdparty/picoquic/picohttp/`: the
h3zero server callback already serves static files from a
configured directory. The plumbing is three structs:

```c
/* thirdparty/picoquic/picohttp/h3zero_common.h:196 */
typedef struct st_picohttp_server_parameters_t {
    char const* web_folder;              /* --dir=<path> goes here */
    picohttp_server_path_item_t* path_table;
    size_t path_table_nb;
} picohttp_server_parameters_t;

typedef struct st_picohttp_server_path_item_t {
    char const* path;                    /* e.g. "/wt" */
    size_t path_length;
    picohttp_post_data_cb_fn path_callback;
    void* path_app_ctx;
} picohttp_server_path_item_t;
```

So once we wire `h3zero_callback` as the daemon's per-cnx default
callback (replacing our current `server_stream_cb`) with
`param->web_folder` set to the `--dir` argument, **static file
serving is free**. The only active work is:

1. Pick a well-known path — say `/wt` — that is "the
   WebTransport session." Every other URL falls through to
   the static-file handler.
2. Register a `path_table` entry pointing `/wt` at our own
   callback. That callback takes over framing + `--exec` spawn
   for the WT session the way `server_stream_cb` does today.
3. When `--dir` is not passed, `web_folder == NULL` and
   `h3zero_callback` returns a 404 for non-WT paths — which
   matches today's behaviour exactly (no regression).

**The current `server_stream_cb` is incompatible with this
model** — it hooks directly at the QUIC stream level below
HTTP/3. Our daemon currently reads raw stream bytes and frames
them; a real HTTP/3 stack needs to parse the request headers
first, then for `/wt` elevate the stream to WebTransport via
the CONNECT + `:protocol=webtransport` pseudo-header dance.

## Shape of the fix

Three stages, best landed as sub-cycles (43a → 43b → 43c) rather
than one big commit:

### 43a — replace `server_stream_cb` with `h3zero_callback`

The biggest change. Today [webtransportd.c:499](../webtransportd.c#L499)
sets `server_stream_cb` as picoquic's default callback. After
this cycle:

```c
/* In cmd_server, after picoquic_create: */
static picohttp_server_path_item_t path_table[1];
path_table[0].path = "/wt";
path_table[0].path_length = strlen("/wt");
path_table[0].path_callback = wtd_wt_session_callback; /* new */
path_table[0].path_app_ctx = &sctx;

picohttp_server_parameters_t h3_params = { 0 };
h3_params.web_folder = sctx.dir_path;   /* NULL if --dir not given */
h3_params.path_table = path_table;
h3_params.path_table_nb = 1;

h3zero_callback_ctx_t *h3_ctx = h3zero_callback_create_context(&h3_params);
picoquic_set_default_callback(quic, h3zero_callback, h3_ctx);
```

All existing behaviour (WT handshake, frame codec, `--exec`
child) moves into `wtd_wt_session_callback`. That callback
receives `(cnx, bytes, length, event, stream_ctx, path_app_ctx)`
and is picohttp's way of saying "someone is posting data to
`/wt`." We look up or create the `wtd_peer_t` from the `cnx`
(via `picoquic_get_callback_context` → our app_ctx), and do
exactly what `server_stream_cb` does today.

**Critical:** this cycle MUST NOT regress `handshake_echo_test`,
`handshake_multi_test`, `handshake_socket_test`. The WT session
flow through path_callback should be byte-for-byte identical to
what `server_stream_cb` does now. Break this into a proven-
equivalent refactor commit (no `--dir` yet), and only then…

### 43b — add `--dir=<path>` parsing

- Parse `--dir=<path>` in `main`'s argv loop (same
  `parse_arg_value` helper as `--cert=`, `--exec=`).
- Plumb to `cmd_server(cert, key, port, exec_path, dir_path)`.
- `--staticdir=<path>` as an alias for symmetry with websocketd.
- Update `print_usage` to document both.
- Update the `README.md` CLI table (cycle 44 dependency — or
  land cycle 44 first).

### 43c — end-to-end integration test

New file `handshake_static_test.c`. Same POSIX-only wrapping
pattern as `handshake_echo_test` (POSIX only; Windows skip).
Flow:

1. Create a `fixtures/site/` directory in the test (or under
   `/tmp/wtd-static-<pid>/`). Write an `index.html` with a
   known sentinel payload.
2. Fork/exec the daemon: `./webtransportd --server --cert=auto
   --port=... --dir=./fixtures/site`.
3. Client handshake reaches `picoquic_state_ready`.
4. Send an HTTP/3 GET for `/index.html`. Assert the response
   body equals the fixture bytes.
5. Send an HTTP/3 GET for `/../../etc/passwd`. Assert it 404s
   (picohttp's path-resolution does this, but guard against a
   regression).
6. Kill + reap daemon.

For step 4 the client needs an HTTP/3 request primitive, not
just QUIC streams. Two options:
- **Option A**: use picohttp's `democlient.c` `picoquic_demo_
  client_initialize` which knows how to emit a `GET /path`
  HTTP/3 request. Link the demo_client code into the test.
- **Option B**: hand-roll a minimal HTTP/3 GET request as raw
  QUIC stream bytes via `h3zero_create_request_header_frame`.
  Small (~30 lines), doesn't pull the democlient surface in.

Prefer Option B for test isolation — the test should read like
"send these exact bytes, get these back." Option A is there if
Option B turns out to have an HPACK encoder gotcha.

### 43d — `--wt-path=<string>` (deferred)

The `/wt` path is hard-coded in 43a. A future cycle can add
`--wt-path=<string>` so operators can pick their own mount
point (`/api/ws`, `/echo`, whatever). Not needed for v0.1 —
one WT endpoint per daemon instance is fine, and operators who
want multiple can run multiple daemons.

## Critical files

- [webtransportd.c](../webtransportd.c):
  - `main` (argv loop at line 578): add `--dir=` / `--staticdir=`.
  - `cmd_server` (line 447): new `dir_path` parameter, new
    `picohttp_server_parameters_t` + `h3zero_callback_ctx`,
    swap `picoquic_create`'s default_callback.
  - `server_stream_cb` (line 359): moves into a
    path-callback-shaped `wtd_wt_session_callback`.
  - `wtd_peer_t` (line 195): unchanged, but the lookup key
    shifts from `cnx` to `(cnx, stream_id)` since picohttp
    gives us a stream_ctx per HTTP/3 request.
  - `print_usage` (line ~83): new `--dir=<path>` entry.
- New: `handshake_static_test.c`.
- [Makefile](../Makefile): new explicit rule for
  `handshake_static_test` (links picohttp + full vendored).

## Out of scope

- **MIME-type overrides.** picohttp's built-in mapping
  (`.html`, `.js`, `.css`, `.png`, `.wasm`, etc.) is enough for
  a devconsole. An operator who needs `application/wasm`-vs-
  `application/octet-stream` precision can ship static files
  with a Content-Type sidecar if picohttp ever needs it.
- **Directory listing.** `--dir=.` shouldn't expose a listing
  page. picohttp returns 404 for a directory-path request
  unless `index.html` is present; that's the right default.
- **Cache-Control / ETags / Last-Modified.** Static serving for
  a local devconsole — browsers that actually cache across
  runs are out of scope.
- **HTTPS → HTTP redirect.** The daemon is HTTP/3-only. An
  operator who needs a TCP/443 HTTP/1.1 listener runs a
  reverse proxy.
- **`wtd_wt_session_callback` supporting multiple concurrent
  WT sessions on the same cnx.** Today one peer per cnx is
  enforced by `peer_find` lookup; keep that invariant under
  the path-callback model.

## Dependencies / ordering

- Follows cycle 42 (`--cert=auto` — so the quickstart and
  integration test can spin up a TLS listener without a PEM
  pair).
- Precedes cycle 44 (README) — the README needs to document
  `--dir` and link to the static serving story.
- Blocks cycle 45 (flow control) only loosely — flow control
  lives on the WT-session path, which moves to
  `wtd_wt_session_callback`. The blocking `write_all` is the
  same code in a new location.

## Verification

- All three existing handshake tests (`handshake_socket_test`,
  `handshake_echo_test`, `handshake_multi_test`) stay green
  through the 43a refactor. If they break, the refactor is
  wrong; don't merge.
- `handshake_static_test` green under ASAN+UBSAN on linux-gcc
  + macos-clang.
- Smoke: `./webtransportd --server --cert=auto --port=4433
  --dir=./designs` then `curl --http3 -k https://localhost:4433/
  00-fix-cicd.md` returns the file bytes. (Non-automated; one-
  time manual check.)
- `./webtransportd --server --cert=auto --port=4433 --exec=...`
  (no `--dir`) still accepts a WT session on the default path
  and falls through to 404 for `/index.html`. Zero-regression
  baseline.

## Commit shape

Three commits inside one cycle (43a, 43b, 43c), each landing
green independently:

```
Cycle 43a: swap server_stream_cb for h3zero path-callback

No new feature yet — lift the WT session handling out of the
raw-stream callback and into picohttp's path_table machinery.
Existing handshake_echo_test + handshake_multi_test +
handshake_socket_test stay green (byte-equivalent behaviour).
Sets up --dir= to plug in by setting web_folder on the
picohttp_server_parameters_t in cycle 43b.
```

```
Cycle 43b: --dir=<path> serves static files on non-WT paths

--dir= / --staticdir= argv flags plumb into the
picohttp_server_parameters_t.web_folder picoquic already
respects. No change to the WT session path. print_usage and
README CLI table updated.
```

```
Cycle 43c: handshake_static_test end-to-end HTTP/3 GET

Drive the fixture site through a real HTTP/3 GET and assert
the response body. Path-traversal attempt (/../../...) gets a
404. Covers the --dir=./fixtures happy path + the traversal-
block safety case.
```

## Risk notes

- **`server_stream_cb` → path_callback semantic drift.**
  Picohttp's path_callback gets byte events AFTER HTTP/3
  header parsing — the semantics differ subtly from our current
  raw-QUIC-stream callback (e.g., the client MUST send a proper
  CONNECT + `:protocol=webtransport` request to reach us).
  This is the cycle's biggest risk. Mitigation: write the
  refactor against the existing handshake test suite and treat
  any test breakage as a red. If picohttp's WT session model
  doesn't match the one our tests exercise, the right fix
  might be `h3zero_server_accept_webtransport` at stream-create
  time, not at path-callback time. Investigate before committing
  43a.
- **Path-traversal bugs.** We rely on picohttp's path
  resolution to reject `/../`. Haven't audited that code —
  cycle 43c's traversal-block test is the safety net. If
  picohttp's handler is sloppy, either fix it upstream or add
  a wrapper path_callback that validates the path before
  passing through.
- **Client test complexity.** Option B (hand-roll HTTP/3 GET)
  requires encoding an HPACK-like QPACK header block. If that
  turns out to be nontrivial, fall back to Option A
  (link democlient). Picking the wrong option costs ~half a
  day; not a cycle blocker.
- **Operator confusion: "`/wt` vs `/`."** Some frameworks
  (e.g. wasm frontends served on `/`) might assume the root
  path is the WT session. Document the default in `print_usage`
  + README, and note that `--wt-path=<custom>` is on the
  roadmap for operators who need a different layout.
