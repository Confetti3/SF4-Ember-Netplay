#pragma once

#include "../common/ReplayTakeover.hxx"

namespace Dimps { namespace Game { namespace Battle { struct System; } } }

// The game side of a replay takeover (common/ReplayTakeover.hxx has the
// rules): the save state of the takeover frame and which of the recorder's
// sides the player plays. The replay controls (sf4e__ReplayPlayback.cxx)
// decide each step; everything here is the game thread's.
namespace sf4e { namespace replaytakeover {

// Carries out a step for `side`. False when its save state could not be saved
// (no side is taken) or loaded (the battle is left, as a failed load leaves the
// timeline unknown); the caller reports it to the model (Takeover::Carried).
bool Carry(Dimps::Game::Battle::System* system, Act act, int side);
// The side as the player's or the replay's, the save state untouched: the
// game's replay menu sees a replay while it is open, since its Restart
// reseeds the battle only when Player 1 plays back (0x5DB540).
void Hand(Dimps::Game::Battle::System* system, int side, bool player);
// The battle starts or closes: the save state goes, freed while the engine
// still holds its payloads, otherwise only its records (SaveState::Reclaim).
void Drop(bool engineAlive);

} }
