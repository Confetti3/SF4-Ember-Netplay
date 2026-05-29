# SF4 Netplay Launcher v0.2.7.1

Patch release after IONOS firewall fix and VPS relay polish.

## What's new in v0.2.7.1

- **VPS join path:** launcher sets `useCentralSession=2` for relay room codes under `forceVpsRelay`
- **In-game overlay:** VPS host/join messages no longer mention RelayHost or port-forward
- **Broker heartbeat:** verifies relay-manager session health; restarts dead relay or returns `heartbeatOk: false`
- **Broker API:** `GET /v1/rooms/SF4-XXXX/health` for relay diagnostics
- **relay-diag.ps1:** checks room health endpoint; documents IONOS UDP requirement
- **Deploy:** graceful relay-manager stop before broker update (reduces SIGBUS on redeploy)
- **Docs:** IONOS inbound UDP 23456â€“23475 checklist in CASUAL_NETPLAY and TEAM_QUICKSTART

Includes all v0.2.7 VPS relay features. Direct IP (Advanced) unchanged.

## Troubleshooting

[Player troubleshooting guide](https://github.com/Confetti3/SF4-Netplay-Launcher/blob/main/docs/TROUBLESHOOTING.md) — black launcher, crash on **Start game**, recommended settings, Direct IP firewall/ports, and logs.
