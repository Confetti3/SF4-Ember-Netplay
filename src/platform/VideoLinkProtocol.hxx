#pragma once

#include <windows.h>
#include <cstddef>
#include <string>

// What the game's end of the video link (VideoLink.cxx) and the encoder's
// (VideoLinkServe.cxx) share: the shared memory's layout and the clock both
// stamp pictures with. VideoLink.hxx says how the link works.
namespace sf4e { namespace platform {

namespace video {
// The performance counter in 100 ns units: the clock of the encoder's Frame
// time, read by the game when it sends a picture and by the encoder for its sound.
inline long long Clock() {
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
	return counter.QuadPart / frequency.QuadPart * 10000000 + counter.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart;
}
}

namespace videolink {

constexpr unsigned kSlots = 4;

// The start of the shared memory; the slots follow. One side writes each
// counter: the game sent and stop, the encoder taken, opened and the notes.
struct Shared {
	// stop: 0 while running, 1 to finish, -1 to discard a failed capture.
	volatile LONG sent, taken, stop;
	// 0 while the encoder opens, then 1, or -1 when it could not.
	volatile LONG opened;
	UINT32 width, height;
	DWORD game;
	LONGLONG times[kSlots];
	// The encoder writes `file` and, once it holds a video, renames it to `final`.
	wchar_t file[1024], final[1024];
	char openedAs[256], closedAs[256];
};

inline size_t FrameBytes(const Shared* shared) { return static_cast<size_t>(shared->width) * shared->height * 3 / 2; }
inline BYTE* Slot(Shared* shared, LONG index) { return reinterpret_cast<BYTE*>(shared + 1) + (index % kSlots) * FrameBytes(shared); }
// The event the game sets after each picture and on stop, named after the shared memory.
inline std::wstring WakeName(const std::wstring& link) { return link + L"-wake"; }

}

} }
