CHANGES REQUIRED

1. **P2 — Rollback restores the unit file’s limits, not the previous effective limits.** [server/ember-rooms/deploy/setup.sh:572](/C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember-rooms/deploy/setup.sh:572)

   Applying the staged limits before restarting fixes the original 1024-to-2048 upgrade case. However, `old_limits` comes from the backed-up main unit, which excludes runtime overrides and drop-ins. If those differ—for example, the unit says `TasksMax=1024` but the service effectively allows 2048—a failed upgrade now writes 1024 during rollback and reports restoration even though the previous operating limits were 2048. This extends the existing rollback weakness to every restart-based upgrade. Runtime property changes remain effective until reboot. ([systemctl documentation](https://www.freedesktop.org/software/systemd/man/latest/systemctl.html#set-property%20UNIT%20PROPERTY=VALUE%E2%80%A6))

   **Fix:** Capture effective `MemoryMax`, `MemorySwapMax` and `TasksMax` with `systemctl show` before installing the new unit or changing properties, and restore that snapshot on failure. Verify effective values after applying and restoring them. Extend the stub to retain overrides across restarts and assert successful upgrade and failed-upgrade restoration; the added test only counts logged commands and cannot catch this mismatch.

2. **P2 — The interval CPU snippet can substantially underreport usage when rooms close.** [docs/development/PUBLIC_ROOM_CAPACITY_2026-10-31.md:84](/C:/Users/Kate/Desktop/sf4/sf4-nightly/docs/development/PUBLIC_ROOM_CAPACITY_2026-10-31.md:84)

   Each `cpu()` call sums lifetime CPU counters for the processes alive at that moment. Subtracting these totals works only while the process set stays unchanged. When a room closes during the five-minute interval, its entire accumulated CPU history disappears from `b`, subtracting historical work from the interval measurement. For example, 80 rooms consuming 720 CPU-seconds would measure 3% per room; removing a departing room’s 600 previously accumulated CPU-seconds makes the snippet report about **0.5%** across the remaining 79 rooms. It can therefore authorize promotion despite usage exceeding the corrected 2.8% gate. These `/proc` fields are cumulative counters belonging to individual processes. ([Linux proc documentation](https://man7.org/linux/man-pages/man5/proc_pid_stat.5.html))

   **Fix:** Use the service cgroup’s cumulative CPU counter, which retains usage from exited children, together with representative room-count sampling. Alternatively, restrict this short snippet to stable intervals: snapshot PID/start-time identities and room count, reject changed sets or failed reads, and require a nonzero denominator. Do not accept a sample affected by process churn as promotion evidence.

No additional blocker found in the allocator hooks, platform guards, environment settings or 40 MB warning arithmetic. The startup hooks precede application thread creation; I reused round 1’s unchanged dependency assessment. No changed file crosses 1,000 lines.

The scoped `git diff --check` and separate `bash -n` checks for both scripts passed. I reused your reported **111/111 deployment tests** and clippy results; I did not rerun the suite, build Linux binaries or perform live acceptance testing. No files were changed.

