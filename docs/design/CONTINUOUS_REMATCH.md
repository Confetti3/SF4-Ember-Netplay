# Continuous private-room rematches

After the complete victory sequence, the two fighters choose Rematch or
Return to lobby. Both must accept before restarting. Either decline, or the
20-second choice timeout, returns the set to the room. Changing fighters
still happens in the lobby. A completed FT set offers only the return action.

## Session and simulation ownership

Private, non-Practice, non-tournament tables negotiate `Ready.continuous`
through both fighters. Public/server-owned rooms retain their existing
match lifecycle. The usual identical-Sidecar admission check remains in place.
Startup validates the native signatures and supported launch configuration
before advertising this capability. If either fighter declines it, the room
starts an ordinary match. A later Bind refusal sends an opt-out through the
GGPO input stream: the current game continues and its confirmed result
finishes normally, without entering the rematch menu.

The game thread adapter in `sf4e__ContinuousRematch.cxx` observes the verified
Steam 1.05 victory callbacks. It lets the animation finish, then holds the
native simulation while GGPO continues to exchange inputs and save states.
The menu sends a choice and an offer token back to that owner; rendering
does not reset the engine or touch GGPO.
The result panel owns its input capture independently of shell visibility.
Its local request carries the choice and offer token, not a room command.
F10, Start and renderer resets cannot dismiss the native result owner.

Votes use the upper byte of `mappedOn`, stripped before native playback.
`ContinuousChoices.hxx` stores readiness, consent, commit, epoch and timeout
in the rollback state. A peer originates Commit only from confirmed consent.
The native reset is invoked at the resulting deterministic input frame on
forward simulation and rollback. The original request, menu data and seed
are retained. GGPO frame numbers remain monotonic and the same session,
helper connections and spectator stream survive the restart.

Spectators replay the two fighters' decisions; they never vote or hold the
fighters' readiness quorum. Existing spectator catch-up and disconnect
policy remain responsible for slow or lost streams. This change does not
implement joining a match already in progress or rejoining a lost stream.

## Room authority and scores

`RecordContinuousResult` numbers each game within the existing table and
transport generation. Two matching authenticated reports commit one point;
duplicates cannot award it again. Each confirmed game has a separate local
profile receipt. No intermediate game emits MatchEnded or rotates seats.

The next offer waits for the result to commit, with a 300-input-frame bound
from the end of the victory sequence. Missing readiness falls back to normal
finish even if the room connection is unavailable while GGPO keeps running.
The separate 20-second selection timer starts when both fighters are ready.
Both fighters' confirmed
return votes issue `FinishContinuousMatch`. The authority then uses the
existing terminal receipt and seat-rotation path, without awarding the last
point twice. Result disagreement pauses the table and the native adapter
exits through the ordinary abort path. Other tables have separate state.
Draws record a Draw receipt and finish normally, preserving earlier wins.
They do not run the unvalidated native draw-to-rematch transition.

Ordinary games use game number zero; continuous games use numbered results
inside the same generation. Both use one result outbox, profile persistence
path and retained finish retry. The room's shared EndMatch receives an
explicit already-scored flag, validates the continuous quorum and performs
the existing terminal receipt and seat rotation without a second score award.

Session retirement and result/profile persistence run outside GGPO callbacks.
Failure inside a callback uses the existing deferred abort mechanism. The
normal save-state pool contains the rematch state; no extra checkpoint pool
or replacement GGPO session is created.

## Match recordings

Versus has one outer replay-save task. Its result check serializes the native
recorder, waits for the save controller, releases the serialized buffer and
becomes idle. A normal rematch reinstalls the check; a continuous reset does
not. Reusing that idle callback's original check for each confirmed game
keeps one native replay per match without recreating the battle or session.

`ReplayMatchBoundary.hxx` gives that task a monotonic session/game token.
Only a confirmed result permits the check, and a non-null pending native
replay buffer marks its submission once. The original check still decides
whether recording is enabled and the recorder has stopped. Ordinary games
retain their original callbacks. Both callback signatures must match before
the continuous capability is advertised; unsupported code uses ordinary
room rematches. The async save and its buffer cleanup remain native.

The outer tick posts a recording end for each confirmed result and a new
start for each confirmed rematch, reusing the existing player-name and
spectator attribution worker. These irreversible events are not replayed
by rollback. The recorder's streams and codecs still use the existing save
state capture/restore; this change neither resets nor reallocates them.

## Review boundaries

This is an experimental draft, not a general stability claim. The adapter
uses native instruction signatures, verified object identities and unchanged
request bytes as refusal guards. A draw currently returns to the lobby;
the native draw-to-rematch sequence has not been validated. The original
music bank stays loaded across rematches, including a resolved Random choice.

The predecessor was tested manually with two fighters, a spectator, FT3
and seat rotation; the port to Ember 1.3.0 was confirmed with two fighters
and repeated sets in one launch. Those reports apply to the predecessor,
not to this revised nightly contribution. This branch removes obsolete
offline/reconnect experiments, diagnostic reports and the fake-engine probe.
It uses private-room capability negotiation and translates the new messages
in all 14 catalogs. Native speakers should review the wording.

On a7316c87, Flavio confirmed the requested FT3 test on the revised branch;
the local log records three accepted results and normal series closure.
That run exposed a replay regression: only the first match was saved.
The replay-task change above has a three-game/two-set regression test, but
still needs native playback validation for every game, with distinct inputs.

Before leaving draft, repeat native two-PC and three-PC matches on this exact
branch, including a non-host winner, both choices, timeout, FT rotation,
successive sets, spectator departure and the 1.3 Stop watching command.
Review control-plane loss/recovery at the victory boundary, match recording
and replay across a continuous set, and WAN/relay behavior. Existing network
fixtures and model tests do not prove native reset rollback safety, native
presentation, or those live scenarios. Also test a draw after earlier wins,
an unavailable adapter on one PC, and room-only connection loss before either
Ready arrives. Check the ordinary result, profile and return to the lobby.

There is no optional trace engine or diagnostic reset limit in this branch.
The only native history retained is rollback state needed to act on confirmed
results. No trace, executable, game asset or user settings are included.

Development was AI-assisted with Codex. Flavio directed the requested
behavior and performed the manual multi-PC gameplay testing. The draft asks
the upstream maintainer to review the source and determine integration scope.
