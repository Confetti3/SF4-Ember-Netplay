#pragma once
// The battle's two fighters, read from the battle's characters: Training
// moves them and turns its recordings by where they stand, and replay
// takeover's dummy blocks away from the player's fighter.
#include <array>

#include "../training/FighterPlacement.hxx"

namespace Dimps { namespace Game { namespace Battle { struct System; namespace Chara { struct Actor; } } } }

namespace sf4e { namespace battle {
// Each fighter's actor, null for one not in the battle.
std::array<Dimps::Game::Battle::Chara::Actor*, 2> ReadActors(Dimps::Game::Battle::System* system);
// Each fighter's root position (x, y, z) as the engine keeps it, null for one
// not in the battle.
training::FighterRoots ReadRoots(Dimps::Game::Battle::System* system);
} }
