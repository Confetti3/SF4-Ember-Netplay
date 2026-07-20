# SF4 Netplay Launcher v0.6.0-rc1 (release candidate — for testing)

> **Experimental unofficial port** — not production-ready, friends-only test
> software. Sessions may fail. See docs/SCOPE_AND_LIMITATIONS.md.

This RC is a netplay smoothness and observability build. It changes how the
game behaves during connection hiccups, removes two hidden per-frame sleeps,
and adds deep rollback telemetry. **It needs live two-player testing before
it can ship as v0.6.0.**

## Install

1. Download **`sf4-netplay-launcher-*-0.6.0-rc1.zip`** from the release assets.
2. Extract the full zip to a new folder.
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.
4. Confirm both players show **v0.6.0-rc1** and host a fresh room.

Both players must use the same release (the room will reject mixed builds).

## What changed

- **Brief connection hiccups no longer freeze the fight.** Previously a
  "connection unstable" warning instantly paused the game until the network
  recovered. Now play continues on prediction and only stalls when rollback
  genuinely runs out of frames. A resume can also no longer un-pause a
  debug pause or revive an aborted match.
- **Removed a hidden 1–16 ms sleep every frame during matches** (GGPO's
  polling timeout) and the **50–150 ms one-shot freeze** GGPO's speed-sync
  used when one player ran ahead. Speed corrections are now spread out at
  ≤3 ms per frame, so catching up feels like a slight slowdown instead of a
  hitch.
- **New rollback diagnostics** (off by default): set
  `SF4E_ROLLBACK_DIAGNOSTICS=1` to log frame-time/rollback/stall telemetry
  every 10 s and a full summary at match end.
- **New desync detection (v2, observational)**: peers exchange state
  checksums every half second and log exactly which subsystem diverged.
  It does NOT end matches in this RC (the old detection still does);
  `SF4E_STRICT_DESYNC=1` enables strict mode for testing.
- **Losing the room server mid-fight no longer kills the fight** (UDP relay
  and direct matches): the match continues, rematch/results are disabled,
  and the game exits to the menu cleanly afterward.

## What to test (please report!)

1. Normal matches at your usual delay — does pacing feel smoother? Any new
   stutter or CPU increase?
2. Briefly disrupt one player's network (5–10 s of loss): the fight should
   show one warning, keep playing or stall briefly, then recover — not
   freeze on the first warning. A long outage should still end the match.
3. Rematches, training room, spectators — same as v0.5.0 (no regressions
   expected; audio double-trigger fixes from v0.5.0 must still hold).
4. Watch for any `Desync v2:` lines in the logs (with diagnostics on) —
   report them even if the match felt fine.

## Known limitations

- Rollback distance in the telemetry is a proxy (resimulation bursts), not
  an exact number.
- v2 desync checks are diagnostics-only in this build.
- If the room connection is lost mid-fight, that portion of the match is
  unverified by desync detection (logged as such).

Trouble? See docs/TROUBLESHOOTING.md.
