# Continuous private-room rematches

After the complete victory sequence, the two fighters choose Rematch or
Return to lobby. Both must accept before restarting. Either decline, or the
20-second choice timeout, returns the set to the room. Changing fighters
still happens in the lobby. A completed FT set offers only the return action.

## Session and simulation ownership

Private, non-Practice, non-tournament tables negotiate `Ready.continuous`
through both fighters. Public/server-owned rooms retain their existing
match lifecycle. The usual identical-Sidecar admission check remains in place.

The game thread adapter in `sf4e__ContinuousRematch.cxx` observes the verified
Steam 1.05 victory callbacks. It lets the animation finish, then holds the
native simulation while GGPO continues to exchange inputs and save states.
The menu sends a choice and an offer token back to that owner; rendering
does not reset the engine or touch GGPO.

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

The next offer waits for the result to commit. Both fighters' confirmed
return votes issue `FinishContinuousMatch`. The authority then uses the
existing terminal receipt and seat-rotation path, without awarding the last
point twice. Result disagreement pauses the table and the native adapter
exits through the ordinary abort path. Other tables have separate state.

Session retirement and result/profile persistence run outside GGPO callbacks.
Failure inside a callback uses the existing deferred abort mechanism. The
normal save-state pool contains the rematch state; no extra checkpoint pool
or replacement GGPO session is created.

## Review boundaries

This is an experimental draft, not a general stability claim. The adapter
uses native instruction signatures, verified object identities and unchanged
request bytes as refusal guards. A draw currently returns to the lobby;
the native draw-to-rematch sequence has not been validated. The original
music bank stays loaded across rematches, including a resolved Random choice.

The predecessor was tested manually with two fighters, a spectator, FT3
and seat rotation; the port to Ember 1.3.0 was confirmed with two fighters
and repeated sets in one launch. Those reports apply to the predecessor,
not to a freshly compiled revision of this cleaned contribution. This branch
removes obsolete offline/reconnect experiments and launch modes, uses normal
private-room capability negotiation, and localizes the new status messages.

Before leaving draft, repeat native two-PC and three-PC matches on this exact
branch, including a non-host winner, both choices, timeout, FT rotation,
successive sets, spectator departure and the 1.3 Stop watching command.
Review control-plane loss/recovery at the victory boundary, match recording
and replay across a continuous set, and WAN/relay behavior. Existing network
fixtures and a fake-engine GGPO driver do not prove native reset rollback
safety, native presentation, or those live scenarios.

Optional `SF4E_REMATCH_REPORT` sets a local JSON trace path, with a separate
file per session. It is off by default; write failures disable tracing.
The trace records bounded samples and local session details. Review it before
sharing. No trace, executable, game asset or user settings are included in
the contribution.

Development was AI-assisted with Codex. Flavio directed the requested
behavior and performed the manual multi-PC gameplay testing. The draft asks
the upstream maintainer to review the source and determine integration scope.
