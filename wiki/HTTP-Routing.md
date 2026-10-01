# HTTP Connection Routing And Reuse

HTTP/1 clients resolve each exchange target into one internal immutable route
plan. That plan owns the normalized target and the published Transport snapshot;
connection establishment, idle lookup, and return all use its structured
compatibility key. A redirect creates a fresh plan. Returning a connection does
not reconstruct a key from the current URL or a replacement Transport.

The current connector implements direct HTTP and HTTPS. Proxy modes and their
identity fields are modeled for subsequent connectors; this page does not
advertise outbound proxy support. Route plans and keys are implementation details,
not a public dialing or connection-injection API.

## Compatibility Matrix

All rows require the same connector and resolution policy domains, required
protocol, and DNS semantics. Equality compares complete values; a hash match is
never sufficient. Published domains use unique non-recycled process-local
identities, not TLS context addresses, credentials, bearer tokens, or credential
hashes. They are not persistent identifiers across process restarts.

| Route | Target binding | Additional compatibility fields | Connector status |
| --- | --- | --- | --- |
| Direct HTTP | Normalized origin host, effective port, HTTP | No TLS layer | Implemented |
| Direct HTTPS | Normalized origin host, effective port, HTTPS | Origin TLS security domain | Implemented |
| HTTP origin through plain HTTP proxy | Conservative per-origin binding | Ordered proxy endpoint(s), proxy authentication domain, hop protocol/DNS semantics | Identity modeled; connector pending |
| HTTP origin through HTTPS proxy | Conservative per-origin binding | Plain-proxy fields plus outer proxy TLS security domain | Identity modeled; connector pending |
| HTTPS origin through plain HTTP CONNECT | Permanently target-bound tunnel | Ordered proxy endpoint(s), proxy authentication domain, origin TLS security domain | Identity modeled; connector pending |
| HTTPS origin through HTTPS CONNECT | Permanently target-bound tunnel | Plain-CONNECT fields plus independent outer proxy TLS security domain | Identity modeled; connector pending |

Forward channels retain origin authority in their key for now, even though a
future connector may deliberately allow sharing across compatible HTTP origins.
CONNECT never turns back into a generic forward-proxy channel. Origin authority
also remains separate from proxy endpoints for HTTP request encoding/accounting.
Local versus proxy target-DNS semantics have distinct identity values; SOCKS and
HTTP/2 connector implementations remain outside this change.

## Normalization And Policy Ownership

ASCII DNS names are case-folded. Parsed IPv4 and IPv6 literals are canonicalized;
IPv6 authorities retain brackets and effective ports. IPv6 zone identifiers
remain case-sensitive. Trailing-dot DNS names and alternate/ambiguous IPv4
spellings are conservatively distinct. Implicit and explicit default ports
compare equal. Paths, queries, fragments, URI userinfo, and application headers
are not connection identity fields. No secret URL components are retained in the
route plan.
HTTP/1 request/redirect diagnostics log destination host/port or a bounded
rejection category, not serialized headers/bodies or complete Location URLs.

A plan strongly retains its published DNS admission and origin TLS owners.
Configured resolver caches retain their existing borrowed-cache lifetime
contract. The connector consumes the same normalized endpoint and frozen
resolver/security configuration from which the key was derived. New transports
receive new policy domains even when their option values happen to match;
explicitly sharing one Transport is how clients share its compatible connections.
Publishing another Transport during a suspended acquisition does not change the
old plan, the old connection's return key, or the old pool owner.

Read/Expect/body/redirect/User-Agent policy and per-acquisition connect deadlines
remain request policy. They do not split connection identity. A plan does not
change timeout budgets, introduce admission limits, or prove that a partially
read response is reusable. The exchange still requires complete framing,
keep-alive, no EOF/close-delimited body, and no unread suffix before returning a
connection.

## Internal Connection Leases

Buffered requests and scoped `with_response` exchanges carry a move-only
internal lease. It owns the original route plan and a strong reference to the
original Transport state from dialing until disposition. Moving a lease
transfers its single disposition; moved-from destruction does nothing.

Only the exchange owner can return it after complete, unambiguous HTTP/1
framing, keep-alive eligibility, no EOF/close-delimited body, no read-ahead
suffix, and settlement of its I/O/watchdogs. Every other path retires by aborting
and disconnecting the stream. Destruction never marks an unfinished response
reusable, drains a body, or starts hidden asynchronous TLS shutdown. Allocation
exceptions during pool insertion still leave the lease responsible for
retirement. There is no public unchecked reuse flag or escaping lease API.

`transport::clear()` atomically detaches idle entries with the pool-generation
change; detached streams are destroyed outside lifecycle/pool locks. An
already acquired or dialing lease may finish its current exchange, but its late
return is retired. New work may acquire normally. `shutdown()` additionally
closes acquisition and awaits settlement; cancelling that wait does not cancel
or destroy active public frames. A returned CONNECT identity remains bound to
its target; retiring it never recovers the underlying proxy channel for a new
tunnel (the connectors are still separate follow-ups).

Internal state retention does not relax caller lifetimes: keep the client,
scheduler, borrowed request/URL/string inputs, and configured resolver cache
alive until normal awaited return. Scoped streaming readers and their pending
reads must not escape the handler.

## Standalone Pool Compatibility

Legacy `connection_pool::{acquire_result,acquire,release}` signatures remain
available and use a separate legacy identity namespace. They cannot inject a
connection into a Transport's plan-based pool. Their loose acquire/release
contract still requires callers to supply the original host/port/scheme and
keep one stable TLS/resolver policy per standalone pool; they cannot infer a
security domain from a borrowed mutable TLS context. To change its TLS or DNS
policy, first settle all old acquisitions/exchanges and dispose of all
checked-out connections, then clear the idle entries. `clear()` alone is not a
barrier against late returns. Alternatively, route new work through a fresh pool
and keep the old pool/context alive and unchanged for old operations/returns.
For automatic route/security isolation and client-managed lifecycle, migrate to
`client(shared_ptr<transport>, policy)`.
These legacy adapters do not gain the Transport's lease/generation guarantees.
