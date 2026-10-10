#pragma once

#include <memory>

#include "../common/ReplayInputLane.hxx"
#include "../common/ReplayTransport.hxx"

namespace Dimps { namespace Game { namespace Battle { struct System; } } }

// Pause, frame step and speed while the game's Battle Log plays a replay,
// with the frame meter and the input lanes that go with them
// (common/ReplayTransport.hxx has the rules). Everything but Submit is the
// game thread's.
namespace sf4e { namespace replayplayback {

// Detours the replay cadence function (0x5D7750). Inside the Detours
// transaction, like the other installs. Outside a replay's playback the
// detour calls the game's function and does nothing else.
void Install();

// Any thread: a command from the keyboard (the overlay) or the pad (Tick).
// It acts at the next cadence call, if the playback can take it then.
void Submit(replaytransport::Command command, replaytransport::Device device);

// The battle starts or closes: the transport plays at 1x again.
void StartBattle(Dimps::Game::Battle::System* system);
void CloseBattle();

// After an offline battle update: whether the frame meter is to be shown
// the frame it reached. Only while the meter is shown, and only when the
// battle moved on; a held frame would start the meter over.
bool FeedMeter(Dimps::Game::Battle::System* system);

// Once a game tick, with the player's controller: reads the pad's controls
// during playback and fetches the replay's inputs for the lanes.
void Tick(int deviceType, int deviceIndex, bool connected);

const replaytransport::View& GetView();
// The lanes of the replay Ember is playing, or null: a replay the player
// started from the game's own list, or one still being read.
std::shared_ptr<const replaylane::Lanes> GetLanes();

} }
