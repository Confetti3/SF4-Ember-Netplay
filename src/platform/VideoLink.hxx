#pragma once

#include <string>

// The video export's encoder in a process of its own (VideoEncoder.hxx says
// why). The game keeps kSlots NV12 pictures of shared memory, 1.5 bytes a
// pixel each: 50 MB at 4K. It copies a picture into a free slot and goes on;
// the encoder's process takes them out in order, captures the game's sound
// itself, and closes the file when told to or when the game goes away, so a
// game closed during an export still leaves a file that plays. The file is
// written to a uniquely reserved temporary file and takes its own name only once it holds a
// video, so an export that fails leaves an earlier one of that replay alone.
namespace sf4e { namespace platform { namespace videolink {

constexpr unsigned kSlots = 4;

// In the game. Starts the encoder's process: encoder with --encode-video and
// the link's name, by default the Launcher.exe beside this module.
bool Start(const std::wstring& file, unsigned width, unsigned height, const std::wstring& encoder = std::wstring());
// One picture, as VideoEncoder's Frame takes it. Dropped while the encoder
// is still opening or when all slots are full.
void Send(const void* luma, int lumaPitch, const void* chroma, int chromaPitch);
// Asks the encoder to close the file, without waiting for it.
void Stop();
// A picture capture failed: stop and discard the temporary video.
void Fail();
// After Stop: true once the encoder's process has ended, with ok saying the
// file holds a video; false while it is still closing it. Abort ends a
// process that does not.
bool Closed(bool& ok);
void Abort();
// Stop, then up to 30 seconds for Closed. True when the file holds a video.
bool Finish();

// In the encoder's process: serves the link of that name until Finish or
// the game's end. The process's exit code, 0 for a file that holds a video.
int Serve(const std::wstring& link);

} } }
