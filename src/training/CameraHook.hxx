#pragma once
namespace Dimps { namespace Game { namespace Battle { struct System; } } }
// Lays the phone's camera offsets over the game's camera for the picture of
// one frame, and takes them off again before the game's next update, so the
// game's own state (its rollback saves among them) never holds them.
// Game thread. Scope puts both round one call of the battle update, however it returns.
namespace sf4e { namespace training { namespace camera {
// Before an update: puts back what Apply changed.
void Restore(Dimps::Game::Battle::System* system);
// After an update: lays the offsets over the camera for this frame's picture.
void Apply(Dimps::Game::Battle::System* system);
struct Scope {
    explicit Scope(Dimps::Game::Battle::System* s) : system(s) { Restore(system); }
    ~Scope() { Apply(system); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Dimps::Game::Battle::System* system;
};
} } }
