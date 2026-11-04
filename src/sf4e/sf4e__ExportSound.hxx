#pragma once

#include <cstdint>
#include <vector>

// The game's sound made by the picture for a fast video export.
//
// USF4's sound leaves through one CRI output (CriSmpSoundOutputXAudio2, CRI
// Audio/PCx86 3.43.05): a thread of CRI's own wakes every 10 ms and keeps
// four blocks of 512 sample frames queued on an XAudio2 voice, each made by
// one call to CRI's mixer. That thread keeps to the clock, which a replay
// played faster than it is shown does not. While held, the thread makes
// nothing (the speakers fall silent) and Render calls the same mixer for as
// much sound as the pictures so far are long, which goes into the file.
namespace sf4e { namespace exportsound {

// At start, with the other hooks: attaches to the output's block maker
// (SSFIV.exe 0x70A9A0) once its bytes are this game build's.
void Install();
// The thread that renders, on the first picture of a fast export: true when
// the output has been seen and CRI's thread now makes nothing; the sound
// starts from nothing.
bool Hold();
// As many sample frames, 48 kHz stereo float interleaved, as the file needs
// once it holds `pictures` pictures, appended to stereo. False when not held.
bool Render(std::uint64_t pictures, std::vector<float>& stereo);
// CRI's thread makes the sound again, for the speakers.
void Release();

} }
