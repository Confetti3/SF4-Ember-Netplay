# Shared brief: whole-codebase thermo-nuclear code quality review

Repository under review: C:\Users\Kate\Desktop\sf4\sf4-nightly  (git worktree, branch `nightly`, head b9ce0e31).
Review the code AS IT STANDS at that head. There is no PR diff; the whole tree is in scope.

## Hard rules
- READ-ONLY. Do not edit, create, delete or move any file inside sf4-nightly. Do not run cargo build, cargo test,
  cmake, ninja, npm install, npm test, or any build/test. Do not run git commands that change state
  (no checkout, stash, commit, reset). `git log`/`git blame`/`git show` are fine.
- Do not read: build/, dist/, vcpkg*, sdk/typescript/node_modules, src/ui/backends/imgui_impl_*.cpp (vendored ImGui),
  assets/, locales/ (except when a finding needs them).
- Write your findings ONLY to the output file named in your task prompt (under the scratchpad findings/ folder).
  Return a short summary (under 300 words) as your final message; the file carries the detail.

## The review standard (adapted from the thermo-nuclear-code-quality-review skill)
Full skill text: C:\Users\Kate\.claude\skills\thermo-nuclear-code-quality-review\SKILL.md  (read it first).
Because the scope is a whole tree, not a diff, read the skill's rules this way:
- "Files pushed past 1k lines" becomes: files already over ~900 lines — is there an obvious decomposition that
  would materially improve maintainability? Say what the seams are.
- Look hardest for "code judo": restructurings that preserve behavior while deleting whole branches, modes,
  helpers or layers. Prefer deleting complexity over rearranging it.
- Flag spaghetti growth: ad-hoc conditionals, scattered special cases, feature logic leaking into shared paths,
  one-off booleans/nullable modes, copy-pasted logic where a canonical helper exists, thin wrappers.
- Flag boundary/type problems: silent fallbacks papering over unclear invariants, cast-heavy or loosely-shaped
  objects where a typed model should exist.
- Prefer a small number of high-conviction findings over a flood of nits.

## Project-specific focus (the owner's actual ask)
The owner wants to LOWER EMBER'S RUNTIME OVERHEAD (CPU, bytes on the wire, allocations, serialization churn)
and improve STREAMING/incremental delivery of state, WITHOUT changing behavior. In particular she remembers
"a large JSON file that is sent every 15 seconds" and wants that kind of thing found and reduced.
So in your area, in addition to the quality review, actively look for:
- recurring sends (timers, intervals, leases, heartbeats, polls) and what they carry; full-snapshot resends where a
  delta, a digest-skip, or send-on-change would preserve behavior
- JSON serialized/parsed more than once per hop; pretty-printing on hot paths; serde_json::Value round trips;
  nlohmann::json round trips; string copies of payloads; per-tick allocations
- fan-out multipliers (per peer × per member × per table)
- work done every tick that could be event-driven
For every overhead finding give: payload type, rough serialized size, interval, fan-out, transport
(pipe / QUIC / HTTP / stdout), and an estimate of bytes/s or CPU saved by the proposed change.

## Prior art you must respect (read before proposing anything in these areas)
- docs/perf/ROOM_PERF_BASELINE.md : nine optimizations already landed; per-table room deltas were deliberately NOT
  pursued and the doc says why. Do not re-propose what is already done; do build on it.
- docs/design/NETPLAY_INVARIANTS.md, docs/design/GGPO_LIFECYCLE.md, docs/design/DESYNC_V2.md : determinism and
  timing invariants of the rollback sim.
- docs/design/PUBLIC_ROOMS.md, docs/development/SERVER_TRAFFIC.md : what travels where and why.
- docs/reviews/2026-09-code-quality-review.md and docs/reviews/2026-09-28-v1.0-review.md : earlier reviews.
Cross-version compatibility matters: older clients exist in the wild (example: `roomChatDelta` advertised at join;
older clients get the full chat). Any wire or JSON shape change must say how old clients keep working.

## Finding schema (use EXACTLY this, one block per finding, so consolidation is mechanical)
### F-<area>-<n>: <one-line title>
- File: <path>:<line>  (add more File: lines if needed)
- Category: structural | judo | spaghetti | boundary | size | overhead | streaming
- Severity: P1 (clear structural regression or major overhead) | P2 (worth doing) | P3 (nice)
- Claim: <what is wrong, with evidence from the code>
- Proposed change: <concrete; name the functions/types that change>
- Behavior preserved: <what stays identical>
- Behavior at risk: <what could change: determinism, wire compat, timing, old clients; or "none identified">
- Estimated saving: <bytes/s, CPU, lines deleted, concepts removed; or n/a>
- Confidence: high | medium | low

End the file with a `## Area summary` of 5-10 lines: overall health, the biggest judo move, the biggest
overhead item, and anything you could not assess.
