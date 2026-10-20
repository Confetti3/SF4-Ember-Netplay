# Continuous private-room rematches

After the complete victory sequence, the two fighters choose Rematch or
Return to lobby. Both must accept before restarting. Either decline, or the
20-second choice timeout, returns the set to the room. Changing fighters
still happens in the lobby. A completed FT set offers only the return action.

## Session and simulation ownership

The host must enable the experimental continuous-rematch rule for the table;
it is off by default. It is available only in private, non-Practice,
non-tournament rooms, including private LAN rooms. Both fighters' Ready
requests and every member of the frozen native roster must advertise native
support. Queued members promoted to spectators are included before making
that decision. A missing/false capability, including an older client,
selects the ordinary lifecycle for the entire table before native launch.
Unsupported spectators stay in the match; they are not dropped at frame one.
Idle members and future watchers do not change an already frozen roster.
The identical-Sidecar admission check remains in place.

Startup checks executable signatures and replay callbacks. Play Offline is
not a capability gate: an offline-started client can later join or watch a
room. Debug modes that change the save-state contract remain unsupported
and advertise that before admission. An unexpected object-lifetime failure
after native launch uses the existing deferred abort path. A failed restart
preflight before Commit sends an opt-out in the input stream; it cannot
originate a successful Commit and later retract it.

The game thread adapter in `sf4e__ContinuousRematch.cxx` observes the verified
Steam 1.05 victory callbacks. It lets the animation finish, then holds the
native simulation while GGPO continues to exchange inputs and save states.
The menu sends a choice and an offer token back to that owner; rendering
does not reset the engine or touch GGPO.
The immutable RematchPanel view selects a modal in OverlayLayersState. That
same layer policy owns visibility, keyboard capture, controller sampling and
MenuPads' hold under its existing lock. Its local request carries the choice
and offer token, not a room command. F10, Start and renderer resets cannot
dismiss the game-thread result owner.

Votes use the upper byte of `mappedOn`. One `PrepareFrameInputs` path removes
it and the Practice raw-input reservation before both forward playback and
resimulation, even when continuous rematches are inactive or closed.
`ContinuousRematchChoices.hxx` saves readiness, consent, commit, epoch and
timeouts with the normal GGPO state. The adapter's local Session groups the
unsaved native ownership, history and presentation state; its phase is explicit.
The existing SimulatedStateFrame supplies the timeline identity.

A fighter originates Commit only after the consent is confirmed and its
native preflight passes. The preflight checks the same object identities,
request and menu bytes captured at bind; it does not run every frame. A
spectator checks its own context once per restart and can end only its view
on failure. It never changes the fighters' accepted decision.

A fighter whose per-battle bind refuses retains only the validated native
result owner and sends No. Its current game, result and replay can finish
normally. Only a missing/invalid native result owner requires the existing
abort path; the opt-out itself does not remove ordinary gameplay inputs.

The reset waits nine input frames after both Commit bits are observed, past
the pinned GGPO maximum prediction of eight frames (asserted at compile time).
This alone would confirm only the Commit edge, not the future reset frame.
Consequently each sender retains the complete Commit input (zero raw and
zero low mapped bits) until the new epoch itself is confirmed. Prediction
therefore repeats that same word across the reset. A changed gameplay input
can first correct a frame after the confirmed reset, not a frame before it.
A late No before Commit can still correct the saved decision during the
barrier and prevent reset. Local observations cannot replace an already
sent Commit by No. The native reset branch has no fighter-local refusal
check that could diverge from the agreed input tape.

This relies on the pinned GGPO prediction/input-confirmation contract, the
main-thread ownership of battle objects while held, and the immutable
Commit producer. It does not assert that arbitrary forced rollback across
the native reset is safe. Pure model tests exercise delayed/corrected votes
and input retention, not native mementos. Forced rollback stress is not
advertised as a supported continuous-rematch configuration.

The original request and menu data stay loaded. GGPO frames remain monotonic;
the session, helper connections and spectator stream survive the restart.
The native seed behavior and its limits are documented in
[NATIVE_REMATCH_RESET.md](NATIVE_REMATCH_RESET.md).

Spectators replay the two fighters' decisions; they never vote or hold the
fighters' readiness quorum. Existing spectator catch-up and disconnect
policy remain responsible for slow or lost streams. This change does not
implement joining a match already in progress or rejoining a lost stream.

## Room authority and scores

`RecordContinuousResult` numbers each game within the existing table and
transport generation. Ordinary and continuous results use the same ReportQuorum (first, agreed,
disputed or duplicate). Two matching authenticated reports commit one point;
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

When the host applies different table rules during a game, the authority first
closes that table's current generation through the existing Cancel receipt.
It applies the rules only after reserving that durable receipt; a full ledger
leaves the game and rules unchanged. The single result carries the normal
MatchEnded event and the final snapshot, so existing teardown returns the
fighters and spectators without editing their running simulation. Other tables
are unaffected. The next session reads the new limit from the live owner.

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
the native draw-to-rematch sequence has not been validated. This PR leaves native
music selection to its owner; the optional music PR integrates at confirmed
game boundaries and is reviewed separately.

Flavio tested predecessor builds with two fighters, a spectator, FT3,
seat rotation, separate native replay recordings and repeated sets. Those
reports do not validate this revised input barrier or the new admission rule.

Before leaving draft, repeat native two-PC and three-PC matches on this exact
branch. Enable the table's experimental option, test a non-host winner,
both choices, timeout, FT rotation, successive sets and replay of each match.
Start a spectator with Play Offline, then join and watch an enabled table;
it must stay through the continuous rematch. Use one unsupported configuration
to verify that the whole table instead uses the ordinary path before launch.
Also verify ordinary rooms with the option off, spectator departure, Stop
watching, WAN/relay delay, room-only recovery at victory and changes to table
rules. Inspect confirmed hashes, GGPO return codes and memory/key counts
through a sustained series. Existing network fixtures and model tests do not
prove native reset safety or these live scenarios. A draw after previous wins
must preserve those scores and return normally, without a native reset.

There is no optional trace engine or diagnostic reset limit in this branch.
The only native history retained is rollback state needed to act on confirmed
results. No trace, executable, game asset or user settings are included.

Development was AI-assisted with Codex. Flavio directed the requested
behavior and performed the manual multi-PC gameplay testing. The draft asks
the upstream maintainer to review the source and determine integration scope.
