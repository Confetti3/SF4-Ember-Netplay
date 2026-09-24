# Wine and DXVK check (v0.9.7)

Players on Linux (Proton or Wine) report jerky matches and rooms that break after one or two games. This check ran the published v0.9.7 package (SHA-256 `934bdfb4…1e28da5`, matching the release checksum) under Wine with DXVK. It separates what works under Wine from what still needs a real Linux play session.

It is not SF4 gameplay evidence. The container had no Ultra Street Fighter IV, no GPU, no display with a real vblank and no direct route to the public Iroh relays. Everything below ran without the game.

## Environment

- Ubuntu 24.04 (4 vCPU VM), WineHQ `wine-stable` 11.0 in a 64-bit prefix. The 32-bit game side runs in WoW64.
- DXVK 2.7.1 (`d3d8`, `d3d9`, `d3d10core`, `d3d11` and `dxgi` set to native).
- Mesa llvmpipe (Vulkan 1.4) on Xvfb.
- No `winetricks` runtimes: Wine's builtin `msvcp140`/`vcruntime140` were enough for every Ember binary.

`scripts/wine-dxvk/run.sh <package> <dxvk> <work>` repeats the probe, launcher and injection steps. The Rust results come from cross-building `rust/sf4-net` for `x86_64-pc-windows-gnu` and running the test executables with `wine`. Linking needs `lld`, and the tests must be built with `--release`, because the debug `iroh-relay` cdylib exceeds the PE limit of 65535 exports.

## What works under Wine and DXVK

| Area | Result |
| --- | --- |
| Launcher UI | `Launcher.exe` starts. The launch-recovery screen renders through DXVK's D3D9 with the ImGui backend that the in-game overlay also uses. |
| Game-folder picker | "Choose game folder" opens Wine's folder dialog. Plain Wine has no Steam registry key, so this is the expected first-run path. |
| Injection | With a stand-in 32-bit `SSFIV.exe`, `DetourCreateProcessWithDllsW`, `DetourCopyPayloadToProcess` and Sidecar's `DllMain` all succeed. The sidecar log shows `payload received version=9` and `Sidecar install committed`. The stand-in has no-op code at every hooked offset (up to `+0x6a9268`), so all detours attach. |
| Helper processes | `sf4-net.exe` (x64) and `ember-discord.exe` start beside the game. `GetBinaryTypeW`, the handle-list attribute, suspended creation and job assignment all work. The job object closes both helpers when the game exits. |
| Helper unit tests | 77 of 77 pass under Wine in 31 s. They include real QUIC and UDP on loopback: `fifty_match_generations_reuse_endpoints_and_close_each_mapping`, `raw_udp_survives_control_close_and_filters_stale_remote_and_local_packets`, `closing_room_does_not_poison_new_room_on_same_endpoint`, `actor_admits_full_sixteen_member_room_and_fifteen_game_links`, the Raft coordination suite and the named-pipe authentication tests. |
| Helper process test | Two real helpers start, authenticate over their named pipes (server-PID check included) and report status. `Host` then fails with `host_unavailable`, because this sandbox returns 503 for direct HTTPS to `use1-1.relay.n0.iroh.link`. That is a sandbox network limit, not a Wine result. |
| Port hand-off to GGPO | Reserve `127.0.0.1:0`, connect the bridge, release the port, then GGPO's `SO_REUSEADDR` bind to `INADDR_ANY` and deliver a packet: 500 of 500 succeed. |
| Closed GGPO socket | A connected bridge socket reports `WSAECONNREFUSED` (10061) under Wine where Windows reports `WSAECONNRESET`. `bridge.rs` `local_unreachable` accepts both, so this does not end a game. An unconnected GGPO socket gets no ICMP error under Wine. |
| Timers | QPC runs at 10 MHz. `Sleep(1)` takes 1.09 ms and `WaitForSingleObject(event, 2)` takes 2.17 ms (p99 below 2.5 ms), with or without `timeBeginPeriod(1)`. That is finer than the Windows default. |
| Frame limiter | An emulation of the game's FIXED limiter (spin until one period after the previous exit) holds 16.667 ms at p50 and p95 over 600 frames. This holds idle, with a HelperClient-style 2 ms `PeekNamedPipe` poller, and with a D3D9 `Present` each frame. The worst frame was 19.6 ms, and no case had more than one frame over +2 ms. |
| VSync override | With `D3DPRESENT_INTERVAL_IMMEDIATE` (what `fD3D::BuildPresentParameters` forces), DXVK selects `VK_PRESENT_MODE_IMMEDIATE_KHR`, and `Present` returns in about 0.03 ms. |

## Problems found

1. **`preflight.cmd` passes without checking anything.** It runs `powershell.exe -File preflight.ps1`. Wine's `powershell.exe` is a stub that prints nothing and exits 0. The manifest hashes are never verified, and the player sees no error.
2. **In-app updates cannot extract under Wine.** `github_release_client.cxx` runs `%SystemRoot%\System32\tar.exe`, which Wine 11 does not ship. The update was not run end to end here: the file's absence was confirmed, and the rest comes from reading the code. Linux players need to update by extracting a fresh package.
3. **Each helper pipe poll is a wineserver round trip.** `PeekNamedPipe` costs about 120 µs per call under Wine. The HelperClient loop runs about 450 times per second while idle, and the Discord client runs a second loop. That is roughly 5% of a core spent in the wineserver shared by the game's threads. It caused no measurable limiter jitter here, but that was on an idle machine. The real game makes many more wineserver calls, so this remains a plausible contributor to jerkiness.

## Not established

This check did not reproduce either reported symptom. The paths that differ in a real Linux session were not exercised:

- The game's own frame loop, and DXVK shader compilation during a match. A stall of 3 s or more trips GGPO's disconnect timeout (the limit grows with input delay, see `sf4e__NetplayConfig.hxx`).
- VSync on a real display. Xvfb has no vblank: VSync-on `Present` never blocked here apart from one 30 ms outlier. Under gamescope, Steam Deck or Wayland, or with `DXVK_FRAME_RATE` or `d3d9.presentInterval` set, `Present` can still wait for vblank. The limiter runs after `Present`, so a +3 ms pacing shift would then cost a whole refresh.
- Public relays, two machines and Proton's Steam runtime.

The next useful evidence is the complete `sf4e\logs` folder from a Linux player, taken right after a jerky match or a broken room. Search it for:

- `Display: VSync forced off`
- `Pacing: limiter runs on thread`
- `match_teardown_timeout`, `match_room_end_timeout`, `gameplay_connection_lost` and `local_socket` (from `IrohMatchSession.cxx` and `IrohRoom.cxx`)
- `HelperLoad` `tick_lag_max`
