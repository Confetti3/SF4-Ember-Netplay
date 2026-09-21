# USF4 menu input publication analysis

Analysis date: 2026-09-08. Report type: ordinary reverse engineering, flavor = null. Tools: MSVC 14.51.36231 dumpbin, source inspection and local C++ fixtures.

## Summary and scope

The native input update is a stronger suppression boundary than individual held-button getters: it publishes held, rising, falling, repeated and mapped input for both players. The implementation filters these caches while Ember owns input, retaining physical provider state for the overlay. The ordered training chord is detected immediately after publication, before the caller resumes native menu/pause handling. This is static evidence plus fixture validation, not proof of native runtime ordering or physical-controller acceptance. See [authorized scope](../design/menu-input-scope.md) and [runtime checks](../guides/CONTROLLER_MENUS.md).

## Evidence

All observations below were made on 2026-09-08. Paths are relative to this repository unless absolute. Binary content hash: `5d724595a8ab3c6c6d6f4959187f756f5be35bb497e51e5233c4e73b18b0b9eb`.

| ID | Source reference | Reproduction | Content hash / excerpt |
| --- | --- | --- | --- |
| E-001 | Copied executable identified in scope | `Get-FileHash ../sf4-ember-rooms/build/controller-evidence/SSFIV.exe` | SHA-256 above |
| E-002 | `build/menu-native.asm`, VA 00512180–00512A37 | Run the dumpbin command below; inspect this address range | n/a, regenerable disassembly; cache publication at 00512966 |
| E-003 | Same disassembly, VA 006D8300 and 006D85B0 | Inspect XInput and DirectInput normalization in E-002 output | n/a; physical Back = 0x100, Start = 0x200 |
| E-004 | Copied binary imports | Same dumpbin executable with `/imports` and the scoped binary path | n/a; XINPUT9_1_0 input-state/capability/vibration imports |
| E-005 | `src/tests/controller_navigation_test.cxx` | `./build/current/ControllerNavigationTest.exe` after a designated build | n/a, executable fixture output; publication-cache canaries and ordered-chord/neutral-gate checks |
| E-006 | IDA bootstrap/open attempt | Existing skill start/open scripts, session `ember-controller-menu`; offline service state dependent | n/a; service healthy but binary-open timed out, no database returned; no dynamic evidence claimed |

From the repository root, regenerate static evidence without launching SF4:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x86\dumpbin.exe' /disasm:nobytes '../sf4-ember-rooms/build/controller-evidence/SSFIV.exe'
```

## Findings

| ID | Severity | Evidence | Confidence / status | Location and conclusion |
| --- | --- | --- | --- | --- |
| F-001 | n/a_re | E-002, E-005 | high static / candidate native | 00512180, no-argument thiscall input publication. Two player blocks start at this+0x14 with stride 0x50. Five uint32 caches occupy absolute offsets 0x18–0x28 and 0x68–0x78. Fixture validates exactly these writes without touching surrounding metadata. |
| F-002 | n/a_re | E-003, E-004 | high static / candidate native | XInput Start 0x10 maps to physical 0x200 at 006D83D0; Back 0x20 maps to 0x100 at 006D83DA. DirectInput configured physical slots +0x24/+0x28 produce those masks at 006D86BC/006D86DB. These precede fighting-button remapping. |
| F-003 | n/a_re | E-002, E-005 | medium / candidate native | Clearing all five caches after original publication blocks more paths than filtering held getters alone. Keeping provider state intact lets the overlay read its copied input. Caller/pause coexistence still needs native acceptance. |

## P-001: input ownership call flow

Path type: callflow. Start: native Pad::System input update. Goal: one input owner, with no held-button handoff.

1. Original update polls providers and publishes player input caches (E-002, F-001).
2. The hook reads the explicitly assigned provider and detects previously-held physical Back plus fresh Start only in offline training (E-003/E-005, F-002).
3. Opening requests capture immediately. While captured, or until both native player inputs are neutral after closing, clear the five input caches (E-002/E-005, F-003).
4. Renderer uses copied controller/context state and semantic navigation; it does not call native input readers. Training recording/playback checks the same capture/drain ownership.

Residual risks: physical device normalization, input-update caller order, native pause coexistence and gameplay isolation are not dynamically validated. Getter filters and the existing main-menu observer-state guard remain as defense in depth. No new protocol or settings migration is required.

## Timeline and verification boundary

1. Confirmed `sf4-current` designation and preserved dirty source baseline.
2. Read imports and attempted supported IDA open; timed out without usable database.
3. Used scoped dumpbin disassembly to identify publication and physical normalization.
4. Implemented the game-thread hook and tested exact cache offsets, chord ordering, owner changes, focus loss and release gates.
5. Added controller shell/training journeys and DX9 viewport/DPI rendering. Native acceptance remains user-owned; no SF4 process was launched or installed.

Checklist: scope recorded; imports inspected; evidence/findings/callflow linked; reproduction command supplied; no dynamic or gameplay claim substituted for static evidence.
