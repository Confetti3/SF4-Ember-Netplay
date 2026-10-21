#pragma once
#include "TrainingSession.hxx"

namespace Dimps { namespace Game { namespace Battle { struct System; } } }
namespace sf4e { namespace training {
View ReadView();
// Where the fighters stand and the battle they stand in, without the rest of
// the view: what a pad press keeps for the save it may become. Any thread.
struct Place { std::uint64_t generation = 0; float x[2] = {0, 0}; };
Place ReadPlace();
// Whether the pad's chord starts the recording rather than opening the
// controls (TrainingSession.hxx), as the mode was last published. Any thread.
bool ChordStartsRecording();
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
// inputs: both sides' pads for the frame, a replay's (FighterSample::input); null in a match.
void ObserveMatch(Dimps::Game::Battle::System* system, int stateFrame, int lastConfirmedInput, const unsigned* inputs = nullptr);
// SetMatchPractice: whether the battle being prepared is a Practice room's
// match. Game thread.
void SetMatchPractice(bool enabled);
// Any thread: whether such a match is on, until its battle closes. The
// runtime snapshot carries it (RuntimeSnapshot::practiceMatch) for the HUD's
// notice, the match frame meter and Ember's menu over the fight.
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
#ifdef SF4E_SELFTEST
// For the in-game self-test: lets the lab into an offline Versus battle too,
// where it otherwise runs in a Training battle only. Game thread.
void AllowOfflineVersusForTest();
// Reads the fighters' boxes whether or not Hitboxes is on. Game thread.
void ReadHitBoxesForTest();
#endif
// Whether the player wants that meter. Any thread.
void WatchMatches(bool enabled);
// How many frames the meter keeps, and whether an exchange after a pause goes
// on after the last (MeterOptions::history, continuous). Any thread.
void SetMeterHistory(int frames, bool continuous);
// Whether Hitboxes is on (MeterOptions::hitboxes); the fighters' boxes are
// read only while it is. Any thread.
void ShowHitBoxes(bool shown);
void CloseBattle();
void StopCapture();
// The override exists only during one native offline training update.
bool ReadOverride(int side, Input& result);
} }
