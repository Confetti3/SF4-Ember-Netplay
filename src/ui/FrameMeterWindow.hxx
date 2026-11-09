#pragma once
#include "../training/FrameMeter.hxx"
#include <imgui.h>
#include <functional>
#include <string>
#include <vector>

// The frame meter as the player sees it: its bars in each style, the numbers
// beside them, the state line under them, and the colour key. It draws a
// training::MeterView and nothing else, so any game that feeds the model
// (training/FrameMeter.hxx) draws its meter with these. SF4's Training HUD
// (TrainingMeter.cxx) puts it under its recording strip and beside its
// input displays; a game without that lab draws the window alone.
namespace sf4e { namespace ui {
// The frame meter's window, alone over a rollback match or a replay the
// runtime is watching, or with the training HUD.
constexpr const char* FrameMeterWindow = "Training frame meter";
// The meter's own window, centred, its bottom edge at hudBottom, width wide.
// Takes no input. above: drawn first inside it (SF4's Training lab puts its
// recording strip there), or nothing. Returns its top left corner.
ImVec2 DrawFrameMeterWindow(const training::MeterView& meter, float hudScale, float width, float hudBottom,
    const training::MeterOptions& options, const std::function<void()>& above = {});
// The passive HUD's size for a HUD size (MeterOptions::size): it follows the
// game's window, smaller with a smaller or narrower one down to the smallest
// text that still reads, times that size and the player's Interface size.
// Windows' DPI is left out: the HUD is laid out in the game's pixels, which
// the window's size already measures.
float FrameMeterHudScale(int size);
// The meter's width at this viewport and HUD scale, centred on the screen.
float TrainingMeterWidth(float hudScale);
// The HUD's windows: dark enough to read over a bright stage, which still shows through.
constexpr float TrainingHudBackground = .62f;
// A fighter's readings as text, one per line: startup, active, recovery, the
// result and the last advantage, and the meaty timing.
std::string TrainingFrameData(const training::MeterView& meter, int side);
void DrawTrainingColorKey(bool angled);
// One row of the F6 colour key: a locale key, what it names and the exact
// colour (ImU32) the frame meter bars draw for it. Flat bars colour a cell by
// its phase; angled bars by its resolved kind (training::MeterCell; an entry
// names one kind; Knockdown covers Down and Rise).
struct TrainingKeyEntry { const char* label; training::Phase phase; training::MeterKind kind; unsigned color; };
std::vector<TrainingKeyEntry> TrainingColorKeyEntries(bool angled);
// The colour a bar cell draws, angled or flat: the one function the bars and
// the key both take their colours from.
unsigned TrainingCellColor(const training::MeterCell& cell, bool angled);
} }
