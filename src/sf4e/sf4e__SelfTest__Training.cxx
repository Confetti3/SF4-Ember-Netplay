#include "sf4e__SelfTest.hxx"

#include <cmath>
#include <string>
#include <vector>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../training/MoveInputs.hxx"
#include "../training/TrainingRuntime.hxx"
#include "sf4e__GameEvents.hxx"

// The training test: the frame meter and the hitboxes against moves whose
// frame data is known, typed in numpad notation and played by the lab, a
// recording and its playback, and the lab's saved position. An offline Versus battle of Ryu
// against Ryu is entered the way a room's match is, with no menu, and the lab
// is let into that battle. It uses only what the lab's own controls send
// (training::Submit) and reads.
namespace sf4e { namespace selftest {
namespace {

using training::Action;
using training::Mode;

// Frames the test records before it stops the recording.
constexpr int kRecorded = 8 * kSecond;
// Done: the outcome is written (Finish).
enum class Stage { ToBattle, Move, Starting, Settling, Measure, Record, Standby, Recording, Replaying, SavePosition, Dash, Dashed, Reset, Done };

// Ryu's crouching LP: startup 3, active 2, recovery 7. The numbers on the
// meter's line have to be these. Its cells follow the startup number
// (FrameMeter.hxx: ClassifyMeter), so the recovery has one cell less.
constexpr int kStartup = 3, kActive = 2, kRecovery = 7;

struct Lab : Steps<Stage> {
	// Where a played move goes once it has ended, and where it goes if it does not.
	Stage then = Stage::Done, otherwise = Stage::Done;
	std::string playing;
	// VS mode was not in front when the fighters were to be set.
	bool sidesMissed = false;
	// Frames recorded, and the furthest frame the playback was seen at.
	int recorded = 0, reached = 0;
	float saved = 0, dashed = 0;
	// While the crouching LP plays: the boxes were read, each fighter showed
	// where it can be hit, and on how many frames Player 1's attack box was live.
	bool boxesRead = false, hurt[2] = {};
	int hitFrames = 0;
} s_lab;

void WatchBoxes(const training::BoxView& boxes) {
	if (!boxes.valid) return;
	s_lab.boxesRead = true;
	bool hit = false;
	for (const auto& box : boxes.boxes) {
		if (box.kind == training::BoxKind::Hurt) s_lab.hurt[box.side] = true;
		if (box.kind == training::BoxKind::Hit && box.side == 0) hit = true;
	}
	if (hit) s_lab.hitFrames++;
}

// Ryu against Ryu on the first stage.
void SetFighters() {
	Dimps::GameEvents::VsMode::ConfirmedCharaConditions ryu{};
	ryu.charaID = 0; ryu.unc_edition = Dimps::Game::Battle::ED_USF4;
	const Dimps::GameEvents::VsMode::ConfirmedCharaConditions sides[2] = { ryu, ryu };
	s_lab.sidesMissed = !GameEvents::VsPreBattle::SetSides(sides, 0);
}

// loop: for Action::Loop, whether playback repeats.
bool Submit(Action action, std::vector<training::Input> frames = {}, bool loop = false) {
	training::Command command;
	command.action = action; command.generation = training::ReadView().generation;
	command.frames = std::move(frames); command.side = 0; command.loop = loop;
	return training::Submit(std::move(command));
}

// Player 1 plays the typed moves through the selected slot, facing right.
// A test run is a game of its own, so that slot is the only one with
// anything in it and Play plays it. Once the playback has begun, ended, and
// the fighter has stood for half a second, the test goes on at `then`.
void Play(const char* moves, const std::string& what, Stage then, Stage otherwise) {
	s_lab.playing = what; s_lab.then = then; s_lab.otherwise = otherwise;
	std::vector<std::string> steps;
	combo::Compiled compiled;
	std::string error;
	if (!combo::ParseSteps(moves, steps, error) || !combo::Synthesize(steps, true, compiled, error)) { s_lab.Fail(what, std::string("'") + moves + "' is not moves: " + error, otherwise); return; }
	if (!Submit(Action::ClearHistory) || !Submit(Action::Load, std::move(compiled.frames)) || !Submit(Action::Play)) { s_lab.Fail(what, "the lab did not take the moves", otherwise); return; }
	s_lab.Enter(Stage::Starting);
}

void Measure(const training::View& view) {
	const Cells cells = CountCells(view.meter, 0);
	const auto& move = view.meter.moves[0];
	const bool numbersRight = view.meter.startupFrames[0] == kStartup && move.active == kActive && move.recovery == kRecovery;
	const bool cellsRight = cells.startup == kStartup && cells.active == kActive && cells.recovery == kRecovery - 1;
	Say(numbersRight, "Ryu crouching LP numbers", "startup " + std::to_string(view.meter.startupFrames[0]) + ", active " +
		std::to_string(move.active) + ", recovery " + std::to_string(move.recovery) + (numbersRight ? "" : ", expected 3/2/7"));
	Say(cellsRight, "Ryu crouching LP cells", std::to_string(cells.startup) + ", " + std::to_string(cells.active) + " and " +
		std::to_string(cells.recovery) + " cells" + (cellsRight ? "" : ", expected 3, 2 and 6"));
	const bool boxesRight = s_lab.boxesRead && s_lab.hurt[0] && s_lab.hurt[1] && s_lab.hitFrames == kActive;
	Say(boxesRight, "Ryu crouching LP hitboxes", !s_lab.boxesRead ? "no boxes were read" :
		std::string("hurt boxes ") + (s_lab.hurt[0] && s_lab.hurt[1] ? "on both" : "missing") + ", attack box live on " +
		std::to_string(s_lab.hitFrames) + " frames" + (boxesRight ? "" : ", expected hurt boxes on both and 2 frames"));
}

}

bool StartTraining() {
	GameEvents::VsPreBattle::VersusSettings settings;
	settings.editionSelect = TRUE;
	// The longest round the game offers: the test is done long before it.
	settings.timeLimit = { 0, 9999 };
	// No replay is saved, however the battle ends: the test writes nothing to the saves.
	settings.recordReplay = FALSE;
	// The lab runs in a Training battle only; a test run lets it into this one.
	training::AllowOfflineVersusForTest();
	// The crouching LP's boxes are checked whether or not Hitboxes is on.
	training::ReadHitBoxesForTest();
	if (!GameEvents::VsPreBattle::GoToVersus(settings, SetFighters)) { Say(false, "battle", "the main menu was not in front"); return false; }
	s_lab.Enter(Stage::ToBattle);
	return true;
}

void TickTraining() {
	Lab& lab = s_lab;
	lab.waited++;
	const training::View view = training::ReadView();
	const bool neutral = view.meter.current[0].valid && training::ClassifyStatus(view.meter.current[0].status) == training::Phase::Neutral;
	// The crouching LP's boxes, while it plays.
	if ((lab.stage == Stage::Starting || lab.stage == Stage::Settling) && lab.then == Stage::Measure) WatchBoxes(view.boxes);
	switch (lab.stage) {
	case Stage::ToBattle:
		// Fought with whatever fighters the save had, every number below would point the wrong way.
		if (lab.sidesMissed) { lab.Fail("battle", "VS mode was not in front, so Ryu against Ryu was not set"); break; }
		// The lab is up and the fighter has stood for a second, so the round is being fought.
		if (view.available && view.ready && neutral) lab.held++; else lab.held = 0;
		if (lab.held > kSecond) {
			Say(true, "battle", "Ryu against Ryu, the lab is in");
			// Playback repeats by default; every step here plays once.
			if (Submit(Action::Loop)) lab.Enter(Stage::Move); else lab.Fail("battle", "the lab did not take Loop off");
		}
		else if (lab.Late(120)) lab.Fail("battle", view.available ? "the round never began" : "the lab never came up in the battle");
		break;
	case Stage::Move:
		Play("2LP", "Ryu crouching LP", Stage::Measure, Stage::Record);
		break;
	case Stage::Starting:
		// The playback has to begin before its end can be waited for.
		if (view.mode == Mode::Playback) lab.Enter(Stage::Settling);
		else if (lab.Late(5)) lab.Fail(lab.playing, "the playback never started", lab.otherwise);
		break;
	case Stage::Settling:
		if (view.mode == Mode::Idle && neutral) lab.held++; else lab.held = 0;
		if (lab.held >= kSecond / 2) lab.Enter(lab.then);
		else if (lab.Late(20)) lab.Fail(lab.playing, "the move never ended", lab.otherwise);
		break;
	case Stage::Measure:
		Measure(view);
		lab.Enter(Stage::Record);
		break;
	case Stage::Record:
		// The lab's record control: standby first, then recording, then stop.
		// Eight seconds are recorded and played back for five, long enough for
		// the HUD's strip to be seen.
		if (Submit(Action::AdvanceRecording)) lab.Enter(Stage::Standby); else lab.Fail("record", "the lab did not take the command", Stage::SavePosition);
		break;
	case Stage::Standby:
		if (view.mode != Mode::Standby) { if (lab.Late(5)) lab.Fail("record", "standby never began", Stage::SavePosition); break; }
		if (Submit(Action::AdvanceRecording)) { lab.recorded = 0; lab.Enter(Stage::Recording); } else lab.Fail("record", "the lab did not take the command", Stage::SavePosition);
		break;
	case Stage::Recording:
		if (view.mode == Mode::Recording && lab.recorded < kRecorded && ++lab.recorded == kRecorded) Submit(Action::AdvanceRecording);
		else if (lab.recorded == kRecorded && view.mode == Mode::Idle) {
			const int length = view.lengths[view.selected];
			Say(length >= kRecorded - kSecond && length <= kRecorded + kSecond, "record", std::to_string(length) + " frames in slot " + std::to_string(view.selected + 1));
			if (Submit(Action::Play)) { lab.reached = 0; lab.Enter(Stage::Replaying); } else lab.Fail("play recording", "the lab did not take the command", Stage::SavePosition);
		}
		else if (lab.Late(20)) { Submit(Action::Stop); lab.Fail("record", "the recording never started or never stopped", Stage::SavePosition); }
		break;
	case Stage::Replaying:
		if (view.mode == Mode::Playback && view.cursor > lab.reached) lab.reached = view.cursor;
		if (!lab.Late(5)) break;
		Say(lab.reached > 2 * kSecond, "play recording", "reached frame " + std::to_string(lab.reached) + " of " + std::to_string(view.lengths[view.selected]));
		Submit(Action::Stop);
		lab.Enter(Stage::SavePosition);
		break;
	case Stage::SavePosition:
		// The playback just stopped has to be over before the position is saved.
		if (view.mode != Mode::Idle) { if (lab.Late(5)) lab.Fail("save position", "the playback never stopped"); break; }
		lab.saved = view.x[0];
		if (Submit(Action::Save)) lab.Enter(Stage::Dash); else lab.Fail("save position", "the lab did not take the command");
		break;
	case Stage::Dash:
		// A forward dash moves Player 1 away from where the position was saved.
		if (view.checkpoint) Play("66", "reset position", Stage::Dashed, Stage::Done);
		else if (lab.Late(5)) lab.Fail("save position", "nothing was saved");
		break;
	case Stage::Dashed:
		lab.dashed = view.x[0];
		if (lab.dashed - lab.saved < 0.1f) lab.Fail("reset position", "the dash did not move the fighter, so the reset proves nothing");
		else if (Submit(Action::Restore)) lab.Enter(Stage::Reset);
		else lab.Fail("reset position", "the lab did not take the command");
		break;
	case Stage::Reset:
		if (!lab.Late(1)) break;
		Say(std::fabs(view.x[0] - lab.saved) < 0.01f, "reset position", "Player 1 at " + std::to_string(view.x[0]) +
			", saved at " + std::to_string(lab.saved) + ", dashed to " + std::to_string(lab.dashed));
		lab.Enter(Stage::Done);
		Finish();
		break;
	case Stage::Done:
		break;
	}
}

} }
