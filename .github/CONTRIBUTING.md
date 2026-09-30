# How to contribute

> **SF4 Ember Netplay** is an **experimental unofficial port**, not production-ready software. Bug reports help improve a friends-only test build; do not present this project as stable or "working" netplay for general use.

## Testers and players: Submitting bugs

For questions, match-ups and testing sessions, join the
[Ember Discord](https://discord.gg/uPNqF5A5uq). Bugs still belong in Issues.

**Please do**:
* Submit issues when you find a bug! The issue template
  has a good checklist of things to include when submitting an issue. Issue
  reports without a complete description may be rejected outright.
* Include video evidence when sending issues! Video is extremely helpful
  for both describing and reproducing issues. If you have the computing
  resources to spare, you may want to consider recording tests with
  [OBS](https://github.com/obsproject/obs-studio) or another screen-recording
  solution.
* Attach the logs described in [Saving logs for a bug report](../docs/guides/SAVING_LOGS.md).

**Please do not**:
* Submit feature requests. As players, we already have a lot of features in mind,
  to the point where we could work on this project as if it was a full time job.
* Report security problems in a public issue. Follow [SECURITY.md](SECURITY.md) instead.

## Contributors: Working on the code

### Where to start reading

| Area | Read |
| --- | --- |
| Overview | [README](../README.md), [docs map](../docs/README.md), [scope and limitations](../docs/guides/SCOPE_AND_LIMITATIONS.md) |
| Building | [BUILDING](../docs/development/BUILDING.md) |
| Architecture | [EMBER_IMPLEMENTATION](../docs/design/EMBER_IMPLEMENTATION.md) |
| Rules you must not break | [NETPLAY_INVARIANTS](../docs/design/NETPLAY_INVARIANTS.md), [GGPO_LIFECYCLE](../docs/design/GGPO_LIFECYCLE.md), [SAVESTATE_FREE](../docs/design/SAVESTATE_FREE.md) |
| Match integrity | [DESYNC_V2](../docs/design/DESYNC_V2.md), [NATIVE_MATCH_RESULT](../docs/design/NATIVE_MATCH_RESULT.md) |
| Room behaviour | [CUSTOM_ROOMS](../docs/guides/CUSTOM_ROOMS.md) (the player-facing rules double as the spec) |

Code entry points:

* **In-game DLL:** [`src/sidecar/sidecar.cxx`](../src/sidecar/sidecar.cxx) installs the hooks; [`src/sf4e/sf4e__Overlay.cxx`](../src/sf4e/sf4e__Overlay.cxx) draws the overlay and handles window messages.
* **Rollback:** [`sf4e__Game__Battle__System__Ggpo.cxx`](../src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx) (GGPO callbacks) and [`sf4e__Game__Battle__System__SaveState.cxx`](../src/sf4e/sf4e__Game__Battle__System__SaveState.cxx) (save and restore).
* **Netplay runtime:** [`sf4e__NetplayFacade.cxx`](../src/sf4e/sf4e__NetplayFacade.cxx) and the `sf4e__NetplayRuntime*.cxx` files beside it.
* **Rooms:** [`src/session`](../src/session) (SessionServer, SessionClient, IrohRoom, MatchAuthority, RoomModel).
* **Menus:** [`src/ui`](../src/ui) (ApplicationShell, RoomPanel, FighterSelector, GameMenu, MenuNavigation).
* **Launcher and updater:** [`src/launcher`](../src/launcher).
* **Networking helper (Rust):** [`rust/sf4-net/src`](../rust/sf4-net/src), starting at `lib.rs`.
* **GGPO patches:** [`vcpkg-overlays/ports/ggpo`](../vcpkg-overlays/ports/ggpo).

### Build and test

* Build with [`scripts/build-current.ps1`](../scripts/build-current.ps1) and package with [`scripts/package-team.ps1`](../scripts/package-team.ps1), as [BUILDING](../docs/development/BUILDING.md) describes. A package must come from a build that ran the tests.
* Every change comes with a test beside the code it touches, registered in [`CMakeLists.txt`](../CMakeLists.txt) under [`src/tests`](../src/tests). Rust changes add tests to the crate and pass `cargo test`.
* A test that proves a fix should fail without the fix. Check that by reverting the fix locally once.
* Changes to rooms, recovery or the helper also run the network fixtures (`Iroh*Test`, `CustomRoomGameTest`) one at a time, with nothing else heavy running.
* Tests are not gameplay. Say so when a change still needs a real match between two PCs.

### Code quality bar

Every change is reviewed against this bar before it merges. Working code is not enough on its own.

**Look for the simpler design first.** Before polishing an implementation, ask whether a different structure makes whole branches, flags, modes or helper layers disappear. Prefer the change that deletes complexity over one that moves it around. If a restructuring of nearby code makes the feature a natural extension of what exists, do that.

**Presumptive blockers.** A change that does any of these needs a strong, stated reason:

* Pushes a file from under 1,000 lines to over 1,000. Decompose first; find the boundary where responsibilities change for different reasons.
* Adds ad-hoc conditionals or one-off special cases to an existing flow. Move the logic behind its own abstraction, helper or state model instead.
* Leaves an obvious simplification on the table and keeps incidental complexity.
* Adds a thin wrapper, pass-through helper or indirection that does not make anything clearer.
* Duplicates an existing helper, or puts logic in the wrong layer when a canonical home exists.
* Scatters feature checks across shared code.
* Hides a real invariant behind optional values, casts, silent fallbacks or loosely shaped data. Make the boundary explicit and typed.

**Also flag:**

* Two parallel fields or functions that only differ by one value: one typed value or one shared path usually replaces them.
* State that can be left half updated. Prefer a structure where related updates happen together.
* Serial orchestration of independent work when a simpler parallel shape exists.
* "Temporary" branches that will become permanent.

**Questions to ask of every change:**

* Is there a move that makes this dramatically simpler?
* Can fewer concepts, branches or layers express it?
* Does it improve or worsen the local architecture, and is the logic in the right file and layer?
* Did a file or function become harder to scan?
* Does each abstraction earn its keep?

Prefer direct, boring code over clever code. Push hard on structure, not on renames.

### Project conventions

**Threads and the game.** Sidecar code runs inside SSFIV.exe, where the window's messages, the drawing and the simulation can each run on a different thread.

* Only the drawing thread touches ImGui. Window-message input reaches it through the input bridge (`Win32InputBridge`), whose lock never wraps a Windows call.
* GGPO callbacks never destroy the session. `AbortGgpoMatch` defers that to the outer tick, and every callback leaves GGPO's frame count consistent.
* Hold a lock only while moving shared data, never across a call that can re-enter or wait on another thread.

**Localization.** All player-facing text lives in the 14 catalogs in [`locales`](../locales), as [locales/README](../locales/README.md) describes:

* Reference each key as a literal `loc::T("area.key")` at the call site, so the source-coverage test can find it.
* Add the key to every catalog in the same position.
* Regenerate the CJK font subsets after changing `ja.po`, `ko.po` or `zh-Hans.po`.

**Packaging.** [`src/common/PackageInventory.inc`](../src/common/PackageInventory.inc) is the one list of shipped files. Packaging, preflight and the updater all read it.

**Dependencies.** Changes to GGPO go in a new patch under [`vcpkg-overlays/ports/ggpo`](../vcpkg-overlays/ports/ggpo), with a `port-version` bump. Logic that both the patch and a test need goes in a header the port copies in, like `input-repair.h`.

**Vendored code.** Edits to vendored files, such as the ImGui Win32 backend in [`src/ui/backends`](../src/ui/backends), stay small and are marked `SF4 Ember` so a future upgrade can find them. Put the real logic in project files.

**Comments** explain why, in plain sentences. They describe what the code guarantees. Match the surrounding code's naming, density and idiom.

**Writing.** Commit messages, comments, docs and release notes use plain, brief prose and no em dashes.

**Commits:**

* One logical change per commit.
* A short imperative subject line.
* A body that says in a few sentences what was wrong and what changed.
* Describe fixes plainly and do not spell out how a problem could be abused. Report security problems privately through [SECURITY.md](SECURITY.md).

**Release notes** describe what players will notice. Keep them short and concrete.
