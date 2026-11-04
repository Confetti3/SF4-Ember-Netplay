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
// This is the game's end; the encoder's is VideoLinkServe.hxx, and what both
// share is VideoLinkProtocol.hxx.
namespace sf4e { namespace platform { namespace videolink {

// Starts the encoder's process: encoder with --encode-video and the link's
// name, by default the Launcher.exe of Ember's install. fast: for a game
// that plays faster than it shows. Every picture is kept and stamped a
// sixtieth of a second after the one before, so the file plays at the game's
// own speed however fast they came; Send waits for a free slot instead of
// dropping one, which holds the game to the encoder's pace; and the sound is
// what SetSound hands over with each picture, not the speakers'.
bool Start(const std::wstring& file, unsigned width, unsigned height, const std::wstring& encoder = std::wstring(), bool fast = false);
// Fast: the sound that goes with the next picture Send takes, 48 kHz stereo
// float interleaved; at most kSoundPerSlot frames (VideoLinkProtocol.hxx).
void SetSound(const float* stereo, unsigned frames);
// One picture, as VideoEncoder's Frame takes it. Dropped while the encoder
// is still opening or when all slots are full, unless fast.
void Send(const void* luma, int lumaPitch, const void* chroma, int chromaPitch);
// Asks the encoder to close the file, without waiting for it. After Fail
// the file is still discarded.
void Stop();
// A picture capture failed: stop and discard the temporary video.
void Fail();
// After Stop or Fail, once a frame or so: Pending while the encoder's process
// closes the file, then Done when the file holds a video under its name, or
// Failed. A process still closing 30 seconds after Stop is ended, and the
// export has failed; Poll does not wait for that and stays Pending until the
// process is gone. Either way the link is gone after Done or Failed, with the
// temporary file (one still held is tried again on the next Start); with no
// link, Failed.
enum class Result { Pending, Done, Failed };
Result Poll();

} } }
