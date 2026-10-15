# SF4 Ember Netplay v1.0.0-rc7 (test build)

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

**This is an Ember 1.0 test build, published as a GitHub pre-release for testers.** It has everything in v1.0.0-rc5 plus the changes below. If you would rather not test, stay on v0.9.9, the current full release. v0.9.9 stays marked Latest, and the updater only follows Latest.

rc7 replaces rc6, which was briefly published and withdrawn before anyone tested it.

## Changes since v1.0.0-rc5

### Rollback memory checks

These diagnostics and hardening checks were added after a tester's rc5 crash dump. The heap corruption crash, exit code `0xC0000374`, happened about 40 minutes into a relayed, high-ping session with frequent rollbacks, while the game freed an old rollback save. The original cause is still unknown. This is not a confirmed fix for that crash.

- **Rollback saves now track ownership of saved game memory.** If a step would free or reuse memory a save still holds, or write a save into a game object that no longer exists, that step is skipped and the match ends cleanly with a message instead of corrupting memory. Normal matches behave as before.
- **Old rollback saves are freed without touching the running game.** Previously, freeing a save temporarily put its saved data into the live game object, freed it there and put the live data back, every frame. It now frees the saved data on its own and writes nothing into the running game.
- **Every match ends with a summary in `sf4e.log`.** For example:

  ```text
  SaveSlots [battle_close_exit]: guards engine_clears=0 skipped_load_writes=0 skipped_free_writes=0 leaked_descriptors=0 releases=152340 tracked_keys=2
  ```

  The first four counts should be `0`. `releases` counts freed saved blocks and is expected to be large. `tracked_keys` should stay small and should not grow from match to match. Report any nonzero first-four count, any `MementoGuard:` line, or a `tracked_keys` count that keeps growing.

- **Heap checkpoints remain available in settings for crash hunting.** See [Heap checkpoints for testers](https://github.com/Confetti3/SF4-Ember-Netplay/blob/v1.0.0/docs/guides/SAVING_LOGS.md#heap-checkpoints-for-testers). Use them only when reproducing a problem, since they can affect performance.

### Rooms

- **A former host can rejoin with an invitation copied after leaving.** A player reported being unable to rejoin after handing host to another player and leaving. Handing host off changes who is host in the room, but until the old host leaves, every invitation still points at the old host's PC. A copy taken before leaving sent that player back to their own PC and failed as "could not reach the host". Such an invitation is now refused with a message saying it points back to your own game and to ask the host to copy the invitation again. A copy taken after you left works.
- **Rooms stay joinable after an hour.** Invitations previously expired one hour after the room was created and were never renewed, so rooms open longer refused every new copy and every rejoin. The invitation each member shows is now renewed while the room is open. A copied invitation still expires within an hour.

### Display resets

- **The overlay is no longer freed while it is being drawn.** A display reset, such as Alt+Tab in fullscreen or a resolution change, could free and recreate the overlay on one thread while another was still drawing it. A tester's log showed this about 20 times in one session. Overlay frames during a reset are now skipped instead.

## Compatibility

Everyone in a room needs the complete v1.0.0-rc7 package, including spectators. rc5, rc6 and v0.9.9 cannot join an rc7 room.

Extract the complete package into a new, empty folder. The in-app updater does not offer pre-releases. An rc install will not be offered the final 1.0.0 either, so testers will need to extract that package manually later.

## What to report

All 87 automated tests and all room recovery tests pass, including a new host handoff and rejoin test over direct and relay-only connections. A 5-minute offline Training session with forced 7-frame rollbacks ran 1,954 rollbacks with no desyncs, about 3.8 million save releases and zero guard counts. Online matches with these changes are untested; that is what this rc is for.

- **Long online sessions.** Play for 30 minutes or more, especially on relayed or high-ping connections with lots of rollback, and with a spectator.
- **Rooms open for more than an hour.** Have someone join or rejoin late.
- **Host handoff and rejoining.** Hand host to another player, leave, then rejoin with an invitation copied after you left.
- **Fullscreen display changes.** Try Alt+Tab or changing display settings during a session.
- **Logs after every session, including successful ones.** Send `sf4e.log` and `sf4e.1.log` etc. from `%APPDATA%\sf4e\logs`, so we can check the guard lines.
- **Unexpected match endings.** If a match ends unexpectedly with a message, note roughly when it happened.
- **Crash dumps.** Send dumps privately. They can contain invitations, player names and chat.

## Install and play

The rc7 pre-release has been removed. The same code shipped as [v1.0.0](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.0.0); download its package. Extract the complete package into a new, empty folder on each machine and run `Launcher.exe`. Running `preflight.cmd` first is optional.

See the [player guide](https://github.com/Confetti3/SF4-Ember-Netplay/blob/v1.0.0/docs/guides/USER_NETPLAY.md) for play instructions.
