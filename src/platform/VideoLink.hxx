#pragma once

#include <string>

#include "VideoShared.hxx"

// The game's end of the video export. The encoder runs in a process of its
// own (VideoEncoder.hxx says why). The game keeps kSlots NV12 pictures of
// shared memory, 1.5 bytes a pixel each: 50 MB at 4K. It copies a picture
// into a free slot and goes on; the encoder's process takes them out in
// order, captures the game's sound itself, and closes the file when told to
// or when the game goes away, so a game closed during an export still leaves
// a file that plays. The file is written under a name no other export or
// video has ("<file>.<process>-<time>.tmp") and takes its own only once it
// holds a video, so an export that fails leaves an earlier one alone.
// Not thread safe: the caller keeps every call under one lock.
namespace sf4e { namespace platform { namespace videolink {

// Starts the encoder's process: encoder with --encode-video and the link's
// name, by default the Launcher.exe beside this module.
bool Start(const std::wstring& file, unsigned width, unsigned height, const std::wstring& encoder = std::wstring());
// One picture, as VideoEncoder's Frame takes it. Dropped while the encoder
// is still opening or when all slots are full.
void Send(const void* luma, int lumaPitch, const void* chroma, int chromaPitch);
// The encoder's process ended without being asked to: it did not open, or it died.
bool Lost();
// Asks the encoder to close the file, or with keep false to leave none,
// without waiting for it.
void Stop(bool keep);
// After Stop or Lost: true once the encoder's process has ended, with how;
// false while it is still closing the file. What a process that was ended
// left of the file is removed.
bool Closed(Result& result);
// For a process that does not end. Abandon lets go of the link at once and
// hands over what is left; End, which may take a second and so belongs
// outside the caller's lock, ends the process and removes its file.
struct Abandoned { HANDLE process = nullptr; std::wstring file; };
Abandoned Abandon();
void End(Abandoned& left);

} } }
