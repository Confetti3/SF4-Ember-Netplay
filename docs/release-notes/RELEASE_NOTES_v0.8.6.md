# SF4 Ember Netplay v0.8.6

This release answers a round of tester reports: frame drops with Evil Ryu,
rollback jumping from 1 to 7 on good connections, players who could no
longer leave their seat or rematch after a few games, and a C. Viper Super
that passed through Dudley. The first three have concrete causes in the code
and are addressed here. The fourth now has the tools to find it.

## Rollback performance

- **Rollback save states are released without replaying the game.** GGPO
  releases its oldest saved frame before every save, which means once per
  simulated frame and once more for every re-simulated frame. Each release
  used to save the live game, restore the old state, clear it, and restore
  the live game again. That is two extra memento records and two full
  restores of every character, effect and VFX unit per frame. A 7-frame
  rollback repeated it seven times.
  The engine's memento release only needs the old key installed, not the old
  game state (`docs/design/SAVESTATE_FREE.md` has the disassembly). The release now
  swaps the key in, clears it, and puts the live key back. That removes the
  extra records and restores from every frame. Characters with heavy
  persistent effects, whose restores cost the most, carried the largest share
  of that work.
  `SF4E_LEGACY_SAVESTATE_FREE=1` restores the previous behaviour for
  comparison.
- **Releasing a state no longer touches the live game.** The old release
  restored the live game once for every released state. That count depends on
  each player's own rollbacks, so any imperfection in a restore could make the
  two machines diverge. The new release does not restore anything.
- **The interface no longer copies its whole state twice a frame.** Input
  handling and the overlay each copied the complete room and runtime snapshot
  every frame. They now share the published snapshot. The lifecycle trace also
  builds its record only when a traced value changes.

## Rooms: seats and rematches

- **Either fighter can abandon an unresolved game.** When the two fighters'
  result reports are missing or disagree, the table pauses. Neither fighter
  could leave the seat or ready again, and only the host could clear it.
  Seated fighters now have **Abandon unresolved game**, which records no
  result and changes no score. It appears only once the table is paused, never
  during a live game.
- **A result confirmed after the fight stops is still reported.** The winner
  was published only after a simulated frame. If the opponent's final inputs
  were confirmed just after simulation stopped, which heavy rollback makes more
  likely, that player never reported the result, and the table paused after 30
  seconds. The result is now also published from the network poll and at
  battle close.
- **The match record is saved before the room is told it was saved.** The
  teardown acknowledgement was released once the profile write was queued,
  not once it was written. It now waits for the write, and the settings worker
  retries a failed write.

## Diagnostics

- The log now names every refused room action with its reason, every disputed
  result, and, at battle close, the captured result, its frame, and the
  confirmed input frame.
- `SF4E_ROLLBACK_DIAGNOSTICS=1` adds save/restore time per unit group (chara,
  effect, VFX, other) and the new release timer. **Export diagnostics**
  includes Free state, Effect restore and VFX restore.
- The desync hash now also covers each character's action, action frame,
  posture and time scale, so a move landing on a different frame is
  detected.
- `SF4E_ROLLBACK_STRESS=<1..8>` (development) makes an offline Versus or
  Training battle save, release, load and re-simulate like a GGPO session with
  that rollback distance. After every replayed frame it compares hashes. Training
  Lab playback works under it, so a recorded sequence can be checked for
  replay divergence on one PC. `SF4E_SAVESTATE_FREE_VERIFY=1` checks that
  every release leaves the live game unchanged.

## Install or upgrade

For a fresh installation, download **sf4-ember-netplay-0.8.6.zip** and its `.sha256` sidecar. Extract the complete package into a new writable folder, run **preflight.cmd**, then run **Launcher.exe**.

For an intact published v0.8.5 installation, download **upgrade-sf4-ember-netplay-0.8.5-to-0.8.6.zip** and its checksum. Extract it into a separate folder, close SF4 and Ember, run **Install Upgrade.cmd**, and select your existing v0.8.5 Ember folder.

Everyone in a room must use the same Ember release. This release changes the desync hash, so v0.8.5 and v0.8.6 players cannot share a room. Preferences and your win/loss record remain under `%APPDATA%\sf4e`. Requires Windows 10 or later (x64), an owned Steam copy of Ultra Street Fighter IV and the Microsoft Visual C++ x86 runtime.

## Validation and limits

The designated Windows build runs the 39-suite native test set. New coverage in this release:

- a fighter abandoning a disputed result, followed by the terminal acknowledgement fence, a rematch and releasing the seat;
- the abandon control appears only for seated fighters on a paused table;
- a settings revision is not reported saved while its write fails, and is reported once the retry succeeds;
- the swap release frees exactly the released state across a ten-slot save ring with the live keys unchanged, checked against the previous path;
- a result confirmed after simulation stops remains publishable;
- every diagnostics operation is named and fits the summary buffer.

Packaging verifies provenance and hashes and builds the upgrade from the published v0.8.5 package.

**These changes have not yet been observed in gameplay.** The release-path change rests on disassembly of the engine's memento release and on the ownership tests above, not on a recorded match. No Evil Ryu frame-time comparison, rollback stress run or two-PC rematch series has been recorded for this release, so it does not claim a measured frame-rate or rollback improvement. If a problem appears, launch with `SF4E_LEGACY_SAVESTATE_FREE=1` to return to the v0.8.5 release path, and keep both players' `%APPDATA%\sf4e\logs` along with **Help & About → Export diagnostics**.

Rollback stress checks that one machine replays its own frames identically. Two machines can still diverge through state the hash does not cover, such as hitbox geometry or the evolving RNG, and the desync detector still ends a match on a genuine divergence.
