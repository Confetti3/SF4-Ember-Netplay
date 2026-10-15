# Stage quality and the battle RNG (reverse engineering notes)

Analysis date: 10 October 2026, in the IDA database of the installed Steam `SSFIV.exe` (image base `0x400000`). Nothing here has been run in the game. Question: a contributor reported that two peers with different `GRAPHICS/StageQuality` settings desync, seen as background sprites missing on one side, because the stage scripts draw from the gameplay RNG only on HIGH.

Result: every step of the mechanism is real, but the RNG it advances is not gameplay state. It is not part of Ember's rollback state and nothing the desync hash covers reads it. A StageQuality mismatch changes presentation only (background animation, which LOW turns off by design, and which random announcer line or optional effect each side gets). No code change was made; the options are at the end.

## The setting

The graphics profile is a static struct at `0xA85520` (getter `0x4019A0`, constructor `0x4011F0`). StageQuality is the dword at `+0x20`, global `0xA85540`: `1` = HIGH (the constructor default), `0` = LOW. The profile writer `0x401310` maps it to the option `GRAPHICS/StageQuality` (`off_A4F00C`) as `"HIGH"` or `"LOW"` (`0x933338`) and logs `FAILED:Profile -> StageQuality` on error. The option registration `0x403020` offers only LOW and HIGH.

Readers of `+0x20` (all in the stage unit): `0x59CD00` (stage update), `0x59AED0` (stage message handler, forwards battle events to the object and reaction managers at `+0xD4`, `+0xD8`), `0x59B910` (stage render task), `0x59C880` (a battle-flow callback, render setup), `0x59E190`, and two stage Lua bindings (`0x5A7453`, `0x5AC684`, the second tweaks an object on stage `0x15` on LOW). The update, the message handler and the flow callback (`0x59CD00`, `0x59AED0`, `0x59C880`) run when `quality != 0 || battle type == 3`, where the battle type is `System vtable+0x68 → +0x14` and 3 is the bonus stage. `0x59B910` and `0x59E190` read `+0x20` (branches not followed); the two Lua bindings test the setting alone. ModelQuality (`+0x24`) readers such as `0x5203F0` set material colours.

## The stage update

`0x59CF60` registers `0x59CD00` as the `STAGE MAIN` task (an `EmTaskFunctor<Stage::Unit>`) in the update task core, so it runs inside `System::BattleUpdate` and therefore also during rollback resimulation. At `0x59CD1D`..`0x59CD2A` it tests `*(0x4019A0()+0x20) != 0 || battleType == 3`. On HIGH it queues three stage jobs (`0x59B270`, `0x59B290`, `0x59B2B0`), calls `+0xD8 vtable+0x10`, `0x5B1FB0(+0xE0)`, then at `0x59CE0E` `+0xF0 vtable+8`, joins the jobs (`0x538490`) and advances the stage clocks at `+0xFC`, `+0x100`. On LOW it only calls `0x5AF3B0(+0xEC)`.

`+0xF0` is the stage script manager, created in the stage setup `0x59B470` by `0x6E8FA0` (object vtable `0x9C7828`), which then registers the stage libraries (`0x5A6270`, table `0xA63860`). Its `+8` is `0x6E87B0`: it walks the script thread list and calls each thread's `+8`, `0x6E7CB0`, which `lua_resume`s (`0x6DB6B0`) threads in state 2 or 3, sets 3 on yield, 4 on finish and -1 with the error code on error, and drops threads in state 4.

## Util.GetRandom and GetFltRandom

The `Util` library table is at `0xA63768` (pairs of name, function). `GetRandom` (`0x9527DC`) is `0x5A7BC0` and `GetFltRandom` (`0x9527CC`) is `0x5A7C80`. Both call `System::GetSingleton` (`0x5DBA30`, the object at `0xAA7D70`), then `System vtable+0xA0`, which is `0x5D9920` (`lea eax, [ecx+50h]`), then the EmRandom's `+0xC` (`0x4C9B50`) or `+0x10` (`0x4C9B70`). EmRandom (vtable `0x9425A0`) is a plain LCG on its `+4` seed: `seed = seed * 0x19660D + 0x3C6EF35F`; `+0xC` returns the seed, `+0x10` returns `(seed >> 9 | 0x3F800000) - 1.0f` (`flt_933454` is 1.0). GetRandom uses `(seed >> 8) & 0xFFFFFF` modulo the range. So the stage scripts draw from `System+0x50`, the same EmRandom that Ember's `System::GetRandom` returns.

## Who else draws from System+0x50

Sites that fetch the System and call `+0xA0`: the two Lua functions; `0x59FE70` (Stage::Object constructor, seeds the object's own EmRandom at `+0x258` with one draw; reached from `0x5A14C0`, which a stage Lua binding calls at `0x5A6E44`, and from the ObjectManager vtable method `0x5A1D80`); `0x5A6000` (Stage::Reaction constructor, same, via the factory `0x5A6100`); `0x596900` (picks a random sound id from the table `0x951DB8` for message type 6, the announcer and stage voice lines, dispatched from `0x596960`); `0x5BA020`, `0x5BA1C0`, `0x5BAC40` (effect signals: a random variant and a random-chance test before a signal is sent); and `0x5D6D50`, a System method (vtable `+0x164`) that returns a random bit only when the side equals `System+0xDCC` (set from the request at `0x5DAA30`, `0x5D8FA0`), that side's record has `+0x1C == 2`, and option 3 of the mode settings is 2. Its callers are unresolved: no `GetSingleton → vtable+0x164` call exists, and the eleven `[edx+164h]` loads all follow member-object loads. The RNG is seeded from the battle request at `0x5DAE9F` (`0x5DAD40`), and again on restart (`0x5DB480`, `0x5DC150`). The LCG step exists only in the two EmRandom methods. The other `[reg+0A0h]` vtable calls checked belong to other objects (HUD sprites, the singleton `0x519480`, chara code); a System pointer cached in a member and used later was not searched for exhaustively.

## Why this is not a gameplay desync

The System memento (`RecordToMemento 0x5D7B80`, called with `ecx = System+4` from `0x5DAAB0`) copies `System+0x10..+0x4F` (four vectors) and then `System+0xDD0` onward. `+0x50` (EmRandom vtable) and `+0x54` (seed) are skipped, and Ember's `SaveState` stores no RNG either. `RecordAllToInternalMementoKeys` (`0x5D7F80`) records the System and the units at `System+0x5C`, `+0x64`, `+0x68`, `+0x6C`, `+0x78`, `+0x7C` (chara, VFX, HUD, camera, effect, command; `GetUnitByIndex 0x5D9720` is `System+0x5C+4*i`). The stage unit (`+0x60`) is never rolled back.

So the RNG already diverges between two peers every time their rollbacks differ in depth, whatever the stage quality, and the stage itself replays extra frames on each resimulation. If anything covered by Ember's semantic hash (positions, health, gauges, action id and frame, posture, time scale) read this RNG, ordinary rollback would already desync matches with equal settings. That fits the consumer list above: scripts, stage objects, effects, voice lines, and one mode-gated System method whose callers are unresolved. This closes the open item in `docs/design/DESYNC_V2.md`: the evolving RNG is located (`System+0x54`) and is not saved.

The reported symptom, background sprites missing on the LOW side, is what LOW does: the stage update and render tasks do not run. No field log with an Ember desync notice for this case was found.

## 100% CPU

Not found. A script that errors is parked (state -1) and skipped by `0x6E7CB0`, it is not resumed again, so this path does not spin.

## Options

1. No change (chosen). The divergence is presentation only.
2. Save `System+0x54` in `SaveState::d` and restore it in `CopyIntoPlace`, and move the stage draws (the two Lua functions and the two stage constructors) to a stage-owned stream so that HIGH does not advance it. Then voice lines and optional effects match on both peers and through rollback, for spectators too. This changes save-state contents and is a separate decision. Saving the seed without moving the stage draws would not help, since the stage is not rolled back and keeps drawing during resimulation.
3. Forcing one stage quality for both fighters during an Ember match was rejected: it overrides a performance setting, or removes background animation for players on HIGH, to fix something that does not affect the match.
