# Training Lab implementation history

Developer notes moved from the [Training Lab player guide](../guides/TRAINING_LAB.md). They record how the overlay was built and validated; nothing here is a player instruction.

## Current flyout implementation

The designated integration target is `sf4-current`; build and package it through `scripts/build-current.ps1` and `scripts/package-team.ps1`. `DrawTrainingFlyout` owns the same fixed window used by the live overlay and the render harness, drops controller menu input, and uses keyboard legends. The native publication filter captures all gameplay input only while Ember is open or closing inputs are draining; training simulation and meter calculations are unchanged.

The render harness covers ten viewport/DPI configurations, including narrow 150% and 720p 200% layouts, long command errors, pending/rejected commands, disabled actions, safe overwrite confirmation and keyboard HUD prompts regardless of connected controller type. It checks centered bounds, contained children, a non-scrolling outer panel, passive input ownership and unchanged background pixels outside training screenshots. Keyboard journeys exercise the actual flyout and shared navigation model while unwanted controller input is discarded. Native Start pause behavior and gameplay input isolation still require user runtime acceptance.

## Historical implementation and validation

Worktree: `C:/Users/Kate/Desktop/sf4/sf4-training`, branch `feat/sf6-style-training`, based on `ef29cc4` (combined Ember/custom rooms). The existing checkouts were preserved. No game installation, game launch, push or PR was performed.

The x86 RelWithDebInfo `Sidecar.dll` builds with VS 18 Build Tools. The game-independent training tests cover inactive/stale generations, slot bounds, recording limits, repeatable frame reads, playback order/looping, teardown/reset isolation, action changes, held exchanges and missing samples. The DX9 harness exercises the actual panel and passive HUD across seven viewport/DPI configurations. These checks do not prove native training behavior.

Build output: `build/training/Sidecar.dll`. Native reader evidence and remaining phase-data work are recorded in `2026-09-08_reverse-USF4-training-report.md`.

Test1 validation, 8 September 2026: TrainingSession, NativeMatchResult, GgpoGate, SaveStateOwnership, OverlayPresentation and UiRender passed (6/6). UiRender covered 2,100 DX9 frames across seven viewport/DPI configurations with device resets. The user subsequently reported that the training viewer works and requested a smaller translucent layout with signed frame advantage.

Test2 validation: the x86 RelWithDebInfo build and TrainingSession, NativeMatchResult, GgpoGate, SaveStateOwnership, OverlayPresentation and PackageInstaller passed. UiRender passed 2,100 DX9 frames across seven viewport/DPI configurations, including HUD bounds and signed-value fixtures. The 720p and narrow 150% DPI captures were visually inspected. Package preflight verified manifest hashes and 1,794 artwork images; the actual package passed native updater inventory validation. Test2 Sidecar SHA-256: `c48a78a29a548dbef394059d6048c801e7c518fc15eec731680f21d7a46f65ae`. Package: `dist/sf4-netplay-launcher-training-20260908-test2.zip`. New native advantage timing remains for user testing.

Test3 validation: the failing action-chain regression now passes along with airborne/slow special phases, delayed projectile contacts, knockdown recovery, startup speed/freeze handling, follow-up startup, missing boundaries and reset isolation. TrainingSession, NativeMatchResult, GgpoGate, SaveStateOwnership, OverlayPresentation and PackageInstaller passed on the final build. The UI update passed 2,100 DX9 frames across seven viewport/DPI configurations; the 720p capture was visually inspected with advantage and startup beside each player. `git diff --check` passed. Final Sidecar SHA-256: `5565ce62ac3ec808af3945799c77c26cba55668e581e3ba307294b8d1240274e`. New native timing acceptance remains pending.

Before native acceptance, check meter progression in standing, crouching, walking, attacking, hit and guard states; pause/resume; a cancel; reset; character change; focus loss; and a normal online match with no training controls or overrides. Test dummy playback using the native Player setting. Full SF6 phase parity additionally needs verified active-hitbox, actionable/recovery and hitstop readers, plus gameplay correlation for normals, cancels, throws and projectiles.

The 2026-09-10 long-session repair separates recovery timestamps from the native
16-bit simulation counter. Previously, the signed half of that counter made
every recovery timestamp look unset, leaving advantage at `--` until training
was reloaded or the counter wrapped again. Continuity is now checked modulo
16 bits; recovery uses an independent elapsed-frame clock. Focused tests cover
both boundaries and 131,100 observations without reloading. Native long-session
acceptance remains pending; see [repair evidence](../validation/COSTUME_MENU_TRAINING_FIX.md).
