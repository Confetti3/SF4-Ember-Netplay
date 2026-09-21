# Discord activity and invitations

This local candidate integrates the official Discord Social SDK with Ember. Real two-account, two-PC invite acceptance remains required before release; automated checks do not establish game or Discord invitation acceptance.

## Application setup

The public Ember application ID is `1546980049692135514`. Enable Social SDK access for that application in the [Discord Developer Portal](https://discord.com/developers/applications/1546980049692135514). Upload `src/ui/ember.png` as the Rich Presence artwork asset named **ember**. The owner confirmed this upload on 2026-09-08; its appearance on another account remains part of acceptance. No bot token, client secret, or player OAuth flow is used.

The supplied SDK is **1.10.19337**, archive `DiscordSocialSdk-1.10.19337.zip`, SHA-256 `d784097504685953849cc2842561d8a8013326fe0b8df65a1de63ddefde62045`. `cmake/discord-sdk-pin.json` is the authoritative pin. The archive is not committed. CMake verifies its hash and builds from its extracted contents, including the vendor redistribution notice.

## Build and package

Use the existing x86 game build configuration and add `SF4E_BUILD_DISCORD=ON` and `SF4E_DISCORD_SDK_ARCHIVE` pointing to the original downloaded archive. The companion is an isolated MSVC x64/C++20 external project; game architecture and compiler standard are unchanged. This checkout's `scripts/build-discord-local.ps1` records the working local Build Tools and dependency paths. Pass `-RustChecks` to run formatting, Rust tests and Clippy. The script expects the repository as its working directory.

Install the complete build, then run `scripts/package-team.ps1` with matching `-BuildDir` and `-InstallDir`. The shared inventory requires `ember-discord.exe`, `discord_partner_sdk.dll`, `discord-build.json`, and `notices/Discord-SDK.txt`. Packaging rejects an unset application ID or missing Discord runtime. Updater and installer validation use the same inventory. The SDK is statically linked only through its import library; its supplied x64 runtime DLL must ship beside the companion. No SDK voice features are used.

## Lifecycle and privacy

Launcher owns the companion for the game's lifetime, including offline sessions. Both sides reuse Ember's helper bootstrap and framing: current-user pipe permissions, local-only pipe, process identity checks, inherited random nonce, bounded messages, and worker-thread IO. Presence is a copied snapshot; SDK callbacks enqueue one accepted invite. The game thread validates commands against the current room generation and invitation revision. Companion failure cannot call game teardown.

The companion uses desktop RPC without a Discord login flow. It registers the absolute adjacent `Launcher.exe --discord-launch` command. Launch registration is refreshed when a new package starts; a per-user launcher guard prevents a second game. The secret is delivered by the SDK callback, never launch arguments. Rich Presence updates are coalesced, retried with backoff, refreshed after Discord restart, and cleared when disabled or shutting down.

Activity is limited to Starting Ember, In menus, Playing offline, In a room, Queued, Ready, Fighting, and Spectating. It includes elapsed session time and current/configured occupancy. It excludes user-authored room and player names, characters, stages, scores and networking metrics.

The `emd1:` Discord ticket is exactly 127 characters: a five-character prefix and unpadded base64url of the existing 91-byte invitation header. Clipboard formats remain supported. The joining client supplies its actual build to the existing authenticated handshake. The host retains enforcement of its build, capability and expiry. Confirmed guests retain the validated invitation in memory and may advertise the same room. Departure, failure, unhealthy coordination, expiry, admission lock, capacity and publication settings suppress invitation advertisement as applicable. Secrets are not persisted in settings or diagnostics.

An accepted invitation waits for the native main-menu gate, network availability and controller assignment. Loading and fights cannot be interrupted. Switching from an existing room or offline play requires an explicit action. A newer invitation invalidates queued actions for its predecessor. Joining the current room is a no-op. New room members remain idle until they choose a table action.

## Acceptance checklist

Use the identical packaged build on two PCs and two Discord accounts. Record the package hash, results and any failures. Do not include invitation secrets in evidence.

1. Host an unlocked room. Verify Ember artwork, activity, elapsed time and occupancy in Discord. Invite the second account through Discord. Verify joining while Ember is running and entering idle.
2. Have the guest invite another account or reverse host/guest roles. Verify guest invitation advertisement and the same room occupancy.
3. Close Ember on the joining PC. Accept a fresh invite; verify Launcher starts the game once, injection succeeds, controller readiness is respected, and the room is joined at the main menu.
4. Accept duplicate invitations, replace one pending invite, cancel it, and accept an invite to the current room. Verify no duplicate game or unintended room switch.
5. While in another room, offline play, loading and a fight, accept an invite. Verify the pending notice and explicit safe-menu switch. Finish the fight normally before switching.
6. Choose a table, queue and watch. Verify Queued, Ready, Fighting and Spectating states and ordinary gameplay, rematches and spectators.
7. Lock admission and fill capacity. Verify outgoing invitation advertisement disappears and already-shared tickets receive the normal room rejection. Verify expired tickets and a different-build guest are rejected.
8. Toggle each Discord setting. Restart Discord and verify activity restores. Stop the companion during a local test and verify gameplay continues. Close Ember and verify activity and companion exit.
9. Repeat a join and ordinary gameplay across different networks. Record real results separately from local helper, UI, or RPC smoke tests.

The manual `DiscordSmokeTest.exe` accepts the installed companion path. It briefly publishes Starting Ember, clears activity, and checks launch registration and clean exit. It sends no invitations and does not launch SF4; success is only desktop RPC/lifecycle evidence.
