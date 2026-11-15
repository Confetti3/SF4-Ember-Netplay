#pragma once
#include <filesystem>
#include <functional>

#include "../common/ReplayTransport.hxx"
#include "ReplaysView.hxx"

// The camera panel: a small floating window over Training and over a replay,
// on F12, with the camera's hotkeys while it is open. It drives the same
// camera::Phone() the phone's Camera tab does (training/CameraControl.hxx).
namespace sf4e { namespace ui {
// Once per frame. `available`: Training or a replay is on; `focused`: the game window has the keyboard.
// `views` is where the saved views are kept (read the first time).
// During a replay the timeline is drawn under the picture: `replay` is the playback's controls (on: the replay is
// playing under Ember's controls, not exporting), `send` carries a pause or step to them.
struct ReplayBar { bool on = false; bool paused = false; int divisor = 1; bool lanes = false, lanesKnown = false, meter = false; std::function<void(replaytransport::Command)> send; };
void DrawCameraPanel(bool available, bool focused, const std::filesystem::path& views, const ReplayBar& replay);
// The same from what the overlay holds: `replayOn` is a replay playing under Ember's controls, `send` carries
// the timeline's pause or step to them; the saved views are kept in `directory`.
void DrawCameraPanel(bool available, bool focused, const std::filesystem::path& directory, bool replayOn, const ReplaysView& replays,
                     std::function<void(replaytransport::Command)> send);
bool CameraPanelOpen();
// The video editor is showing over a replay (the panel is open on its Editor view): it carries the replay controls
// itself, so the replay controls' own strip stays hidden.
bool CameraEditorShown();
} }
