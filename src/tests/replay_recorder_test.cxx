// Capture and Restore of the native replay recorder on memory laid out the way
// the engine lays it out. Append below reproduces the engine's type-4 RLE
// append (0x7831C0), so a rollback that restores a snapshot and appends the
// corrected frames can be compared byte for byte with a recording that never
// mispredicted.
#include <cstdint>
#include <vector>

#include "../common/ReplayRecorder.hxx"
#include "test_support.hxx"

namespace rp = sf4e::replay;

namespace {

constexpr std::uint32_t kRepeatFlag = 0x400000;
constexpr std::uint32_t kCapacity = 3 * 64;

void Put(rp::Codec& c, std::uint32_t value) {
	c.cursor[0] = std::uint8_t(value);
	c.cursor[1] = std::uint8_t(value >> 8);
	c.cursor[2] = std::uint8_t(value >> 16);
	c.cursor += 3;
}

void Append(rp::Codec& c, std::uint32_t value) {
	if (c.cursor >= c.base + c.capacity) return;
	if (c.last == value) {
		c.repeat++;
	} else {
		if (c.repeat) Put(c, kRepeatFlag | c.repeat);
		c.last = value;
		c.repeat = 0;
		Put(c, value & 0x3FFFFF);
	}
	c.frames++;
	c.bytes = std::uint32_t(c.cursor - c.base);
}

struct FakeRecorder {
	rp::Recorder recorder{};
	rp::Codec codecs[rp::kStreams]{};
	std::vector<std::uint8_t> buffers[rp::kStreams];

	FakeRecorder() {
		for (int i = 0; i < rp::kStreams; i++) {
			buffers[i].assign(kCapacity, 0xCD);
			rp::Codec& c = codecs[i];
			c.base = c.cursor = buffers[i].data();
			c.capacity = kCapacity;
			c.last = 0xFFFFFFFF;
			recorder.streams[i].codec = &codecs[i];
			recorder.streams[i].buffer = buffers[i].data();
			recorder.streams[i].capacity = kCapacity;
		}
	}

	std::vector<std::uint8_t> Written(int stream) const {
		const rp::Codec& c = codecs[stream];
		return std::vector<std::uint8_t>(c.base, c.cursor);
	}
};

void TestRollbackRewritesTheTail() {
	const std::uint32_t corrected[] = { 1, 1, 1, 2, 2, 3, 3, 3, 3, 4 };
	const std::uint32_t predicted[] = { 2, 2, 2, 2, 2 };
	constexpr int kSaved = 5;

	FakeRecorder clean;
	for (std::uint32_t v : corrected) Append(clean.codecs[0], v);

	FakeRecorder rolled;
	rp::Snapshot saved{};
	for (int i = 0; i < kSaved; i++) Append(rolled.codecs[0], corrected[i]);
	rp::Capture(rolled.recorder, saved);
	for (std::uint32_t v : predicted) Append(rolled.codecs[0], v);
	CHECK(rp::Restore(saved, rolled.recorder));
	for (int i = kSaved; i < 10; i++) Append(rolled.codecs[0], corrected[i]);

	CHECK(rolled.codecs[0].frames == 10);
	CHECK(rolled.Written(0) == clean.Written(0));
	CHECK(rolled.codecs[0].last == clean.codecs[0].last);
	CHECK(rolled.codecs[0].repeat == clean.codecs[0].repeat);
}

void TestRestoreBringsBackRecorderFields() {
	FakeRecorder live;
	live.recorder.rest[0] = 7;
	live.recorder.streams[2].frames = 30;
	rp::Snapshot saved{};
	rp::Capture(live.recorder, saved);

	live.recorder.rest[0] = 9;
	live.recorder.streams[2].frames = 45;
	Append(live.codecs[2], 5);
	CHECK(rp::Restore(saved, live.recorder));
	CHECK(live.recorder.rest[0] == 7);
	CHECK(live.recorder.streams[2].frames == 30);
	CHECK(live.codecs[2].frames == 0);
	CHECK(live.codecs[2].cursor == live.buffers[2].data());
}

void TestRestoreRefusesReallocatedStreams() {
	FakeRecorder live;
	rp::Snapshot saved{};
	rp::Capture(live.recorder, saved);
	Append(live.codecs[0], 1);
	live.recorder.rest[0] = 3;

	rp::Codec moved = live.codecs[4];
	live.recorder.streams[4].codec = &moved;
	CHECK(!rp::Restore(saved, live.recorder));
	CHECK(live.recorder.rest[0] == 3 && live.codecs[0].frames == 1);

	live.recorder.streams[4].codec = &live.codecs[4];
	std::vector<std::uint8_t> other(kCapacity);
	live.recorder.streams[6].buffer = other.data();
	CHECK(!rp::Restore(saved, live.recorder));
	CHECK(live.recorder.rest[0] == 3 && live.codecs[0].frames == 1);
}

// The engine's append checks only that the cursor is inside the stream, then
// may write a repeat record and a value. Guarded, a stream that fills never
// writes past its end, and a rollback to an earlier cursor records again.
void TestAppendStaysInsideTheStream() {
	std::vector<std::uint8_t> memory(kCapacity + 16, 0xCD);
	rp::Codec c{};
	c.base = c.cursor = memory.data();
	c.capacity = kCapacity;
	c.last = 0xFFFFFFFF;
	auto guarded = [&](std::uint32_t value) { if (rp::CanAppend(c, value)) Append(c, value); };
	// Fill all but the last record, then leave a repeat pending.
	for (std::uint32_t v = 0; c.cursor + 3 < c.base + c.capacity; ++v) guarded(v);
	CHECK(c.cursor == c.base + c.capacity - 3);
	const rp::Codec beforeRepeat = c;
	guarded(c.last); guarded(c.last);
	CHECK(c.repeat == 2 && c.cursor == beforeRepeat.cursor);
	// Unguarded, this is where the engine wrote 3 bytes past the stream.
	CHECK(!rp::CanAppend(c, 7));
	for (std::uint32_t v = 100; v < 200; ++v) guarded(v);
	for (std::size_t i = kCapacity; i < memory.size(); ++i) CHECK(memory[i] == 0xCD);
	// With no repeat pending, the final record still fits.
	c = beforeRepeat;
	CHECK(rp::CanAppend(c, 9));
	guarded(9);
	CHECK(c.cursor == c.base + c.capacity && !rp::CanAppend(c, 10));
	for (std::size_t i = kCapacity; i < memory.size(); ++i) CHECK(memory[i] == 0xCD);
	// A rollback restores the codec, and with it the room to record.
	c = beforeRepeat;
	CHECK(rp::CanAppend(c, 11));
	rp::Codec empty{};
	CHECK(!rp::CanAppend(empty, 1));
}

void TestUnallocatedStreamsRestore() {
	rp::Recorder live{};
	live.rest[10] = 1;
	rp::Snapshot saved{};
	rp::Capture(live, saved);
	live.rest[10] = 2;
	CHECK(rp::Restore(saved, live));
	CHECK(live.rest[10] == 1);
}

}

int main() {
	TestRollbackRewritesTheTail();
	TestRestoreBringsBackRecorderFields();
	TestRestoreRefusesReallocatedStreams();
	TestUnallocatedStreamsRestore();
	TestAppendStaysInsideTheStream();
	return 0;
}
