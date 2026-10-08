#pragma once

#include <string>

struct IDirect3DDevice9;

// A replay's playback written to a video file by Ember's own encoder
// (platform/VideoLink.hxx). The thread that renders owns the frame grab: it
// opens it and the link on the first frame after Begin and sends each frame.
// The game thread asks (Begin, End) and moves the export on once a tick
// (Poll), so an export whose frames stop coming, as behind an exclusive
// fullscreen game left with Alt+Tab, still ends. One lock keeps the two
// apart, and a device reset waits for a frame that is being grabbed.
namespace sf4e { namespace replaycapture {

// Idle: nothing asked. Recording: frames go to the encoder, or will from the
// next one. Closing: the encoder is closing the file. Done: the file holds
// the video. Failed: there is no new file, and an earlier one is as it was.
enum class State { Idle, Recording, Closing, Done, Failed };
// Why it failed. Encoder: Windows gave none for the picture, or it stopped.
// Picture: the game's picture could not be read, or changed size on the way.
// File: the file could not be written or take its name.
enum class Failure { None, Encoder, Picture, File };

// Game thread. Begin: record from the next rendered frame into file, an
// .mp4. End: stop and close the file. Poll, once a tick: notices an encoder
// that did not open or went away, asks it to close, and waits for the file
// without holding the game; the state after it. Why: what a Failed is owed
// to. Clear: back to Idle after Done or Failed.
void Begin(const std::wstring& file);
void End();
State Poll();
State GetState();
Failure Why();
void Clear();

// Render thread. Frame: once per rendered frame, before the overlay is drawn.
void Frame(IDirect3DDevice9* device);
// The thread that resets the device. Release: before the device resets or
// goes, lets go of the grab's surfaces, waiting for a Frame in progress, and
// keeps Frame from making them again. Restored: after the reset, Frame may.
void Release();
void Restored();

} }
