# Rematch recovery and connection checks

The reported follow-up symptoms were changing rematch buttons/status text, a dropped match connection that never returned to a usable room, and an unavailable connection check. The normal game log did not contain this play session; these repairs are grounded in reproduced production state-machine failures, not a claim to have replayed that exact match.

The helper required a published fighter selection before authorizing a probe. The player UI publishes that selection on Ready, after offering Check connection. A failing Rust regression reproduced this ordering. Probes now require authenticated room membership, occupied opponent seats, and the exact table revision, without requiring a character selection. The real helper/GGPO integration scenario checks both seated players before sending selections.

The application combined remote connection health with local checkpoint delivery. An ordinary revision could therefore announce recovery and clear pending Ready. SessionController now keeps those observations separate: unfinished application still blocks mutations, while a healthy connection remains healthy. The application observes the completed tick once, so it does not reset the recovery clock with an intermediate healthy observation.

Local abort completion previously required healthy remote control. Its failing regression left the UI Playing and prevented room replacement after the opponent disappeared. Local native/helper retirement now moves a coordinated session to PostMatch while preserving Lost control and blocking Ready. The application runs local cleanup even during a control outage and closes the room on a terminal helper teardown timeout. Native socket retirement and helper closure remain required before resource reuse.

`session-<pid>.log` under the settings directory's `logs` folder records lifecycle changes independently of the optional game logger. Each file is capped at 2 MiB and contains state codes and errors, with no invitations, player names, or endpoint addresses.

Validation commands: `scripts/build-current.ps1`, the `SessionControllerTest` regression, the Rust `probe_binding_requires_committed_fighter_pair_and_exact_revision` test, and `IrohAuthorizedMatchTest` with the freshly built helper. The designated build receipt and `build/current/rematch-followup-*.log` files retain the final validation outcomes. Local helper/GGPO runs do not establish native SF4 or two-PC poor-connection gameplay acceptance.
