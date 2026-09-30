#pragma once

// The native replay recorder, Dimps::Game::Battle::ReplaySystem (singleton at
// 0xAA7228). The Command unit's "CMD POST" task (0x59AA20) appends one input
// record per battle update, so every frame a rollback re-simulates is
// appended again. The recorder lives outside Battle::System and no memento
// covers it, so a save state keeps a copy of it by value.
//
// The object is plain data apart from its seven round streams. Each stream
// owns a buffer and an RLE codec (append 0x7831C0) that never rewrites bytes
// behind its write cursor, so putting the object and codecs back truncates
// every stream to its saved length without copying any buffer.
// ReplayRecorderTest covers capture and restore on buffers at these offsets.

#include <cstddef>
#include <cstdint>

namespace sf4e { namespace replay {

static_assert(sizeof(void*) == 4, "the recorder layout is the 32-bit engine's");

constexpr int kStreams = 7;
constexpr std::size_t kRecorderBytes = 0xAE0;

// The type-4 RLE codec (0x783330). Append (0x7831C0) writes 3-byte records
// at the cursor: a 22-bit value, or a repeat count flagged 0x400000.
struct Codec {
	const void* vtable;
	std::uint32_t frames;
	std::uint32_t bytes;
	std::uint8_t* base;
	std::uint8_t* cursor;
	std::uint32_t capacity;
	std::uint32_t last;
	std::uint32_t repeat;
};
static_assert(sizeof(Codec) == 0x20, "codec size");
static_assert(offsetof(Codec, cursor) == 0x10 && offsetof(Codec, repeat) == 0x1C, "codec fields");

// A round stream (0x782860), 0x1C bytes from recorder +4.
struct Stream {
	const void* vtable;
	Codec* codec;
	std::uint8_t* buffer;
	std::uint32_t capacity;
	std::uint32_t bytes;
	std::uint32_t frames;
	std::uint32_t codecType;
};
static_assert(sizeof(Stream) == 0x1C && offsetof(Stream, frames) == 0x14, "stream wrapper layout");

struct Recorder {
	const void* vtable;
	Stream streams[kStreams];
	std::uint8_t rest[kRecorderBytes - sizeof(void*) - kStreams * sizeof(Stream)];
};
static_assert(sizeof(Recorder) == kRecorderBytes && offsetof(Recorder, streams) == 4, "recorder layout");

// Append (0x7831C0) checks only cursor < base + capacity, then writes a
// pending repeat record and the new value: up to 6 bytes. With the cursor 3
// bytes short of the end and a repeat pending, it wrote 3 bytes past the
// 61,440-byte stream, which a round of about 20,480 records reaches (long
// round times). A stream takes a frame only while its largest append still
// fits, repeats included, so it stops for good at the first frame it cannot
// take and never resumes after a gap: the recording stays one contiguous
// prefix. Derived from the codec alone, which save states keep, so a
// rollback to an earlier cursor records again.
constexpr std::size_t kLargestAppend = 6;

inline bool CanAppend(const Codec& codec) {
	if (!codec.base || codec.cursor < codec.base) return false;
	const std::size_t used = static_cast<std::size_t>(codec.cursor - codec.base);
	return used <= codec.capacity && codec.capacity - used >= kLargestAppend;
}

struct Snapshot {
	Recorder recorder;
	Codec codecs[kStreams];
};

inline void Capture(const Recorder& live, Snapshot& out) {
	out.recorder = live;
	for (int i = 0; i < kStreams; i++) {
		if (live.streams[i].codec) out.codecs[i] = *live.streams[i].codec;
	}
}

// False, changing nothing, when a live stream's codec or buffer is not at the
// address saved. The engine frees and reallocates the streams between
// sessions, and a save state never outlives its battle, so a mismatch means
// the state is from another session and nothing in it can be put back.
inline bool Restore(const Snapshot& saved, Recorder& live) {
	for (int i = 0; i < kStreams; i++) {
		if (live.streams[i].codec != saved.recorder.streams[i].codec ||
			live.streams[i].buffer != saved.recorder.streams[i].buffer) return false;
	}
	live = saved.recorder;
	for (int i = 0; i < kStreams; i++) {
		if (live.streams[i].codec) *live.streams[i].codec = saved.codecs[i];
	}
	return true;
}

} }
