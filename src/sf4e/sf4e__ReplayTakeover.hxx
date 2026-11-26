#pragma once
// Replay takeover in the game (common/ReplayTakeover.hxx, docs/design/REPLAY_TAKEOVER.md).
// Taking over saves the battle (fSystem::SaveState, which keeps the recorder)
// and stops the recorder, so its recorded words no longer land over what the
// battle reads; from then on the battle reads both sides from here
// (fSystem::ReadButtons): the player's own controller for the fighter taken,
// and the recording or the dummy for the other. Retry loads the save and stops
// the recorder again; Back to the replay loads it and leaves the recorder
// playing. Everything here is the game thread's.
#include <cstdint>

#include "../common/ReplayTransport.hxx"

namespace Dimps { namespace Game { namespace Battle { struct System; } } }
namespace sf4e { namespace replay { struct Recorder; } }

namespace sf4e { namespace replaytakeover {
// A takeover command the replay cadence took (ReplayTransport.hxx: FateOf),
// acted on before the next battle update in the phase it finds then.
void Ask(replaytransport::Command command);
// Before each offline battle update, after replayplayback::BeforeUpdate.
void BeforeUpdate(Dimps::Game::Battle::System* system);

// What the battle reads for a side (0 or 1): its native pad outside a
// takeover; during one, the player's own controller for the fighter taken,
// and `word` for the other.
enum class Source { Native, Player, Other };
Source SourceOf(int side, unsigned& word);

takeover::Phase GetPhase();
takeover::Plan GetPlan();
// The replay's frame the battle stands at as the recorder's cursor counts it,
// the next record to play, with `simulated` the battle's frames simulated now:
// the recorder's own cursor, or during a takeover the one it was taken at plus
// the frames simulated since. Held frames (a pause menu) do not count.
std::uint32_t Cursor(const sf4e::replay::Recorder& recorder, int simulated);
// The record the battle update under way plays during a takeover: what the
// fighter taken would have done there is the recording's at it.
std::uint32_t Frame();
// The battle starts or closes: any takeover ends and its save is given back,
// freed while the engine is alive and only its records once it is gone.
void End(bool engineAlive);
} }
