# Vendored crates

Crates here replace their crates.io release through `[patch.crates-io]` in
`rust/sf4-net/Cargo.toml`. Each one is first committed exactly as published
(`.gitattributes` keeps this folder byte-for-byte), and Ember's changes follow
in later commits: `git log -- rust/vendor/<crate>` lists the vendoring commit
first, and `git diff <vendoring commit> -- rust/vendor/<crate>` shows only
Ember's changes. The helper's CMake rule, the room service image and
dependabot all know about this folder; a build that copies `rust/sf4-net`
alone fails to resolve the patch.

## iroh 1.1.0

Source: `iroh-1.1.0.crate` from crates.io, sha256
`329552fad4e46b5ccb4439d5635216f6289eec2c46f3fde4bc732d762694b9e5`. Used under
the MIT licence (the crate is MIT OR Apache-2.0). The crate ships only
`LICENSE-BSD3` (for the parts derived from Tailscale), so `LICENSE-MIT` and
`LICENSE-APACHE` were added from the v1.1.0 tag of n0-computer/iroh. Modified
by SF4 Ember Netplay, 2026-11-23 and 2026-11-25; every change is marked
`Ember patch` in `src/net_report.rs`.

### How iroh picks the home relay

Each net report (every 20 to 26 s) starts QUIC address discovery probes against
the relays, keeps the latencies of the first 3 to answer, and picks the lowest,
keeping the current relay unless another is under two thirds of its latency.
On a network that blocks UDP, HTTPS probes measure the relays instead. Every
5 minutes, or after a major network change, the report runs in full mode.

### What the patch changes

1. **Every relay gets a probe.** Upstream probed the first 5 relays of the
   map, which iterates in URL order. With Ember's six relays,
   `usw1-1.relay.n0.iroh.link` sorted last: for players whose UDP works it
   never got a latency and could never be the home relay, so West Coast
   players who could not connect directly were relayed through Kansas (a
   NorCal-Seattle pair measured 142-175 ms).
2. **A full report keeps the switching rule.** The full report clears the
   previous report to plan its probes, and upstream lost the current relay
   with it, so it picked without the two-thirds rule. Two relays a few
   milliseconds apart (n0's and Ember's relay in one region) swapped the home
   relay every 5 minutes. `Reports::chosen` carries the relay across;
   a report in which no relay answered keeps it too.
3. **The stored probe connection is the home relay's.** Upstream keeps the
   connection of the first relay to answer a probe. Its address is read again
   only when it changes (a NAT rebinding), and then a report held that relay
   alone, which became the home relay with no comparison. The connection is
   now closed unless its relay is the one the report chose
   (`QadConns::keep_only`).
4. **One report without the home relay does not leave it** (2026-11-25).
   A report holds only the first 3 answers, so the home relay can be missing
   from it while it works. Players' 1.6.4 logs showed exactly that on most
   full reports: the home relay went to whichever relay had answered and came
   back one or two reports later, 20 to 50 s, every 5 minutes. The relay is
   now kept through one report that has answers but none from it
   (`ChosenRelay::unmeasured`) and left on the second such report in a row; a
   report in which no relay answered is not counted. Change 3 makes the next
   report probe every relay again, since it closes the stored connection to
   another relay. That report is the same race for the first 3 answers, so a
   home relay that loses it twice running is still left: the switching is
   rarer, not impossible. Having the probe round wait for the home relay's
   own answer would measure it every time; that is a larger change to
   `spawn_qad_probes`, not made here.

A relay that stopped working is therefore held for one report, 20 to 26 s,
before the home relay moves.

Tests: `test_full_report_keeps_preferred_relay_hysteresis`,
`test_unmeasured_preferred_relay_is_left_on_the_second_report` and
`test_stored_qad_conn_only_to_preferred_relay` in `src/net_report.rs`, run by
the CTest test `IrohPatchRust`; `iroh_is_the_vendored_copy` in `sf4-net`
fails if Cargo.lock stops using this copy. By hand, with the target directory
outside the tree:

    cd rust/vendor/iroh
    CARGO_TARGET_DIR=<outside the checkout> cargo test --locked --features test-utils --lib net_report

Dependabot ignores iroh (a bump would leave this patch unused), so it raises
no iroh pull requests: check iroh's releases and security advisories by hand
when touching the helper's dependencies.

iroh 1.5.0 has the same `MAX_RELAYS = 5` and the same preferred-relay code
(read in its published crate on 2026-11-23). When upgrading iroh: vendor the
new release unchanged in one commit, re-apply these changes if upstream has
not made them, and run both test suites.
