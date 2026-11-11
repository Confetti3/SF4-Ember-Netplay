#include "sf4e__ExportSound.hxx"

#include <windows.h>
#include <detours/detours.h>
#include <atomic>
#include <cstring>

#include "spdlog/spdlog.h"

#include "../common/ExportSoundClock.hxx"

namespace {
using namespace sf4e::exportsound;

// CriSmpSoundOutputXAudio2 as SSFIV.exe builds it (0x70A530) and makes a
// block with it (0x70A9A0): 1 while the output runs, the mixer CRI set and
// what it is called with, and the lock the block maker holds around it.
constexpr std::size_t kRunning = 4, kMixer = 8, kMixerContext = 12, kLock = 0x33018;
typedef unsigned (__cdecl* Mixer)(void* context, int channels, float** planes, unsigned frames);
typedef void (__thiscall* MakeBlock)(void* output, void* submit);
// The block maker's first bytes in this game build, the last four being the lock's offset.
constexpr unsigned char kNative[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x40, 0x53, 0x56, 0x8B, 0xF1, 0x8D, 0x86, 0x18, 0x30, 0x03, 0x00};

MakeBlock s_makeBlock = nullptr;
bool s_attached = false;
std::atomic<void*> s_output{nullptr};
std::atomic<bool> s_held{false};
// When a picture last asked for sound (GetTickCount64). Past kStarved
// without one, CRI's thread makes the sound again: the game may be waiting
// on CRI (a battle closing stops its sounds) while no picture is drawn, and
// would wait forever. Pictures come every few milliseconds while one plays.
constexpr ULONGLONG kStarved = 200;
std::atomic<ULONGLONG> s_asked{0};
std::atomic<bool> s_starvedSaid{false};
// The thread that renders only, between Hold and Release.
std::uint64_t s_made = 0;
float s_planes[6][kBlock];
float s_stereo[kBlock * 2];

// CRI's thread, and once the output starting up: while held, no block is made.
void __fastcall HeldMakeBlock(void* output, void*, void* submit) {
	s_output.store(output);
	if (s_held.load()) {
		if (GetTickCount64() - s_asked.load() < kStarved) return;
		if (!s_starvedSaid.exchange(true)) spdlog::info("Export sound: no picture for {} ms; the game's own thread makes its sound meanwhile", kStarved);
	}
	s_makeBlock(output, submit);
}
}

namespace sf4e { namespace exportsound {

void Install() {
	BYTE* const at = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr)) + 0x30A9A0;
	if (std::memcmp(at, kNative, sizeof kNative) != 0) {
		spdlog::warn("Export sound: the game's sound output holds other bytes than this game build's; fast exports are silent");
		return;
	}
	s_makeBlock = reinterpret_cast<MakeBlock>(at);
	s_attached = DetourAttach(reinterpret_cast<PVOID*>(&s_makeBlock), reinterpret_cast<PVOID>(&HeldMakeBlock)) == NO_ERROR;
	if (!s_attached) spdlog::warn("Export sound: the hook on the game's sound output could not be attached");
}

bool Hold() {
	if (!s_attached || !s_output.load()) return false;
	s_made = 0;
	s_asked.store(GetTickCount64());
	s_starvedSaid.store(false);
	s_held.store(true);
	return true;
}

bool Render(std::uint64_t pictures, std::vector<float>& stereo) {
	BYTE* const output = static_cast<BYTE*>(s_output.load());
	if (!s_held.load() || !output) return false;
	float* planes[6];
	for (int plane = 0; plane < 6; plane++) planes[plane] = s_planes[plane];
	s_asked.store(GetTickCount64());
	const std::uint64_t due = SamplesDue(pictures);
	while (s_made < due) {
		unsigned frames = 0;
		// What the block maker does before it hands the block to XAudio2,
		// under its lock: the mixer is called only while the output runs.
		CRITICAL_SECTION* const lock = reinterpret_cast<CRITICAL_SECTION*>(output + kLock);
		EnterCriticalSection(lock);
		const Mixer mixer = *reinterpret_cast<Mixer*>(output + kMixer);
		if (mixer && *reinterpret_cast<int*>(output + kRunning) == 1) frames = mixer(*reinterpret_cast<void**>(output + kMixerContext), 6, planes, kBlock);
		LeaveCriticalSection(lock);
		if (frames == 0 || frames > kBlock) {
			for (auto& plane : s_planes) std::memset(plane, 0, sizeof plane);
			frames = kBlock;
		}
		Downmix(planes, frames, s_stereo);
		stereo.insert(stereo.end(), s_stereo, s_stereo + frames * 2);
		s_made += frames;
	}
	return true;
}

void Release() { s_held.store(false); }

} }
