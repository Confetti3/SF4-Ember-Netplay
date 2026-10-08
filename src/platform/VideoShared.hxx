#pragma once

#include <windows.h>
#include <cstddef>

// What the game and the video export's encoder process agree on: the shared
// memory between them (VideoLink.hxx fills it, VideoServe.hxx empties it), the
// clock a picture is stamped with, and how the encoder's process ends.
namespace sf4e { namespace platform { namespace videolink {

constexpr unsigned kSlots = 4;

// The start of the shared memory; the slots follow. One side writes each
// counter: the game sent and stop, the encoder taken, opened and the notes.
struct Shared {
	volatile LONG sent, taken;
	// 0 while the export runs; then 1 to close the file, 2 to leave none.
	volatile LONG stop;
	// 0 while the encoder opens, then 1, or -1 when it could not.
	volatile LONG opened;
	UINT32 width, height;
	DWORD game;
	LONGLONG times[kSlots];
	// The encoder writes `file` and, once it holds a video, renames it to `final`.
	wchar_t file[1024], final[1024];
	char openedAs[256], closedAs[256];
};

inline std::size_t FrameBytes(const Shared* shared) { return static_cast<std::size_t>(shared->width) * shared->height * 3 / 2; }
inline BYTE* Slot(Shared* shared, LONG index) { return reinterpret_cast<BYTE*>(shared + 1) + (index % kSlots) * FrameBytes(shared); }

// The performance counter in 100 ns units: the time a picture was drawn at.
inline long long Clock() {
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
	return counter.QuadPart / frequency.QuadPart * 10000000 + counter.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart;
}

// The encoder process's exit code. Written: the file holds the video under
// its own name. NoVideo: it was asked to leave none, or closed without a
// picture. NoLink: the shared memory was not there. NotOpened: Windows gave
// no encoder for the picture. NotWritable: the file could not be made in its
// folder. NotNamed: the finished file could not take its name, as when an
// earlier one is open in a player. Anything else is a process that was ended.
enum Result : unsigned long { Written = 0, NoVideo = 1, NoLink = 2, NotOpened = 3, NotWritable = 4, NotNamed = 5 };

} } }
