#pragma once
#include "../training/TrainingSession.hxx"
#include "../common/TrainingPad.hxx"
#include <functional>
#include <string>
#include <vector>
#include "MenuNavigation.hxx"
#include "TrainingInRoom.hxx"

namespace sf4e { namespace ui {
using TrainingSubmit = std::function<bool(training::Command)>;
void DrawTrainingPanel(const training::View& view, const TrainingSubmit& submit);
// Shared by the live overlay and render tests; owns fixed geometry and input focus.
void DrawTrainingFlyout(const training::View& view, const TrainingSubmit& submit);
// F7 on a recorded slot: opens the controls under token on their recordings,
// asking before the slot is overwritten. Refused (the focus period ended),
// it changes nothing and leaves no request for a later opening.
bool OpenTrainingRecordings(input::TrainingControls& controls, const input::TrainingControls::Token& token);
// Where the lab keeps the dummy's reply and the hotkeys (training.json) and
// the saved recordings. Until this is set they live in memory only, which is
// what the render tests want.
void SetTrainingDirectory(std::wstring directory);
// Reset position and save position on keys the player picks (F2 and F11 at
// first), over the fight or the controls. Also sends the dummy's plan each battle.
void TrainingHotkeys(const training::View& view,const TrainingSubmit& submit);
// The pad's Back (TrainingPad.hxx): a tap resets the position, a hold saves
// where the fighters stood as it went down. For the battle it was pressed in only.
void TrainingPadPosition(const training::View& view,const TrainingSubmit& submit,const input::TrainingPadEvent& pad);
// Whether a hotkey sits on the key that many after F1, so the game is not given it.
bool TrainingHotkeyBound(int fromF1);
// The two hotkeys as the HUD's hint names them, those without a key left out.
std::string TrainingKeyHints();
// What the last position command did, while it is news; empty after a few seconds.
std::string TrainingNotice(bool& failed);
MenuNavigation& TrainingNavigation();
std::string TrainingFrameData(const training::MeterView& meter, int side);
// The Frame data page's choices for drawing the meter, as training.json keeps
// them; the training HUD and the match meter read them every frame. Set is for
// tests, as the page sets them.
training::MeterOptions TrainingMeterOptions();
void SetTrainingMeterOptions(const training::MeterOptions& options);
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
// Over the battle while it is about to be taken away, with who called and go
// now under the game's words when the call has them.
void ChallengerBanner(const training::View& view, const ChallengerCall& call = {});
// Top of the room line as a fraction of the viewport height: under the game's
// timer, round markers and name logos, which end about a quarter of the way down.
constexpr float TrainingRoomStatusTop = .26f;
// Where a member waiting in Training stands in their room, on one muted line.
// Passive: takes no input.
void DrawTrainingRoomStatus(const TrainingRoomStatus& status);
// Passive, except for the chip that opens the controls.
// pointer: the pointer is over the chip, so the overlay takes
// the mouse (only) from the game.
struct TrainingHudInput { bool open = false, pointer = false; };
// Bottom edge of the frame meter as a fraction of the viewport height, above
// the game's super meters.
constexpr float TrainingHudBottom = .82f;
// called: a room calls the player back (ChallengerCall::called); the chip is
// not drawn then.
TrainingHudInput DrawTrainingHud(const training::View& view, bool called = false);
// The frame meter's window, alone over a rollback match or a replay the
// runtime is watching, or with the training HUD.
constexpr const char* FrameMeterWindow = "Training frame meter";
// The frame meter alone, over a rollback match the runtime is watching.
void DrawMatchMeter(const training::View& view);
// In a match under a table's Training rule: says that shared save and reset
// are not available there.
void DrawMatchPracticeNotice();
} }
