#pragma once

#include <string>

struct IDirect3DDevice9;

// A replay's playback written to a video file by Ember's own encoder
// (platform/VideoLink.hxx). The thread that renders owns the frame grab and
// the link to the encoder: it opens them on the first frame after Begin,
// sends each frame, and closes them after End. The game thread only asks
// (Begin, End) and reads the state, so neither thread touches the other's
// work, and a device reset waits for a frame that is being grabbed.
namespace sf4e { namespace replaycapture {

// Idle: nothing asked. Recording: frames go to the encoder, or will from the
// next one. Closing: the encoder is closing the file. Done: the file holds
// the video. Failed: there is no file (no encoder, no grab, or it stopped).
enum class State { Idle, Recording, Closing, Done, Failed };

// Game thread. Begin: record from the next rendered frame into file, an
// .mp4; withOverlay when the frame is to be taken after Ember's overlay is
// drawn (an export's caption) instead of before it. End: stop and close the file; Done or Failed follows
// without the game thread waiting. Clear: back to Idle after either.
void Begin(const std::wstring& file, bool withOverlay);
void End();
void Clear();
State GetState();

// Render thread. Frame: once per rendered frame, before the overlay is
// drawn or, when AfterOverlay says so, after it. Release: before the device resets or goes, lets go of the grab's
// surfaces and disables grabs; it waits for a Frame in progress. Resume is
// called after the native reset returns, and enables only a usable device.
bool AfterOverlay();
void Frame(IDirect3DDevice9* device);
void Release();
void Resume(IDirect3DDevice9* device);

} }
