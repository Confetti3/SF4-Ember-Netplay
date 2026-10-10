#include "sf4e__ReplayPlayback.hxx"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>
#include <detours/detours.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../platform/ReplayFiles.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__ReplayCapture.hxx"
#include "sf4e__ReplayStore.hxx"

namespace {

using rSystem = Dimps::Game::Battle::System;
using Dimps::Game::Battle::ReplaySystem;
namespace transport = sf4e::replaytransport;
using transport::Command;
using transport::Device;

static_assert(transport::kFlowReady == rSystem::BF__READY && transport::kFlowFight == rSystem::BF__FIGHT &&
	transport::kFlowRoundStart == rSystem::BF__ROUND_START, "ReplayTransport.hxx flows differ from the native ones");
static_assert(transport::kPauseOrHold == rSystem::SSF_PAUSE_OR_HOLD && transport::kReplayFreeze == rSystem::SSF_REPLAY_FREEZE,
	"ReplayTransport.hxx simulation flags differ from the native ones");
static_assert(transport::kRecorderPlaying == sf4e::replay::RecorderPlaying, "ReplayTransport.hxx recorder mode differs from ReplayRecorder.hxx");

// Commands from any thread, taken at the next cadence call. Bounded: the
// cadence does not run outside a battle, and nothing waits that long.
struct Queued { Command command; Device device; };
std::mutex s_queueMutex;
std::vector<Queued> s_queue;
constexpr std::size_t kMostQueued = 16;

// The rest is the game thread's.
transport::Transport s_transport;
transport::View s_view;
transport::AdvanceGate s_meterGate;
transport::PadControls s_pad;
bool s_padActive = false, s_deviceKnown = false;
// F5 turns the replay's own meter choice the other way; the lanes stay as
// the player last left them for the rest of the game's run.
bool s_meterFlipped = false, s_lanesOn = false;
// The round the transport was last reset for, and whether the fight was
// reached in this battle (the strip then shows itself once).
int s_round = -1;
bool s_reachedFight = false;
// The ways the detour went this battle, each logged the first time (in-game check 7).
unsigned s_loggedRoutes = 0;
// The lanes: the file asked for, the detail request's revision, the lanes.
std::string s_laneFile;
std::uint64_t s_laneRevision = 0, s_laneRequests = 0;
std::shared_ptr<const sf4e::replaylane::Lanes> s_lanes;
// Detail requests from here are told apart from the Replays screen's by the top bit.
constexpr std::uint64_t kLaneRevisionBit = 0x8000000000000000ull;

void Leave() {
	s_view.playback = s_view.armed = s_view.exporting = false;
}

const char* RouteName(transport::Route route) {
	return route == transport::Route::Transport ? "Ember's controls" : route == transport::Route::FullSpeed ? "the game's, at 1x for an export" : "the game's";
}

void NoteRoute(int route, const char* what) {
	const unsigned bit = 1u << (route + 1);
	if (s_loggedRoutes & bit) return;
	s_loggedRoutes |= bit;
	spdlog::info("Replay controls: the cadence is {}", what);
}

struct Cadence : rSystem {
	void Run();
};

void Cadence::Run() {
	rSystem* system = this;
	transport::Context context;
	context.netplay = sf4e::Game::Battle::System::ggpo != nullptr;
	const sf4e::replay::Recorder* recorder = context.netplay ? nullptr : ReplaySystem::staticMethods.GetSingleton();
	context.recorderMode = recorder ? recorder->mode : sf4e::replay::RecorderStopped;
	if (!context.netplay && context.recorderMode == transport::kRecorderPlaying)
		context.training = (system->*rSystem::publicMethods.GetGameMode)() == Dimps::Game::Battle::GAMEMODE_TRAINING;
	if (!transport::Playback(context)) {
		// Netplay, Versus, Training: the game's function and nothing else.
		if (s_view.playback) Leave();
		NoteRoute(-1, "the game's (no replay playing)");
		(this->*rSystem::publicMethods.ReplayCadence)();
		return;
	}
	DWORD* const flags = rSystem::GetSimulationFlags(system);
	int* const slow = rSystem::GetSlowMotion(system);
	context.exporting = sf4e::replaystore::ExportPlaying() || sf4e::replaycapture::GetState() == sf4e::replaycapture::State::Recording;
	context.fight = (system->*rSystem::publicMethods.IsFight)();
	context.pauseMenu = (*flags & transport::kPauseOrHold) != 0;
	const transport::Route route = transport::Decide(context);
	NoteRoute(static_cast<int>(route), RouteName(route));

	// Each round starts playing at 1x, the game's half speed flag cleared,
	// so its legend reads normal speed when the round is introduced.
	if (transport::RoundBoundary(static_cast<int>(*rSystem::staticVars.CurrentBattleFlow)) || recorder->round != s_round) {
		s_round = recorder->round;
		s_transport.Reset();
		*slow = 0; s_transport.Wrote(false);
	}

	std::vector<Queued> pending;
	{ std::lock_guard<std::mutex> lock(s_queueMutex); pending.swap(s_queue); }
	if (route == transport::Route::Transport) s_transport.FoldNative(*slow != 0);
	for (const Queued& queued : pending) {
		// An export plays untouched, and under the pause menu the keys are its.
		if (context.exporting || context.pauseMenu) continue;
		if (transport::MovesTransport(queued.command)) {
			if (route != transport::Route::Transport) { ++s_view.refused; continue; }
			s_transport.Apply(queued.command);
		}
		else if (queued.command == Command::Meter) s_meterFlipped = !s_meterFlipped;
		else s_lanesOn = !s_lanesOn;
		++s_view.taken; s_view.device = queued.device;
	}
	// Reaching the fight shows the strip once, so the controls are seen.
	if (route == transport::Route::Transport && !s_reachedFight) { s_reachedFight = true; ++s_view.taken; }

	switch (route) {
	case transport::Route::FullSpeed:
		*slow = 0; s_transport.Wrote(false);
		(this->*rSystem::publicMethods.ReplayCadence)();
		break;
	case transport::Route::Original:
		(this->*rSystem::publicMethods.ReplayCadence)();
		break;
	case transport::Route::Transport:
		if (s_transport.Hold()) *flags |= transport::kReplayFreeze;
		else *flags &= ~static_cast<DWORD>(transport::kReplayFreeze);
		*slow = s_transport.SlowFlag() ? 1 : 0;
		break;
	}

	s_view.playback = true;
	s_view.armed = route == transport::Route::Transport;
	s_view.exporting = context.exporting;
	s_view.paused = s_transport.Paused();
	s_view.divisor = s_transport.Divisor();
	s_view.round = recorder->round;
	s_view.cursor = recorder->cursor;
	s_view.meter = sf4e::replaystore::MeterWanted() != s_meterFlipped;
	s_view.lanes = s_lanesOn;
}

}

void sf4e::replayplayback::Install() {
	void (Cadence::* detour)() = &Cadence::Run;
	DetourAttach(reinterpret_cast<PVOID*>(&rSystem::publicMethods.ReplayCadence), *reinterpret_cast<PVOID*>(&detour));
}

void sf4e::replayplayback::Submit(Command command, Device device) {
	std::lock_guard<std::mutex> lock(s_queueMutex);
	if (s_queue.size() < kMostQueued) s_queue.push_back({command, device});
}

void sf4e::replayplayback::StartBattle(Dimps::Game::Battle::System* system) {
	s_transport.Reset();
	s_round = -1;
	s_meterGate.Reset();
	// "Play again" may start a battle without closing the last one; a half
	// speed Ember left in the game's flag must not carry over.
	const sf4e::replay::Recorder* recorder = Game::Battle::System::ggpo ? nullptr : ReplaySystem::staticMethods.GetSingleton();
	if (system && recorder && recorder->mode == transport::kRecorderPlaying &&
		(system->*rSystem::publicMethods.GetGameMode)() != Dimps::Game::Battle::GAMEMODE_TRAINING) {
		*rSystem::GetSlowMotion(system) = 0;
		s_transport.Wrote(false);
	}
}

void sf4e::replayplayback::CloseBattle() {
	if (s_view.playback || s_reachedFight) spdlog::info("Replay controls: the battle closed ({} commands taken, {} refused outside the fight)", s_view.taken, s_view.refused);
	s_transport.Reset();
	// The game zeroes its half speed flag as it closes (0x5DA5F0).
	s_transport.Wrote(false);
	s_meterGate.Reset();
	s_pad.Reset(); s_padActive = false; s_deviceKnown = false;
	s_meterFlipped = false; s_round = -1; s_reachedFight = false; s_loggedRoutes = 0;
	Leave();
	std::lock_guard<std::mutex> lock(s_queueMutex);
	s_queue.clear();
}

bool sf4e::replayplayback::FeedMeter(Dimps::Game::Battle::System* system) {
	if (!s_view.playback || !s_view.meter) { s_meterGate.Reset(); return false; }
	return s_meterGate.Advanced(static_cast<std::uint16_t>(rSystem::GetNumFramesSimulated_FixedPoint(system)->integral));
}

void sf4e::replayplayback::Tick(int deviceType, int deviceIndex, bool connected) {
	const bool listening = s_view.playback && !s_view.exporting;
	if (listening && !s_deviceKnown) {
		// Until a control is used, the strip shows the player's own device.
		s_deviceKnown = true;
		s_view.device = deviceType == input::PadXInput && connected ? Device::Pad : Device::Keyboard;
	}
	// DirectInput pads have no known button layout, so only XInput's are read.
	unsigned held = 0, physical = 0;
	const bool pad = listening && connected && deviceType == input::PadXInput &&
		Dimps::Pad::ReadController(deviceType, deviceIndex, held, &physical);
	if (!pad) s_padActive = false;
	else if (!s_padActive) {
		// Buttons already down when the playback starts are not presses.
		s_padActive = true; s_pad.Reset(physical);
	}
	else {
		Command commands[4];
		const int count = s_pad.Sample(physical, commands);
		for (int i = 0; i < count; i++) Submit(commands[i], Device::Pad);
	}

	// The lanes read the file a Watch request plays, through the worker that
	// reads replay details, so the game thread opens no file.
	const std::string& file = replaystore::PlayingFile();
	if (file != s_laneFile) {
		s_laneFile = file; s_lanes.reset();
		if (!file.empty()) {
			s_laneRevision = kLaneRevisionBit | ++s_laneRequests;
			platform::replays::WantDetail(file, s_laneRevision);
		}
	}
	if (!s_laneFile.empty() && !s_lanes) {
		const auto detail = platform::replays::LatestDetail();
		if (detail.revision == s_laneRevision && detail.state == replayinputs::DetailState::Ready && detail.value && detail.value->file == s_laneFile)
			s_lanes = std::make_shared<const replaylane::Lanes>(replaylane::Build(detail.value->match));
		else if (detail.revision == s_laneRevision && detail.state != replayinputs::DetailState::Pending && detail.state != replayinputs::DetailState::Ready) {
			spdlog::warn("Replay controls: the inputs of {} could not be read; no input lanes for it", s_laneFile);
			s_laneRevision = 0;
		}
	}
}

const sf4e::replaytransport::View& sf4e::replayplayback::GetView() { return s_view; }

std::shared_ptr<const sf4e::replaylane::Lanes> sf4e::replayplayback::GetLanes() { return s_lanes; }
