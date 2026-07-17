# AGENTS.md

This file gives AI agents repository-specific working rules for Elio. It is
intended for coding agents, review agents, release agents, and GitHub workflow
agents. Follow it together with `CONTRIBUTING.md`, the GitHub issue templates,
and the pull request template.

## Repository Facts

- Elio is a C++20 coroutine library for Linux.
- Library code is header-only and lives under `include/elio/`.
- Tests live under `tests/`; examples and benchmarks live under `examples/`.
- Project documentation lives in `wiki/`, not in a `docs/` directory. The
  `wiki-sync` workflow publishes `wiki/**` to the GitHub Wiki.
- Build configuration is CMake-based. Dependencies such as fmt, Catch2,
  nghttp2, and Asio may be fetched through CMake `FetchContent`.

## Basic Agent Conduct

- Start by reading the relevant code and existing documentation. Do not assume
  the design from names alone.
- Keep changes tightly scoped to the requested issue or PR.
- Preserve user and maintainer changes. Never revert unrelated local changes.
- Do not add generated files, local build trees, caches, logs, or temporary
  notes to commits.
- Prefer small, reviewable changes over broad refactors.
- Use English for GitHub issues, pull requests, commit messages, and public
  review comments.
- Public GitHub comments should be sanitized: do not mention local absolute
  paths, temporary directories, private tool state, quota problems, or internal
  agent names unless the detail is directly useful to maintainers.

## Build And Test Rules

- Never perform an in-source build.
- Use out-of-source build directories such as `build/`, `build-debug/`, or a
  temporary directory outside the repository.
- Limit local build parallelism. Use a small explicit value such as
  `cmake --build <dir> --parallel 2` unless the maintainer asks otherwise.
- For focused local validation, disable unrelated optional components when
  appropriate, for example TLS/HTTP/HTTP2/RDMA/TCP benchmarks.
- Do not treat dependency-fetch, network, or cache failures as code failures.
  Report them separately.
- If a change is documentation-only and CI intentionally skips code jobs, the
  skip is not itself a failure.
- Do not wait for `main` CI after merging a PR when the PR head already passed
  and there were no concurrent main changes that can affect the result.

Common CMake pattern for focused validation:

```bash
cmake -S . -B build-agent \
  -DCMAKE_BUILD_TYPE=Debug \
  -DELIO_BUILD_TESTS=ON \
  -DELIO_BUILD_EXAMPLES=ON \
  -DELIO_ENABLE_TLS=OFF \
  -DELIO_ENABLE_HTTP=OFF \
  -DELIO_ENABLE_HTTP2=OFF \
  -DELIO_ENABLE_RDMA=OFF \
  -DELIO_ENABLE_RDMA_CM=OFF \
  -DELIO_ENABLE_RDMA_IBVERBS=OFF \
  -DELIO_ENABLE_RDMA_CUDA=OFF
cmake --build build-agent --parallel 2
ctest --test-dir build-agent --output-on-failure
```

## Code Review And Audit Scope

Audit code, API contracts, documentation, CI, packaging, and release metadata.
Do not limit review to crashes or compiler errors.

High-value review areas include:

- coroutine lifetime and frame ownership;
- scheduler shutdown, worker drain, work stealing, and affinity;
- cancellation propagation and exactly-once resumption;
- synchronization primitives and blocked coroutine wakeups;
- network stream readiness and exact-length read/write behavior;
- RPC request lifecycle, server-side concurrency limits, and cancellation;
- HTTP, WebSocket, SSE, and HTTP/2 timeout and resource-boundary behavior;
- CI path filtering, package export, and downstream consumer behavior;
- README, wiki, changelog, release tag, and version metadata consistency.

When a finding depends on an unclear responsibility boundary, clarify the
contract first. Some safety responsibilities belong to the library; others may
belong to callers. Document the boundary before expanding the library's
responsibility.

## Implementation Style

- Follow `CONTRIBUTING.md` for C++ style, commit messages, branch strategy, and
  PR requirements.
- Keep library code header-only unless the project explicitly changes that
  policy.
- Use C++20 features where they simplify the code.
- Do not block scheduler worker threads. Use blocking-pool facilities for long
  blocking operations.
- Add comments only for non-obvious constraints, ordering requirements,
  concurrency invariants, or surprising behavior.
- For structured data, use structured parsing or existing helpers rather than
  ad hoc string manipulation.
- For concurrency fixes, prefer explicit state machines and single-owner
  transitions over loosely coupled flags.

## Issues

- Search existing issues before filing a new one.
- File one issue per distinct concern. Do not bundle unrelated defects.
- Classify accurately: bug, API contract, documentation, CI/build, performance,
  release, or design gap.
- Titles should reflect the real category and scope, not every issue should be
  titled as a bug.
- Use the repository issue templates and fill in relevant environment, build,
  dependency, and reproduction context.
- For static-review findings that were not compiled or tested, say so.
- Include concrete file and line evidence when possible.
- If the issue is about caller/library responsibility boundaries, state the
  proposed boundary explicitly.

## Pull Requests

- Use one branch and one PR per logical issue or tightly coupled issue group.
- PR titles should follow conventional commit style.
- Fill out `.github/PULL_REQUEST_TEMPLATE.md` in English.
- Link issues with `Closes #...` or `Related to #...` as appropriate.
- Mark breaking changes clearly and include migration guidance.
- Update tests, wiki, README, and `CHANGELOG.md` when the change affects public
  behavior, documented contracts, release status, or user-visible APIs.
- Avoid committing unrelated formatting, generated files, local specs, build
  outputs, or editor state.
- Before opening or updating a PR, check `git status --short` and review the
  diff.

## Review Workflow

- The first Copilot review is automatically triggered when a PR is opened; do
  not immediately comment just to request the first review.
- If a PR is updated and a new Copilot review is needed, comment with
  `@copilot` and explicitly ask for review only, not direct code changes.
- Do not use `@copilot-pull-request-reviewer`.
- If Copilot is unavailable or over quota, use independent read-only agent
  review instead. Post the useful review findings as normal GitHub PR comments.
- Review comments posted to GitHub should focus on actionable code, contract,
  test, or documentation issues. Do not include local run paths, temporary
  directories, irrelevant commit IDs, or internal process notes.
- If Copilot or another bot pushes commits and CI is blocked pending approval,
  approve the workflow only after verifying the pushed changes are appropriate.
- If a PR has passing required checks, no conflicts, and no unresolved review
  objections, it may be merged when the maintainer has allowed agents to merge
  that class of PR.

## CI And GitHub Operations

- Use `gh` when interacting with GitHub from an agent environment.
- Inspect failing checks and logs before guessing at CI causes.
- Distinguish real test failures from infrastructure failures, skipped jobs,
  dependency fetch failures, and external provider issues.
- For docs-only or metadata-only changes, verify that path-filter behavior is
  intentional and that required checks do not incorrectly treat skipped jobs as
  failures.
- Do not include unnecessary files in commits. Use `git status --short` before
  staging and before creating a PR.
- Prefer non-interactive git commands. Avoid destructive commands such as
  `git reset --hard` or `git checkout --` unless explicitly instructed by a
  maintainer.

## Documentation And Release Metadata

- Documentation updates belong in `wiki/` unless the README, changelog, code
  comments, or templates are the correct user-facing surface.
- Keep README version/status, `CHANGELOG.md`, CMake project version, release
  tag, and GitHub release target aligned for release work.
- Patch releases on the current line may be tagged from `main`; older maintained
  minor lines use `release/X.Y` branches as described in `CONTRIBUTING.md`.
- Remove stale roadmap or planning documents when they are no longer accurate
  and are not useful to users.

## Safety Boundaries To Preserve

- A cancellation request is cooperative. It is not a guarantee of immediate
  stop or safe asynchronous frame destruction.
- Awaitables that suspend on I/O must own the cancellation/completion state
  machine and must resume the coroutine exactly once.
- Public cancellation guarantees should not silently differ by backend.
- Sync primitives that advertise cancellable waits must wake and clean up
  blocked waiters correctly.
- `read` and `write` should be readiness-aware at the public stream layer.
  Exact byte-count semantics belong to `read_exactly` and `write_exactly`.
- RPC servers need explicit concurrency and overload behavior. Do not assume
  clients will self-limit enough to protect a server.
