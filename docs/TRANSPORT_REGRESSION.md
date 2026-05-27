# Netplay transport regression matrix

Run after transport stack changes (Phases 1–3). Complements [SMOKE_TEST.md](SMOKE_TEST.md).

## Defaults during rollout

- VPS: `BROKER_GGPO_TRANSPORT=legacy` until UDP soak passes, then `auto`
- Client: `SF4E_GGPO_TRANSPORT` unset or `legacy` matches production default

## Matrix

| # | Mode | Steps | Pass |
|---|------|-------|------|
| 1 | Legacy VPS | `SF4E_GGPO_TRANSPORT=legacy`, Get code, 2P match, rematch | Session tunnel works; dev overlay shows tunnel sends |
| 2 | UDP relay | VPS `BROKER_GGPO_TRANSPORT=auto`, client `SF4E_GGPO_TRANSPORT=udp`, 2P match | Overlay transport `udp_relay`; no desync |
| 3 | Auto ladder | VPS + client `SF4E_GGPO_TRANSPORT=auto`, 2P match | Match completes; dashboard shows `transportActive` |
| 4 | UDP fallback | Block ggpo UDP port on client firewall, `auto` | Match still starts via legacy tunnel; `transport_fallback` event on dashboard |
| 5 | P2P LAN | Same LAN, `auto`, both register endpoints | Prefer `p2p` when broker predicts punchable |
| 6 | Join path | Guest joins via SF4- code only | Connect plan applied; guest registers endpoint |
| 7 | Prune | Idle room past timeout | Both session + ggpo relay processes stopped on VPS |
| 8 | Version | Launcher v5 config + old Sidecar v4 | Clear version mismatch, no silent failure |

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

## Flip production default

Only after rows 1–4 pass on two machines:

1. Set VPS `BROKER_GGPO_TRANSPORT=auto`
2. Ship launcher with default `auto` (optional env still overrides)
3. Monitor dashboard Matches panel for `transportActive` and disconnect events
