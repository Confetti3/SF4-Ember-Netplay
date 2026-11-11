#include "../common/ExportSoundClock.hxx"
#include <cmath>

#include "test_support.hxx"
using namespace sf4e::exportsound;

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// 800 sample frames a picture, exactly, however many pictures.
static void EachPictureIsEightHundredFrames() {
	CHECK(SamplesDue(0) == 0);
	CHECK(SamplesDue(1) == 800);
	CHECK(SamplesDue(60) == kRate);
	CHECK(SamplesDue(60ull * 60 * 60 * 10) == 10ull * 60 * 60 * kRate);
}

// Made in blocks of 512 to keep up with the pictures, the sound is never
// short of them and never more than a block ahead; no picture carries more
// than kMostPerPicture frames.
static void BlocksKeepUpWithThePictures() {
	unsigned long long made = 0;
	for (unsigned long long pictures = 1; pictures <= 100000; pictures++) {
		unsigned carried = 0;
		while (made < SamplesDue(pictures)) { made += kBlock; carried += kBlock; }
		CHECK(made >= SamplesDue(pictures) && made - SamplesDue(pictures) < kBlock);
		CHECK(carried <= kMostPerPicture);
	}
}

// The game's 6 to 2 matrix: each side has its front at full, its surround and
// the centre at -3 dB; the other side and the LFE do not reach it.
static void DownmixIsTheGamesMatrix() {
	const float k = 0.70710677f;
	float fl[2] = {1, 0}, fr[2] = {0, 0}, sl[2] = {0, 1}, sr[2] = {0, 0}, c[2] = {0, 0}, lfe[2] = {1, 1};
	const float* planes[6] = {fl, fr, sl, sr, c, lfe};
	float stereo[4] = {};
	Downmix(planes, 2, stereo);
	CHECK(Near(stereo[0], 1) && Near(stereo[1], 0));
	CHECK(Near(stereo[2], k) && Near(stereo[3], 0));
	float none[1] = {0}, centre[1] = {1}, right[1] = {0.5f}, rightSurround[1] = {1};
	const float* more[6] = {none, right, none, rightSurround, centre, none};
	Downmix(more, 1, stereo);
	CHECK(Near(stereo[0], k) && Near(stereo[1], 0.5f + k + k));
}

int main() {
	EachPictureIsEightHundredFrames();
	BlocksKeepUpWithThePictures();
	DownmixIsTheGamesMatrix();
	return 0;
}
