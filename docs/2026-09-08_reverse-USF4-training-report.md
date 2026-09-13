# USF4 training reader analysis

Analysis date: 8 September 2026. Tools: IDA 9.4 / Hex-Rays through the local MCP worker and existing project source. Scope: [authorized local reader discovery](training-native-scope.md). Report flavor: ordinary reverse analysis; no vulnerability or threat claim.

The existing status, damage and vitality readers support an observed-state meter. Native debug registrations identify action ID and animation-frame readers. Follow-up analysis after the user's test1 feedback established the shared grounded recovery dispatch and two additional read-only getters for action posture and basic-action inhibit. Test2 uses these for a conservative signed recovery comparison. Static call paths establish the implementation basis; native accuracy of the new numbers remains unverified. Separate startup/active/recovery phases remain unavailable.

## Evidence

| Evidence | Source and observation | Reproduction | SHA-256 |
| --- | --- | --- | --- |
| E-001 | Installed manifest reports build 834219; local executable is 7,026,688 bytes | `Get-FileHash build/native-evidence/SSFIV.exe -Algorithm SHA256` | `5d724595a8ab3c6c6d6f4959187f756f5be35bb497e51e5233c4e73b18b0b9eb` |
| E-002 | `build/native-evidence/training-readers.json` contains address-bearing decompilations of the seven reader/caller functions | Open the matching executable in IDA; decompile the addresses listed below | `76ac45628ef31bd1130ae18ff3b5eefcb24f3a723790ea4c4c7af619e639f2cc` |
| E-003 | `src/Dimps/Dimps__Game__Battle__Chara.hxx` defines native status values, including `AS_SKILL=16`, `AS_DAMAGE__GUARD=22` | Inspect `Actor::Status` in that header | n/a (source changes with the worktree) |
| E-005 | `build/native-evidence/training-recovery.json`, 13 decompilations of recovery, posture, command-inhibit and time-scale paths | Reopen the same executable/IDB and decompile the addresses in the artifact | `6db1d47fb885fa38cb270ebf51e2f5fc158fdd4e3dad8f87feddbb32280248a0` |

The executable and full decompiler output remain local ignored artifacts. No binary bytes or external source code were embedded into the product. Import inventory (IDA `ida_nalt.get_import_module_name`, E-004, hash n/a): VERSION, DSOUND, XINPUT9_1_0, curllib, WS2_32, steam_api, XINPUT1_3, cryptopp, IMM32, KERNEL32, USER32, GDI32, SHELL32, ole32, OLEAUT32, dbghelp, d3d9, d3dx9_43, DINPUT8, WINMM, WMVCore. This is a module inventory, not a security finding. The analyzed IDB was saved and its owned worker closed after evidence collection.

## Findings

| Finding | Severity | Evidence | Confidence / status | Location |
| --- | --- | --- | --- | --- |
| F-001: native status is a coarse state enum, insufficient to split attack phases | n/a_re | E-002, E-003 | High for reader semantics; gameplay correlation pending | VA `0x542520`, reads actor dword index 1992 |
| F-002: native `getAction` resolves an action ID from actor vtable `+0x50` | n/a_re | E-002 | High static / implemented as typed reader | `0x41f9f0` -> actor vtable `+0x50` -> `0x52d700` |
| F-003: native `getCharacterFrame` uses actor vtable `+0x54` and Q16.16 conversion | n/a_re | E-002 | High static / implemented as typed reader | `0x41fc30` -> actor vtable `+0x54` -> `0x52d720` |
| F-004: fight and mode guards reuse native readers | n/a_re | E-002 | High static / runtime not tested | `0x5d9f60`, `0x5d97c0` |
| F-005: grounded attack/hit/block recovery shares an action-end dispatch | n/a_re | E-005 | High static / measurement timing needs gameplay | `0x54eac0`, `0x54eae0`, `0x54ec60` -> `0x549760` |
| F-006: posture and command-inhibit readers constrain recovery samples | n/a_re | E-005 | High static / added typed readers | `0x541ea0`, `0x542390`; existing unit time scale at `0x5da060` |

Actor vtable base `0x94eb9c` contains `0x52d700` at `+0x50` and `0x52d720` at `+0x54`. The existing actor status function is at `0x542520`. The action reader returns -1 when its action object is absent; the frame reader supplies zero in that case. Product display requires a nonnegative action ID before presenting action timing.

## Call path

P-001 (`path_type=callflow`): native registration `0x420e60` binds `getAction` and `getCharacterFrame` -> debug callers `0x41f9f0` / `0x41fc30` -> actor virtual readers (`F-002`, `F-003`) -> the new typed read-only API -> post-update training sample -> copied UI snapshot. The renderer never obtains a native actor pointer.

P-002 (`path_type=callflow`, F-005/F-006): state dispatch table `0x94f378` maps status 16/21/22 to `0x54eac0` / `0x54eae0` / `0x54ec60`. All reach `0x549760`: it queries actor vtable `+0x1b4` (posture), waits on `+0x104` (action end), then dispatches posture 0/1 to standing/crouching through `0x548890`. Other postures take airborne/downed paths and are excluded. Posture reader `0x541ea0` calls the action object at actor `+0x4c`, then action vtable `+0x54`, returning -1 if absent. Actor `+0x220` is `0x542390`, returning actor dword index 2140; nonzero immediately rejects the native basic-action evaluator `0x548f30`. The existing system `0x5da060` supplies combined unit time scale; zero cannot count as recovery, fractional rates invalidate comparison.

The free-state set includes standing/crouching, their transitions, turns, walks and guard postures. Inspected standing/crouching and guard handlers call the basic-action evaluator with mask 2047. Contact requires damage state 21/22, not merely guard posture. A changed attack action before free recovery is treated as a cancel and discarded. The comparison requires both fighters to remain grounded, does not support delayed projectile ownership, and is not a universal test of every move's cancel permissions. Synthetic tests cover positive/negative/zero and mirrored values, native freeze/inhibit rejection, repeated contacts, cancels, missing actors, resets and discontinuities.

Training commands carry a battle generation. The battle hook checks offline native Training and ready/fight state before mutations; input overrides exist only for the duration of the local update. Native memento ownership is separate from GGPO save slots. Reset/teardown invalidate stale requests and samples.

## Timeline and limits

1. Inspected the current Ember/custom-room source and created the isolated worktree.
2. Confirmed existing actor states and training/save-state hooks from local source.
3. Examined [SF4BV's public source](https://github.com/lullius/SF4BV/tree/42055e5e029ae39feaf5e96e20caccba42c69053) as a research lead. It uses older offsets and heuristic box activation; none of its code or offsets were adopted.
4. Copied the installed executable and opened it with the skill's IDA helper. The helper timed out at 600 seconds; the worker subsequently completed and was enumerated as a ready session.
5. Followed native debug registration/caller/vtable paths, recorded E-002 and added action/frame readers.
6. Built the local x86 candidate and exercised synthetic controller/meter and actual DX9 UI tests. In-game correlation remains pending.

Before claiming SF6 parity, establish live active hitbox timing, general cancel/actionability and projectile ownership with native evidence and gameplay. The recovery comparison needs native correlation against known moves on hit and block, especially the exact simulation-step boundary. The original viewer's user-reported success does not establish accuracy of the newly added measurements.

## Test3: special/target-combo chains and startup

The user reported that special moves and target combos never display signed advantage. A regression through the real `FrameMeter::Observe` path failed with `Target combo/special action chain lost frame advantage`. Test2 explicitly cleared and disarmed the exchange on action changes, airborne posture and delayed contact. Test3 retains the attacking side and recovery timestamps through these transitions. Further contact resets the defender's recovery; the attacker's prior timestamp survives a delayed projectile. Knockdown recovery is marked `wakeup`, distinguishing that result from ordinary hit/block stun. Trades and interrupted attackers invalidate the exchange. Synthetic action-chain, airborne, delayed-contact and knockdown cases now pass. This fixes the identified model exclusions; character-specific live acceptance remains pending.

Startup evidence E-006: `build/native-evidence/training-startup.json`, SHA-256 `421ea5bb341ab01371688b1695102f32a0fd98385fb000ead60033c7ddf3d596`, records native reader decompilations and installed BAC samples. Native `0x52da50` (actor vtable +0x44) resolves an action ID in the current BAC bank, checks the bank's signed action count and nonzero offsets, and returns the script pointer or null. It is called only after the existing action-ID reader returns a nonnegative current ID. Native `0x52d6a0` corroborates the 24-byte script header and relative command-list layout.

The [primary RainbowLib BAC reader](https://github.com/dantarion/ssf4ae-tools/blob/5ac3aa02cdd2e40f4aca876b8ad305e66da9fc1f/RainbowTool/RainbowLib/BACFile.cs) identifies the first four 16-bit header fields as first attack boundary, last attack boundary, interruptible frame and total animation frames. This layout was checked against the installed `patch_ae2_tu3/battle/regulation/ae2_111/RYU/RYU.bac` (SHA-256 `9b4cb7b0aa645df6f164916240c397ce3e22856ba80f235322d89b4600f3b6e2`). For example, script 256 `5LP` declares first attack tick 9, a damaging hitbox begins at 9, and speed changes make direct tick-to-frame conversion invalid. Script 384 `HADOKEN_L` declares boundary 26 while its only character hitbox is a proximity box; that metadata can supply startup without waiting for projectile contact. Recovery script 400 has zero/zero boundaries and must not become zero-frame startup. No external implementation code was copied.

Finding F-007 (n/a_re, E-006, high confidence for layout; native timing correlation pending): count observed advancing simulation samples up to the script's first attack boundary, rather than displaying its raw animation tick. Repeated frozen action frames do not increment the count. If the step reaches the boundary and also starts hitstop, the observed animation advance still counts. Completed startup survives recovery-only actions; a target-combo follow-up updates it. Zero/zero, absent, inconsistent or excessively large headers display unavailable. This is authored attack timing, not proof of live box activation. Branching scripts and exact first-step timing still require gameplay checks.
