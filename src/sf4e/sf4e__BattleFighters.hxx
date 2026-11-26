#pragma once
// Where the battle's two fighters stand, read from their actors: Training
// moves them and turns its recordings by them, and replay takeover's dummy
// blocks away from the player's fighter.
#include "../training/FighterPlacement.hxx"

namespace Dimps { namespace Game { namespace Battle { struct System; } } }

namespace sf4e { namespace battle {
// Each fighter's root position (x, y, z) as the engine keeps it, null for one
// not in the battle.
training::FighterRoots ReadRoots(Dimps::Game::Battle::System* system);
} }
