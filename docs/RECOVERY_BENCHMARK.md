# Current helper comparison

The long-run figures retained below describe an earlier helper and must not be
reported as measurements of the final September 10 recovery-election repair.
The closing handoff records any final bounded comparison separately, including
the actual helper hash and route. A short comparison cannot establish long-run
resource growth or statistical performance equivalence.

`RecoveryBenchmark` is built with the current integration target. It exercises
real GGPO traffic through two isolated Iroh helpers using synthetic input/state
callbacks. It does not launch SF4, write the player's profile, or establish
native gameplay acceptance.

Build with `scripts/build-current.ps1`. Run the explicit comparisons from
`sf4-current` after other local compiler and helper workloads have stopped:

```powershell
./scripts/recovery-benchmark.ps1 -Frames 1800 -TimeoutMs 90000 -SkipGgpo -BaselineGgpoDll build/current/GGPO.dll -CandidateGgpoDll build/current/GGPO.dll -OutputDirectory build/benchmark-default-new
./scripts/recovery-benchmark.ps1 -Frames 1800 -TimeoutMs 90000 -SkipGgpo -RelayOnly -BaselineGgpoDll build/current/GGPO.dll -CandidateGgpoDll build/current/GGPO.dll -OutputDirectory build/benchmark-relay-new
```

Choose fresh output directories for each attempt. Explicitly selecting the same
GGPO DLL prevents a preserved adjacent DLL from changing one side's runtime.
The minimum comparative workload is 240 frames. Longer runs reduce the effect
of process CPU counter granularity. The 90-second phase timeout allows the
30-second forward workload to finish with prediction stalls; it does not
change the requested frame count or pace. Use the harness directly for short
diagnostics. Default seed is `0x5f4e2026`; the workload advances at approximately
60 Hz with two milliseconds of proxy delay and every seventeenth packet
dropped independently in each direction. Synchronization is unmodified; each
worker starts its seeded impairment sequence at the quiescent prework barrier.
Input delay remains zero on both sides throughout this comparison.

The wrapper uses the preserved source's isolated observability build for the
baseline and `build/current/sf4-net.exe` for the candidate. Override
`-BaselineHelper` or `-CandidateHelper` explicitly when comparing another
recorded intermediate build. The original source snapshot and previous package
remain intact. `build/recovery-baseline-observability-v2.json` and
`build/recovery-baseline-observable.diff` identify the baseline's sole source
change: current selected-path observations sampled during the existing helper
statistics tick. Gameplay forwarding is unchanged.

Each run records exact helper, harness, and loaded GGPO DLL hashes. The wrapper
stages the selected DLL beside a unique runtime copy of the harness and rejects
a helper-only comparison when the two GGPO builds differ. Both helpers must use
the same locked Rust toolchain, Release profile, target, and static CRT flags.

Results are usable only when all measurement gates pass:

- Both peers accept exactly the requested forward-frame count, confirm input
  through frame N minus one, and finish in saved state N. Full 64-bit state
  digests must match an independent seeded-input oracle after rollback.
- Before and after the workload, helper packet and byte counters reconcile with
  the corresponding proxy boundaries. Postwork snapshots remain frozen while
  teardown events are processed. Unplanned local drops, malformed events,
  disconnects, forwarding errors, or incomplete teardown reject the result.
- GGPO stops polling before both proxy drains begin. Its sockets and helper
  mappings remain alive during drain and the final statistics barrier.
- The periodically sampled selected paths remain unchanged during each run.
  Baseline and candidate must use matching paths after excluding ephemeral UDP
  ports. A mismatch preserves the raw runs but produces no comparative deltas.
  Path changes between statistics samples remain unobservable.

Forwarding cost is total helper CPU amortized per successful QUIC enqueue or
local UDP delivery over the workload and drain interval. It includes room
coordination overhead; it is not isolated bridge latency. CPU excludes startup,
the final statistics wait, and teardown. A zero CPU delta means the process
counter reported no increment during that interval, not that forwarding is
free. Working-set start/end/growth and sampled peaks are reported separately
from OS process-lifetime peaks.

Prediction stalls, rollback callback loads, maximum replay depth, and GGPO's
cached RTT estimates accompany these costs. The cached RTT is not a direct
forwarding-latency measurement. Cross-peer enqueue-minus-delivery counts are
reported as transport residuals, separately from planned proxy loss.

The optional `ggpo` phase is a separate loopback diagnostic; the wrapper never
uses it as evidence for the Iroh helper comparison. Re-run the same seeded
workloads against the final build after correctness fixes, and retain rejected
route-mismatch runs alongside accepted comparisons.

## September 10 measured comparison

Both 1,800-frame paired workloads passed all confirmation, independent state
oracle, packet/byte reconciliation, stable path, and teardown gates. The seed,
impairment schedule, frame pace, harness, and GGPO DLL were identical. Default
routing selected direct IPv4 `172.28.96.1` at both ends for both builds; forced
relay selected `https://use1-1.relay.n0.iroh.link./` at both ends for both builds.
These are one paired run per route, not statistical proof of a speedup or a
long-duration memory-leak test.

| Measurement | Direct baseline | Direct candidate | Relay baseline | Relay candidate |
| --- | ---: | ---: | ---: | ---: |
| Helper CPU, milliseconds | 781.25 | 734.375 | 1,390.625 | 1,296.875 |
| Amortized helper CPU per forwarding operation, microseconds | 111.83 | 104.94 | 197.98 | 184.85 |
| Prediction stalls | 8 | 12 | 282 | 278 |
| Maximum replay depth, frames | 7 | 7 | 8 | 8 |
| Rollback loads | 2,955 | 2,939 | 2,872 | 2,854 |
| Working-set growth, bytes | 606,208 | 258,048 | 65,536 | 126,976 |
| Sampled peak working set, bytes | 49,283,072 | 58,470,400 | 46,940,160 | 54,231,040 |

The candidate used about six percent less amortized helper CPU in these runs,
with unchanged maximum rollback depth and four more direct / four fewer relay
prediction stalls. Its sampled memory peak was higher by 9,187,328 bytes on
direct and 7,290,880 bytes on relay. Interval growth remained small in both
builds; the peak includes the candidate's larger coordination runtime.

Exact artifacts:

- Baseline observable helper: `6b4a9ddee2b22cb8e7ffeb27d9906784d26c03fbda7326b55209a8aa5f25b127`.
- Candidate helper: `6359a91183608f9cd3b6831a601b78cc88f92db5d5a70b47c727871db7623037`.
- Shared GGPO DLL: `a1f0c2b5021cd8a1f53b3d1d048bf1d646eaf7d42a9b85dccf61ac2a748d0569`.
- Harness: `56e2f2244ce15ba863d68d945d683eae767de5702c36e9da74df4ca72639dd2b`.

Raw runs, process logs, loaded-DLL receipts, and comparisons are retained in
`build/recovery-final-benchmark-default-v1/` and
`build/recovery-final-benchmark-relay-v1/`. Their comparison files are
`comparison-20260910-001854.json` and `comparison-20260910-002019.json`.
The independent post-run check `build/recovery-final-benchmark-artifact-verification.json`
confirmed both comparisons still match all four executed artifact hashes.
Later builds may reuse this evidence only if all four artifact hashes match.
