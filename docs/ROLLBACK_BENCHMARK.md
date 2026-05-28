# Rollback benchmark (internal)



Measure infrastructure and tune **host input delay** using the in-game bottom overlay (**RTT**, **LFB**, **RFB**). Launcher UI stays as-is.



## Quick start



```powershell

cd C:\Users\ptwar\Desktop\SF4e



# 1) Automated probe + delay presets (no game)

python scripts/netplay-rollback-benchmark.py probe



# 2) Log real matches (bottom GGPO bar during fights)

python scripts/netplay-rollback-benchmark.py template -o benchmark-results/matches.csv

# Play 3+ matches per delay; edit CSV with peak LFB/RFB from overlay



# 3) Pick best delay from your logs (scenario D = production VPS path)

python scripts/netplay-rollback-benchmark.py analyze benchmark-results/matches.csv --scenario D

python scripts/netplay-rollback-benchmark.py analyze benchmark-results/matches.csv --probe-json benchmark-results/probe-<timestamp>.json

```



Optional env: `SF4E_BROKER_URL`, `SF4E_VPS_HOST`, `SF4E_PACKAGE_DIR`.



## What the probe measures



| Probe | Gates exit? | Purpose |

|-------|-------------|---------|

| ICMP ping to VPS | No (WARN if parse fails) | Baseline RTT when available |

| NAT probe `:8790/udp` | No | One-way RTT + public IP/port; used for RTT estimate if ICMP fails |

| Broker `/v1/health` | **Yes** | `brokerGgpoTransport` should be `auto` |

| Room + GGPO port | **Yes** | UDP relay path alive (`ggpoPort` >= 24456) |

| `transport-regression-test.py` | **Yes** | Full infra regression (18 checks) |



On Windows, ICMP may show `[WARN]` while probe still exits 0. The script then estimates RTT from the NAT probe (one-way × 2).



**Presets** (starting points for host **input delay** in launcher):



| Preset | Use when |

|--------|----------|

| **responsive** | Low RTT, accept more rollbacks |

| **standard** | Default balance |

| **smooth** | WAN / high LFB/RFB at standard — stability first |



Probe picks **smooth** when it recommends more frames than standard and broker is `auto`.



## Manual match log (ground truth)



During **round 2+**, watch the **bottom** GGPO overlay (shown when netplay ping is available):



| Column | Source |

|--------|--------|

| `rtt_ms` | Bottom overlay **RTT ms** |

| `lfb_max` / `rfb_max` | Peak **LFB** / **RFB** that round |

| `transport_active` | **Dev overlay only** (`SF4E_NETPLAY_DEV=1`): `Transport: udp_relay` / `p2p` / `legacy` |



The bottom bar does not show transport mode; log transport from the dev overlay when comparing scenarios B/C.



**Scenarios** (same as [NETPLAY_TRANSPORT_BENCHMARK.md](NETPLAY_TRANSPORT_BENCHMARK.md)):



| ID | Setup |

|----|--------|

| D | Simple VPS, broker `auto` (production path) — default for `analyze --scenario D` |

| C | Force UDP relay (if testing env allows) |

| B | Legacy tunnel only (comparison) |

| A | LAN direct (optional) |



**Good rollback at chosen delay:** LFB/RFB usually 0–2, rare visible rewinds, no desync.



**Tune:** If LFB/RFB often 3+, host raises **input delay** +1 in launcher and adds rows to CSV.



### Delay sweep (internal validation)



For each host **input delay** to test (start at probe suggestion, then ±1):



| delay | matches | Log per match |

|-------|---------|---------------|

| N | 3+ | `rtt_ms`, `lfb_max`, `rfb_max`, `transport_active`, `desync` |



Run `analyze` after at least two delays with 3+ matches each.



## Recommended workflow



1. Run `probe` on **both** players’ PCs (compare est. RTT and suggested delay).

2. Host sets input delay to probe suggestion.

3. Play 3+ matches per delay (scenario D), log CSV peaks.

4. Run `analyze --scenario D` (optional `--probe-json` from step 1).

5. Re-run `transport-regression-test.py` after VPS/client changes.



## Output files



- `benchmark-results/probe-<timestamp>.json` — automated probe snapshot (gitignored)

- `benchmark-results/matches.csv` — manual fight logs (gitignored)



## Relating to implementation work



| Observation | Action |

|-------------|--------|

| Legacy transport in most rows | Fix udp_relay / rematch (engineering) |

| High LFB/RFB at delay=2, udp_relay | Raise host delay or improve VPS region |

| Probe FAIL on transport regression | Fix VPS before tuning delay |

| Good LFB/RFB, legacy rare | Current v0.3.6 path is working |

| Probe smooth, matches stable at standard | WAN hosts use smooth; LAN-like paths may use standard |


