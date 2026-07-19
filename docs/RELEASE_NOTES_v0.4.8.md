# SF4 Netplay Launcher v0.4.8

Client stability hotfix for the **GGPO Assertion Failed** dialog that could appear immediately after both players received `SF4R`.

## Install

1. Download **`sf4-netplay-launcher-*-0.4.8.zip`** from [Releases](https://github.com/Confetti3/SF4-Netplay-Launcher/releases/latest).
2. Extract the full zip to a new folder.
3. Run **`preflight.cmd`**, then **`Launcher.exe`**.
4. Confirm both players show **v0.4.8** and host a fresh room.

Both players must use the same release. Do not copy only `Sidecar.dll` or mix v0.4.8 with an older `GGPO.dll`.

## Fixes

- Rejects late `SF4R`/`SF4W` relay control replies before GGPO parses them as game packets.
- Rejects short, unknown-type, truncated, and oversized GGPO UDP messages before logging or dispatch.
- Prevents untrusted UDP input from opening GGPO assertion dialogs.
- Preserves valid GGPO synchronization traffic and the v0.4.7 match-generation/rematch protocol.

## Verification

- Native loopback regression coverage injects the exact `SF4R@` failure pattern plus short and malformed GGPO packets.
- The same test sends a valid GGPO sync request afterward and requires a valid sync reply.
- VPS transport regression remains unchanged because this is a client receive-boundary fix; no relay redeployment is required.
