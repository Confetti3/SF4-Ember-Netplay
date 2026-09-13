# SF4 Netplay Launcher — Iroh + ImGui Migration Plan

**Planning date:** September 7, 2026
**Repository reviewed:** `Confetti3/SF4-Netplay-Launcher`
**Baseline:** `main`, commit `53750e69069aa1a45dc6da09ad8b7fa8000497cb`
**Status:** Implementation proposal based on source inspection and current Iroh documentation. No repository changes, Windows builds, or live-network tests were performed for this plan.

## 1. Target outcome

Replace the developer-operated broker/relay dependency for ordinary friend-to-friend play with Iroh, and remove the Qt launcher workflow. Normal users should open the launcher, reach the existing in-game ImGui interface, choose Host or Join, share/paste an invitation, and play/rematch without entering an IP address, port, relay address, or transport mode.

“Everything in ImGui” means the existing game overlay becomes the primary application interface, not merely that Qt is replaced with a second independent launcher UI. Preserve a small ImGui bootstrap/recovery surface for situations where the game cannot yet render: missing installation, launch/injection failure, and update installation. Share UI components with the overlay; do not create another product-sized UI.

Do not rewrite the SF4 hooks, memento rollback system, deterministic simulation, input capture, audio rollback, or GGPO itself as part of the transport migration.

### Non-negotiable product constraints

- No developer-operated VPS required for the default private-room flow.
- No separate networking installation, account, VPN, driver, or Rust toolchain required from players.
- No manual port forwarding or IP/port configuration in the ordinary interface.
- No extra console/helper window and no transport-selection wizard.
- Networking remains asynchronous relative to UI/game execution, with bounded queues and explicit ownership.
- Existing rollback and lifecycle invariants remain the behavioral baseline.

“No VPS of our own” is not “no external infrastructure.” Iroh's default connectivity uses third-party relay/address-lookup infrastructure. Its public relays are documented for development/hobby use, are rate-limited, provide no uptime guarantee, and officially support only the latest stable Iroh release. Use them as the initial friends-only deployment, not as an unlimited production guarantee. Managed relays are the future infrastructure option that does not require operating a VPS. Do not put a private management API key in a distributed client. [I1][I2]

## 2. Existing integration points

| Current component | Planned treatment |
|---|---|
| `src/launcher/qt/*` | Replace normal setup UI with shared ImGui views, then remove Qt code and deployment artifacts. |
| `src/launcher/netplay/netplay_launch_controller.*` | Extract reusable settings/preflight/update operations; stop treating a completed launcher wizard as the only way to start a session. |
| `src/launcher/connect_strategy.*`, `room_broker_client.*`, `upnp_portmap.*` | Retain temporarily for developer-only A/B tests; remove from the default Iroh path and eventually the release package. |
| `src/common/sf4e__NetplayConfig.hxx` | Version the bootstrap/configuration contract; remove fixed-IP/broker assumptions from the new contract. |
| `src/session/sf4e__SessionClient.*`, `sf4e__SessionServer.*` | Keep lobby logic and game-facing behavior; replace GameNetworkingSockets-specific connections and delivery metadata behind a transport-neutral interface. |
| `src/session/sf4e__SessionProtocol.*` | Preserve existing application message semantics and compatibility checks; add a versioned envelope and explicit application message IDs. |
| `src/session/sf4e__GgpoTransport.*` | Add the Iroh transport preparation path; stop application-owned NAT punching and VPS registration for that path. |
| `src/session/sf4e__GgpoRelay.*` | Reuse the loopback virtual-peer pattern, replacing the GNS/JSON tunnel with raw binary Iroh datagrams. |
| `src/sf4e/sf4e__NetplayFacade.*` | Convert payload-driven startup into commands that can host/join/leave repeatedly after the game has started. |
| `src/sf4e/sf4e__Overlay.*`, `sf4e__OverlayPrefs.*` | Integrate the application shell, preserve match controls, and migrate preferences. |
| `CMakeLists.txt`, `CMakePresets.json`, release/preflight scripts | Add a pinned Rust helper build and tests; later remove Qt/GNS/broker packaging dependencies after auditing remaining consumers. |

The current build is x86. The existing relay creates loopback UDP peers and maps their ports to session connection IDs. Those are important migration boundaries: preserve the game architecture and adapt packet movement around it. [R2][R3][R4][R5]

## 3. Recommended process architecture

Start with a bundled **Rust networking helper**, provisionally named `sf4-net.exe`, rather than embedding a Rust runtime in the injected game DLL.

```text
Launcher.exe — minimal bootstrap and hidden process supervisor
  Starts/validates the game, injects Sidecar.dll, supervises helper lifetime

USF4.exe + Sidecar.dll — existing x86 game integration
  ImGui application shell
  Game-thread session controller and existing lobby/game logic
  Existing GGPO and rollback implementation
       | authenticated local control IPC
       | loopback UDP for GGPO packets
       v
sf4-net.exe — hidden local Rust process
  One Iroh Endpoint per running application instance
  Reliable room/control connections
  Separate gameplay connections carrying QUIC datagrams
  Direct-path/relay management, packet bridge, connection statistics
       |
       v
Opponent's Iroh helper — direct where available; Iroh relay otherwise
```

This is a local shipped executable, not a hosted server. It must launch and terminate automatically with the application and never require a player to manage it.

Iroh's documented C binding platform matrix lists Windows x86_64, not x86, and requires building bindings from source. That does not prove an x86 Rust build is impossible, but it makes in-process binding support an unnecessary initial dependency. Use the native Rust API in an x64 helper and leave Sidecar/GGPO x86. This establishes **64-bit Windows as the initial package requirement**; explicitly document that compatibility change. A proven x86 embedding can be considered later, without changing the transport contract. [I3]

### Ownership and local security

The bootstrap remains alive invisibly as supervisor, using explicit process handles/lifetime management. The helper must not be killed merely because the bootstrap's temporary UI closes. Normal shutdown closes sessions, stops packet forwarding, shuts down the endpoint, and then terminates the helper. Helper crash becomes a visible application event; do not silently resume an interrupted deterministic fight.

Use a per-run, current-user-restricted named pipe for commands/events. Bind local UDP forwarding sockets only to loopback and register exact source/destination mappings over authenticated IPC. Use a per-run nonce passed through a controlled bootstrap mechanism, not a public CLI secret. Loopback alone is not authentication. Never implement an arbitrary remote-to-local UDP forwarder.

Network workers publish events and statistics; they never mutate game memory or call GGPO simulation callbacks. UI actions enqueue typed commands applied on the appropriate game thread. Give IPC events, datagrams, and logs explicit memory/size limits. No synchronous pipe reads, DNS calls, HTTP calls, or network joins in drawing or simulation callbacks.

## 4. Application/core separation

Add small focused modules rather than expanding `sf4e__Overlay.cxx`:

```text
src/netplay/             Session controller, commands, states, settings migration
src/ui/                 Shared ImGui shell and screen components
src/platform/           Bootstrap IPC/process integration
rust/sf4-net/            Iroh endpoint and local GGPO packet bridge
src/tests/              C++ transport/core/UI-model tests
rust/sf4-net/tests/      Rust protocol, framing, bridge, and lifecycle tests
```

Names above are proposed, not existing repository paths.

Expose a narrow command surface such as `HostRoom`, `JoinInvite`, `LeaveRoom`, `SetLobbySettings`, `Ready`, `Rematch`, `StartOffline`, and `RequestUpdate`. Keep room state, match state, connection health, and UI page separate: a room/control failure must be representable while a healthy gameplay connection continues.

Publish read-only view snapshots to the renderer. Establish explicit event ordering and cancellation. Delayed callbacks from a previous room or match must be rejected by their generation/instance identity rather than applied to whichever session currently exists.

## 5. Iroh transport contract

Use the current verified Iroh **1.1.0** release as the initial evaluated baseline, with a pinned toolchain and committed Cargo lockfile. Revalidate dependency/security updates at implementation time. The September 1 release includes security fixes; do not start from an old `iroh-net` example or assume old FFI code reflects the current API. [I4]

### Reliable control traffic

Carry lobby membership, readiness, settings, character/stage confirmation, RNG/prebattle synchronization, results, rematch negotiation, and existing desync messages over framed reliable QUIC streams.

Retain current SessionProtocol message names/fields in the first port where possible. Add a versioned application envelope with bounded message length. A byte stream does not preserve the message boundaries supplied by GNS: implement exact framing, partial-read handling, multiple-messages-per-read handling, and maximum-length rejection.

Replace `HSteamNetConnection`, `SteamNetworkingIPAddr`, `EResult`, GNS callback plumbing, and GNS-generated delivery/message IDs with internal types at the transport boundary. Generate monotonic application message IDs explicitly; preserve how readiness requests identify a match. Do not accidentally use “number of bytes written” as the replacement for message identity. [R6]

Keep an authoritative room owner on the hosting player's machine. Reuse existing C++ SessionServer behavior behind the new transport; the host's local client can use an in-process adapter. The helper should not contain a second Rust implementation of SF4 lobby rules. Host departure ends the room initially; host migration is separate work.

### Time-sensitive gameplay traffic

Use Iroh `send_datagram` / `read_datagram` for raw GGPO packet payloads. Do not put them in JSON/base64, a reliable QUIC stream, gossip, or the control-message IPC queue. Iroh datagrams are unreliable and unordered, which preserves the relevant UDP-like transport semantics rather than adding stream-level retransmission. [I5]

Adapt the existing virtual-peer bridge:

1. The helper binds a distinct ephemeral loopback socket for each remote GGPO peer.
2. GGPO is configured with that local virtual endpoint.
3. The helper receives the raw UDP packet and sends a versioned gameplay envelope through Iroh.
4. The remote helper validates the peer/session/match association and forwards the original packet to local GGPO from the registered virtual socket.

Prefer event-driven forwarding in the helper; do not retain a network bridge that waits for one ImGui draw or one game frame before forwarding. This preserves the loopback pattern but is not a line-for-line copy of the current `Pump()` behavior. Existing packet validation still runs before GGPO dispatch. [R4]

Use one endpoint but separate room/control and gameplay QUIC connections per relevant peer. Give them distinct application protocols, for example `sf4e/control/1` and `sf4e/game/1`. The purpose is lifecycle isolation: resetting a room channel must not implicitly close the gameplay connection. Complete endpoint/helper/network failure still affects both. Iroh recommends a single endpoint per application and supports independent connections through it. [I6]

### Packet, queue, and MTU rules

Negotiate protocol version, supported datagrams, maximum packet size, expected Sidecar/build compatibility, endpoint identity, room ID, player role, and match generation before admitting gameplay traffic. The host validates the invite capability before granting membership.

Use an explicitly serialized, bounded binary envelope for gameplay; derive peer identity from the authenticated connection, not an untrusted claimed sender field. Tag packets with a fresh room instance and match generation. Keep the room endpoint alive across rematches, but close/drain old GGPO sockets and use fresh virtual mappings so old local packets cannot be relabeled as belonging to the new match.

Measure actual maximum GGPO packet sizes, including spectators and large input backlogs. Check `max_datagram_size()` and subtract envelope overhead; the limit can change with the path. Never truncate packets. First prefer proving that the complete packet fits; if fragmentation is necessary, specify bounded, size-limited, generation-aware reassembly with short expiry and test it explicitly. Do not silently fall back to reliable streams. [I5]

Bound all send queues and record drops. Avoid `send_datagram_wait` in the real-time path: it prioritizes older queued datagrams during congestion. Preserve GGPO's retransmission/input redundancy rather than adding another reliable queue. Transport failure aborts or degrades according to explicit state rules; never allow one peer to independently switch to a legacy protocol. [I5][R1]

Iroh manages direct/relay path selection. Keep the same peer and match identity across path changes. Surface path changes in diagnostics, but do not rebuild GGPO simply because relay becomes direct. Relayed traffic can travel over a WebSocket/TCP path, so a usable connection is not a promise of direct-UDP-like jitter: test gameplay under loss on the relayed path. [I7]

## 6. Brokerless invitations and discovery

### Default private-room flow

Replace the broker-resolved short `SF4-XXXX` code with a self-contained, versioned invitation, presented as **Copy invite** and **Paste invite**. The invitation contains the host endpoint ID, routing/relay information, a random room instance, a strong random join capability, protocol version, and host-enforced expiry policy. Never serialize the host's private endpoint key.

Use Iroh's ticket/address serialization as a building block for an application-specific invite. Tickets package dialing information; authentication/authorization to a particular SF4 room remains the application's responsibility. An endpoint ID is not a room password. [I8]

The token will be longer than the existing four-character code. That is an explicit UX tradeoff for removing the room-code database. Support clipboard paste, optional invite files, and optionally an application link handler; the basic flow must work without installing a URL handler. Include no unnecessary local/private addresses in shared tickets. Redact capabilities and tickets from normal logs and exported diagnostics.

Iroh's address lookup resolves endpoint IDs to network addresses; it is not an SF4 room directory or a short-code database. Do not derive an endpoint private key from a short room code or claim a hash of a public topic removes the need to find peers. [I9]

### Existing experimental discovery features

Track **Open rooms**, **Find match**, and spectators explicitly in the parity checklist. Do not leave dead buttons and call the migration complete.

Private-room play is the first shippable milestone. Public discovery is a later workstream: evaluate `iroh-gossip` for signed, expiring public room advertisements, with a verified bootstrap/rendezvous mechanism. Iroh's guide documents the need for bootstrap peers and references a rendezvous service, but this review did not establish a concrete public-service API, limits, or service guarantee suitable for this launcher. Make that a discovery spike, not an assumed implementation detail. [I10]

For a public directory, separate public advertisements from private capabilities, enforce TTLs and sequence numbers, limit advertisements per endpoint, probe a selected room before admitting a player, and make matching an explicit reservation/accept exchange. Never use gossip for GGPO inputs. Cache peers only as an optimization; cached peers cannot guarantee cold-start discovery when everyone is offline.

Spectator support must carry the existing GGPO spectator flow, with per-spectator bandwidth/queue isolation and admission limits. No new free-form video/state streaming protocol. A slow or disconnecting spectator must not stall the players or turn a diagnostic hash mismatch into match termination.

## 7. Unified ImGui experience

| Screen | Required behavior |
|---|---|
| Home | Host, Join, Offline, profile, settings, version/update status. Networking failure must not prevent Offline. |
| Host / Join | Create room, copy/paste invite, validate, cancel, retry, explain incompatible/expired/full rooms. No IP/port prompts. |
| Lobby | Names/roles, character/stage/settings, input delay, readiness, direct/relay status, connection quality, leave, spectator controls when supported. |
| Match | Compact existing HUD with GGPO RTT/rollback information and nonintrusive connection warnings. No forced modal on harmless path changes. |
| Post-match | Persistent room, clear result/rematch/leave controls, preserved selections. |
| Settings | Profile, controls, graphics/netplay preferences and supported diagnostics, replacing the split launcher/overlay settings experience. |
| Diagnostics / About | Bounded readable logs, redacted export, version/build details, dependency attribution, updater status. |
| Bootstrap / recovery | Only needed before/without a functioning game overlay: installation location, validation, launch failure, update installation. |

Refactor overlay initialization so the application Home screen is available in Idle mode. The current façade configures overlay behavior from whether a launcher payload is active; changing only the widgets would leave runtime host/join missing. [R7][R8]

The normal flow becomes:

`Open launcher → bootstrap game → ImGui Home → Host/Join → Lobby → Fight → Rematch`

Repeated host/join/leave should not require restarting the game. Replace repeated calls to one-shot initialization with explicit begin/end-session operations, safe cancellation, and generation-scoped resources. Separate bootstrap configuration from mutable session configuration. Migrate old `config.json` and `overlay_prefs.json` atomically into a versioned settings model, retaining backups and existing selections. [R9]

Input handling must be tested beyond visual rendering: keyboard, mouse, controller navigation, focus loss, Alt-Tab, text input, ultrawide/DPI changes, and Direct3D9 device loss/reset. Text entry/navigation in the overlay must not leak into the underlying game; opening a diagnostics page must not accidentally pause deterministic netplay. Apply gameplay mutations only at safe lifecycle boundaries. Keep developer/debug views gated rather than exposing them as normal player settings.

Update checks/downloads may be requested from ImGui, but installation must happen only after game/helper binaries are unloaded. Preserve the existing update source and strengthen package integrity checks rather than inventing a P2P updater. The restart/install handoff should be explicit in the same application UI.

## 8. Implementation milestones

### M0 — Freeze baseline and record parity

Build the exact baseline package. Run existing `GgpoUdpValidation`, `RollbackDiagnostics`, `GgpoGate`, `PacingController`, `StateHash`, and `SaveStateOwnership` tests, plus SessionInteractiveTest/manual two-player coverage. Record private-room join, fight, rematch, leave/rejoin, spectator behavior, offline operation, settings persistence, and current bugs. Capture real GGPO packet sizes and direct/relay latency distributions.

**Exit:** A reproducible baseline and feature/invariant checklist, not a README-only assumption about working behavior.

### M1 — Extract core and transport boundaries

Introduce the session commands/states and transport-neutral connection/message types. Keep existing networking operational through a legacy adapter. Extract the reusable settings/preflight/update services from the launch controller. Add game-independent state-machine tests and a mock transport.

**Exit:** Existing Qt/overlay behavior still works, and core host/join/ready/leave transitions are testable without Qt or a live game.

### M2 — Prove the packaged Iroh helper

Build the x64 helper with pinned Rust/Iroh dependencies. Implement supervised startup, authenticated IPC, endpoint lifecycle, reliable framing, raw datagrams, loopback mapping, cancellation, and statistics. Test two helpers on actual different networks; force relay-only as well as direct paths. Disable automatic port mapping in at least the no-router-configuration test configuration to demonstrate it is not a dependency.

**Exit:** The packaged helper works without Rust installed, has no visible extra window, and moves a representative GGPO packet workload without truncation, unbounded queues, or a frame-sized forwarding delay.

### M3 — Private-room vertical slice

Implement ticket parsing/creation, capability admission, host-owned room control, version/hash rejection, prebattle setup, gameplay authorization, and the GGPO bridge. Keep the existing UI as a temporary caller where useful. Retain endpoint lifetime across rematches while renewing match generation and virtual socket mappings.

**Exit:** Two PCs create/join a private room, complete a real fight, rematch, and leave/rejoin with the old VPS unreachable. Direct and forced-relay cases both pass. No one-sided fallback is possible.

### M4 — Move the complete normal flow into ImGui

Add the shared shell and screens; enable them in Idle mode. Switch launcher default to minimal bootstrap. Add runtime session commands, cancellation, error recovery, controller navigation, settings migration, and updater/recovery presentation. Keep all network work out of rendering/simulation callbacks.

**Exit:** A normal player needs only the game ImGui interface from Host/Join through repeated matches. Missing-game and failed-launch paths still have usable ImGui recovery.

### M5 — Close feature-parity gaps

Port and test spectator behavior and any retained advanced/developer facilities. Resolve Open rooms/Find match discovery through the explicit bootstrap/service spike. The first private-room release may label these features unavailable, but full parity is not complete until they are implemented or a deliberate scope change is approved.

**Exit:** Every old user-facing feature is either operational and tested in ImGui or recorded as an explicit release-scope limitation; no hidden dependency on the old broker remains.

### M6 — Harden, package, and retire legacy paths

Run the test matrix below. Add Cargo/CMake integration to CI, package all required helper runtime dependencies, update version manifests, signing/integrity checks, crash/log paths, preflight scripts, and dependency notices. Upgrade launcher/Sidecar/helper as one coherent package. Verify unsupported OS/architecture and mismatched binaries fail with useful messages.

Remove Qt deployment, the GNS network implementation, RelayHost/UPnP/broker code from the release only after auditing all remaining consumers. Keep the old commit/release for developer comparison rather than making legacy VPS fallback a hidden production dependency. Archive obsolete VPS docs, replace user instructions, and verify the final ZIP on clean machines.

**Exit:** The release starts offline and online without Qt, GNS session infrastructure, the old VPS, port-forward instructions, or a separately installed networking product.

M1 establishes the contracts. M2/M3 networking and M4 UI work can then proceed as separate workstreams, integrating against the same typed session controller. Do not combine a transport rewrite, rollback rewrite, and unrelated graphics rewrite in one change.

## 9. Verification and release gates

### Network and lifecycle matrix

Test same LAN; separate home NATs; double NAT/CGNAT; IPv4/IPv6 combinations; direct path unavailable; outbound UDP blocked with relay reachable; relay-to-direct and direct-to-relay transitions; initial relay failure; relay interruption after a direct fight starts; Wi-Fi/interface changes; and total connectivity loss.

Test host/join cancellation at each stage, incompatible builds, expired/reused invites, room full, rapid Ready toggles, quit during loading, disconnect during a fight, control-only failure with healthy gameplay, helper crash, and both players leaving/rejoining. An actual dropped gameplay connection must return to a safe state rather than pretending a new connection resumes the same deterministic fight.

Run at least 50 consecutive rematches in one room and repeated create/join/leave cycles. These are proposed acceptance targets, not tests already completed.

### Impairment and performance

Compare old direct UDP and Iroh under matched paths using a test grid such as 20/60/120 ms RTT, 0/1/3/5% packet loss, jitter, and reordering. Report player-relevant GGPO RTT/stalls/rollback separately from helper overhead and Iroh path statistics. Measure p50/p95/p99 local bridge delay, queue age/drops, CPU, memory, and game-frame time.

Set acceptance budgets after baseline measurement; an initial engineering target is no extra frame of packet buffering and sub-millisecond typical local forwarding. This is a target to verify, not a promised result. Do not change input delay, prediction policy, or the documented 1500/3000 ms notify/disconnect defaults merely to conceal transport stalls.

### Security and robustness

Fuzz invite parsing, control framing, gameplay envelopes, and size boundaries. Test malicious lengths, malformed packets, endpoint/role spoofing, stale match generations, unauthorized gameplay connections, invitation brute-force resistance, arbitrary relay URLs, local IPC impersonation, and shutdown races. Accept only policy-approved relay schemes/hosts where invitations introduce routing data; never treat untrusted ticket data as arbitrary file/network commands.

Verify no room secrets or private endpoint keys appear in logs, telemetry, crash exports, command lines, or public advertisements. A peer's encrypted connection is not proof that its packets are well-formed or its client is honest. Build hashes remain compatibility checks, not anti-cheat attestation.

### Invariants to retain

Keep the existing undetoured rollback advance callback, preallocated savestate ownership, game-main-thread save/load/free, post-VsBattle GGPO creation, nonblocking `ggpo_idle(session, 0)`, bounded pacing corrections, warning-versus-fatal distinction, enabled desync diagnostics, and spectator-safe mismatch behavior. Preserve healthy-fight degradation on room/control loss; complete gameplay failure follows safe teardown. No transport refactor should change these semantics. [R1]

## 10. Recommended first implementation change

Start with **M1 plus a game-independent helper/packet-bridge test harness**. Prove the transport seam, local process boundary, and datagram size/latency behavior before removing Qt or old network code. The first end-to-end acceptance milestone is not “Iroh connected”; it is **two real SF4 clients fighting and rematching through Iroh with the old VPS blocked**.

## Sources

Repository references use the inspected commit where practical; implementation should compare any newer/local changes before applying this plan.

- [R1] Netplay invariants: https://github.com/Confetti3/SF4-Netplay-Launcher/blob/53750e69069aa1a45dc6da09ad8b7fa8000497cb/docs/NETPLAY_INVARIANTS.md
- [R2] Build targets/dependencies: https://github.com/Confetti3/SF4-Netplay-Launcher/blob/53750e69069aa1a45dc6da09ad8b7fa8000497cb/CMakeLists.txt
- [R3] x86 presets: https://github.com/Confetti3/SF4-Netplay-Launcher/blob/53750e69069aa1a45dc6da09ad8b7fa8000497cb/CMakePresets.json
- [R4] Existing loopback bridge: https://github.com/Confetti3/SF4-Netplay-Launcher/blob/53750e69069aa1a45dc6da09ad8b7fa8000497cb/src/session/sf4e__GgpoRelay.cxx
- [R5] Existing transport interface: https://github.com/Confetti3/SF4-Netplay-Launcher/blob/53750e69069aa1a45dc6da09ad8b7fa8000497cb/src/session/sf4e__GgpoTransport.hxx
- [R6] Session client interface: https://github.com/Confetti3/SF4-Netplay-Launcher/blob/53750e69069aa1a45dc6da09ad8b7fa8000497cb/src/session/sf4e__SessionClient.hxx
- [R7] Netplay lifecycle facade: https://github.com/Confetti3/SF4-Netplay-Launcher/blob/53750e69069aa1a45dc6da09ad8b7fa8000497cb/src/sf4e/sf4e__NetplayFacade.cxx
- [R8] Overlay interface: https://github.com/Confetti3/SF4-Netplay-Launcher/blob/53750e69069aa1a45dc6da09ad8b7fa8000497cb/src/sf4e/sf4e__Overlay.hxx
- [R9] Current application behavior/settings: https://github.com/Confetti3/SF4-Netplay-Launcher/blob/53750e69069aa1a45dc6da09ad8b7fa8000497cb/README.md
- [I1] Public relay policy: https://docs.iroh.computer/iroh-services/relays/public
- [I2] Relay architecture/deployment: https://docs.iroh.computer/concepts/relays
- [I3] C bindings/platforms: https://docs.iroh.computer/languages/c
- [I4] Iroh 1.1.0 release: https://www.iroh.computer/blog/iroh-1-1-0
- [I5] Connection/datagram API: https://docs.rs/iroh/1.1.0/iroh/endpoint/struct.Connection.html
- [I6] Endpoint model: https://docs.iroh.computer/concepts/endpoints
- [I7] Network behavior and relayed paths: https://docs.iroh.computer/configuring-networks
- [I8] Tickets: https://docs.iroh.computer/concepts/tickets
- [I9] Address lookup: https://docs.iroh.computer/concepts/address-lookup
- [I10] Gossip and bootstrap requirements: https://docs.iroh.computer/connecting/gossip
