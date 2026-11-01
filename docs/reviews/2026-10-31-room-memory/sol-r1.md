CHANGES REQUIRED

1. **P2 — The upgrade can leave the old task limit active despite installing `TasksMax=2048`.** [server/ember-rooms/deploy/ember-rooms.service:87](/C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember-rooms/deploy/ember-rooms.service:87)

   An earlier resource-only deployment calls `systemctl set-property --runtime` with **all three limits**, potentially creating a `TasksMax=1024` override. This change’s new environment forces the restart path, which installs the unit and restarts without reconciling those overrides. Runtime overrides survive service restarts until reboot and take precedence over the main unit, so the service can remain at 1024 while setup’s warnings read 2048 from the staged file. This is a conditional upgrade failure, not a claim about the current VPS state. ([systemctl documentation](https://www.freedesktop.org/software/systemd/man/latest/systemctl.html# set-property), [unit precedence](https://www.freedesktop.org/software/systemd/man/latest/systemd.unit.html))

   **Fix:** Reconcile the managed resource properties on both reload and restart paths, preserve their previous effective values for rollback, and verify the resulting properties with `systemctl show`. Extend the test stub to retain resource overrides across restarts and cover upgrading from 1024 to 2048 alongside the environment change; it currently logs `set-property` without modeling its persistent effect.

2. **P2 — The CPU promotion gate does not enforce the capacity target it claims to protect.** [docs/development/PUBLIC_ROOM_CAPACITY_2026-10-31.md:77](/C:/Users/Kate/Desktop/sf4/sf4-nightly/docs/development/PUBLIC_ROOM_CAPACITY_2026-10-31.md:77)

   The target reserves at most **2.8 vCPU / 70%** for rooms, but step 3 authorizes 100 rooms if usage stays “near 3%” per room. At 3%, that is **3 vCPU / 75%**; even the measured 2.9% exceeds the stated budget. Additionally, `ps %cpu` averages over each process’s lifetime, so older quiet periods can dilute the load observed during the week at 80 rooms. ([procps `ps` documentation](https://man7.org/linux/man-pages/man1/ps.1.html))

   **Fix:** Make promotion conditional on representative interval measurements projecting to at most 2.8 vCPU at 100 rooms, or explicitly revise the CPU budget and headroom policy. Use sampled CPU-time deltas, as the existing soak monitor does, rather than a lifetime-average snapshot.

No blocker found in the allocator implementation itself: the Rust ABI and constants match glibc, the call order is valid, both hooks precede application thread creation, and the platform guards exclude Windows and musl appropriately. No pre-main thread creation was identified in the inspected sources and available locked dependencies. Explicitly setting the thresholds disables dynamic adjustment as intended. ([glibc definitions](https://raw.githubusercontent.com/bminor/glibc/master/malloc/malloc.h), [allocator documentation](https://sourceware.org/glibc/manual/2.35/html_node/Malloc-Tunable-Parameters.html))

The 40 MB warning arithmetic and revised test fixtures are consistent. Both scripts passed `bash -n`; the scoped diff passed `git diff --check`; no changed file crosses 1,000 lines. I reused your reported 110/110 deployment tests and clippy results; I did not rerun them or perform a Linux build or live acceptance test. No files were changed.

