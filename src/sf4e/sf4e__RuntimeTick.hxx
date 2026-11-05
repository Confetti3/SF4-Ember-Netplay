#pragma once
// Ember's part of one application tick. The game calls Tick once per tick
// from its outer loop, and its own battle and frame work run inside it, at
// fixed points, because the order is what keeps the room link, the rollback
// session and the published frame consistent:
//   the runtime's tick, then the room client's and the hosted server's
//   steps, the control plane's health, the Training call and the battle's
//   frame bookkeeping;
//   the game polls its battle (PollBattle), and the runtime retires a
//   finished spectator view;
//   the game runs its own update (UpdateGame);
//   the runtime publishes the frame the overlay draws next.

namespace ember {

// What the game does inside Ember's tick.
class GameTick {
public:
    // After the runtime's steps: the battle's network poll and what that
    // poll settled (aborts, results, spectators).
    virtual void PollBattle() = 0;
    // The game's own update for this tick.
    virtual void UpdateGame() = 0;
protected:
    ~GameTick() = default;
};

// Game thread, once per application tick.
void Tick(GameTick& game);

} // namespace ember
