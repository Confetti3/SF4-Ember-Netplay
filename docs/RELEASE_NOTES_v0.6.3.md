# SF4 Netplay Launcher v0.6.3 (testing)

> **Experimental unofficial test build** — not production-ready software.
> Both players must install the same complete release zip.

This corrective release removes the launcher-side room-code requirement from
Direct IP hosting and joining. It includes all v0.6.2 Direct IP transport,
occupied-room lifetime, and 50-room VPS capacity changes.

## Install

1. Download `sf4-netplay-launcher-*-0.6.3.zip` from this release.
2. Extract the complete zip into a new folder.
3. Run `preflight.cmd`, then `Launcher.exe`.
4. Confirm both players show **v0.6.3**.

Mixed builds are rejected by the room compatibility check.

## Direct IP

- Host: choose **Advanced → Host → Direct IP**, select the session port, and
  click **Start game**. No room code or broker room is required.
- Joiner: choose **Advanced → Join → Direct IP:port**, enter the host's public,
  VPN, or LAN `IP:port`, and click **Start game**.
- The host must forward the selected session port on TCP and UDP for public
  internet connections and allow it through Windows Firewall.

## Fixes

- Changing the host connection method from Relay to Direct IP now immediately
  enables **Start game** instead of retaining Relay's **Get code** requirement.
- Direct IP join requests carry only the host address; they no longer duplicate
  it into the relay room-code field.
- The controller selects the join strategy from the explicit connection method
  instead of inferring it from room-code-shaped input.
- The Simple join field now clearly accepts either `SF4-XXXX` or `IP:port`.

## Validation

- Native x86 RelWithDebInfo Launcher build.
- Full native CTest suite.
- Room broker capacity and idle-policy tests.

Live two-PC Direct IP play remains recommended because it requires two separate
game installations and networks.
