# Contributing to Elio

Thank you for your interest in contributing to Elio! This document provides
guidelines and instructions for contributing to the project.

## Table of Contents

- [Getting Started](#getting-started)
- [Development Setup](#development-setup)
- [Code Style](#code-style)
- [Testing](#testing)
- [Commit Messages](#commit-messages)
- [Pull Requests](#pull-requests)
- [Branching and Release](#branching-and-release)
- [Reporting Issues](#reporting-issues)

## Getting Started

1. Check existing [issues](https://github.com/Coldwings/Elio/issues) to avoid
   duplicate work.
2. For non-trivial changes, open an issue first to discuss the approach.
3. Fork the repository and create a feature branch from `main`.
4. Make your changes, add tests, and ensure CI passes.
5. Submit a pull request.

## Development Setup

### Prerequisites

- **Compiler**: GCC 12+ or Clang 15+ with C++20 support
- **OS**: Linux (kernel 5.1+ for io_uring, or any modern Linux for epoll)
- **CMake**: 3.20 or higher
- **Optional**: liburing-dev, libssl-dev (for TLS/HTTPS)

### Building

```bash
git clone https://github.com/Coldwings/Elio.git
cd Elio
cmake -B build -G "Unix Makefiles"
cmake --build build -j$(nproc)
```

Dependencies (fmtlib, Catch2, nghttp2) are fetched automatically via CMake
FetchContent.

### CMake Options

```bash
cmake -B build \
  -DELIO_BUILD_TESTS=ON \
  -DELIO_BUILD_EXAMPLES=ON \
  -DELIO_ENABLE_TLS=ON \
  -DELIO_ENABLE_HTTP=ON \
  -DELIO_ENABLE_HTTP2=ON \
  -DELIO_ENABLE_DEVELOPER_WARNINGS=ON \
  -DELIO_WARNINGS_AS_ERRORS=ON
```

Enable `ELIO_ENABLE_DEVELOPER_WARNINGS` and `ELIO_WARNINGS_AS_ERRORS` during
development to catch issues early. These flags only apply to tests and examples
and are not propagated to downstream consumers.

## Code Style

- **C++20** — use concepts, `if constexpr`, structured bindings, and coroutines
  where appropriate.
- **Header-only** — all library code lives under `include/elio/`. Do not
  introduce `.cpp` files in the library.
- **Naming** — `snake_case` for functions, variables, and namespaces;
  `PascalCase` for types and templates; `UPPER_CASE` for macros.
- **Comments** — default to no comments. Add one only when the *why* is
  non-obvious (a hidden constraint, a workaround, surprising behavior).
  Do not explain *what* the code does — let well-named identifiers do that.
- **No blocking on worker threads** — route long blocking calls through
  `elio::spawn_blocking` or `runtime::blocking_pool`.

## Testing

All changes must pass the existing test suite. New features require new tests.

```bash
# Run the full test suite
ctest --test-dir build --output-on-failure

# Run tests filtered by Catch2 tag
./build/tests/elio_tests "[scheduler]"
./build/tests/elio_tests "[sync]"
./build/tests/elio_tests --list-tags

# ASAN (memory safety)
cmake --build build --target test_asan

# TSAN (thread safety)
cmake --build build --target test_tsan
```

Frequently used tags: `[scheduler]`, `[task]`, `[sync]`, `[channel]`, `[event]`,
`[condvar]`, `[cancel_token]`, `[autoscaler]`, `[chase_lev_deque]`,
`[generator]`, `[batch][file_helpers]`, `[http][parser]`, `[http2]`,
`[websocket]`, `[sse]`, `[rpc]`, `[signal]`, `[hash]`, `[integration]`.

### Test Requirements

- Unit tests must pass on both x64 and arm64.
- ASAN tests must pass (Debug build).
- TSAN tests must pass where applicable.
- New public APIs must have corresponding test coverage.

## Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/) format:

```
<type>(<scope>): <short summary>
```

**Types**: `fix`, `feat`, `refactor`, `perf`, `test`, `docs`, `chore`, `build`.

**Scopes**: `runtime`, `coro`, `sync`, `io`, `net`, `http`, `tls`, `rpc`,
`signal`, `time`, `log`, `debug`.

Examples:

```
fix(sync): correct semaphore::release() phantom permit leak
feat(runtime): add elio::go_to() free function for worker-pinned spawning
refactor(coro): merge when_any resolve variants into single template
perf(runtime): add alignas(64) to worker_thread hot fields
```

## Pull Requests

1. **Title**: follow the same conventional commit format as commit messages.
2. **Description**: explain *what* changed and *why*. Reference related issues.
3. **CI must pass**: all required GitHub Actions checks, including matrix
   builds (x64/arm64, Release/Debug), ASAN/TSAN, release benchmarks, and the
   package-consumer check.
4. **One logical change per PR**: keep PRs focused. Split unrelated changes.
5. **Breaking changes**: clearly document in the PR description and prefix the
   commit with `!` (e.g., `fix(sync)!: make channel non-movable`).

## Branching and Release

### Branch Strategy

- **`main`** — development mainline. Always builds and passes tests.
- **`release/X.Y`** — long-lived branch for a minor version series (e.g.,
  `release/0.4` covers all 0.4.x releases). Created from the initial `vX.Y.0`
  tag. Patch releases (vX.Y.1, vX.Y.2, …) are tagged on this branch.
- **Feature branches** — short-lived branches for individual changes, merged
  to `main` via PR.

### Release Process

1. All changes land on `main` first.
2. When cutting a new minor version, tag `vX.Y.0` on `main` and create a
   `release/X.Y` branch from that tag.
3. Bug fixes for a released version are cherry-picked from `main` to the
   `release/X.Y` branch, then tagged as `vX.Y.Z` on that branch.
4. The project aims to maintain the last two minor versions.

### Backporting

Backporting is currently done manually via `git cherry-pick`. When cherry-picking
a fix to a release branch, open a PR targeting the `release/X.Y` branch so that
CI validates the change.

## Reporting Issues

### Bug Reports

Please include:

- Elio version (tag or commit hash)
- Compiler and version (e.g., GCC 14.1)
- Linux kernel version
- Minimal reproducer if possible
- Expected vs actual behavior

### Feature Requests

Open an issue describing the use case, the proposed API, and any relevant
alternatives you have considered.

## License

By contributing to Elio, you agree that your contributions will be licensed
under the [MIT License](LICENSE).
