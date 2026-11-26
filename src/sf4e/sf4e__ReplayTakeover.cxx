#include "sf4e__ReplayTakeover.hxx"

#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../common/ReplayRecorder.hxx"
#include "sf4e__Game__Battle__System.hxx"

namespace {
using rSystem = Dimps::Game::Battle::System;
using fSystem = sf4e::Game::Battle::System;
using Dimps::Game::Battle::ReplaySystem;

// The takeover frame. Optional to the battle (Stake::Optional): when it
// cannot be saved the takeover does not start, and nothing else changes.
fSystem::SaveState s_state;

}

void sf4e::replaytakeover::Hand(Dimps::Game::Battle::System* system, int side, bool player) {
	sf4e::replay::Recorder* recorder = ReplaySystem::staticMethods.GetSingleton();
	if (!recorder || side < 0 || side > 1) return;
	const std::int32_t bit = 1 << side;
	recorder->playingSides = player ? recorder->playingSides & ~bit : recorder->playingSides | bit;
	// The player's controller is pad slot 0, Player 1's own; Player 2 reads
	// it while taken and its own slot again after.
	if (side == 1)
		if (int* slot = rSystem::GetPlayerPadSlot(system, 1)) *slot = player ? 0 : 1;
}

bool sf4e::replaytakeover::Carry(Dimps::Game::Battle::System* system, Act act, int side) {
	switch (act) {
	case Act::None: return true;
	case Act::Begin:
		Drop(true);
		s_state.stake = sf4e::memento::Stake::Optional;
		if (!fSystem::SaveState::Save(&s_state)) { spdlog::warn("Replay takeover: the frame could not be saved; no takeover"); return false; }
		fSystem::SaveState::ForgetFinishedSounds(&s_state);
		Hand(system, side, true);
		spdlog::info("Replay takeover: Player {} from record {}", side + 1, ReplaySystem::staticMethods.GetSingleton()->cursor);
		return true;
	case Act::Restart: case Act::Return:
		// The state was saved before the side was taken: loading it puts the
		// recorder's sides back to the replay's, and Restart takes the side again.
		if (!s_state.used || !fSystem::SaveState::Load(&s_state)) {
			spdlog::error("Replay takeover: the takeover frame did not load; leaving the battle");
			*rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
			return false;
		}
		Hand(system, side, act == Act::Restart);
		return true;
	case Act::End:
		Hand(system, side, false);
		Drop(true);
		return true;
	}
	return true;
}

void sf4e::replaytakeover::Drop(bool engineAlive) {
	if (!s_state.used) return;
	if (engineAlive) fSystem::SaveState::Free(&s_state);
	else fSystem::SaveState::Reclaim(&s_state, "replay_takeover", 0);
}
