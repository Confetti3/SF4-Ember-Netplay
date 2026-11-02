#include "sf4e__SelfTest.hxx"

#include <string>
#include <vector>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../training/MoveInputs.hxx"
#include "../training/TrainingRuntime.hxx"
#include "sf4e__GameEvents.hxx"

// The Combo check test: Training entered from the main menu as Training
// select does, Ryu against Ryu; Player 1's combo recorded as the lab plays it,
// then checked against a few fighters as the dummy, each loaded in turn
// through Training's own character select. Ryu, the fighter it was recorded
// on, has to take every hit.
namespace sf4e { namespace selftest {
namespace {

using training::Action;
using training::Mode;
namespace cc = training::combo_check;

// Walked up to the dummy, then two jabs into a fireball.
constexpr const char* kCombo = "[6]100f > 2LP > 2LP xx 236LP";
const std::vector<int> kFighters = { 0, 3, 7 };

enum class Stage { ToTraining, Record, Recording, Checking, Done };
struct Lab : Steps<Stage> {
	std::uint64_t comboId = 0;
} s_lab;

bool Send(training::Command command) {
	command.generation = training::ReadPlace().generation;
	return training::Submit(std::move(command));
}
bool Send(Action action) { training::Command command; command.action = action; return Send(std::move(command)); }

void Report(const cc::FightView& check) {
	for (const auto& run : check.runs) {
		const std::string what = "fighter " + std::to_string(run.fighter) + (run.crouching ? " crouching" : " standing");
		// Only Ryu has to take it all; the others are what the check is for.
		Say(run.done && (run.fighter != 0 || cc::Landed(run, check.hits)), what, std::to_string(run.hits) + " of " + std::to_string(check.hits) + " hits" + (run.done ? "" : ", not run"));
	}
	Say(check.runs.size() == kFighters.size() * 2, "runs", std::to_string(check.runs.size()) + " of " + std::to_string(kFighters.size() * 2));
}
}

bool StartComboCheck() {
	GameEvents::TrainingSides sides;
	for (auto& side : sides.sides) { side.charaID = 0; side.unc_edition = Dimps::Game::Battle::ED_USF4; }
	GameEvents::TrainingMode::Choose(sides);
	s_lab.Enter(Stage::ToTraining);
	return true;
}

void TickComboCheck() {
	Lab& lab = s_lab;
	lab.waited++;
	const training::View view = training::ReadView();
	switch (lab.stage) {
	case Stage::ToTraining:
		if (view.available && view.ready && view.check.free) lab.held++; else lab.held = 0;
		if (lab.held > kSecond) { Say(true, "training", "Ryu against Ryu, the lab is in"); lab.Enter(Stage::Record); }
		else if (lab.Late(120)) lab.Fail("training", view.available ? "the round never began" : "Training never came up");
		break;
	case Stage::Record: {
		std::vector<std::string> steps; combo::Compiled compiled; std::string error;
		if (!combo::ParseSteps(kCombo, steps, error) || !combo::Synthesize(steps, true, compiled, error)) { lab.Fail("record", error); break; }
		training::Command loop; loop.action = Action::Loop; loop.loop = false;
		training::Command load; load.action = Action::Load; load.side = 0; load.frames = std::move(compiled.frames);
		lab.comboId = view.check.comboId;
		if (!Send(loop) || !Send(Action::ComboRecord) || !Send(std::move(load)) || !Send(Action::Play)) { lab.Fail("record", "the lab did not take the commands"); break; }
		lab.Enter(Stage::Recording);
		break;
	}
	case Stage::Recording:
		if (view.check.comboId != lab.comboId && view.mode == Mode::Idle) {
			const auto combo = training::LastCombo();
			Say(combo.hits >= 3, "record", std::to_string(combo.frames.size()) + " frames, " + std::to_string(combo.hits) + " hits");
			training::Command start; start.action = Action::ComboStart;
			start.check.fighters = kFighters; start.check.fast = true;
			if (!Send(std::move(start))) { lab.Fail("check", "Combo check did not start"); break; }
			lab.Enter(Stage::Checking);
		}
		else if (lab.Late(20)) lab.Fail("record", "the recording never ended");
		break;
	case Stage::Checking:
		// Running from the frame after it was sent; a load between runs shows no Training.
		if (view.check.running) lab.held = 1;
		else if (lab.held && view.available) { Report(view.check); lab.Enter(Stage::Done); Finish(); break; }
		if (lab.Late(300)) { Send(Action::ComboStop); Report(view.check); lab.Fail("check", "the check did not finish in 5 minutes"); }
		break;
	case Stage::Done: break;
	}
}

} }
