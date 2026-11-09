#pragma once
#include "../training/TrainingSession.hxx"
#include "../common/TrainingPad.hxx"
#include "../common/ButtonPrompts.hxx"
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>
#include "MenuNavigation.hxx"
#include "TrainingInRoom.hxx"
#include "TrainingHudLayout.hxx"
#include "FrameMeterWindow.hxx"

namespace sf4e { namespace ui {
using TrainingSubmit = std::function<bool(training::Command)>;
void DrawTrainingPanel(const training::View& view, const TrainingSubmit& submit);
// Shared by the live overlay and render tests; owns fixed geometry and input focus.
void DrawTrainingFlyout(const training::View& view, const TrainingSubmit& submit);
// F7 on a recorded slot: opens the controls under token on their recordings,
// asking before the slot is overwritten. Refused (the focus period ended),
// it changes nothing and leaves no request for a later opening.
bool OpenTrainingRecordings(input::TrainingControls& controls, const input::TrainingControls::Token& token);
// The press of Record (F7, or the pad button the player bound) and of Play
// (F8, or its button): the keys and the pad send the same command, which the
// session resolves as it stands when it runs (Action::Record, AdvancePlayback).
// token: the owner the Record press went down under (TakeOverwriteAsk).
void TrainingRecordPress(const training::View& view,const TrainingSubmit& submit,const input::TrainingControls::Token& token);
// Whether the session asked about overwriting a recorded slot for the last
// Record sent (View::overwriteAsk), once; token is then that press's owner,
// under which the overlay opens the recordings, or nothing once it has gone.
bool TakeOverwriteAsk(const training::View& view,input::TrainingControls::Token& token);
void TrainingPlayPress(const training::View& view,const TrainingSubmit& submit);
// Where the lab keeps the dummy's reply and the hotkeys (training.json) and
// the saved recordings. Until this is set they live in memory only, which is
// what the render tests want.
void SetTrainingDirectory(std::wstring directory);
// Reset position and save position on keys the player picks (F2 and F11 at
// first), over the fight or the controls. Also sends the dummy's plan each battle.
void TrainingHotkeys(const training::View& view,const TrainingSubmit& submit);
// Whether Record and Play, by key or pad, may act now: not while the controls
// are open, nor while a room calls the player back (TRAINING_IN_ROOMS.md),
// where the controls stay shut.
inline bool TrainingPressesAct(const input::TrainingControls& controls,bool called) { return !controls.Open()&&!called; }
// What the pad asked for (TrainingPad.hxx), for the battle it was pressed in
// only: Back's tap resets the position and its hold saves where the fighters
// stood as it went down; Record (the chord on standby, or the player's own
// button) and Play do what F7 and F8 do, under the focus period they went down
// in, when TrainingPressesAct as each is applied.
void ApplyTrainingPadEvent(const training::View& view,const TrainingSubmit& submit,input::TrainingControls& controls,const input::TrainingPadEvent& pad,bool called);
// Whether a hotkey sits on the key that many after F1, so the game is not given it.
bool TrainingHotkeyBound(int fromF1);
// The keys that record and play a slot, and stop either again, counted from
// F1 as the hotkeys are. Fixed: no hotkey can take them (FreePositionKey).
constexpr int TrainingRecordKey = 6, TrainingPlayKey = 7;
// A key counted from F1 as the controls and the HUD name it.
std::string TrainingKeyName(int fromF1);
// The pad's Back and Start as the prompts name them (an Xbox pad's unless a
// DirectInput pad is in use), and the two as the chord that, on standby,
// starts a recording (TrainingPadInput).
gamepads::PositionNames TrainingPadNames();
// The gameplay pad's own Record and Play buttons as the runtime resolves them
// (masks and names; none for one that is off or does not work). The overlay
// sets them each frame for the strip of hints.
void SetTrainingPadButtons(const input::TrainingButtons& buttons);
const input::TrainingButtons& TrainingPadButtons();
std::string TrainingPadChord();
// The position hotkeys as the HUD's strip names them: reset then save, each
// with its key and a short locale key for what it does; a hotkey set to Off
// is left out.
struct TrainingKeyHint { std::string key; const char* label; };
std::vector<TrainingKeyHint> TrainingPositionKeys();
// What the last position command did, while it is news; empty after a few seconds.
std::string TrainingNotice(bool& failed);
MenuNavigation& TrainingNavigation();
// The Frame data page's choices for drawing the meter, as training.json keeps
// them; the training HUD and the match meter read them every frame. Set is for
// tests, as the page sets them.
training::MeterOptions TrainingMeterOptions();
void SetTrainingMeterOptions(const training::MeterOptions& options);
// The reset and save hotkeys counted from F1, -1 for Off, as the Position
// page sets them; for tests.
void SetTrainingPositionKeys(int reset, int save);
// Over the battle while it is about to be taken away, with who called and go
// now under the game's words when the call has them.
void ChallengerBanner(const training::View& view, const ChallengerCall& call = {});
// Top of the room line as a fraction of the viewport height: under the game's
// timer, round markers and name logos, which end about a quarter of the way down.
constexpr float TrainingRoomStatusTop = .26f;
// Where a member waiting in Training stands in their room, on one muted line.
// Passive: takes no input.
void DrawTrainingRoomStatus(const TrainingRoomStatus& status);
// The passive HUD's size (FrameMeterHudScale) at the HUD size picked on the HUD page.
float TrainingHudScale();
// called: a room calls the player back (ChallengerCall::called); the chip is
// not drawn then.
TrainingHudInput DrawTrainingHud(const training::View& view, bool called = false);
// The strip of keys over the meter (TrainingShortcuts.cxx): its left edge at
// left, its bottom just above meterTop, and no wider than width. The result's
// area is the strip's.
TrainingHudInput DrawTrainingShortcuts(const training::View& view, bool called, float left, float meterTop, float width, float hudScale);
// What the HUD's shortcut strip drew in its last call, for the render tests:
// its text height in pixels, the width its line took and the width it had,
// and each piece's text in order (key, button, chip, label or notice).
// Empty when the strip was not drawn.
struct TrainingStripShown { float textPixels = 0, width = 0, room = 0; std::vector<std::string> pieces; };
const TrainingStripShown& LastTrainingStrip();
// The fighters' boxes over the fight, when Hitboxes on the lab's HUD page is on.
void DrawTrainingBoxes(const training::View& view);
// The Data panel (TrainingData.cxx), while it is on and the game has read it,
// clear of the HUD's area (DrawTrainingHud).
void DrawTrainingData(const training::View& view, const TrainingHudArea& hud);
// The frame meter alone, over a rollback match the runtime is watching.
void DrawMatchMeter(const training::View& view);
// Over a fight Ember's menu opens over (input::MenuOverFight): the lines that
// say what Start or F10 does there, one locale key each, at the bottom of the
// screen. A Practice match says that shared save and reset are not available
// and that End practice ends it; a watched game, that Stop watching leaves it.
constexpr const char* MatchKeysWindow = "Match keys";
// Each fighter's input display beside the training HUD's meter, P1 and P2.
constexpr const char* InputDisplayWindows[2] = {"Input display P1", "Input display P2"};
void DrawMatchKeysNotice(std::initializer_list<const char*> keys);
} }
