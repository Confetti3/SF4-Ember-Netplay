#pragma once
#include "TrainingSession.hxx"

namespace Dimps { namespace Game { namespace Battle { struct System; } } }
namespace sf4e { namespace training {
View ReadView();
// Where the fighters stand and the battle they stand in, without the rest of
// the view: what a pad press keeps for the save it may become. Any thread.
struct Place { std::uint64_t generation = 0; float x[2] = {0, 0}; };
Place ReadPlace();
// The call back from Training that stands for the battle, as the room's call
// owner holds it, set each tick and read before every countdown tick
// (TrainingSession.hxx: CallControl). Any thread.
void SetCall(const CallControl& call);
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
void ObserveMatch(Dimps::Game::Battle::System* system, int stateFrame, int lastConfirmedInput);
// SetMatchPractice: whether the battle being prepared uses a table's
// Training rule. Game thread.
void SetMatchPractice(bool enabled);
// Any thread: whether such a match is on, for the HUD's notice that shared
// save and reset are not available there.
bool MatchPracticeActive();
// Two bits of the pad's raw word that once carried a Training table's shared
// save and reset. The feature is retired; the bits stay reserved and are
// cleared as before, so the game never sees them.
constexpr unsigned ReservedInputBits = 0x60000000;
static_assert((ReservedInputBits & FightButtons) == 0, "reserved bits overlap the fight buttons");
// At a Training table: the raw word without those bits. Applied to the local
// pad before it goes to GGPO and to both of GGPO's inputs before the game
// plays a frame, simulated or resimulated. Any other match is left alone.
unsigned ClearReservedInputBits(unsigned raw);
// Whether the player wants that meter. Any thread.
void WatchMatches(bool enabled);
void CloseBattle();
void StopCapture();
// The override exists only during one native offline training update.
bool ReadOverride(int side, Input& result);
} }
