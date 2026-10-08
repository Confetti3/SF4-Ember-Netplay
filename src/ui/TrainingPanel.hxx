#pragma once
#include "../training/TrainingSession.hxx"
#include "../training/MatchPractice.hxx"
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
// Puts moves on the combo creator's Moves line, as typing them there does.
void SetComboMoves(const std::string& line);
// Replay or stop the combo, reset position, start or stop its trial, record
// a combo: the combo creator's rows on keys the player picks (F1 to F4 at
// first), over the fight or the controls. Also sends the dummy's plan each battle.
// padSelect: the pad's Select button is down; a tap resets the position and a hold saves it.
void TrainingHotkeys(const training::View& view,const TrainingSubmit& submit,bool padSelect=false);
// Whether a hotkey sits on the key that many after F1, so the game is not given it.
bool TrainingHotkeyBound(int fromF1);
MenuNavigation& TrainingNavigation();
// Passive, except for its chips: open the controls, replay the selected slot,
// stop playback. pointer: the pointer is over the chips, so the overlay takes
// the mouse (only) from the game.
struct TrainingHudInput { bool open = false, replay = false, stop = false, pointer = false; };
// Bottom edge of the frame meter as a fraction of the viewport height, above
// the game's super meters.
constexpr float TrainingHudBottom = .82f;
TrainingHudInput DrawTrainingHud(const training::View& view);
// The frame meter alone, over a rollback match the runtime is watching.
void DrawMatchMeter(const training::View& view);
// In a match under a table's Training rule: draws the shared position controls'
// unavailable message. Returns zero; padSelect does not request a command.
unsigned MatchPracticeKeys(bool padSelect);
} }
