#pragma once

#include <cstdint>

// The arithmetic of a fast video export's sound (sf4e__ExportSound.hxx): the
// game's sound is made by the picture, not by the clock, so the file's sound
// and picture match however fast the replay plays.
namespace sf4e { namespace exportsound {

constexpr unsigned kRate = 48000, kPicturesPerSecond = 60;
// What CRI's mixer makes in one call, as the game's own output asks it.
constexpr unsigned kBlock = 512;
// The most sample frames one picture can carry: its 800 and a block left
// over from the one before.
constexpr unsigned kMostPerPicture = kRate / kPicturesPerSecond + kBlock;

// The sample frames the file holds once it holds this many pictures.
inline std::uint64_t SamplesDue(std::uint64_t pictures) { return pictures * kRate / kPicturesPerSecond; }

// CRI's six planes (front left, front right, surround left, surround right,
// centre, LFE: the order its XAudio2 output reads them in, SSFIV.exe
// 0xA77C28) to stereo, interleaved, with the matrix the game gives XAudio2
// on a stereo device (0x9CBA08): a side and the centre at -3 dB each, no LFE.
inline void Downmix(const float* const planes[6], unsigned frames, float* stereo) {
	constexpr float k = 0.70710677f;
	for (unsigned i = 0; i < frames; i++) {
		const float centre = planes[4][i] * k;
		stereo[2 * i] = planes[0][i] + centre + planes[2][i] * k;
		stereo[2 * i + 1] = planes[1][i] + centre + planes[3][i] * k;
	}
}

} }
