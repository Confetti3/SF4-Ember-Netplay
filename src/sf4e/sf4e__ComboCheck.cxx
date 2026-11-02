#include "sf4e__ComboCheck.hxx"

#include <spdlog/spdlog.h>

#include "../training/TrainingRuntime.hxx"

namespace sf4e { namespace combo_check {
namespace {
using namespace sf4e::training;
using namespace sf4e::training::combo_check;

Runner runner;
// The dummy's settings before the check, put back after it.
DummyState kept;
bool restore = false;

bool Send(Command command) {
	command.generation = ReadView().generation;
	return training::Submit(std::move(command));
}
bool Send(Action action) { Command command; command.action = action; return Send(std::move(command)); }

// The dummy stands or crouches, never blocks, is never dizzied and takes no counter hits.
DummyState RunDummy(bool crouching) {
	DummyState dummy; dummy.action = crouching ? 1 : 0; dummy.guard = 0; dummy.counterHit = 0; dummy.stun = 2;
	return dummy;
}

void StartRun(const Step& step) {
	Command dummy; dummy.action = Action::DummyState; dummy.dummy = RunDummy(step.crouching);
	Command place; place.action = Action::Place; StartAt(runner.GetCombo(), runner.GetStart(), place.place);
	Command play; play.action = Action::ComboPlay; play.frames = runner.GetCombo().frames;
	// One batch: the dummy set and both placed before the frame the combo starts on.
	if (!Send(dummy) || !Send(place) || !Send(Action::ComboTally) || !Send(std::move(play)))
		spdlog::warn("Combo check: fighter {} {} not started, Training did not take the commands", step.fighter, step.crouching ? "crouching" : "standing");
}

void Finish() {
	if (restore) { Command dummy; dummy.action = Action::DummyState; dummy.dummy = kept; Send(dummy); restore = false; }
	int landed = 0;
	for (const Run& run : runner.Runs()) landed += Landed(run, runner.GetCombo());
	spdlog::info("Combo check: {} of {} runs landed every hit", landed, runner.Runs().size());
	for (const Run& run : runner.Runs())
		spdlog::info("Combo check: fighter {} {}: {} of {} hits", run.fighter, run.crouching ? "crouching" : "standing", run.hits, runner.GetCombo().hits);
}
}

bool Record() { return Send(Action::ComboRecord); }

bool Start(const Settings& settings) {
	const View view = ReadView();
	if (!view.available || view.check.combo.frames.empty() || settings.fighters.empty()) return false;
	runner.Begin(settings, view.check.combo);
	kept = view.dummy; restore = true;
	spdlog::info("Combo check: {} runs, a combo of {} frames and {} hits", runner.Runs().size(), view.check.combo.frames.size(), view.check.combo.hits);
	return true;
}

void Stop() {
	if (!runner.Running()) return;
	runner.Stop(); Send(Action::Stop); Finish();
}

void Tick() {
	if (!runner.Running()) return;
	const View view = ReadView();
	Seen seen;
	seen.generation = view.generation; seen.ready = view.available && view.ready; seen.dummy = view.check.dummy;
	seen.playing = view.mode == Mode::Playback; seen.free = view.check.free; seen.best = view.check.best;
	const Step step = runner.Next(seen);
	switch (step.what) {
	case Do::Load: { Command load; load.action = Action::ComboLoad; load.slot = step.fighter; if (!Send(std::move(load))) spdlog::warn("Combo check: fighter {} not loaded", step.fighter); break; }
	case Do::Play: StartRun(step); break;
	case Do::Finished: Finish(); break;
	case Do::Wait: break;
	}
}

const Runner& State() { return runner; }
} }
