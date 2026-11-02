#pragma once
#include "../training/TrainingSession.hxx"

// Combo check in the game: the runner (training/ComboCheck.hxx) driven from
// the runtime's tick through Training's own commands. The runtime's thread only.
namespace sf4e { namespace combo_check {

// A Training command for the battle now shown.
bool Send(training::Command command);
bool Send(training::Action action);
// Arms a recording of Player 1's next combo.
bool Record();
// The check on the last recorded combo; false when there is none or Training is not open.
bool Start(const training::combo_check::Settings& settings);
void Stop();
// Once a runtime tick: the runner's next step.
void Tick();
const training::combo_check::Runner& State();

} }
