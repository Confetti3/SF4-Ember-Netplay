# LAN rooms

LAN play adds local room discovery to Ember's existing private-room transport.
It does not replace the room authority, match admission, spectators, GGPO, or
input-delay policy. Related request: [#12](https://github.com/Confetti3/SF4-Ember-Netplay/issues/12).

## Using it

1. Connect the PCs to the same local network and run the same Ember build.
2. Open **Online → LAN play**. Choose **Host LAN room** on one PC and set its
   name, capacity and rules using the usual room setup.
3. On the other PCs, select that room from the LAN list. Discovery may take
   several seconds. Incompatible builds and full rooms are visible but disabled.
4. Play, spectate and leave using the existing room controls. A copied LAN
   invitation also works through **Join room** on the same network.

The room connection needs no Ember account, public-room service, invitation-link
service, Discord or internet relay. The supported Steam game and its normal
launch requirements still apply. This does not create a Wi-Fi network: connect
to the router, hotspot or ad-hoc network in Windows first.

Allow `sf4-net.exe` through the firewall on the trusted local network. Networks
with client isolation or blocked multicast can prevent discovery or connections.
The current discovery dependency uses the system's default multicast interface;
automatic selection of secondary/VPN/hotspot interfaces is not implemented.
Do not assume a particular hotspot or multi-adapter setup works until tested.

## Integration

- `ApplicationShell` uses the existing menu rows, controller navigation and
  create-room preferences. LAN is a per-room choice; visiting it does not change
  the saved public/private preference. No new room-action schema is introduced.
- `IrohRoom` sends `lan_watch`, `host` with an explicit room kind, and `lan_room`
  through the existing helper IPC. The overlay pulses browser visibility using
  the room board's existing pulse pattern. The runtime drives a single native
  subscription, renewed every five seconds with a 15-second helper lease.
  Browser generations reject stale replies; leaving the view stops its lease.
- One `Transport` value (`Online`, `RelayOnly`, `Lan`) selects the primary,
  coordination and latency builders. Async `begin` consumes the opening epoch
  and chooses/restores the endpoint once. Hosted/connected events publish the
  policy through `IrohRoom`, the runtime snapshot and the shell. The shell uses
  one LAN-create flag beside the saved public/private preference and hides short
  links in LAN rooms. Host and Join accept only `RoomKind::{Online, Lan}`;
  `RelayOnly` remains an endpoint policy, not a room kind.
  Invitation text is decoded at the helper boundary, not used as UI state.
- The helper creates a LAN endpoint from Iroh's `Minimal` preset, with relays and
  port mapping disabled. Primary, coordination and member-latency endpoints use
  local mDNS address lookup. Gameplay and probes use the same direct transport.
  The existing online endpoint is retained for ordinary online services and
  restored after LAN use. Therefore LAN does not require internet, but it is not
  a switch that disables every background internet service in the application.
- Discovery uses the pinned Iroh-maintained `iroh-mdns-address-lookup` adapter
  and the application service name `sf4e-lan-v1`. The room-list subscriber is
  separate from the address resolver: the adapter suppresses passive events
  while resolving a peer. Metadata includes name, exact build, member count,
  capacity and a changing heartbeat. The list holds at most 64 rooms and expires
  silent entries after 45 seconds. Explicit withdrawal sends a tombstone because
  clearing user data alone does not remove the adapter's existing TXT attribute.
- A small pinned `swarm-discovery` patch receives full UDP datagrams. Its former
  1,472-byte receive buffer returned WSAEMSGSIZE on Windows when unrelated mDNS
  traffic was larger, terminating discovery. See
  [`EMBER_PATCH.md`](../../rust/vendor/swarm-discovery/EMBER_PATCH.md) for provenance
  and removal conditions. No discovery wire format is changed by that patch.
- The writable technical leader advertises the native room snapshot's details.
  A successor can advertise after handoff; a follower or frozen room cannot serve
  new discovery invitations. Any LAN peer can request that capability while
  the room is advertised; discovery is not an access-control boundary. Native
  admission remains authoritative if the displayed capacity or name changes
  between browsing and joining.
- Selecting a room sends a typed LAN join request with the chosen endpoint,
  without changing the pasted invitation field. An authenticated
  QUIC connection using `sf4e/lan/1` requests its full invitation, with a five-second
  deadline and the existing invitation size bound. The returned endpoint and
  exact build must match. At most four invitation-serving tasks run in their own
  pool, outside the room's control/game/checkpoint task budget. Room capabilities
  are not included in multicast metadata.
- Full `sf4l1:` invitations carry the existing room capability, expiry, build and
  coordination authority without an internet relay. Online `sf4e*` and Discord
  formats remain unchanged. LAN invitations cannot be exported to Discord or
  the online short-link service. Invitation-renewal and handoff events still reach
  the native client, with an optional Discord secret (`null` for LAN).
  A discovered name is untrusted display text,
  not proof of the host's identity.

## Automated coverage

From `rust/sf4-net`, run the helper suite with its pinned Rust toolchain:

```powershell
cargo test --locked
```

`lan::tests` exercises real local discovery, invitation exchange, metadata
changes and withdrawal, as well as bounds, incompatible builds and expiry.
`service::lan::tests` covers browser lease expiry, transport restoration, stale
views, rejected attempts, cancellation and refusing to replace an active room.
It also checks that learning a LAN ticket's authority from its authenticated
control connection keeps coordination local. Saturated invitation-serving slots
do not consume the room task budget, and a LAN attempt rejects an online
invitation instead of silently changing policy.
The discovery test needs local multicast to be permitted.

`service::tests::membership_retirement` reproduces the rapid-rejoin race without
network timing. From that same directory, run the dependency's loopback
oversized-datagram regression with:

```powershell
cargo test --manifest-path ../vendor/swarm-discovery/Cargo.toml --lib oversized_datagram_does_not_stop_discovery
```

`LanJourney` uses the existing headless ImGui controller harness to open the LAN
browser, join a discovered room and create a LAN room with a saved public-room
preference. It checks incompatible/full rows, returning to ordinary Create,
hiding LAN short links, and rejecting malformed metadata with a JSON exception
that the existing helper event handler catches.
The existing localization and shell journey suites cover the shared UI.

From the repository root, enable the opt-in native/helper tests in a normal
Windows build:

```powershell
cmake -S . -B build/lan -DSF4E_LAN_TESTS=ON
cmake --build build/lan --target LanJourneyTest IrohRoomIntegrationTest IrohGameIntegrationTest IrohRecoveryIntegrationTest
ctest --test-dir build/lan -R "^(LanJourney|IrohLan.*)$" --output-on-failure
```

Use the repository's normal build prerequisites and preset/toolchain settings;
`build/lan` above is only an example output directory. The tests reuse existing
fixtures and process/UDP locks:

| Test | Coverage |
| --- | --- |
| IrohLanRoom | Discovery, rejected native build, matching invitations, metadata updates, three room lifecycles, 90 Ready/Unready cycles, rules and leaving |
| IrohLanGame | Direct routes, three gameplay generations, 30 exact 1024-byte round trips, packet limits and control-loss independence |
| IrohLanRejoin | Committed, uncommitted and unconfirmed departure followed by rejoining |
| IrohLanHandoff | Two-member leader departure, usable successor, discovery moving to that successor |
| IrohLanRecovery | Three-member leader loss during a synthetic GGPO session, retained grants/ports and continuing frames |

These are local process and synthetic-game tests. They do not establish real
USFIV gameplay correctness, Wi-Fi quality, or operation across two physical PCs.

## Physical-network acceptance

Before promoting the feature in a release, test identical builds on two or more
PCs: wired LAN and the intended Wi-Fi/hotspot/ad-hoc setup, including internet
disconnected while the game runs under its supported Steam setup. Check room
discovery, full/wrong-build rooms, character selection, matches, spectators,
leaving/rejoining and host handoff. Test firewall denial and recovery, clean host
exit, cable/Wi-Fi loss, multiple adapters and the normal online room path after
leaving LAN. Record the topology, adapter used, build, helper routes and both
players' logs. Do not equate this fixture coverage with those hardware results.
