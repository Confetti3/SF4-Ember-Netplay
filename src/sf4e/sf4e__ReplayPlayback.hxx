#pragma once

#include <cstdint>
#include <memory>

#include "../common/ReplayInputLane.hxx"
#include "../common/ReplayTransport.hxx"

namespace Dimps { namespace Game { namespace Battle { struct System; } } }

// Pause, frame step and speed while the game's Battle Log plays a replay,
// with the frame meter and the input lanes that go with them
// (common/ReplayTransport.hxx has the rules). Everything but Submit is the
// game thread's.
//
// One playback is a session, from a battle's start to its close (or the next
// start, as "Play again" starts one without a close). The session owns what
// the controls leave behind: commands waiting, the pad's held buttons, the
// transport, the meter's gate and the strip's first showing. Starting or
// closing a battle ends the session and drops all of it; only the lanes'
// visibility, the player's own choice, is kept for the game's run.
namespace sf4e { namespace replayplayback {

// Detours the replay cadence function (0x5D7750). Inside the Detours
// transaction, like the other installs. Outside a replay's playback the
// detour calls the game's function and does nothing else.
void Install();

// Any thread: a command from the keyboard (the overlay) or the pad (Tick),
// made under the playback session named (View::session). It acts at the next
// cadence call, if that session is still the playback's and it can take it then.
void Submit(replaytransport::Command command, replaytransport::Device device, std::uint32_t session);

// The battle starts or closes: a new session, playing at 1x.
void StartBattle(Dimps::Game::Battle::System* system);
void CloseBattle();

// After each offline battle update: observes the recorder as the update left
// it (ReplayTransport.hxx: Observation) for the strip, the lanes and the
// export's progress, and says whether the frame meter is to be shown the
// frame the update reached. Only while the meter is shown, and only when the
// battle moved on; a held frame would start the meter over.
bool AfterUpdate(Dimps::Game::Battle::System* system);

// Once a game tick, with the player's controller: reads the pad's controls
// during playback and asks for the replay's inputs for the lanes.
void Tick(int deviceType, int deviceIndex, bool connected);

const replaytransport::View& GetView();
// The lanes of the replay Ember is playing, or null: a replay the player
// started from the game's own list, or one still being read. Built by the
// worker that reads replay details, never on the game thread.
std::shared_ptr<const replaylane::Lanes> GetLanes();

} }
