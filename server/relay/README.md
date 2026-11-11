# Ember relay

`relay.embernetplay.link` runs n0's own relay server, `iroh-relay` 1.1.0 (the
same release as the helper's `iroh`), on the VPS. The helper uses it in place
of n0's two US relays (`rust/sf4-net/src/invite.rs`, `RELAYS` and `ACTIVE`).
Since 1.5.7 Europe has an Ember relay as well, `relay-eu.embernetplay.link`,
run by a community host (see "A relay hosted by someone else" below). Asia
is still on n0's relay.

## Why

Since 2026-11-08 the router filters of some US internet providers (Xfinity
xFi Advanced Security, Spectrum Security Shield, Cox Panoramic) answer TLS
connections to `*.relay.n0.iroh.link` with a block page. A player behind one
cannot reach a host whose home relay is blocked, and invitations and public
room tickets carry only the host's relay, so the join fails. iroh picks a home
relay by latency alone, so the blocked relays are taken out of the helper's
map rather than ranked below ours.

## How it runs

- nginx terminates HTTPS on 443 with the `*.embernetplay.link` wildcard
  certificate and proxies to the relay's plain HTTP port, 127.0.0.1:47895
  (WebSocket upgrade at `/relay`, `/ping` for latency probes).
- Port 80 answers `/generate_204` from the relay: iroh's captive-portal check
  is plain HTTP, and a redirect there would flag every player as behind a
  captive portal.
- QUIC address discovery answers on UDP 7842 directly, with the same
  certificate passed in by systemd (`LoadCredential`). Both ufw and the IONOS
  Cloud Panel firewall must allow UDP 7842.
- nginx caps relay WebSockets at 1800 in all (each holds two of the worker
  connections the other sites share), about 600 players at three endpoints
  each, and at 600 per address: the public room hosts on the VPS and the room
  box each share one address, as do households and carrier NATs. A refused
  `/relay` does not move a player to another relay (iroh keeps preferring one
  whose probes answer), so raise `worker_connections` and the cap together
  before it is reached.
- The relay logs warnings only; nginx keeps no access log.

## Deploy

Build the binary on server1 (`cargo install --locked iroh-relay --version
=1.1.0 --features server`), stage this `deploy` folder as `~/ember-relay` on
the VPS with the binary at `bin/iroh-relay` and `sha256sum iroh-relay >
bin/iroh-relay.sha256`, then:

```bash
sudo bash ~/ember-relay/setup.sh
```

It installs the binary, config, unit and nginx site, opens UDP 7842 in ufw,
restarts the relay and checks it locally (ping, HTTPS, a complete relay
WebSocket upgrade through nginx, the captive-portal answer, the QUIC port).
Any failure from the first change on puts the previous install back and
returns the service to its earlier state.

From a client, `cargo test --lib live_relays -- --ignored --nocapture` in
`rust/sf4-net` checks every relay the helper uses end to end: home relay,
QUIC address discovery, no captive portal, and a relay-only connection.

The wildcard certificate is renewed by hand (expires 2027-03-30); restart
`ember-relay` after a renewal so QUIC picks up the new one.

## A relay hosted by someone else

`community/` is a kit for a relay in another region on a host's own machine,
behind their Nginx Proxy Manager: one container, the proxy's own certificate,
and `check.sh` to check it from outside. Its README is the host's guide.
`relay-eu.embernetplay.link` runs from it.

A hosted relay sees its players' addresses, so the host is someone trusted
with that. To bring one into the builds:

1. Give it a name in Ember's domain, such as `relay-eu.embernetplay.link`,
   as a CNAME to the host's proxy (IONOS panel, DNS tab). The builds pin the
   name, not the host: if the host stops, the name moves to another relay
   and no player needs a new build. The host's proxy gets its own
   certificate for the name; the wildcard key never leaves the VPS.
2. The host sets the relay up and `check.sh <name>` passes from outside
   their network.
3. Run the live test against it alone, before any build knows it:

   ```bash
   EMBER_LIVE_RELAY=https://relay-eu.embernetplay.link./ cargo test --lib live_relays -- --ignored --nocapture
   ```

4. Append the URL and its region code to `RELAYS` in
   `rust/sf4-net/src/invite.rs` (append only) and put its index in `ACTIVE`
   in place of the n0 relay for that region. Invitations are bound to the
   build, so this ships like 1.5.5 did: a client release and room hosts from
   the same source. Run the live test again without `EMBER_LIVE_RELAY`.

If a hosted relay stops answering its probes (HTTPS `/ping` and QUIC),
helpers near it pick the next nearest relay that does; nothing else has to
change until the name is repointed. A relay whose probes answer while its
`/relay` WebSocket does not keeps being chosen (iroh 1.1.0 selects by probe
latency alone), and players near it cannot host or join: `community/check.sh
<name>` from the VPS or any Linux machine shows that state, and the host's
guide says to take such a relay fully down.
