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

// Sets when a replay's header says it was played, in seconds since 1970.
static void SetPlayed(Bytes& replay, std::uint32_t played) {
	const std::uint64_t filetime = played * 10000000ull + 116444736000000000ull;
	WriteU32(replay.data() + 0x10, static_cast<std::uint32_t>(filetime));
	WriteU32(replay.data() + 0x14, static_cast<std::uint32_t>(filetime >> 32));
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

// Rewrites a fixture replay's last four bytes so it has the CRC `crc`: a
// real collision, which only the contents can tell apart.
static void ForgeCrc(Bytes& body, std::uint32_t crc) {
 WriteU32(body.data()+body.size()-4,0);
 const auto base=Crc32(body.data(),body.size());
 std::uint32_t basis[32]={},masks[32]={};
 for(int bit=0;bit<32;++bit) {
  WriteU32(body.data()+body.size()-4,std::uint32_t{1}<<bit);
  auto column=Crc32(body.data(),body.size())^base;
  auto mask=std::uint32_t{1}<<bit;
  for(int row=31;row>=0;--row)if(column&(std::uint32_t{1}<<row)) {
   if(basis[row]) {column^=basis[row];mask^=masks[row];}
   else {basis[row]=column;masks[row]=mask;break;}
  }
 }
 auto target=crc^base;
 std::uint32_t patch=0;
 for(int row=31;row>=0;--row)if(target&(std::uint32_t{1}<<row)) {
  CHECK(basis[row]);target^=basis[row];patch^=masks[row];
 }
 CHECK(target==0);
 WriteU32(body.data()+body.size()-4,patch);
 CHECK(Crc32(body.data(),body.size())==crc);
}

// Forge only fixture payload bytes to exercise a real CRC collision. The
// archive index sees the same CRC, so only exact body comparison can reject it.
static Bytes SameCrcDifferentBody(const Bytes& original) {
 Bytes body = original;
 body[0x200] ^= 1;
 ForgeCrc(body, Crc32(original.data(), original.size()));
 CHECK(body != original);
 return body;
}
