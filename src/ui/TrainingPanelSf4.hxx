#pragma once

// SF4's own pages of the Training lab's controls (TrainingPanel.hxx:
// TrainingExtraPages): the dummy's settings as SF4's Training menu numbers
// them, the typed replies' moves list from USF4's command tables, Combo check
// over SF4's roster and the phone remote. SF4's overlay installs them before
// it first draws the controls, and so does anything that draws SF4's
// controls without it (the render tests, the UI lab).
namespace sf4e { namespace ui {
void InstallSf4TrainingPages();
} }
