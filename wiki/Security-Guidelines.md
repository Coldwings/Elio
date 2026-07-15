# Security Guidelines

This page summarizes secure configuration practices for applications that use
Elio. It complements the vulnerability reporting policy in `SECURITY.md`.

## Library Guarantees vs Caller Responsibilities

This section defines the default responsibility boundary used for Elio issue
triage and API review. API-specific documentation can narrow or extend these
rules; when it does, the more specific contract wins.

### Elio Library Guarantees

Elio should enforce these properties inside the library:

- Validate transport and protocol frames received from peers before exposing
  them as higher-level messages. This includes HTTP, WebSocket, SSE, RPC, TLS
  wrapping, and other network trust boundaries.
- Reject malformed protocol state at the parsing boundary instead of silently
  dispatching it to application handlers. Examples include invalid frame
  headers, invalid masking or length encodings, illegal state transitions, and
  control frames that must be handled by the protocol layer.
- Preserve protocol ordering and state semantics that are internal to Elio,
  such as handling queued control frames before later data frames when the
  wire protocol requires it.
- Enforce configured size limits, timeouts, session caps, and close/cancel
  behavior. Invalid or over-limit input should fail closed and must not require
  unbounded buffering or allocation before rejection.
- Maintain memory safety, waiter cleanup, cancellation cleanup, and ownership
  safety for public APIs when callers satisfy the documented preconditions.
- Provide safe defaults for high-level helpers where practical, such as TLS
  certificate verification and bounded protocol defaults.
- Honor documented thread-safety guarantees under the documented concurrency
  model.

### Caller Responsibilities

Applications using Elio are responsible for these conditions:

- Choose resource limits and timeouts that match the application's threat
  model. Defaults are general-purpose; `0` or "unlimited" settings are an
  explicit application policy choice.
- Validate application-level payloads after Elio has parsed the transport
  frame. Elio can parse HTTP paths, headers, RPC frames, and WebSocket
  messages, but it does not know the application's authorization rules,
  business invariants, schema versions, or method permissions.
- Respect documented thread-safety and concurrency preconditions. If an API is
  documented as not safe for concurrent use, callers must serialize access.
- Keep borrowed buffers, spans, callbacks, TLS contexts, scheduler objects,
  streams, RDMA memory registrations, and other externally owned resources
  alive for the documented duration.
- Use low-level fast paths only with valid inputs. RDMA, verbs-adjacent APIs,
  custom QP attributes, registered buffers, and raw parser helpers may rely on
  explicit preconditions to avoid overhead in hot paths.
- Observe operation results and close/cancel resources according to the API
  contract. Ignoring errors or continuing to use an object after a documented
  terminal state is caller misuse.
- Protect deployment secrets and local operating-system resources, including
  private keys, certificate files, service accounts, filesystem permissions,
  and exposed listening sockets.

### Undefined or Ambiguous Boundaries

If a safety concern is not clearly assigned to Elio or to the caller, clarify
the contract before broadening the library's responsibilities:

- File or fix a contract/documentation issue when the public boundary is
  ambiguous, inconsistent, or only discoverable from implementation details.
- Classify the concern as a library bug when Elio violates a documented
  guarantee, exposes malformed peer input past a protocol boundary, ignores a
  configured limit, or requires callers to prevent an internal protocol safety
  failure.
- Classify the concern as caller misuse when the caller violates an explicit
  precondition, such as object lifetime, non-concurrent access, required
  external serialization, or low-level buffer validity.
- Treat security-boundary failures as library-side when an untrusted peer can
  trigger memory corruption, undefined behavior, unbounded resource growth, or
  unsafe protocol dispatch despite the caller satisfying documented
  preconditions and configuring reasonable limits.
- Prefer documenting the boundary first, then deciding whether code changes are
  still required.

## Keep Dependencies Current

- Use a supported Elio release line.
- Track security updates for OpenSSL, nghttp2, liburing, RDMA-Core, CUDA, and
  other optional dependencies enabled in your build.
- Rebuild and retest applications after dependency updates.

## TLS and Certificates

- Keep `verify_certificate` enabled for outbound HTTPS, WebSocket, and SSE
  clients unless a test environment explicitly requires otherwise.
- Load trusted CA paths before connecting to production services.
- Configure ALPN when using HTTP/2.
- Treat private keys and certificates as application secrets; do not embed them
  in source code or examples.

## Resource Limits

- Set request, response, header, and RPC frame limits to match the application
  protocol instead of relying only on defaults.
- Use read/connect timeouts for network-facing clients and servers.
- Close or cancel slow peer operations when they exceed application policy.

## Input Validation

- Validate application-level message contents after Elio parses transport
  frames.
- Reject unexpected HTTP methods, paths, headers, RPC method IDs, and payload
  sizes before invoking privileged business logic.
- Treat data from peers as untrusted even when TLS is enabled.

## Operational Guidance

- Run tests with sanitizers in development and CI for code that handles
  untrusted input.
- Monitor logs for repeated parse failures, connection resets, oversized
  payloads, and timeout errors.
- Use least-privilege service accounts and filesystem permissions for
  applications that bind sockets or load certificates.
- Review exposed TCP, Unix-domain socket, RDMA, WebSocket, SSE, and RPC
  endpoints before deploying.

Report suspected vulnerabilities privately through the process in
`SECURITY.md`.
