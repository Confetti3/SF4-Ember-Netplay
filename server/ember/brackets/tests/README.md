# ember-brackets tests

`cargo test -p ember-brackets` runs everything against fakes on loopback
ports: `support/service.rs` is a bracket service answering the `http`
adapter's contract (ADAPTERS.md), keeping its sets, moving winners on and
logging each request, and `support/bridge.rs` a real bridge whose players
are found by their Discord accounts.

`bracket_site.rs` and `bracket_site_desk.rs` play the real bracket site
(`server/bracket-site`) instead of the fake; they need Node, so they run only
with `--ignored`:

```sh
cargo test -p ember-brackets --test bracket_site -- --ignored
cargo test -p ember-brackets --test bracket_site_desk -- --ignored
```
