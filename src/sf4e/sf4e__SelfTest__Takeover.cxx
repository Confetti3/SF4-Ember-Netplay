// The takeover test (docs/design/REPLAY_TAKEOVER.md), on a replay the battle
// log plays out of Ember's archive (its largest, for a long first round). It
// sends the takeover's commands as the keys do (replayplayback::Submit) and
// plays the fighter taken itself (MatchInput). Each pass plays kPass frames
// from the moment taken; frames are compared by the replay's frame they stand
// at (the view's cursor), so a tick that sees two updates or none costs a
// sample, never a false difference.
//   Followed  P1 taken and played as recorded, P2 following: the recording.
//   Taken     Retry, P1 walking right then left: it has to differ.
//   Retried   Retry, the same walk: equal to Taken, frame for frame.
//   Recorded  Back to the replay: equal to Followed.
//   Stand, Block, Low   P2 taken and still, P1 the dummy, one Retry apart:
//             blocking walks P1 away, and a low block is another stance.
//   RoundEnd  near the round's end P1 is taken again and played as recorded,
//             so the round ends under the takeover: the battle has to hold
//             there, Retry has to bring the fight back, and Back to the
//             replay has to play on.
#include "sf4e__SelfTest.hxx"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../common/ReplayTakeover.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include "sf4e__BattleFighters.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__ReplayPlayback.hxx"
#include "sf4e__ReplayStore.hxx"
#include "sf4e__ReplayTakeover.hxx"

namespace sf4e { namespace selftest {
namespace {
namespace fs = std::filesystem;
using rSystem = Dimps::Game::Battle::System;
using replaystore::Step;
using replaytransport::Command;
using takeover::Phase;

enum class Stage { Listing, Watch, Watching, Done };
enum class Pass { Waiting, Followed, Taken, Retried, Recorded, Stand, Block, Low, Replay, RoundEnd, Held, Again, Back, Over };
// What the fighter taken does: nothing, what was recorded, or a walk right then left.
enum class Own { Nothing, Recording, Walk };
constexpr int kTakeAt = 120, kPass = 90, kHeld = 2 * kSecond;
// How long before the round's recorded end the last takeover begins, and how long it may take to end.
constexpr std::uint32_t kBeforeEnd = 600;
constexpr int kRoundEndTicks = 30 * kSecond;

struct Frame {
	float x[2] = {0, 0};
	int action[2] = {-1, -1};
	bool operator==(const Frame& o) const { return x[0] == o.x[0] && x[1] == o.x[1] && action[0] == o.action[0] && action[1] == o.action[1]; }
	float Distance() const { return std::fabs(x[0] - x[1]); }
};
using Frames = std::map<std::uint32_t, Frame>;

struct Run : Steps<Stage> {
	std::wstring replay;
	int tries = 0, ticks = 0;
	bool left = false;
	Pass pass = Pass::Waiting;
	// The pass's first replay frame, whether it has begun (the jump back seen), and its ticks.
	std::uint32_t base = 0, roundEnd = 0;
	int round = 0;
	bool begun = false;
	int passTicks = 0, fight = 0;
	Own own = Own::Nothing;
	takeover::Recording recording;
	std::map<Pass, Frames> frames;
	Frame held;
	int heldSame = 0;
} s_run;

bool Read(Frame& frame) {
	using Actor = Dimps::Game::Battle::Chara::Actor;
	const auto actors = battle::ReadActors(rSystem::staticMethods.GetSingleton ? rSystem::staticMethods.GetSingleton() : nullptr);
	for (unsigned side = 0; side < 2; ++side) {
		if (!actors[side]) return false;
		frame.x[side] = (actors[side]->*Actor::publicMethods.GetCurrentRootPosition)()[0];
		frame.action[side] = (actors[side]->*Actor::publicMethods.GetActionID)();
	}
	return true;
}

void Send(Command command) { replayplayback::Submit(command, replaytransport::Device::Keyboard, replayplayback::GetView().session); }

// The round the recorder plays, from the replay's file (ReplayInputs.hxx): the
// fighter taken plays it from there, not from the recorder's own stream that
// the takeover reads, so the two have to agree.
void ReadRecording(int round) {
	std::ifstream in(fs::path(s_run.replay), std::ios::binary);
	const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	replayinputs::Match match;
	if (replayinputs::Parse(bytes.data(), bytes.size(), match) && round < static_cast<int>(match.rounds.size()))
		s_run.recording = takeover::Recording(match.rounds[static_cast<std::size_t>(round)]);
	spdlog::info("Takeover test: the file's round {} holds {} frames", round + 1, s_run.recording.Frames());
}

int Differing(const Frames& a, const Frames& b, int& common) {
	int count = 0; common = 0;
	for (const auto& [frame, seen] : a) {
		const auto other = b.find(frame);
		if (other == b.end()) continue;
		++common; count += !(seen == other->second);
	}
	return count;
}
float DistanceAtEnd(const Frames& frames) { return frames.empty() ? 0.f : frames.rbegin()->second.Distance(); }

// The next pass, after the commands that start it.
void Next(Pass pass, Own own, std::initializer_list<Command> commands) {
	for (const Command command : commands) Send(command);
	s_run.pass = pass; s_run.own = own; s_run.begun = false; s_run.passTicks = 0;
}

void Report() {
	auto& f = s_run.frames;
	int common = 0;
	const int followed = Differing(f[Pass::Followed], f[Pass::Recorded], common);
	Say(followed == 0 && common > kPass * 2 / 3, "the fighter taken and the other follow the recording",
		std::to_string(followed) + " of " + std::to_string(common) + " frames differ");
	const int live = Differing(f[Pass::Taken], f[Pass::Recorded], common);
	Say(live > 10, "the player's input reaches the fighter taken", std::to_string(live) + " of " + std::to_string(common) + " frames differ from the recording");
	const int retried = Differing(f[Pass::Taken], f[Pass::Retried], common);
	Say(retried == 0 && common > kPass * 2 / 3, "Retry plays the takeover again exactly", std::to_string(retried) + " of " + std::to_string(common) + " frames differ");
	const float stand = DistanceAtEnd(f[Pass::Stand]), block = DistanceAtEnd(f[Pass::Block]);
	Say(block > stand, "the dummy blocks away from the player", "apart " + std::to_string(stand) + " standing, " + std::to_string(block) + " blocking");
	const int low = Differing(f[Pass::Block], f[Pass::Low], common);
	Say(low > 10, "the dummy's low block is another stance", std::to_string(low) + " of " + std::to_string(common) + " frames differ");
}

void Leave(const char* why) {
	s_run.pass = Pass::Over;
	s_run.left = GameEvents::MainMenu::LeaveLocalBattleLog();
	spdlog::info("Takeover test: leaving the replay ({}), {}", why, s_run.left ? "asked" : "the battle log is not in front");
}

// One tick of the passes, once the replay plays.
void Passes(const replaytransport::View& view) {
	Run& s = s_run;
	Frame now;
	if (!Read(now)) return;
	++s.passTicks;
	// A pass begins once its jump back is seen; then it keeps its window's frames.
	const auto keep = [&](Pass done, Phase phase) {
		if (!s.begun) s.begun = view.takeover == phase && view.cursor <= s.base + kPass / 2;
		if (s.begun && view.cursor > s.base && view.cursor <= s.base + kPass) s.frames[done][view.cursor] = now;
		return s.begun && view.cursor >= s.base + kPass;
	};
	switch (s.pass) {
	case Pass::Waiting:
		if (!view.armed || ++s.fight < kTakeAt) return;
		s.round = view.round;
		ReadRecording(view.round);
		s.base = view.cursor;
		Next(Pass::Followed, Own::Recording, {Command::TakeoverPrimary});
		return;
	case Pass::Followed:
		if (keep(Pass::Followed, Phase::Playing)) Next(Pass::Taken, Own::Walk, {Command::TakeoverPrimary});
		break;
	case Pass::Taken:
		if (keep(Pass::Taken, Phase::Playing)) Next(Pass::Retried, Own::Walk, {Command::TakeoverPrimary});
		break;
	case Pass::Retried:
		if (keep(Pass::Retried, Phase::Playing)) Next(Pass::Recorded, Own::Nothing, {Command::TakeoverSecondary});
		break;
	case Pass::Recorded:
		if (!keep(Pass::Recorded, Phase::Off)) break;
		s.base = view.cursor;
		Next(Pass::Stand, Own::Nothing, {Command::TakeoverOther, Command::TakeoverSecondary});
		break;
	case Pass::Stand:
		if (keep(Pass::Stand, Phase::Playing)) Next(Pass::Block, Own::Nothing, {Command::TakeoverOther, Command::TakeoverPrimary});
		break;
	case Pass::Block:
		if (keep(Pass::Block, Phase::Playing)) Next(Pass::Low, Own::Nothing, {Command::TakeoverOther, Command::TakeoverPrimary});
		break;
	case Pass::Low:
		if (!keep(Pass::Low, Phase::Playing)) break;
		Report();
		s.roundEnd = view.roundFrames[view.round];
		Next(Pass::Replay, Own::Nothing, {Command::TakeoverOther, Command::TakeoverSecondary});
		break;
	case Pass::Replay:
		// The replay plays on to near the round's end, where the last takeover begins.
		if (view.round != s.round) { Say(false, "a takeover's round ends", "the round was over more than 10 s before its recording"); Leave("no fight left"); break; }
		if (view.takeover != Phase::Off || !view.armed || view.cursor + kBeforeEnd < s.roundEnd) break;
		s.base = view.cursor;
		Next(Pass::RoundEnd, Own::Recording, {Command::TakeoverPrimary});
		break;
	case Pass::RoundEnd:
		if (view.takeover == Phase::RoundOver) { s.held = now; s.heldSame = 0; Next(Pass::Held, Own::Nothing, {}); }
		else if (s.passTicks > kRoundEndTicks) { Say(false, "a takeover's round ends", "still " + std::to_string(static_cast<int>(view.takeover)) + " after 30 s"); Leave("no round end"); }
		break;
	case Pass::Held:
		s.heldSame += now == s.held && view.takeover == Phase::RoundOver;
		if (s.passTicks < kHeld) break;
		Say(s.heldSame == s.passTicks, "the battle holds at a takeover's round end", std::to_string(s.heldSame) + " of " + std::to_string(s.passTicks) + " ticks unchanged");
		Next(Pass::Again, Own::Nothing, {Command::TakeoverPrimary});
		break;
	case Pass::Again:
		if (view.takeover == Phase::Playing && view.cursor <= s.base + kPass) {
			Say(true, "Retry after the round's end brings the fight back", "at frame " + std::to_string(view.cursor));
			Next(Pass::Back, Own::Nothing, {Command::TakeoverSecondary});
		}
		else if (s.passTicks > 5 * kSecond) { Say(false, "Retry after the round's end brings the fight back", "phase " + std::to_string(static_cast<int>(view.takeover))); Leave("no retry"); }
		break;
	case Pass::Back:
		if (view.takeover == Phase::Off && view.cursor > s.base + kPass) { Say(true, "Back to the replay plays on", "at frame " + std::to_string(view.cursor)); Leave("done"); }
		else if (s.passTicks > 10 * kSecond) { Say(false, "Back to the replay plays on", "phase " + std::to_string(static_cast<int>(view.takeover))); Leave("no replay"); }
		break;
	case Pass::Over:
		break;
	}
}
}

void TakeoverMatchInput(int side, unsigned& mapped, unsigned& raw) {
	Run& s = s_run;
	if (s.stage != Stage::Watching) return;
	const std::uint32_t frame = replaytakeover::Frame();
	switch (s.own) {
	case Own::Nothing: mapped = raw = 0; break;
	case Own::Recording: mapped = raw = replayinputs::PadWord(s.recording.At(side, frame)); break;
	case Own::Walk: mapped = raw = frame - s.base < kPass / 2 ? replayinputs::Right : replayinputs::Left; break;
	}
}

bool StartTakeover() {
	platform::replays::WantListing();
	s_run.Enter(Stage::Listing);
	return true;
}

void TickTakeover(bool atMainMenu) {
	Run& run = s_run;
	run.waited++;
	const replaystore::Status& status = replaystore::GetStatus();
	switch (run.stage) {
	case Stage::Listing: {
		const auto listing = platform::replays::LatestListing();
		if (!listing) {
			if (run.waited % kSecond == 0) platform::replays::WantListing();
			if (run.Late(120)) run.Fail("archive listing", "none after two minutes");
			break;
		}
		std::uintmax_t largest = 0;
		for (const auto& replay : *listing) {
			std::error_code failed;
			const std::uintmax_t size = fs::file_size(replay.path, failed);
			if (!failed && size > largest) { largest = size; run.replay = replay.path.wstring(); }
		}
		if (run.replay.empty()) { run.Fail("archive listing", "no replay"); break; }
		Say(true, "archive listing", "testing with " + platform::WideToUtf8(fs::path(run.replay).filename().wstring()));
		replaystore::Start({replay::Mode::Watch, platform::WideToUtf8(run.replay), {}, true}, atMainMenu, true);
		run.Enter(Stage::Watch);
		break;
	}
	case Stage::Watch:
		if (status.step != Step::Idle) { run.Enter(Stage::Watching); break; }
		// Just after the main menu comes up the game is still settling: asked again each second, for half a minute.
		if (run.tries >= 30) { run.Fail("watch", "not started: '" + status.notice + "'"); break; }
		if (run.waited < kSecond) break;
		run.tries++;
		replaystore::Start({replay::Mode::Watch, platform::WideToUtf8(run.replay), {}, true}, atMainMenu, true);
		run.waited = 0;
		break;
	case Stage::Watching: {
		const replaytransport::View view = replayplayback::GetView();
		if (status.step == Step::Playing && view.playback && run.pass != Pass::Over) Passes(view);
		if (status.step == Step::Playing && run.pass != Pass::Over && ++run.ticks == 180 * kSecond) {
			Say(false, "takeover", "the passes never finished (" + std::to_string(static_cast<int>(run.pass)) + ")");
			Leave("out of time");
		}
		if (status.step == Step::Idle) {
			if (run.pass != Pass::Over) Say(false, "takeover", "the replay ended at pass " + std::to_string(static_cast<int>(run.pass)));
			run.Enter(Stage::Done);
			Finish();
		}
		break;
	}
	case Stage::Done:
		break;
	}
}

} }
