# Ember patch

This is the published `swarm-discovery` 0.6.4 source, from upstream commit
`360811856f3f6fe9883e3b2988a5d8b961b4dd6a`, under its included Apache 2.0 license.
Upstream: https://github.com/rkuhn/swarm-discovery

The receive loop used a 1,472-byte buffer. On Windows, a larger UDP datagram
returns WSAEMSGSIZE (10040), rather than a truncated successful read. The
receiver propagated that error and its supervisor stopped all discovery.
Unrelated devices advertising larger mDNS responses could therefore disable
Ember's room browser and address lookup until their endpoints were recreated.

`src/receiver.rs` now receives the full UDP payload range. The existing DNS
parser still rejects malformed/unrelated messages. A loopback regression sends
a large unrelated packet followed by a valid discovery query and requires the
receiver to deliver that query. No wire format or discovery policy is changed.

Remove this patch when a compatible upstream release includes the fix. Other
source files are unchanged from the published crate.

Vendored packaging keeps the production sources and licenses only. Examples, integration tests and their Cargo entries are omitted; the crate-level README include is replaced with a short module description. The receive-buffer regression in src/receiver.rs is retained.
