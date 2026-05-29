# SF4 Netplay Launcher v0.2.7

Relay-first rollback netplay for **Ultra Street Fighter IV** (Steam) with a modern **SF4 Netplay Launcher** launcher.

## What's new in v0.2.7

- **VPS-hosted relay:** game traffic runs on the broker VPS (`74.208.200.95`) — host and joiner connect outbound; no host router port-forward or `RelayHost.exe` for casual relay
- **Broker `forceVpsRelay`:** room codes resolve to the VPS; session relay starts per room with matching `Sidecar.dll` hash
- **Join preflight:** clearer errors when the VPS relay is unreachable (not “forward on host router”)
- **Direct IP unchanged:** Advanced → Direct IP still uses host public IP + session port as in v0.2.6

Also includes v0.2.6: direct IP fix, in-app updater download/apply fix.

## Download

[Latest release zip](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest) — extract to one folder and run `Launcher.exe`.

## Quick start (relay)

| Host | Joiner |
|------|--------|
| Simple mode → **Create relay room** | Paste **`SF4-XXXX`** from host |
| **Start game** (no port forward) | Wait for host **Connected**, then **Start game** |
| Share the relay code | No port forward needed |

## Quick start (direct IP — Advanced)

| Host | Joiner |
|------|--------|
| **Advanced** → **Direct IP** | Paste **`public.ip:23456`** |
| Forward **TCP+UDP** on session port | No port forward needed |

## Broker health

```bash
curl http://74.208.200.95:8787/v1/health
```

Expect `"forceVpsRelay": true` and `"relayHost": "74.208.200.95"`.


## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) � black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.

## Reporting issues

Include the **Git** line from `BUILD_INFO.txt` and a screenshot when reporting issues.
