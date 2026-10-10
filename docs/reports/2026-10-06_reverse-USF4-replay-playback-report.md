# Starting replay playback from Ember (reverse engineering notes)

Analysis date: 6 October 2026, on the installed Steam `SSFIV.exe` (the build the other 2026 reports use), with a capstone script over the raw image. Nothing here has been run in the game yet. Goal: let Ember's Replays screen play an archived replay and get the player back to that screen, without the Replay Channel menus in between.

## What the replay list in the game is made of

`Dimps::GameEvents::Network::ReplayChannel::BattleLog::ModeEvent` (vtable `0x93DF04`) is the event behind Replay Channel > Local battle log. It is a `LocalReplay::ModeBase` (vtable `0x93EDD4`) with a named state machine:

| Function | Role |
| --- | --- |
| `0x46FC40` (vtable slot 2) | Registers the states by name with their factories: `Select` (`BattleLog::SelectEvent`, vtable `0x93E108`), `Versus` (`LocalReplay::VersusEvent`, factory `0x485650`, vtable `0x93EE38`), `Battle` (`LocalReplay::BattleEvent`, factory `0x484F00`, vtable `0x93ED30`), `Upload`. |
| `0x470600` (slot 4) | Creates the state manager (`0x6AC8E0`, kept at `+0x3C`), sets the first state by name (`manager->vtable+0x50("Select")`), starts it (`+0x54`), names the event `LocalBattleLog`, and registers a `CHECK STORAGE` task. |
| `0x46F910` (listener) | State events: on event 7 for a state named `Versus` it fades the menu sound; on event 5 it asks the flow for row 0 (`+0x9C(0,0,0,0,1)`), which is how the battle log leaves to Player Data. |
| `0x4842B0` and the `BATTLE CHECK END` task in `0x484940` | The replay battle's end, after which the mode is back on `Select`. |

The flow table knows the event as `LocalBattleLog` (rows `LocalBattleLog, 0, PlayerData` and `LocalBattleLog, 1, MainMenu`; Ember adds `MainMenu, 15, LocalBattleLog`).

## How a replay gets into the replay system

`Dimps::Game::Battle::ReplaySystem` (vtable `0x954DAC`, singleton from `0x5D6700`):

| Vtable | Address | Role |
| --- | --- | --- |
| `+0x40` | `0x5D5CA0` | Start playback of the current round: points the cursor at the round's stream, mode 1. |
| `+0x44` | `0x5D65A0` | Start recording the current round (mode 2). |
| `+0x4C` | `0x5D5D20` | Per frame record or play (the replay corruption report covers this). |
| `+0x50` | `0x5D6250` | Load a replay from a buffer: checks the `#BRP` header (`0x41AE50`, `0x41AEA0`, `0x41B4E0`), version `0x320` at byte 6, a positive round count at `+0x18`, then takes the rounds. This is the only entry that turns file bytes into something the battle can play. |
| `+0x54` | `0x5D6410` | The loaded replay's header, through `0x5D5F00`. |
| `+0x58` | `0x5D6430` | Serialize the recording into a `#BRP` buffer (what the save after a battle writes). |

Twenty call sites fetch the singleton and call `+0x50`. The one in the local replay code is `0x482CE0`, a step of `ReplayChannel::ExchangeChannel::ReplayPlayer` (vtable `0x93EC4C`), a helper that both channels use to play a slot:

1. `0x483070`: refuses while the save controller is busy (`0x67C430`), then sets the first step `0x482C50`.
2. `0x482C50`: waits for the controller and reads its current record (`0x67C4A0`).
3. `0x483600`: asks the save controller to read the slot into the helper's buffer, `0x67C450(slot at +0x24, buffer at +0x34, 0xC800, 0, 0)` then `0x67C3F0(0)`, registers the wait, and sets the next step `0x482CE0`.
4. `0x482CE0`: takes the controller's record and data (`0x67C4C0`, `0x67C4A0`, `0x67C410`, `0x67C4F0`), calls `ReplaySystem+0x50(buffer)`, reads the slot record (`0x6772A0`), builds the two fighters' battle request from the replay header (`0x4C9370`, `0x43B8F0`), and leaves the result in `+0x50` (0 ok, 1 failed).

The save controller is `Dimps::Game::SaveDataController` (singleton `0x67C880`, implementation `SaveDataControllerWin32`, vtable `0x94A014`, which reads and writes through Steam). `0x67C450` is its "read slot N into this buffer" entry; `0xC800` (51,200 bytes) is the largest replay, the same size the preallocated slot files have.

`LocalReplay::VersusEvent` (`0x4858E0` runs its `VERSUS UPDATE`, `RENDER`, `POSTUPDATE`, `CHECK` tasks) then reads the loaded header from the replay system (`0x485680`: `ReplaySystem+0x18`, fighters, edition flags) and `LocalReplay::BattleEvent` (`0x483ED0` sets the battle up from it, `0x484940` runs it) plays it. The replay battle never touches the slot files again: everything comes from the replay system's loaded copy.

## What this means for Ember

Playback needs three things the game already does on its own: the replay bytes loaded through `ReplaySystem+0x50`, the battle request built from the header (`0x4C9370`), and the `LocalBattleLog` mode moved to its `Versus` state, which runs the battle and comes back to `Select`.

The shortest route from the Replays screen that stays on the game's own code paths:

1. Ember already jumps `MainMenu` to `LocalBattleLog` (flow row 15). Once `BattleLog::ModeEvent` is the foreground event (named `LocalBattleLog`), Ember finds it, takes its state manager at `+0x3C`, and asks it for `Versus` by name (`manager->vtable+0x50("Versus")`) after loading the archived replay's bytes into the replay system itself (`ReplaySystem+0x50(bytes)`). The import into a slot is then not needed for watching, only for keeping it in the game's list.
2. When the battle ends the mode returns to `Select`. The listener `0x46F910` shows how the mode leaves: flow row 0 of `LocalBattleLog`. Ember asks for row 1 (`MainMenu`) instead, and the overlay already reopens on the Replays screen when the main menu is back.

Still to confirm before writing code:

- Whether `VersusEvent` needs the save controller's "current record" as well as the replay system's data (`0x482CE0` reads both; `0x485680` reads only the replay system). If it does, the slot import stays the first step and the controller is asked to read that slot (`0x67C450`), which is the same thing the game does.
- The exact function the UI's `play replay` command (`0x4900B0`, listener at `+0xA8`) ends in for the local list, to copy its argument order rather than guess it. The `ReplayPlayer` helper gives the sequence; the local list's own trigger was not isolated.
- That `manager->vtable+0x50` accepts a state change while `Select` is running (the game calls it before `+0x54` starts the machine; a running switch may go through `+0x1C` on `0x6D6D30`'s object instead).

Everything above is static reading. The first run in the game decides.
