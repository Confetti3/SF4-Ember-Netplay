#pragma once
#include "TrainingSession.hxx"

namespace Dimps { namespace Game { namespace Battle { struct System; } } }
namespace sf4e { namespace training {
View ReadView();
bool Submit(Command command);
// Game-thread context guard, including leaving-battle and network ownership.
bool ControlsAvailable();
// All native calls run inside the battle hook, never from the renderer.
void BeforeUpdate(Dimps::Game::Battle::System* system, bool networkOwned);
void AfterUpdate(Dimps::Game::Battle::System* system);
// The frame meter in a rollback match. It only reads the game, on the game
// thread, after a frame was simulated or resimulated, and shows a frame once
// its inputs are confirmed. stateFrame: the GGPO save frame of the state
// just reached, 0 or less for a spectator, who plays confirmed inputs only.
// lastConfirmedInput: -1 to capture without showing anything yet.
void ObserveMatch(Dimps::Game::Battle::System* system, int stateFrame, int lastConfirmedInput, unsigned padOne, unsigned padTwo);
// Whether the player wants that meter. Any thread.
void WatchMatches(bool enabled);
void CloseBattle();
void StopCapture();
// The override exists only during one native offline training update.
bool ReadOverride(int side, Input& result);
} }
