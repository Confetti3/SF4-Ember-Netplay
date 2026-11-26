#include "sf4e__ReplayTakeover.hxx"

#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../common/ReplayRecorder.hxx"
#include "sf4e__BattleFighters.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__ReplayPlayback.hxx"

namespace {
using rSystem = Dimps::Game::Battle::System;
using fSystem = sf4e::Game::Battle::System;
namespace takeover = sf4e::takeover;
using takeover::Phase;
using takeover::Step;

// The moment a takeover began, kept until it ends: the battle as saved
// (optional to the match, as a replay's snapshots are), and the recorder's
// cursor and the battle's frames simulated there. The update after a load
// plays record `cursor`, and each frame simulated since moves it on by one; a
// load puts the count back with the battle.
struct Takeover {
	Phase phase = Phase::Off;
	takeover::Plan plan;
	fSystem::SaveState save;
	std::uint32_t cursor = 0;
	int simulated = 0;
	takeover::Recording recording;
	// The record the update under way plays, and the other fighter's word for it.
	std::uint32_t frame = 0;
	unsigned otherWord = 0;
	std::vector<Step> asked;
	Takeover() { save.stake = sf4e::memento::Stake::Optional; }
} s_takeover;

int Simulated(rSystem* system) { return rSystem::GetNumFramesSimulated_FixedPoint(system)->integral; }

// Whether the fighter not taken stands to the right of the player's.
bool OtherOnRight(rSystem* system, int side) {
	const auto roots = sf4e::battle::ReadRoots(system);
	return roots[0] && roots[1] && roots[1 - side][0] > roots[side][0];
}

// The round the recorder plays, from its own stream (ReplayRecorder.hxx), so a
// replay the game's own list started is taken over as one Ember started is.
// False when there is nothing whole to follow.
bool ReadRound(const sf4e::replay::Recorder& recorder, takeover::Recording& out) {
	if (recorder.round < 0 || recorder.round >= sf4e::replay::kStreams) return false;
	const auto& stream = recorder.streams[recorder.round];
	sf4e::replayinputs::Round round;
	std::uint32_t played = 0;
	if (!stream.buffer || !sf4e::replayinputs::ReadStream(stream.buffer, stream.bytes, played, round) || !round.frames) return false;
	out = takeover::Recording(round);
	return true;
}

void GiveBack(bool engineAlive) {
	if (!s_takeover.save.used) return;
	if (engineAlive) fSystem::SaveState::Free(&s_takeover.save);
	else fSystem::SaveState::Reclaim(&s_takeover.save, "replay_takeover", 0);
}

// The takeover ends: its save is given back and the replay is the recorder's again.
void Finish(bool engineAlive) {
	GiveBack(engineAlive);
	s_takeover.phase = Phase::Off;
}

// The recorder stops playing, so what the battle reads is the takeover's from the next update.
void Stop(sf4e::replay::Recorder& recorder) {
	recorder.mode = sf4e::replay::RecorderStopped;
	s_takeover.phase = Phase::Playing;
}

// A save that does not load leaves the battle, as a replay's jump back does.
bool Load(rSystem* system) {
	if (fSystem::SaveState::Load(&s_takeover.save)) return true;
	spdlog::error("Takeover: the save did not load; leaving the battle");
	*rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
	Finish(true);
	return false;
}

// Only in the fight, with a round to follow: the cadence takes a takeover's
// command only under Ember's controls, and the fight may end before it acts.
// False when it cannot begin.
bool Begin(rSystem* system, sf4e::replay::Recorder& recorder, int side) {
	if (!(system->*rSystem::publicMethods.IsFight)()) return false;
	takeover::Recording recording;
	if (!ReadRound(recorder, recording)) { spdlog::warn("Takeover: round {} has no recording to follow", recorder.round + 1); return false; }
	if (!fSystem::SaveState::Save(&s_takeover.save)) { spdlog::warn("Takeover: the battle could not be saved"); return false; }
	fSystem::SaveState::ForgetFinishedSounds(&s_takeover.save);
	s_takeover.plan.side = side;
	s_takeover.cursor = recorder.cursor;
	s_takeover.simulated = Simulated(system);
	s_takeover.recording = std::move(recording);
	Stop(recorder);
	spdlog::info("Takeover: player {} taken at frame {} of round {} ({} recorded)", side + 1, recorder.cursor, recorder.round + 1, s_takeover.recording.Frames());
	return true;
}

// False when a takeover could not begin. A save that does not load leaves the battle (Load).
bool Act(rSystem* system, sf4e::replay::Recorder& recorder, Step step) {
	switch (step) {
	case Step::TakePlayer1: return Begin(system, recorder, 0);
	case Step::TakePlayer2: return Begin(system, recorder, 1);
	case Step::Retry: if (Load(system)) Stop(recorder); return true;
	case Step::BackToReplay: if (Load(system)) Finish(true); return true;
	case Step::CycleOther: s_takeover.plan.other = takeover::NextOther(s_takeover.plan.other); return true;
	}
	return true;
}
}

void sf4e::replaytakeover::Ask(Step step) { s_takeover.asked.push_back(step); }

std::uint32_t sf4e::replaytakeover::BeforeUpdate(rSystem* system) {
	sf4e::replay::Recorder* recorder = sf4e::replayplayback::OfflineRecorder();
	const auto asked = std::exchange(s_takeover.asked, {});
	// A step acts on a replay the recorder plays, or on the takeover holding one.
	if (!recorder || (s_takeover.phase == Phase::Off && recorder->mode != sf4e::replay::RecorderPlaying)) return 0;
	std::uint32_t refused = 0;
	for (const Step step : asked) if (takeover::Applies(step, s_takeover.phase) && !Act(system, *recorder, step)) ++refused;
	if (s_takeover.phase != Phase::Playing) return refused;
	// The knockout or the time running out ends the takeover's round; the cadence holds the battle there.
	if (!(system->*rSystem::publicMethods.IsFight)()) {
		s_takeover.phase = Phase::RoundOver;
		spdlog::info("Takeover: the round is over at frame {}", sf4e::replaytakeover::Cursor(*recorder, Simulated(system)));
		return refused;
	}
	const int side = s_takeover.plan.side;
	const std::uint32_t frame = sf4e::replaytakeover::Cursor(*recorder, Simulated(system));
	takeover::FrameFeed feed;
	feed.recorded[0] = s_takeover.recording.At(0, frame);
	feed.recorded[1] = s_takeover.recording.At(1, frame);
	feed.otherOnRight = OtherOnRight(system, side);
	s_takeover.frame = frame;
	s_takeover.otherWord = takeover::OtherWord(s_takeover.plan, feed);
	return refused;
}

sf4e::replaytakeover::Source sf4e::replaytakeover::SourceOf(int side, unsigned& word) {
	if (s_takeover.phase == Phase::Off || side < 0 || side > 1) return Source::Native;
	if (side == s_takeover.plan.side) return Source::Player;
	word = s_takeover.otherWord;
	return Source::Other;
}

sf4e::takeover::Phase sf4e::replaytakeover::GetPhase() { return s_takeover.phase; }
sf4e::takeover::Plan sf4e::replaytakeover::GetPlan() { return s_takeover.plan; }

std::uint32_t sf4e::replaytakeover::Cursor(const sf4e::replay::Recorder& recorder, int simulated) {
	if (s_takeover.phase == Phase::Off) return recorder.cursor;
	// The engine counts frames in 16 bits: the difference is taken in them, so a count that wraps still adds up.
	return s_takeover.cursor + static_cast<std::uint16_t>(simulated - s_takeover.simulated);
}

std::uint32_t sf4e::replaytakeover::Frame() { return s_takeover.frame; }

void sf4e::replaytakeover::End(bool engineAlive) {
	Finish(engineAlive);
	s_takeover.asked.clear();
}
