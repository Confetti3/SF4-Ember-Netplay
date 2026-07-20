# Rollback hardening live A/B validation

This branch already performed materially better than the previous build in a
real two-player match: connection and rollback behavior improved, occasional
freezes recovered much faster, and the rollback-audio problem did not
obviously return. This is evidence for preserving the current prediction and
distributed-pacing behavior, not a production-readiness claim.

Enable rollback diagnostics on both machines:

```text
SF4E_ROLLBACK_DIAGNOSTICS=1
```

Distributed GGPO time-sync pacing is enabled by default. Select the A/B arm
before launching:

```text
SF4E_GGPO_DISTRIBUTED_TIMESYNC=1
SF4E_GGPO_DISTRIBUTED_TIMESYNC=0
```

Keep transport mode, input delay, game settings, and network path identical
between the pacing-on and pacing-off runs.

## Two-machine matrix

1. Play a normal match with current defaults.
2. Repeat the same path and input delay with distributed pacing enabled.
3. Repeat the same path and input delay with distributed pacing disabled.
4. Play several rematches in each A/B arm.
5. Introduce a brief network interruption that recovers.
6. Introduce an interruption long enough to reach GGPO's prediction threshold.
7. Exercise rollback-heavy inputs and rapid direction changes.
8. Close the match shortly after a visible freeze.
9. Lose the room/control connection while direct or UDP-relay GGPO remains
   healthy. Record which player lost the room connection.
10. Observe audio during rollback, freeze, and recovery.

For every visible freeze, collect the following from both peers:

```text
approximate timestamp
observed freeze duration
whether audio continued
whether local input repeated or stopped
transport mode
input delay
ping
threshold refusal count
continuous threshold-stall duration
rollback burst size
Save/Load/Free maximum
pacing requested and actual wait
FreezeCandidate record
recovery behavior
```

`FreezeCandidate` is rate-limited and reports a complete detoured outer-tick
duration of at least 25 ms. `steam_post_update` remains explicitly scoped to
the original engine callback and must not be interpreted as total rendered
frame time. Periodic and closing summaries include per-operation counts at
16.67, 25, 33.33, 50, and 100 ms.

The pacing close summary records whether pacing was enabled, recommendations,
accepted/replaced/disabled/reset-discarded debt, requested and actual waits,
maximum requested and actual wait, timer failures/timeouts/fallbacks, and
outstanding debt.
