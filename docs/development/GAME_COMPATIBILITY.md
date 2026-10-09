# Game compatibility and startup

The launcher checks the complete `SSFIV.exe` SHA-256 before creating the game
process. It holds a read handle that excludes writes and replacement until
startup completes. The sidecar independently checks the executable before
`Dimps::Locate` or any game hook installation. Unknown builds are refused.

The initial fingerprint in `src/common/GameCompatibility.hxx` was measured on
October 8, 2026 from the user's installed Steam app 45760:

| Field | Observed value |
| --- | --- |
| Steam build ID | 834219 |
| File version | 2.0.0.93908 |
| File length | 7,026,688 bytes |
| SHA-256 | `5d724595a8ab3c6c6d6f4959187f756f5be35bb497e51e5233c4e73b18b0b9eb` |
| PE machine | x86 (`0x14c`) |
| PE timestamp | 1429629324 |
| Image base / size | `0x400000` / `0x9ba000` |
| Entry RVA | `0x4ccc2b` |

The file's instructions at RVAs `0x1121ad`, `0x2d8d8a`, and `0x42f8` match
the existing focus patches (`85 db`, `85 db`, and `ff 15 ac 12 93 00`). This
records the source of the fingerprint; it is not a claim of independent Steam
depot authentication or completed gameplay validation.

Adding another fingerprint requires reviewing the fixed offsets and testing
the candidate with the native game. Do not learn or accept a hash simply
because a user selects a file named `SSFIV.exe`. Executable mods will be refused;
asset-only changes do not change this fingerprint. There is no runtime bypass.
This check is compatibility protection, not anti-cheat or a defense against
another process patching the loaded game's memory.

`GameCompatibilityTest` tests the production file hasher and rejects missing,
truncated and same-sized replacement files. To additionally check an owned
installation, pass its executable path to that test. It verifies the original
read-only and changes only a temporary copy; no game files are distributed.

Bootstrap payload version 3 adds an unnamed startup-result mapping, duplicated
only into the game. The sidecar reports hook/compatibility failure without
waiting under loader lock. The platform initialization hook reports success
after helper startup, at the same readiness point as the previous event.
The launcher requires the result and a live game process. Failure, early exit,
or a 60-second timeout stops the failed launch and returns to launch recovery.
Old payloads are rejected rather than interpreted with the new layout.

`StartupHandshakeTest` covers ready, failed, incomplete, timeout and early-exit
results with actual child processes. Native hook-failure and slow-start
acceptance still require testing the rebuilt launcher and sidecar with SF4.
