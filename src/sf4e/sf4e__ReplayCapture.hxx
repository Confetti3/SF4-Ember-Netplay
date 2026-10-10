#pragma once

#include <string>

struct IDirect3DDevice9;

// A replay's playback written to a video file by Ember's own encoder
// (platform/VideoLink.hxx). The thread that renders owns the frame grab: it
// opens it and the link to the encoder on the first frame after Begin, sends
// each frame, and lets go of them after End. The game thread only asks
// (Begin, End, Cancel) and reads the state. The link is used under one lock
// by both, so a cancel reaches the encoder before it is acknowledged, and a
// device reset waits for a frame that is being grabbed.
namespace sf4e { namespace replaycapture {

// Idle: nothing asked. Recording: frames go to the encoder, or will from the
// next one. Closing: the encoder is closing the file. Done: the file holds
// the video. Failed: there is no file (no encoder, no grab, or it stopped).
enum class State { Idle, Recording, Closing, Done, Failed };

// Game thread. Begin: record from the next rendered frame into file, an
// .mp4, with the encoder's process started from encoder (by default the
// install's Launcher.exe; VideoLink.hxx: Start). End: stop and close the
// file; Done or Failed follows without the game thread waiting. Cancel: stop
// and keep no file, as a failed capture does. The encoder has been told to
// discard its file before Cancel returns true, so no later frame, and not
// even the game's end, can save the unfinished video; before the first frame
// no encoder is started at all. Failed follows. False, doing nothing, once
// the file is already closing. Clear: back to Idle after either.
void Begin(const std::wstring& file, const std::wstring& encoder = std::wstring());
void End();
bool Cancel();
void Clear();
State GetState();

// Render thread. Frame: once per rendered frame, once the layers an export
// takes from Ember's overlay are drawn and before the rest of it
// (sf4e__Overlay.hxx: DrawOverlay). Release: before the device resets or
// goes, lets go of the grab's surfaces and disables grabs; it waits for a
// Frame in progress. Resume is called after the native reset returns, and
// enables only a usable device.
void Frame(IDirect3DDevice9* device);
void Release();
void Resume(IDirect3DDevice9* device);

} }
