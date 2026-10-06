#pragma once
#include "../training/TrainingSession.hxx"
#include <functional>
#include <string>
#include "MenuNavigation.hxx"

namespace sf4e { namespace ui {
using TrainingSubmit = std::function<bool(training::Command)>;
void DrawTrainingPanel(const training::View& view, const TrainingSubmit& submit);
// Shared by the live overlay and render tests; owns fixed geometry and input focus.
void DrawTrainingFlyout(const training::View& view, const TrainingSubmit& submit);
void ShowTrainingRecordings();
// Where the combo creator keeps its book (combos.json). Until this is set the
// book lives in memory only, which is what the render tests want.
void SetComboBookDirectory(std::wstring directory);
MenuNavigation& TrainingNavigation();
// Passive, except for its chips: open the controls, replay the selected slot,
// stop playback. pointer: the pointer is over the chips, so the overlay takes
// the mouse (only) from the game.
struct TrainingHudInput { bool open = false, replay = false, stop = false, pointer = false; };
// Bottom edge of the frame meter as a fraction of the viewport height, above
// the game's super meters.
constexpr float TrainingHudBottom = .82f;
TrainingHudInput DrawTrainingHud(const training::View& view);
} }
