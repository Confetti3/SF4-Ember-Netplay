# Netplay transport regression matrix

Run after transport stack changes (Phases 1–3). Complements [SMOKE_TEST.md](SMOKE_TEST.md).

## Defaults during rollout

- VPS: `BROKER_GGPO_TRANSPORT=auto` (production default as of 2026-05-27)
- Client: unset env uses the broker connect-plan; VPS matches require UDP relay and never auto-fall back to the session tunnel
- Override: `SF4E_GGPO_TRANSPORT=legacy|udp|p2p|auto`

## Matrix

| # | Mode | Steps | Pass |
|---|------|-------|------|
| 1 | UDP initial match | VPS `BROKER_GGPO_TRANSPORT=auto`, two v0.4.8 clients, fresh room | Both log `SF4R`, then phases `Connected → Synchronizing → Running` |
| 2 | UDP rematch | Complete row 1, Ready again twice | Each generation gates on both slots; both rematches reach `Running` |
| 3 | Asymmetric load | Delay one player before entering battle | First player sees `SF4W`; neither starts alone; both eventually reach `SF4R` |
| 4 | UDP blocked | Block the allocated `24456–24505/udp` path (default `MAX_ROOMS=50`) | Clear timeout/abort to lobby; no asymmetric legacy fallback |
| 5 | Same public IP | Two clients behind one NAT | Explicit player slots pair and forward bidirectionally |
| 6 | Forced legacy diagnostics | `SF4E_GGPO_TRANSPORT=legacy` on both clients | Session tunnel works only as an explicit ops/debug override |
| 7 | Join path | Guest joins via SF4- code only | Connect plan applied; guest registers endpoint |
| 8 | Prune | Idle room past timeout | Both session + GGPO relay processes stop on VPS |
| 9 | Version | Mismatched Sidecar builds | Clear hash/version rejection; no silent failure |
| 10 | Malformed GGPO UDP | Run `ctest -R GgpoUdpValidation` | Late relay replies and malformed packets are dropped; a subsequent valid SyncRequest receives SyncReply |

## VPS checks

```bash
curl -s http://127.0.0.1:8787/v1/health
curl -s http://127.0.0.1:8788/v1/health
curl -s http://127.0.0.1:8788/v1/ggpo-sessions
```

After room create (with sidecar hash):

```bash
curl -s http://127.0.0.1:8787/v1/rooms/SF4-XXXX/health
```

Expect `ggpoRelayOk: true` when `BROKER_GGPO_TRANSPORT=auto`.

## Automated gate

Run `ctest --test-dir msvc-build/default -C RelWithDebInfo --output-on-failure -R GgpoUdpValidation`, then `python scripts/transport-regression-test.py` before the two-machine matrix. They must pass malformed-packet handling, v2 compatibility, v3 generations, socket handoff, forwarding, stale-generation isolation, and same-IP rebinding.
