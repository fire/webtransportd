# Contributing to WebTransportD

## Code Organization

| Directory       | Purpose                          |
|----------------|----------------------------------|
| `src/core/`    | Core source files (`.c`)         |
| `src/include/` | Header files (`.h`)              |
| `tests/unit/`  | Unit tests                       |
| `tests/integration/` | Integration tests         |
| `tests/fuzz/`  | Fuzz tests                      |
| `examples/`    | Example code                    |
| `docs/`        | Documentation                   |
| `scripts/`     | Build/test scripts              |

## Building

1. **Prerequisites**:
   - CMake 3.15+
   - Clang or GCC
   - OpenSSL
   - Picoquic (`thirdparty/picoquic`)

2. **Build**:
   ```bash
   mkdir -p build && cd build
   cmake ..
   make
   ```

3. **Run Tests**:
   ```bash
   make test
   ```

## Code Style

- **Indentation**: 4 spaces (no tabs).
- **Naming**: `snake_case` for variables/functions, `SCREAMING_SNAKE_CASE` for macros.
- **Headers**: Include guards (`#pragma once`).
- **Commits**: Atomic, with clear messages (e.g., "Fix handshake timeout in peer_session.c").

## Pull Requests

- **Branch**: `feature/<name>` or `fix/<name>`.
- **Tests**: Include unit/integration tests for new features.
- **Docs**: Update `docs/` if adding new APIs or behaviors.