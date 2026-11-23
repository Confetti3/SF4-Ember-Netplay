# Vendored crates

Crates here replace their crates.io release through `[patch.crates-io]` in
`rust/sf4-net/Cargo.toml`. Each one is first committed exactly as published
(`.gitattributes` keeps this folder byte-for-byte), and Ember's changes follow
in separate commits, so `git log -p -- rust/vendor/<crate>` shows only them.

## iroh 1.1.0

Source: `iroh-1.1.0.crate` from crates.io, sha256
`329552fad4e46b5ccb4439d5635216f6289eec2c46f3fde4bc732d762694b9e5`.
Licence: MIT OR Apache-2.0 (the crate's `Cargo.toml`; `LICENSE-BSD3` covers the
parts derived from Tailscale). Changed by SF4 Ember Netplay, 2026-11-23; every
change is marked `Ember patch` in `src/net_report.rs`.

What the patch changes, both in how a player's home relay is chosen:

1. **Every relay gets a latency probe.** Upstream starts QUIC address discovery
   probes against the first 5 relays of the relay map only, and the map iterates
   in URL order. With Ember's six relays, `usw1-1.relay.n0.iroh.link` sorted
   last, never got a latency and could never be a home relay, so West Coast
   players who could not connect directly were relayed through Kansas (a
   NorCal-Seattle pair measured 142-175 ms). The cap is now `QAD_PROBE_RELAYS` (8);
   `sf4-net`'s invite tests check the relay table stays under it.
2. **The full report keeps the current relay.** Every 5 minutes the report runs
   in full mode, which clears the previous report to plan its probes. Upstream
   that also dropped the previous preferred relay, so the full report chose
   without the "switch only if a third faster" rule, and two relays a few
   milliseconds apart (n0's and Ember's relay in one region) swapped the home
   relay every 5 minutes. `Reports::last_preferred` now carries it across.
   Test: `test_full_report_keeps_preferred_relay_hysteresis`.

Still upstream behaviour: a report keeps the latencies of the first 3 relays to
answer and stops; a relay that answers later is not considered that time.

The same two limits are in iroh 1.5.0 (checked 2026-11-23). When upgrading
iroh: vendor the new release unchanged in one commit, then re-apply these
changes if upstream has not fixed them, and run `cargo test` in
`rust/vendor/iroh` (`net_report`) and `rust/sf4-net`.
