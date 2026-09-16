# Releasing rollback save states

GGPO keeps a ring of `GGPO_MAX_PREDICTION_FRAMES + 2` saved frames. Once the
ring is full, `Sync::SaveCurrentFrame` (pinned fork, `src/lib/ggpo/sync.cpp`)
calls `free_buffer` on the oldest slot before **every** save, including every
save made while re-simulating after a rollback. The cost of
`fSystem::SaveState::Free` is therefore paid once per simulated frame, and once
more for every re-simulated frame.

## v0.8.5 release: round trip (still available)

`FreeByRoundTrip` does this for each release:

1. Temporary `Save` of the live game (records every unit's mementos).
2. `CopyIntoPlace(victim)`: installs the old keys and runs
   `RestoreAllFromInternalMementos` (System, Chara, Effect, VFX, Command, HUD,
   Camera, Training, then `ResetAfterMemento` for both characters).
3. `Clear(victim)`: engine `ClearKey` on each installed key.
4. `CopyIntoPlace(tmp)`: restores the live game with a second full restore.

In a running match that means two memento records and two full restores per
frame, on top of simulation. How many releases each peer performs depends on
its own rollbacks, so if a restore is not perfectly lossless the peers
execute different state transitions.

## Why the victim does not need to be restored

The round trip existed so that "the mementoable object pointer in each key is
valid" when `ClearKey` runs. The engine code shows that nothing more than that
is needed.

Binary: Steam USF4 buildid 834219, SHA-256
`5D724595A8AB3C6C6D6F4959187F756F5BE35BB497E51E5233C4E73B18B0B9EB`, image
base `0x00400000`.

`GameMementoKey` (`Dimps__Game.hxx`) is
`{ mementoableObject, metadata, numMementos, mementos, sizeAllocated, nextMementoIndex }`.

**`GameMementoKey::ClearKey`** (`0x0052F3D0`):

- If the memento buffer is non-null, subtracts `sizeAllocated` from the global
  total (`0x00AA5840`).
- For each metadata entry with a memento pointer, calls
  `(*mementoableObject)->vtable[2](memento)` and nulls the pointer.
- Frees the buffer (engine pool or CRT `free`), zeroes the fields and sets
  `mementoableObject = 0`.

The live object is read only to find its vtable.

**`GameMementoKey::Initialize`** (`0x0052FD40`) calls `ClearKey`, sizes the
buffer with `vtable[0]`, allocates it, and constructs each memento with
`vtable[1]`.

**Slot 2 of the mementoable vtables examined** (the 22 vtables that reference
`0x0057BD60`, plus the character actor's) is either:

- `0x0057BD60`, a stdcall on the memento pointer alone that calls the
  memento's own destructor `memento->vtable[0](memento, 0)`; or
- a no-op (`0x007A6360`, character `Action::Actor`).

**Memento destructors.** Every memento class derives from `Game::Memento`, so
every memento destructor finishes by writing the base vtable
`0x0094E328`. That address has exactly nine code references: the base
constructor `0x0052B510`, the Trace constructor `0x005C2880` (it builds an
embedded base memento), and the seven destructors below. So this is the
complete set of memento destructors.

| Memento class | Destructor | Work |
| --- | --- | --- |
| `Game::Memento` and the classes that share its destructor (16 vtables, including System, Effect ActorManager, Chara Actor, Engine, Hud Cockpit Uc, CommandImpl, ColorFade, Training Manager and character items) | `0x0052B520` | resets its own vtable pointer |
| `Effect::Actor::Memento` | `0x0056BDD0` | calls an empty function, then resets vtable |
| `Vfx::Particle::Memento`, `Vfx::Trace::Memento` | `0x005C09C0` | frees a string buffer owned by the memento |
| memento at `0x005BD6B0` | `0x005BD6B0` | frees a string buffer owned by the memento |
| `Vfx::ParticleContainer::Memento` | `0x005C1030` | destroys two `Vfx::Particle` objects embedded in the memento; each releases its own handle through the global manager `0x006C4EB0` |
| trace container memento | `0x005C2E00` | destroys two `Vfx::Trace` objects embedded in the memento; each releases its own handle through the manager `0x005CD2A0` |
| memento at `0x0053C3D0` | `0x0053C3D0` | drops references the memento holds (an object release and three shared reference counts) |

None of these read the mementoable object's fields. The only effects outside
the memento's own memory are releases of references and handles the memento
itself took when it was recorded. Those are released exactly once in both
paths. Restoring the victim first changes neither what `ClearKey` releases
nor the live state left afterwards.

## v0.8.6 release: swap and clear (default)

For each `(address, key)` the victim recorded:

```
live = *address
*address = key          // victim key installed
ClearKey(address)       // original engine function, not the tracking detour
*address = live         // live key back, byte-identical
```

The records are then dropped with `ownsKeys = false`. No temporary save and no
memento restore run, and the live game (engine objects, battle-flow globals,
GameManager, sound pools and live keys) is not written at all.

It relies on the same assumption as the round trip: a victim's key addresses
are still valid key storage. Keys live inside unit objects that exist for the
whole battle. The round trip already wrote victim keys to those same
addresses, and battle teardown and session start reclaim slots without engine
calls.

## Controls and checks

| Environment variable | Effect |
| --- | --- |
| `SF4E_LEGACY_SAVESTATE_FREE=1` | Use the v0.8.5 round trip, for A/B comparison. Read once per process; the chosen path is logged at each GGPO start. |
| `SF4E_SAVESTATE_FREE_VERIFY=1` | Hash the semantic gameplay state, battle-flow globals, GameManager and (swap path) every live key's bytes before and after each release; log an error if they differ. Development only; it costs a full hash per frame. |
| `SF4E_ROLLBACK_DIAGNOSTICS=1` | `free`, `free.swap` and the legacy `free.*` phases, plus `record.{chara,effect,vfx,other}` and `restore.{chara,effect,vfx,other}` per pass. **Help & About → Export diagnostics** includes Free state, Effect restore and VFX restore. |

Compare characters with the same stage, costume, delay and route. With the
legacy path, `restore.effect` / `restore.vfx` include the two restores inside
every release. A character whose effects are expensive to restore (for
example a persistent aura) shows up there.

The ownership protocol for both paths is modelled in
`src/tests/savestate_ownership_test.cxx`.
