// Export of the game's replay slots, on indexes built at the offsets of LIST
// and replays-swan.dat.

#include "../common/ReplaySlots.hxx"

#include "test_support.hxx"

using namespace sf4e::replayslots;

static Bytes Replay(std::uint8_t fill, std::size_t size) {
	Bytes replay(size, fill);
	std::memcpy(replay.data(), "#BRP", 4);
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
	swan[kSwanSlotBytesOffset + slot * 2] = swan[kSwanSlotBytesOffset + slot * 2 + 1] = 0x0E;
	Seal(swan);
}

static void TestCrcIsTheOneTheGameWrites() {
	const char* text = "123456789";
	CHECK(Crc32(reinterpret_cast<const std::uint8_t*>(text), 9) == 0xCBF43926u);
	CHECK(Sidecar(Bytes(text, text + 9)) == (Bytes{0x26, 0x39, 0xF4, 0xCB}));
}

static void TestIndexValidity() {
	Bytes list = EmptyList(), swan = EmptySwan();
	CHECK(ValidList(list) && ValidSwan(swan));
	CHECK(Record(list, swan, 0) == list.data() + kListRecordsOffset);
	CHECK(Record(list, swan, 299) == list.data() + kListRecordsOffset + 299 * kRecordBytes);
	CHECK(Record(list, swan, 300) == swan.data() + kSwanRecordsOffset);
	CHECK(Record(list, swan, 309) == swan.data() + kSwanRecordsOffset + 9 * kRecordBytes);
	CHECK(!Record(list, swan, -1) && !Record(list, swan, 310));
	CHECK(!ReadSlot(list, swan, 200).used && !ReadSlot(list, swan, 305).used && !ReadSlot(list, swan, 310).used);

	Bytes damaged = swan;
	damaged[2000] ^= 1;
	CHECK(!ValidSwan(damaged) && !Record(list, damaged, 300) && Record(list, damaged, 299));
	CHECK(!ValidSwan(Bytes(swan.begin(), swan.begin() + kSwanMinBytes - 1)));
	Bytes shortList(list.begin(), list.begin() + kListMinBytes - 1);
	CHECK(!ValidList(shortList) && !Record(shortList, swan, 0) && Record(shortList, swan, 300));
	Bytes wrongCount = list;
	WriteU32(wrongCount.data() + 4, 299);
	CHECK(!ValidList(wrongCount));
	CHECK(!ValidList(Bytes()) && !ValidSwan(Bytes()));
}

static void TestExportFromBothIndexes() {
	Bytes list = EmptyList(), swan = EmptySwan();
	const Bytes inList = Replay(0x11, 9562), inSwan = Replay(0x22, 4000);
	Fill(list, swan, 267, inList, 0xA1);
	Fill(list, swan, 304, inSwan, 0xB2);

	struct Case { int slot; const Bytes& replay; std::uint8_t meta; };
	for (const Case& c : {Case{267, inList, 0xA1}, Case{304, inSwan, 0xB2}}) {
		const int slot = c.slot; const Bytes& replay = c.replay; const std::uint8_t meta = c.meta;
		Bytes exported;
		CHECK(Export(list, swan, slot, replay, exported));
		CHECK(exported.size() == kExportHeaderBytes + replay.size());
		CHECK(!std::memcmp(exported.data(), kExportMagic, 8));
		const std::uint8_t* record = exported.data() + 8;
		CHECK(ReadU32(record) == static_cast<std::uint32_t>(slot) && record[4] == 1);
		CHECK(ReadU32(record + 9) == replay.size() && ReadU32(record + 13) == 1549657440);
		CHECK(record[17] == meta && record[kRecordBytes - 1] == meta);
		CHECK(record[kRecordBytes] == 0x0E && record[kRecordBytes + 1] == 0x0E);
		CHECK(Bytes(exported.begin() + kExportHeaderBytes, exported.end()) == replay);
		const SlotInfo info = ReadSlot(list, swan, slot);
		CHECK(info.used && info.size == replay.size() && info.crc == Crc32(replay.data(), replay.size()));
	}
}

static void TestExportRefusesWhatDoesNotBelong() {
	Bytes list = EmptyList(), swan = EmptySwan(), out;
	const Bytes replay = Replay(0x11, 5000);
	Fill(list, swan, 301, replay, 0);
	Fill(list, swan, 210, replay, 0);
	CHECK(!Export(list, swan, 302, replay, out));          // empty slot
	CHECK(!Export(list, swan, 310, replay, out));          // no such slot
	CHECK(!Export(list, swan, 301, Replay(0x12, 5000), out)); // another file
	CHECK(!Export(list, swan, 301, Bytes(replay.begin(), replay.end() - 1), out));
	Bytes noMagic = replay;
	noMagic[0] = 'X';
	Bytes list2 = list, swan2 = swan;
	Fill(list2, swan2, 210, noMagic, 0);
	CHECK(!Export(list2, swan2, 210, noMagic, out));
	Bytes damagedSwan = swan;
	damagedSwan[0] ^= 1;
	CHECK(!Export(list, damagedSwan, 301, replay, out));
	CHECK(!Export(list, damagedSwan, 210, replay, out)); // the slot bytes live in swan
	Bytes damagedList = list;
	WriteU32(damagedList.data() + 4, 0);
	CHECK(!Export(damagedList, swan, 210, replay, out));
	CHECK(Export(damagedList, swan, 301, replay, out)); // swan slots need no LIST
	out.clear();
	// A record that names a replay larger than the game ever saves.
	const Bytes big = Replay(0x44, kLargestReplay + 1);
	Fill(list, swan, 305, big, 0);
	CHECK(!Export(list, swan, 305, big, out));
	CHECK(out.empty());
}

static void TestSlotToReplaceFollowsTheGame() {
	Bytes list = EmptyList(), swan = EmptySwan();
	CHECK(SlotToReplace(list, swan, 200, 309) == 200); // all empty: the first
	for (int slot = 200; slot <= 309; slot++) Fill(list, swan, slot, Replay(0x11, 100), 0);
	// Record() refuses an unsealed swan, so each write there is sealed at once.
	for (int slot = 200; slot <= 309; slot++) { WriteU32(const_cast<std::uint8_t*>(Record(list, swan, slot)) + 13, 2000 + slot); Seal(swan); }
	WriteU32(const_cast<std::uint8_t*>(Record(list, swan, 267)) + 13, 5);
	CHECK(SlotToReplace(list, swan, 200, 309) == 267); // all used: the oldest
	const_cast<std::uint8_t*>(Record(list, swan, 304))[4] = 0;
	Seal(swan);
	CHECK(SlotToReplace(list, swan, 200, 309) == 304); // an empty one first
	CHECK(SlotToReplace(Bytes(), Bytes(), 200, 309) == -1);
}

static void TestImportTakesTheSlotAndSaveTime() {
	Bytes list = EmptyList(), swan = EmptySwan();
	const Bytes source = Replay(0x33, 6000);
	Fill(list, swan, 305, source, 0xC3);
	Bytes exported;
	CHECK(Export(list, swan, 305, source, exported));

	for (int slot : {212, 309}) {
		Bytes replay;
		CHECK(Import(exported, slot, 1700000000, list, swan, replay));
		CHECK(replay == source);
		CHECK(ValidSwan(swan) && ValidList(list));
		const std::uint8_t* record = Record(list, swan, slot);
		CHECK(ReadU32(record) == static_cast<std::uint32_t>(slot) && record[4] == 1);
		const SlotInfo info = ReadSlot(list, swan, slot);
		CHECK(info.used && info.size == source.size() && info.crc == Crc32(source.data(), source.size()) && info.time == 1700000000);
		// The menu's fields keep the fighters; the date and time become the
		// import's (1700000000 is 2023-11-14 22:13:20 UTC) and the title says
		// when it was played.
		CHECK(record[17] == 0xC3 && record[71] == 0xC3 && record[kRecordBytes - 3] == 0xC3);
		const RecordInfo shown = ReadRecordInfo(record);
		CHECK(shown.year == 2023 && shown.month == 11 && shown.day == 14 && shown.hour == 22 && shown.minute == 13);
		CHECK(ReadU32(record + 22) == 21 && !std::memcmp(record + 26, "50115-195-195 195:195", 21));
		CHECK(swan[kSwanSlotBytesOffset + slot * 2] == 0x0E);
		// It exports again as the same replay, with the import's record.
		Bytes again;
		CHECK(Export(list, swan, slot, replay, again));
		CHECK(Bytes(again.begin() + kExportHeaderBytes, again.end()) == source);
		CHECK(Bytes(again.begin() + 8, again.begin() + 8 + kRecordBytes) == Bytes(record, record + kRecordBytes));
	}
	// The source slot is untouched.
	CHECK(ReadSlot(list, swan, 305).time == 1549657440 && ReadU32(Record(list, swan, 305)) == 305);
}

static void TestImportRefusesDamageAndChangesNothing() {
	Bytes list = EmptyList(), swan = EmptySwan(), exported, replay;
	const Bytes source = Replay(0x33, 6000);
	Fill(list, swan, 300, source, 0);
	CHECK(Export(list, swan, 300, source, exported));
	const Bytes listBefore = list, swanBefore = swan;

	Bytes flipped = exported;
	flipped.back() ^= 1;
	CHECK(!Import(flipped, 250, 1, list, swan, replay));
	Bytes cut(exported.begin(), exported.end() - 1);
	CHECK(!Import(cut, 250, 1, list, swan, replay));
	Bytes renamed = exported;
	renamed[0] = 'X';
	CHECK(!Import(renamed, 250, 1, list, swan, replay));
	CHECK(!Import(Bytes(exported.begin(), exported.begin() + 20), 250, 1, list, swan, replay));
	Bytes unused = exported;
	unused[8 + 4] = 0;
	CHECK(!Import(unused, 250, 1, list, swan, replay));
	CHECK(!Import(exported, 310, 1, list, swan, replay));
	CHECK(list == listBefore && swan == swanBefore && replay.empty());

	Bytes damagedSwan = swan;
	damagedSwan[100] ^= 1;
	CHECK(!Import(exported, 250, 1, list, damagedSwan, replay));
	Bytes damagedList = list;
	WriteU32(damagedList.data() + 4, 0);
	CHECK(!Import(exported, 250, 1, damagedList, swan, replay));
	CHECK(list == listBefore && replay.empty());
}

// Offsets taken from a record the game wrote on 2026-10-05 21:35 UTC for
// fighters 34 and 33.
static void TestRecordInfoReadsTheMenuFields() {
	std::uint8_t record[kRecordBytes] = {};
	record[71] = 34; record[105] = 33;
	record[51] = 0xEA; record[52] = 0x07; record[53] = 10; record[54] = 5;
	record[123] = 21; record[124] = 35;
	const RecordInfo info = ReadRecordInfo(record);
	CHECK(info.fighters[0] == 34 && info.fighters[1] == 33);
	CHECK(info.year == 2026 && info.month == 10 && info.day == 5 && info.hour == 21 && info.minute == 35);
	record[71] = 200;
	CHECK(ReadRecordInfo(record).fighters[0] == -1);
}

static void TestExportFromASaverReplayAndEntry() {
	Bytes list = EmptyList(), swan = EmptySwan();
	const Bytes replay = Replay(0x55, 7000);
	Fill(list, swan, 303, replay, 0xD4);
	const std::uint8_t* record = Record(list, swan, 303);
	const Bytes entry(record + 4, record + kRecordBytes); // what the saver keeps
	Bytes exported;
	CHECK(ExportFromReplay(replay, entry, exported));
	Bytes replayBack;
	CHECK(Import(exported, 290, 1700000000, list, swan, replayBack));
	CHECK(replayBack == replay && ReadSlot(list, swan, 290).used);
	CHECK(Record(list, swan, 290)[71] == 0xD4 && Record(list, swan, 290)[kRecordBytes - 3] == 0xD4);
	CHECK(!ExportFromReplay(Replay(0x56, 7000), entry, exported)); // another replay
	CHECK(!ExportFromReplay(replay, Bytes(entry.begin(), entry.end() - 1), exported));
}

// A save whose records are not in slot order (a tester's LIST held slot 36 at
// position 30): the record is found by the slot it names, and an import
// lands on that record, not on the slot's position.
static void TestRecordsOutOfSlotOrder() {
	Bytes list = EmptyList(), swan = EmptySwan();
	std::uint8_t* at30 = list.data() + kListRecordsOffset + 30 * kRecordBytes;
	std::uint8_t* at36 = list.data() + kListRecordsOffset + 36 * kRecordBytes;
	WriteU32(at30, 36); WriteU32(at36, 30);
	CHECK(Record(list, swan, 36) == at30 && Record(list, swan, 30) == at36);
	const Bytes source = Replay(0x51, 4000);
	Fill(list, swan, 36, source, 0xA5);
	CHECK(ReadSlot(list, swan, 36).used && !ReadSlot(list, swan, 30).used);
	Bytes exported, replay;
	CHECK(Export(list, swan, 36, source, exported));
	CHECK(Import(exported, 30, 1700000000, list, swan, replay));
	CHECK(Record(list, swan, 30) == at36 && ReadU32(at36) == 30 && at36[4] == 1 && ReadU32(at36 + 13) == 1700000000);
	CHECK(ReadU32(at30) == 36 && at30[4] == 1);
}

// A saver file with no .index entry: the record is made up from the
// replay's own header, with the fighters and the played time it names.
static void TestExportFromAReplayAlone() {
	Bytes replay = Replay(0x00, 8000);
	WriteU32(replay.data() + 0x20, 38); WriteU32(replay.data() + 0x170, 1);
	const std::uint64_t filetime = 116444736000000000ull + 1791302248ull * 10000000ull; // 2026-10-06 15:57:28 UTC
	WriteU32(replay.data() + 0x10, static_cast<std::uint32_t>(filetime)); WriteU32(replay.data() + 0x14, static_cast<std::uint32_t>(filetime >> 32));
	ReplayHeaderInfo header;
	CHECK(ReadReplayHeader(replay, header) && header.fighters[0] == 38 && header.fighters[1] == 1 && header.time == 1791302248);
	Bytes exported;
	CHECK(ExportFromReplayAlone(replay, exported));
	Bytes list = EmptyList(), swan = EmptySwan(), back;
	CHECK(Import(exported, 300, 1800000000, list, swan, back));
	CHECK(back == replay);
	const std::uint8_t* record = Record(list, swan, 300);
	const RecordInfo info = ReadRecordInfo(record);
	CHECK(info.fighters[0] == 38 && info.fighters[1] == 1 && record[50] == 7 && record[77] == 8 && record[111] == 8);
	CHECK(!std::memcmp(record + 26, "2026-10-06 15:57", 16));
	CHECK(!ExportFromReplayAlone(Replay(0x00, 0x100), exported));
}

int main() {
	TestExportFromAReplayAlone();
	TestRecordsOutOfSlotOrder();
	TestExportFromASaverReplayAndEntry();
	TestRecordInfoReadsTheMenuFields();
	TestCrcIsTheOneTheGameWrites();
	TestIndexValidity();
	TestExportFromBothIndexes();
	TestExportRefusesWhatDoesNotBelong();
	TestSlotToReplaceFollowsTheGame();
	TestImportTakesTheSlotAndSaveTime();
	TestImportRefusesDamageAndChangesNothing();
	printf("replay_slots_test: all tests passed\n");
	return 0;
}
