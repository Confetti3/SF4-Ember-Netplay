#pragma once
#include "TrainingSession.hxx"
#include "MatchPractice.hxx"

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
// SetMatchPractice: whether the battle being prepared uses a table's
// Training rule. Game thread.
void SetMatchPractice(bool enabled);
// Any thread: whether such a match is on. Shared checkpoint requests are ignored.
bool MatchPracticeActive();
void RequestMatchPractice(unsigned bits);
// The local pad's raw word as it goes to GGPO, with reserved practice bits removed.
unsigned WithMatchPractice(unsigned raw);
// Before the game plays a frame from GGPO's inputs, simulated or
// resimulated: takes the two bits out of both raw words. Shared save/reset
// is disabled, so the fight and checkpoint stay as they are. Returns true.
bool BeforeMatchFrame(Dimps::Game::Battle::System* system, unsigned& rawOne, unsigned& rawTwo);
// The part of it that is saved and restored with every frame.
PracticeState MatchPracticeState();
void SetMatchPracticeState(const PracticeState& state);
// Whether the player wants that meter. Any thread.
void WatchMatches(bool enabled);
void CloseBattle();
void StopCapture();
// The override exists only during one native offline training update.
bool ReadOverride(int side, Input& result);
} }
