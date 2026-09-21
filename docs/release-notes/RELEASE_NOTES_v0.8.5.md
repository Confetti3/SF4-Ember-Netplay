# SF4 Ember Netplay v0.8.5

This release fixes the lobby problems players have been reporting, and finishes a user-interface pass.

Two of those reports were the same class of bug: a room that looked perfectly healthy while silently refusing everything you asked it to do. A failed departure could leave you unable to open or join another room until you restarted the launcher, and a member whose checkpoint never applied could sit in the lobby unable to queue, ready or spectate. Both are fixed, along with the room controls that flickered and swallowed presses during a match. There are no gameplay or rollback behaviour changes.

## Interface

- **Room controls no longer flicker during a match.** The room board painted its buttons straight from the live eligibility flag, bypassing the smoothing every other screen already used, so its controls strobed once per room checkpoint — most visibly for queued and spectating members while a match was running. The board now uses the same smoothed state as the rest of the interface.
- **A room action that is unavailable says so.** Selecting a disabled room control used to do nothing at all, with no message. The board now explains why, using the same wording the menu list uses: **Updating room...** while a checkpoint is being applied, **Unavailable** otherwise.
- **Failures are rendered as failures.** The status line is the interface's only feedback channel and had no severity: "Settings were not saved", "Controller disconnected" and "The room action could not be queued" were drawn in exactly the same colour as "Private room / invitation only". Status now carries a severity across the shell, Fighter Select and the Training Lab.
- **Fighter Select explains a blocked selection.** When a saved selection is not usable under the room's rules, the room screens tell the player to open Fighter Select. Fighter Select now states what is wrong once they arrive, instead of showing its usual help text.
- **Match HUD sizes are three different sizes again.** The readability floor was applied after the player's size choice, so Small and Standard produced an identical panel at 720p and all three collapsed to the same size at 480p. The floor now applies to the viewport term only. Behaviour at 1080p is unchanged.
- **Replace room warns what it costs.** The confirmation now states that everyone must rejoin and that the current invitation link stops working, and a notice afterwards points at **Copy invitation** for the new one.
- **Layout fixes.** Room action labels no longer truncate; the members list is sized to whole cards so its bottom entry is never cut through the portrait; the status line no longer shifts the menu list when it grows; the stage gallery derives its columns from the window like the roster and appearance galleries already did; and the "VS" marker no longer overruns its gutter in the battle-slot cards.
- **Wording and consistency.** An empty seat reads "Looking for a fight" once instead of also saying "Open seat" underneath. A record with no games shows **0% WIN** rather than the unavailable dash. "Ready" is now a single colour across the player card, the battle slots and the status line instead of two near-identical greens.
- Text the interface has to shorten — player names, room names — now shows in full on hover.
- The README badge and download links pointed at v0.8.3 from inside the v0.8.4 tag. They now track the release being cut.

## Verification

Full build clean. 39 of 39 native test suites pass, 77 of 77 helper tests pass, plus the standalone UI polish suite and the DX9 render harness (6444 frames across ten viewport and DPI configurations).

New regression coverage added with this release:

- Ten seconds of simulated checkpoint churn must produce zero visible state changes on a room control while the underlying gate flips hundreds of times, and a sustained change must still reach the player. This is the direct regression test for the flickering controls.
- Room board action labels are checked for overflow at every viewport and DPI.
- A room-control failure must be reported with error severity.
- The members list height must be a whole number of member cards.
- Small, Standard and Large match-HUD scales must remain distinct.
- A checkpoint that never applies must stay silent while lag is brief, report itself once the stall persists, and offer a replacement room rather than refusing everything indefinitely.

## Networking and lifecycle fixes

- **A failed room departure no longer locks you out until you restart.** Only one of the helper's three leave-failure responses was retried; the other two were discarded, so the acknowledgement never arrived and the client stayed in Closing with Host, Join and Replace room all refused. All three now retry with backoff, and if the room still has not confirmed within eight seconds the room is released locally so you can open or join another. It never claims the remote departure succeeded — the room authority still removes you through its own connection teardown — and it tells you: *"The room did not confirm your departure. You have left locally."*
- **Joining a room and then being unable to do anything is fixed.** When the control stream was connected but its checkpoint had not applied locally, the session reported itself perfectly healthy while silently refusing every room action — including **Join queue** and **Watch next game**. Members in that state sat in the lobby unable to connect to anyone or spectate, with no message and no way out, because Replace room was refused too. Brief lag is still silent, as intended; a stall is now named after ten seconds and offers a replacement room after thirty.
- **A room action accepted by the interface is no longer discarded.** If a newer authority checkpoint arrived before the action executed, it was dropped with no retry and no message — the other half of the repeated-pressing problem. The newest intent is now held for up to three seconds, revalidated against the room generation, and resubmitted once the room catches up; if it cannot be applied in time, it says so instead of failing silently.
- **Closing the game now completes your departure.** Shutdown cancelled an in-flight graceful leave in the helper, so quitting SF4 was not equivalent to choosing **Leave room** and could leave you listed in the room. Departure now gets a bounded window to be sent and acknowledged before the helper stops.
- **Direct connections behind NAT should succeed more often.** Gateway port mapping (UPnP/NAT-PMP/PCP) was switched off to demonstrate the transport did not depend on it, and stayed off in the shipping build. Iroh documents the cost of that as "potentially worse direct connectivity behind some NATs", which matches reports of players who cannot reach each other directly. Gameplay and coordination endpoints now use iroh's default, which enables it. The relay-only diagnostic keeps it off. Windows may show a one-time firewall prompt.
- **The in-app updater no longer replaces its own running executable.** It launched from the installation it was about to rewrite, which Windows refuses, making failure the expected outcome. The verified updater from the downloaded package is now copied to a unique directory outside the replacement set and run from there — which also means an upgrade started from an older install runs the new updater rather than the one already on disk.
- **A desync now records what actually diverged.** The legacy check compares whole state snapshots and ends the match on any difference, but logged only "Desync detected!", which is not enough to find the cause afterwards. It now names the frame and each differing field with both values, so a player's log identifies the subsystem.

## Known issues

- **Desync can still end a match.** The detector is working as designed — it finds a genuine state divergence between peers and stops the match rather than letting it continue wrongly. The underlying determinism bug is not fixed, because finding it needs the field-level divergence data this release starts recording. If you are kicked out mid-match, export diagnostics and keep `%APPDATA%\sf4e\logs` from **both** players before relaunching.
- **A player lost to a hard crash or power loss can still linger in the room** until the transport reports the connection closed. Clean shutdown and **Leave room** are both handled; an application-level membership timeout is deliberately not added, because a wrong one would evict healthy players mid-match.
- The port mapping change above is a well-evidenced cause for the direct-connection failures, but it has not been confirmed against a specific failing pair. If two players still cannot connect, their logs will now show whether a direct path was attempted.

## Install or upgrade

For a fresh installation, download **sf4-ember-netplay-0.8.5.zip** and its `.sha256` sidecar. Extract the complete package into a new writable folder, run **preflight.cmd**, then run **Launcher.exe**.

Given the updater issue above, upgrading by extracting the full package into a new folder is the recommended path. Existing preferences and your win/loss record are kept under `%APPDATA%\sf4e` and are picked up automatically.

**All participants in a room must use the same Ember release.**
