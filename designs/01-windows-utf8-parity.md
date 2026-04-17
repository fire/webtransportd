# Design: Windows UTF-8 Parity

## Goal
Ensure Windows is a first-class target for UTF-8 handling, matching POSIX for paths, argv, env, and child-stdin/stdout round-tripping.

## Steps
- Embed application manifest with `<activeCodePage>UTF-8</activeCodePage>` for CRT UTF-8 handling (Windows 10 1903+).
  - On mingw: use `windres` + `.rc` file.
  - On clang-cl: use `/MANIFEST:EMBED` link flag.
- Verify `CreateProcessA` in `child_process.c` sees UTF-8 correctly with manifest.
  - If older Windows support is needed, switch to `CreateProcessW` with UTF-16 command line (not speculative).
- Update `build_cmdline()` to escape embedded quotes per MSDN "Parsing C++ Command-Line Arguments" rules.
- Add/extend `handshake_utf8_test` to exec child through a path with `文档`/`日本語` and send UTF-8 payload, asserting round-trip byte equality.
- Ensure `--version`/`--help` output round-trips UTF-8 sentinel bytes without mojibake on Windows.
