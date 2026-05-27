# Netplay transport benchmark

Measure baseline and post-change transport performance. Run after netplay transport changes.

## Prerequisites

- Two PCs (or one PC + VM) with matching `Launcher.exe` + `Sidecar.dll`
- Dev overlay: set `SF4E_NETPLAY_DEV=1` or enable in launcher Advanced
- VPS broker reachable for relay tests

## Scenarios

| ID | Setup | Expected transport |
|----|-------|-------------------|
| A | LAN direct IP, relay off | Direct GGPO |
| B | VPS relay, `SF4E_GGPO_TRANSPORT=legacy` | Session tunnel |
| C | VPS relay, `SF4E_GGPO_TRANSPORT=udp` | UDP GGPO relay |
| D | VPS relay, `SF4E_GGPO_TRANSPORT=auto` | Best available |

## Per-match record (3+ matches per scenario)

Record in a spreadsheet or log:

| Field | How to capture |
|-------|----------------|
| Scenario ID | A/B/C/D |
| Ping (ms) | In-game overlay during round 2 |
| Rollback frames | Dev overlay transport stats + subjective stutter |
| Match duration | Round count x ~99s or rematch count |
| Desync dialog | Yes/No |
| Transport active | Dev overlay: legacy / udp / p2p |
| GGPO tunnel sends | Dev overlay `tunnel send` count (legacy only) |
| GGPO relay frames | Dev overlay `relay out` count (legacy only) |

## VPS cold-path baseline

```bash
curl -s http://74.208.200.95:8787/v1/health
time curl -s -X POST http://74.208.200.95:8787/v1/rooms \
  -H "Content-Type: application/json" \
  -d '{"displayName":"bench","sidecarHash":"<64-char-hex>"}'
```

Note room create latency and whether `ggpoPort` is returned when UDP relay is enabled.

## Exit criteria (Phase 0)

- At least 3 completed matches recorded for scenario B (legacy VPS tunnel)
- Dev overlay shows non-zero tunnel stats during legacy matches
- Numbers saved for comparison after Phase 1 UDP relay rollout
