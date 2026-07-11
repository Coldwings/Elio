# Security Guidelines

This page summarizes secure configuration practices for applications that use
Elio. It complements the vulnerability reporting policy in `SECURITY.md`.

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

