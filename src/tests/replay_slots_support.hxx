#pragma once
#include "../common/ReplaySlots.hxx"
#include "test_support.hxx"
using namespace sf4e::replayslots;

// A replay with a whole header: the magic and two fighters of the roster,
// which a record has to name too (Describes).
static Bytes Replay(std::uint8_t fill, std::size_t size) {
	Bytes replay(size, fill);
	std::memcpy(replay.data(), "#BRP", 4);
	if (size >= kReplayHeaderBytes) { WriteU32(replay.data() + 0x20, fill % 40); WriteU32(replay.data() + 0x170, (fill + 1) % 40); }
	return replay;
}

static void Seal(Bytes& swan) { WriteU32(swan.data(), Crc32(swan.data() + 4, swan.size() - 4)); }

// Both indexes with every slot empty, the sizes the game writes.
static Bytes EmptyList() {
	Bytes list(75958);
	WriteU32(list.data(), 1);
	WriteU32(list.data() + 4, kListSlots);
	for (int slot = 0; slot < kListSlots; slot++) WriteU32(list.data() + kListRecordsOffset + slot * kRecordBytes, slot);
	return list;
}

static Bytes EmptySwan() {
	Bytes swan(44070);
	std::memcpy(swan.data() + 8, "SRL", 4);
	std::memcpy(swan.data() + 28, "SRI", 4);
	for (int slot = kListSlots; slot < kSlots; slot++) WriteU32(swan.data() + kSwanRecordsOffset + (slot - kListSlots) * kRecordBytes, slot);
	Seal(swan);
	return swan;
}

static void Fill(Bytes& list, Bytes& swan, int slot, const Bytes& replay, std::uint8_t meta) {
	std::uint8_t* record = const_cast<std::uint8_t*>(Record(list, swan, slot));
	CHECK(record);
	record[4] = 1;
	WriteU32(record + 5, Crc32(replay.data(), replay.size()));
	WriteU32(record + 9, static_cast<std::uint32_t>(replay.size()));
	WriteU32(record + 13, 1549657440);
	std::memset(record + 17, meta, kRecordBytes - 17);
	// What the game shows has to be showable (PlausibleRecord): no title, two
	// fighters of the roster, 8 February 2019, 20:24.
	WriteU32(record + 22, 0);
	record[51] = 0xE3; record[52] = 0x07; record[53] = 2; record[54] = 8;
	// The fighters are the replay's own, as the game writes them.
	ReplayHeaderInfo header = {{0, 0}, 0};
	ReadReplayHeader(replay, header);
	record[71] = static_cast<std::uint8_t>(header.fighters[0]); record[105] = static_cast<std::uint8_t>(header.fighters[1]);
	record[123] = 20; record[124] = 24;
	swan[kSwanSlotBytesOffset + slot * 2] = swan[kSwanSlotBytesOffset + slot * 2 + 1] = 0x0E;
	Seal(swan);
}

