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
void CloseBattle();
// The override exists only during one native offline training update.
bool ReadOverride(int side, Input& result);
} }
