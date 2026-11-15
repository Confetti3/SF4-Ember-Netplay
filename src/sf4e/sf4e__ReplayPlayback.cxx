#include "sf4e__ReplayPlayback.hxx"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <memory>
#include <vector>
#include <windows.h>
#include <detours/detours.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../common/BattlePause.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__ReplayCapture.hxx"
#include "sf4e__ReplayStore.hxx"
#include "../training/CameraControl.hxx"

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
static_assert(transport::kRecorderPlaying == sf4e::replay::RecorderPlaying && transport::kRounds == sf4e::replay::kStreams &&
	transport::kRounds == sf4e::replay::ExportClock::kRounds, "ReplayTransport.hxx recorder fields differ from ReplayRecorder.hxx");

// Commands from any thread, with the session each was made under.
transport::CommandQueue s_queue;

// The rest is the game thread's. The session's own state, all of it dropped
// by BeginSession:
transport::Transport s_transport;
transport::View s_view;
transport::AdvanceGate s_meterGate;
transport::PadControls s_pad;
bool s_padActive = false, s_deviceKnown = false;
// F5 turns the replay's own meter choice the other way.
bool s_meterFlipped = false;
// The round the transport was last reset for, and whether the fight was
// reached in this battle (the strip then shows itself once).
int s_round = -1;
bool s_reachedFight = false;
// The ways the detour went this battle, each logged the first time (in-game check 7).
unsigned s_loggedRoutes = 0;
// Kept for the game's run: the lanes stay as the player last left them.
bool s_lanesOn = false;
// The lanes: the file asked for, the request's revision, the lanes.
std::string s_laneFile;
std::uint64_t s_laneRevision = 0;
bool s_laneSettled = false;
std::shared_ptr<const sf4e::replaylane::Lanes> s_lanes;

namespace camera = sf4e::training::camera;
using fSystem = sf4e::Game::Battle::System;

// Snapshots of the replay as it plays, one when the controls take it over and then one each s_snapEvery frames, so a
// jump back loads the nearest one before the frame and plays on from it. The memento guard's ownership table holds
// about 2000 payloads and a state takes about 60, so at most kMostSnaps are kept; when they are all taken every other
// one is freed and the spacing doubles, which covers any round more coarsely instead of failing. The recorder's cursor is part
// of a state (SaveState::Save takes the recorder), so loading one puts the playback back with the fighters.
// A state never outlives its battle: CloseBattle frees them all.
struct Snap { int round = 0, frame = 0; fSystem::SaveState state; };
constexpr int kFirstEvery = 120, kMostSnaps = 24;
int s_snapEvery = kFirstEvery;
std::vector<std::unique_ptr<Snap>> s_snaps;
bool s_snapsBroken = false;
// A jump back waiting for the battle update that loads its snapshot.
bool s_loadWaiting = false;
int s_loadRound = 0, s_loadFrame = 0;

void FreeSnaps(bool engineAlive) {
	int n = 0;
	for (auto& snap : s_snaps) {
		if (snap->state.used) { if (engineAlive) fSystem::SaveState::Free(&snap->state); else fSystem::SaveState::Reclaim(&snap->state, "replay_snapshots", n); }
		++n;
	}
	s_snaps.clear(); s_loadWaiting = false; s_snapEvery = kFirstEvery;
}

// Frees every other snapshot (the first of each kept) and doubles the spacing.
void ThinSnaps() {
	std::vector<std::unique_ptr<Snap>> kept;
	for (std::size_t i = 0; i < s_snaps.size(); ++i) {
		if (i % 2 == 0) kept.push_back(std::move(s_snaps[i]));
		else if (s_snaps[i]->state.used) fSystem::SaveState::Free(&s_snaps[i]->state);
	}
	s_snaps = std::move(kept);
	s_snapEvery *= 2;
}
void Leave() {
	s_view.playback = s_view.armed = s_view.exporting = false;
}

// A playback starts or ends: a new session, and nothing the last one left
// behind acts in it. Its commands still waiting go; one posted later under
// its number is refused when it is taken.
void BeginSession() {
	// A "Play again" starts a battle without a close; its old states point into the battle that went.
	FreeSnaps(false); s_snapsBroken = false;
	s_queue.Clear();
	++s_view.session;
	s_transport.Reset();
	s_meterGate.Reset();
	s_pad.Reset(); s_padActive = false; s_deviceKnown = false;
	s_meterFlipped = false; s_round = -1; s_reachedFight = false; s_loggedRoutes = 0;
	Leave();
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

// Game ticks a jump has been playing forward; it gives up after twenty seconds.
int s_seekTicks = 0;
constexpr int kSeekGiveUp = 1200;

const Snap* BestSnap(int round, int frame) {
	const Snap* best = nullptr;
	for (const auto& snap : s_snaps) if (snap->round == round && snap->frame <= frame && (!best || snap->frame > best->frame)) best = snap.get();
	return best;
}

const Snap* EarliestSnap(int round) {
	const Snap* first = nullptr;
	for (const auto& snap : s_snaps) if (snap->round == round && (!first || snap->frame < first->frame)) first = snap.get();
	return first;
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
		camera::Phone().SetTotal(0);
		if (camera::Phone().GetSeek() == camera::Seek::Forward) camera::Phone().ClearGoal();
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

	// Only the commands of this session; one made under another is not its.
	const auto pending = s_queue.Take(s_view.session);
	if (route == transport::Route::Transport) s_transport.FoldNative(*slow != 0);
	for (const auto& queued : pending) {
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
	// The timeline's jump (training/CameraControl.hxx: Seek): a frame ahead is played to at speed; a frame behind
	// is a restart of the replay, then played to. SeekMore (below) does the playing.
	camera::Control& phone = camera::Phone();
	phone.SetTotal(recorder ? static_cast<int>(recorder->streams[recorder->round].frames) : 0);
	// A loop plays the span from the picked key to the next over and over: at its end the playback jumps to its start.
	if (route == transport::Route::Transport && !context.exporting && recorder && phone.GetSeek() == camera::Seek::None && !s_transport.Paused()) {
		const camera::Settings now = phone.Get();
		const int n = static_cast<int>(now.keys.size());
		if (now.loop && now.selected >= 0 && n >= 2) {
			const int a = now.keys[now.selected < n - 1 ? now.selected : now.selected - 1].frame, b = now.keys[now.selected < n - 1 ? now.selected + 1 : now.selected].frame;
			if (b > a && (phone.Frame() >= b || phone.Frame() < a)) phone.AskSeek(a, true);
		}
	}
	{ const Snap* first = recorder ? EarliestSnap(recorder->round) : nullptr; phone.SetFloor(first ? first->frame : 0); }
	int want = 0; bool play = false;
	if (route == transport::Route::Transport && !context.exporting && recorder && phone.TakeSeek(want, play)) {
		want = (std::max)(0, (std::min)(want, phone.Total() - 1));
		// Before the first snapshot there is nowhere to go back to: the nearest place is the first one.
		if (want < phone.Frame() && phone.Floor() > want) want = phone.Floor();
		if (want >= phone.Frame()) { phone.SetGoal(recorder->round, want, play); phone.SetSeek(camera::Seek::Forward); s_seekTicks = 0; }
		else if (const Snap* best = BestSnap(recorder->round, want)) {
			s_loadWaiting = true; s_loadRound = best->round; s_loadFrame = best->frame;
			phone.SetGoal(recorder->round, want, play); phone.SetSeek(camera::Seek::Forward); s_seekTicks = 0;
			spdlog::info("Camera: jumping back to frame {} from the snapshot at {}", want, best->frame);
		}
		else spdlog::info("Camera: no snapshot reaches back to frame {}", want);
	}
	if (phone.GetSeek() == camera::Seek::Forward && ++s_seekTicks > kSeekGiveUp) { spdlog::warn("Camera: the jump did not reach its frame"); phone.ClearGoal(); }
	// Reaching the fight shows the strip once, so the controls are seen.
	if (route == transport::Route::Transport && !s_reachedFight) { s_reachedFight = true; ++s_view.taken; }

	switch (route) {
	case transport::Route::FullSpeed:
		*slow = 0; s_transport.Wrote(false);
		(this->*rSystem::publicMethods.ReplayCadence)();
		break;
	case transport::Route::Original:
		// The pause menu, the knockout, the round's end: the game's own
		// cadence, without the half speed Ember wrote, so it clears the
		// freeze bit rather than toggling it. The transport keeps its speed.
		transport::Relinquish(s_transport, *slow);
		(this->*rSystem::publicMethods.ReplayCadence)();
		break;
	case transport::Route::Transport:
		if (s_transport.Hold() && phone.GetSeek() != camera::Seek::Forward) *flags |= transport::kReplayFreeze;
		else *flags &= ~static_cast<DWORD>(transport::kReplayFreeze);
		*slow = s_transport.SlowFlag() && phone.GetSeek() != camera::Seek::Forward ? 1 : 0;
		break;
	}

	s_view.playback = true;
	s_view.armed = route == transport::Route::Transport;
	s_view.exporting = context.exporting;
	s_view.paused = s_transport.Paused();
	s_view.divisor = s_transport.Divisor();
	s_view.meter = sf4e::replaystore::MeterWanted() != s_meterFlipped;
	s_view.lanes = s_lanesOn;
}

}

void sf4e::replayplayback::Install() {
	void (Cadence::* detour)() = &Cadence::Run;
	DetourAttach(reinterpret_cast<PVOID*>(&rSystem::publicMethods.ReplayCadence), *reinterpret_cast<PVOID*>(&detour));
}

void sf4e::replayplayback::Submit(Command command, Device device, std::uint32_t session) {
	s_queue.Submit(command, device, session);
}

void sf4e::replayplayback::StartBattle(Dimps::Game::Battle::System* system) {
	BeginSession();
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
	FreeSnaps(true);
	if (s_view.playback || s_reachedFight) spdlog::info("Replay controls: the battle closed ({} commands taken, {} refused outside the fight)", s_view.taken, s_view.refused);
	BeginSession();
	// The game zeroes its half speed flag as it closes (0x5DA5F0).
	s_transport.Wrote(false);
}

bool sf4e::replayplayback::AfterUpdate(Dimps::Game::Battle::System* system) {
	if (!s_view.playback) { s_meterGate.Reset(); return false; }
	const sf4e::replay::Recorder* recorder = Game::Battle::System::ggpo ? nullptr : ReplaySystem::staticMethods.GetSingleton();
	if (!recorder) return false;
	std::uint32_t frames[transport::kRounds];
	for (int round = 0; round < transport::kRounds; round++) frames[round] = recorder->streams[round].frames;
	const auto seen = transport::ObserveUpdate(s_meterGate, recorder->round, recorder->cursor, frames,
		static_cast<std::uint16_t>(rSystem::GetNumFramesSimulated_FixedPoint(system)->integral));
	s_view.round = seen.round; s_view.cursor = seen.cursor;
	for (int round = 0; round < transport::kRounds; round++) s_view.roundFrames[round] = seen.roundFrames[round];
	// A meter turned off starts over when it is turned on again.
	if (!s_view.meter) { s_meterGate.Reset(); return false; }
	return seen.advanced;
}

void sf4e::replayplayback::BeforeUpdate(Dimps::Game::Battle::System* system) {
	const sf4e::replay::Recorder* recorder = Game::Battle::System::ggpo ? nullptr : ReplaySystem::staticMethods.GetSingleton();
	if (!recorder || recorder->mode != transport::kRecorderPlaying || !s_view.playback || s_view.exporting) return;
	if (s_loadWaiting) {
		s_loadWaiting = false;
		for (const auto& snap : s_snaps) {
			if (snap->round != s_loadRound || snap->frame != s_loadFrame) continue;
			if (!fSystem::SaveState::Load(&snap->state)) {
				spdlog::error("Camera: the snapshot at frame {} did not load; leaving the battle", snap->frame);
				camera::Phone().ClearGoal(); s_snapsBroken = true;
				*rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
			}
			return;
		}
		camera::Phone().ClearGoal();
		return;
	}
	// Only under Ember's controls, where the player is scrubbing; an export plays untouched.
	if (!s_view.armed || s_snapsBroken || camera::Phone().GetSeek() == camera::Seek::Forward) return;
	const int frame = rSystem::GetNumFramesSimulated_FixedPoint(system)->integral;
	if (frame < 0) return;
	// One when there is none at or before this frame in the round, then one each s_snapEvery frames after the last.
	const Snap* before = BestSnap(recorder->round, frame);
	if (before && frame - before->frame < s_snapEvery) return;
	if (static_cast<int>(s_snaps.size()) >= kMostSnaps) ThinSnaps();
	auto snap = std::make_unique<Snap>();
	snap->round = recorder->round; snap->frame = frame;
	if (!fSystem::SaveState::Save(&snap->state)) {
		spdlog::warn("Camera: the replay could not be snapshotted at frame {}; jumps back restart the replay", frame);
		s_snapsBroken = true; return;
	}
	fSystem::SaveState::ForgetFinishedSounds(&snap->state);
	if (s_snaps.empty()) spdlog::info("Camera: the replay is snapshotted from frame {} of {}", frame, recorder->streams[recorder->round].frames);
	s_snaps.push_back(std::move(snap));
}

bool sf4e::replayplayback::SeekMore(Dimps::Game::Battle::System* system, int done) {
	camera::Control& phone = camera::Phone();
	if (phone.GetSeek() != camera::Seek::Forward) return false;
	int round = 0, frame = 0;
	const sf4e::replay::Recorder* recorder = Game::Battle::System::ggpo ? nullptr : ReplaySystem::staticMethods.GetSingleton();
	if (!phone.Goal(round, frame) || !recorder || recorder->mode != transport::kRecorderPlaying) return false;
	const int at = rSystem::GetNumFramesSimulated_FixedPoint(system)->integral;
	if (recorder->round > round || (recorder->round == round && at >= frame)) {
		// Landed: the replay waits here.
		const bool play = phone.GoalPlay();
		phone.ClearGoal();
		if (!play && !s_transport.Paused()) s_transport.Apply(Command::TogglePause);
		spdlog::info("Camera: the jump reached frame {}", at);
		return false;
	}
	return done < 60;
}

bool sf4e::replayplayback::PlayedInputs(unsigned (&inputs)[2]) {
	const auto lanes = s_lanes;
	std::uint32_t frame = 0;
	if (!lanes || !replaylane::PlayedFrame(s_view.cursor, frame)) return false;
	for (int side = 0; side < 2; side++) {
		replaylane::Row row;
		if (!replaylane::Rows(*lanes, side, s_view.round, frame, &row, 1)) return false;
		inputs[side] = replaylane::PadBits(row.held);
	}
	return true;
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
		// Under the game's pause menu the pad asks for nothing, so no press
		// made there waits to act once the menu closes.
		const int count = s_pad.Sample(physical, commands, battlePause.Paused());
		for (int i = 0; i < count; i++) Submit(commands[i], Device::Pad, s_view.session);
	}

	// The lanes are the replay detail of the file a Watch request plays,
	// prepared with it by the worker that reads replay details, which the
	// playback asks as a requester of its own; the game thread only takes them.
	const std::string& file = replaystore::PlayingFile();
	if (file != s_laneFile) {
		s_laneFile = file; s_lanes.reset(); s_laneSettled = file.empty();
		if (!file.empty()) platform::replays::WantDetail(file, ++s_laneRevision, platform::replays::DetailFor::Playback);
	}
	if (s_laneSettled) return;
	const auto detail = platform::replays::LatestDetail(platform::replays::DetailFor::Playback);
	if (detail.revision != s_laneRevision || detail.state == replayinputs::DetailState::Pending) return;
	s_laneSettled = true;
	if (detail.state == replayinputs::DetailState::Ready && detail.value && detail.value->file == s_laneFile && detail.value->lanes) s_lanes = detail.value->lanes;
	else spdlog::warn("Replay controls: the inputs of {} could not be read; no input lanes for it", s_laneFile);
}

const sf4e::replaytransport::View& sf4e::replayplayback::GetView() { return s_view; }

std::shared_ptr<const sf4e::replaylane::Lanes> sf4e::replayplayback::GetLanes() { return s_lanes; }
