// Export of the game's replay slots, on indexes built at the offsets of LIST
// and replays-swan.dat.

#include "../common/ReplaySlots.hxx"
#include "../common/ReplayFileSafety.hxx"

#include <algorithm>
#include <map>

#include "test_support.hxx"

using namespace sf4e::replayslots;

#include "replay_slots_support.hxx"

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
		CHECK(record[17] == meta && record[kRecordBytes - 3] == meta);
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
	for (int slot = 200; slot <= 309; slot++) Fill(list, swan, slot, Replay(0x11, 500), 0);
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
		// Everything the menu shows stays as it was played: the title, the
		// fighters and the date and time. Only the save time is the import's.
		CHECK(!std::memcmp(record + 17, exported.data() + 8 + 17, kRecordBytes - 17));
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
 for (int byte = 0; byte < 2; ++byte) {
  Bytes invalidState = exported;
  invalidState[8 + kRecordBytes + byte] = 0xFF;
  CHECK(!Import(invalidState, 250, 1, list, swan, replay));
  CHECK(list == listBefore && swan == swanBefore && replay.empty());
 }
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
	CHECK(Record(list, swan, 290)[17] == 0xD4 && Record(list, swan, 290)[kRecordBytes - 3] == 0xD4);
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

// A replay put back into a slot keeps its CRC under a new save time, and the
// archive's file names give that CRC back, so it is not archived twice.
static void TestAnImportedReplayIsKnownByItsCrc() {
	Bytes list = EmptyList(), swan = EmptySwan(), exported, back;
	const Bytes replay = Replay(0x21, 6000);
	Fill(list, swan, 300, replay, 0x44);
	CHECK(Export(list, swan, 300, replay, exported));
	const std::uint32_t saved = ReadSlot(list, swan, 300).time;
	CHECK(Import(exported, 301, saved + 5000, list, swan, back));
	const SlotInfo again = ReadSlot(list, swan, 301);
	CHECK(again.time != saved && again.crc == Crc32(replay.data(), replay.size()));
	std::uint32_t crc = 0;
	std::uint64_t time = 0;
	// One reader of the name gives both the save time and the CRC.
	CHECK(ParseArchiveName(L"20261005-213503-69991186.emberreplay", time, crc) && crc == 0x69991186 && time == 1791236103);
	CHECK(ParseArchiveName(L"19700101-000000-0000000A.emberreplay", time, crc) && crc == 10 && time == 0);
	CHECK(ParseArchiveName(L"20240229-235959-ffffffff.emberreplay", time, crc) && crc == 0xFFFFFFFFu && time == 1709251199);
	CHECK(!ParseArchiveName(L"20261005-213503-69991186.mp4", time, crc));
	CHECK(!ParseArchiveName(L"20261005-213503-6999118.emberreplayy", time, crc));
	CHECK(!ParseArchiveName(L"20261005-213503-6999zzzz.emberreplay", time, crc));
	CHECK(!ParseArchiveName(L"20261305-213503-69991186.emberreplay", time, crc)); // month 13
	CHECK(!ParseArchiveName(L"2026100x-213503-69991186.emberreplay", time, crc));
	CHECK(!ParseArchiveName(L"watched.txt", time, crc));
	// The archive holds it only when the file is that replay: an export whose
	// record describes its body, under the body's own CRC.
	CHECK(WholeArchived(exported, Crc32(replay.data(), replay.size())));
	CHECK(!WholeArchived(exported, Crc32(replay.data(), replay.size()) + 1)); // another name
	CHECK(!WholeArchived(Bytes(exported.begin(), exported.end() - 1), Crc32(replay.data(), replay.size()))); // cut short
	CHECK(!WholeArchived(Bytes(), 0) && !WholeArchived(replay, Crc32(replay.data(), replay.size()))); // empty, or not an export
}

// A record describes a replay only with a whole header and the same fighters.
static void TestARecordNeedsAWholeReplay() {
	Bytes list = EmptyList(), swan = EmptySwan(), out;
	// Only the magic, with the size and CRC the record names: not a replay.
	Bytes stub = {'#', 'B', 'R', 'P'};
	std::uint8_t* record = const_cast<std::uint8_t*>(Record(list, swan, 300));
	record[4] = 1; WriteU32(record + 5, Crc32(stub.data(), stub.size())); WriteU32(record + 9, 4);
	Seal(swan);
	CHECK(!Describes(Record(list, swan, 300), stub.data(), stub.size()) && !Export(list, swan, 300, stub, out));
	// One byte short of the header.
	const Bytes shortOne = Replay(0x07, kReplayHeaderBytes - 1);
	Bytes list2 = EmptyList(), swan2 = EmptySwan();
	Fill(list2, swan2, 301, shortOne, 0);
	CHECK(!Export(list2, swan2, 301, shortOne, out));
	// A whole one is described; with another fighter in the record it is not.
	const Bytes whole = Replay(0x07, kReplayHeaderBytes);
	Fill(list2, swan2, 302, whole, 0);
	CHECK(Export(list2, swan2, 302, whole, out));
	const_cast<std::uint8_t*>(Record(list2, swan2, 302))[71] ^= 1;
	Seal(swan2);
	CHECK(!Export(list2, swan2, 302, whole, out));
	// A fighter outside the roster in the header is nobody's.
	Bytes odd = Replay(0x07, 2000);
	WriteU32(odd.data() + 0x170, 200);
	Fill(list2, swan2, 303, odd, 0);
	CHECK(!Export(list2, swan2, 303, odd, out));
}

// The files as an import finds them and as a Writer leaves them.
struct Folder {
	std::map<std::string, Bytes> files;
	int writes = 0, failAt = -1;
	bool Write(const std::string& name, const Bytes& contents) {
		if (writes++ == failAt) { files[name] = Bytes{0xDE, 0xAD}; return false; } // a failed write may leave anything
		files[name] = contents;
		return true;
	}
};

// An import is a plan of writes; a write that fails puts every file back as it
// was, whichever write it is, and so does undoing a plan that was written.
static void TestAnImportThatFailsLeavesTheFilesAsTheyWere() {
	for (int slot : {290, 305}) {
		Bytes list = EmptyList(), swan = EmptySwan(), exported;
		const Bytes held = Replay(0x10, 5000), incoming = Replay(0x31, 7000);
		Fill(list, swan, slot, held, 0x55);
		Bytes otherList = EmptyList(), otherSwan = EmptySwan();
		Fill(otherList, otherSwan, 300, incoming, 0x66);
		CHECK(Export(otherList, otherSwan, 300, incoming, exported));
		const SlotFiles before{list, Sidecar(list), swan, Sidecar(swan), held, Sidecar(held)};
		Folder start;
		const std::string name = std::to_string(slot);
		start.files = {{name, held}, {name + ".0", Sidecar(held)}, {"replays-swan.dat", swan}, {"replays-swan.dat.0", Sidecar(swan)}, {"LIST", list}, {"LIST.0", Sidecar(list)}};
		WritePlan plan;
		CHECK(PlanImport(exported, slot, 1800000000, before, plan));
		CHECK(plan.size() == (slot < kListSlots ? 6u : 4u) && plan[0].name == name && plan[1].name == name + ".0" && plan[0].before == held);
		// The replay goes first and the indexes that name it last.
		CHECK(plan[2].name == "replays-swan.dat" && plan.back().name == (slot < kListSlots ? "LIST.0" : "replays-swan.dat.0"));
		// Production executes this plan with nightly's exact-file rollback,
		// including when the live table refuses after every write succeeded.
		std::vector<sf4e::replayfiles::Change> changes;
		for (const auto& step : plan) changes.push_back({step.name, step.before, step.now, true});
		for (int failAt = 0; failAt <= static_cast<int>(plan.size()); failAt++) {
			Folder folder = start;
			folder.failAt = failAt == static_cast<int>(plan.size()) ? -1 : failAt; // publication-only refusal has no writer fault
			CHECK(sf4e::replayfiles::Apply(changes, [] { return true; },
				[&](const std::string& file, const Bytes& contents) { return folder.Write(file, contents); },
				[&](const std::string& file) { folder.files.erase(file); return true; },
				[] { return false; }) == sf4e::replayfiles::ApplyOutcome::FailedRestored);
			CHECK(folder.files == start.files);
		}
		// Every write succeeds: the files are the import's, and the slot exports as the new replay.
		Folder folder = start;
		const auto write = [&](const std::string& file, const Bytes& contents) { return folder.Write(file, contents); };
		CHECK(sf4e::replayfiles::Apply(changes, [] { return true; }, write, [&](const std::string& file) { folder.files.erase(file); return true; }, [] { return true; }) == sf4e::replayfiles::ApplyOutcome::Done);
		Bytes again;
		CHECK(Export(folder.files["LIST"], folder.files["replays-swan.dat"], slot, folder.files[name], again));
		CHECK(Bytes(again.begin() + kExportHeaderBytes, again.end()) == incoming && folder.files[name + ".0"] == Sidecar(incoming));
		CHECK(ValidSwan(folder.files["replays-swan.dat"]) && folder.files["replays-swan.dat.0"] == Sidecar(folder.files["replays-swan.dat"]));
		// A write that fails while putting back is told.
		Folder stuck = start;
		stuck.failAt = 2; // the third write of the plan fails, then the first put-back does too
		int calls = 0;
		const auto failing = [&](const std::string& file, const Bytes& contents) { return ++calls != 4 && stuck.Write(file, contents); };
		CHECK(sf4e::replayfiles::Apply(changes, [] { return true; }, failing, [](const std::string&) { return true; }, [] { return true; }) == sf4e::replayfiles::ApplyOutcome::RecoveryIncomplete);
	}
	// A slot that held nothing: newly created files are removed and indexes restored.
	Bytes list = EmptyList(), swan = EmptySwan(), exported;
	const Bytes incoming = Replay(0x31, 7000);
	Bytes otherList = EmptyList(), otherSwan = EmptySwan();
	Fill(otherList, otherSwan, 300, incoming, 0x66);
	CHECK(Export(otherList, otherSwan, 300, incoming, exported));
	WritePlan plan;
	CHECK(PlanImport(exported, 301, 1, SlotFiles{list, Sidecar(list), swan, Sidecar(swan), Bytes(), Bytes()}, plan));
	Folder folder;
	folder.files = {{"replays-swan.dat", swan}, {"replays-swan.dat.0", Sidecar(swan)}};
	folder.failAt = 3;
	const auto write = [&](const std::string& file, const Bytes& contents) { return folder.Write(file, contents); };
	// Production's rollback also removes newly created slot files rather
	// than leaving the imported body behind an empty record.
	std::vector<sf4e::replayfiles::Change> changes;
	for (const auto& step : plan) changes.push_back({step.name, step.before, step.now, !step.before.empty()});
	folder = Folder{};
	folder.files = {{"replays-swan.dat", swan}, {"replays-swan.dat.0", Sidecar(swan)}};
	const auto original = folder.files;
	folder.failAt = 3;
	CHECK(sf4e::replayfiles::Apply(changes, [] { return true; }, write,
		[&](const std::string& file) { folder.files.erase(file); return true; }, [] { return true; }) == sf4e::replayfiles::ApplyOutcome::FailedRestored);
	CHECK(folder.files == original);
	// What Import refuses makes no plan.
	Bytes damaged = exported;
	damaged.back() ^= 1;
	CHECK(!PlanImport(damaged, 301, 1, SlotFiles{list, Sidecar(list), swan, Sidecar(swan), Bytes(), Bytes()}, plan) && plan.empty());
}

// The match just played: its file is on disk and the index still names the
// replay the slot held before. No slot is chosen from an index in that state.
static void TestASlotWhoseIndexIsBehind() {
	Bytes list = EmptyList(), swan = EmptySwan();
	const Bytes old = Replay(0x10, 5000), fresh = Replay(0x20, 8000);
	Fill(list, swan, 301, old, 0x55);
	CHECK(!FileAheadOfRecord(list, swan, 301, old, Sidecar(old)));    // the record names the file
	CHECK(FileAheadOfRecord(list, swan, 301, fresh, Sidecar(fresh))); // a whole file the record does not name
	CHECK(FileAheadOfRecord(list, swan, 302, fresh, Sidecar(fresh))); // and in a slot the index has as empty
	CHECK(!FileAheadOfRecord(list, swan, 301, fresh, Sidecar(old)));  // a file still being written is nobody's yet
	CHECK(!FileAheadOfRecord(list, swan, 302, Bytes(), Bytes()));     // no file
	CHECK(!FileAheadOfRecord(list, swan, 302, Bytes(kLargestReplay), Sidecar(Bytes(kLargestReplay)))); // the game's empty preallocation
}

// A slot whose file is newer than its record: archived from the file when its
// ".0" says it is whole, not otherwise; with a record that names it, as Export.
static void TestASlotFileAheadOfItsRecord() {
	Bytes list = EmptyList(), swan = EmptySwan(), exported;
	const Bytes old = Replay(0x10, 5000);
	Fill(list, swan, 301, old, 0x55);
	Bytes fresh = Replay(0x00, 8000);
	WriteU32(fresh.data() + 0x20, 11); WriteU32(fresh.data() + 0x170, 1);
	bool fromRecord = true;
	CHECK(ExportSlotFile(list, swan, 301, fresh, Sidecar(fresh), exported, fromRecord) && !fromRecord);
	CHECK(Bytes(exported.begin() + kExportHeaderBytes, exported.end()) == fresh);
	CHECK(ReadU32(exported.data() + 8 + 5) == Crc32(fresh.data(), fresh.size()));
	CHECK(!ExportSlotFile(list, swan, 301, fresh, Sidecar(old), exported, fromRecord));
	CHECK(!ExportSlotFile(list, swan, 301, fresh, Bytes(), exported, fromRecord));
	CHECK(ExportSlotFile(list, swan, 301, old, Bytes(), exported, fromRecord) && fromRecord);
}

// A list that names a slot nowhere: its position serves only when the record
// there names no slot, never when it is another slot's.
static void TestRecordNeverTakesAnotherSlots() {
	Bytes list = EmptyList(), swan = EmptySwan();
	std::uint8_t* at30 = list.data() + kListRecordsOffset + 30 * kRecordBytes;
	WriteU32(at30, 36); // slot 30's position now holds slot 36's record, and no record names 30
	const std::uint8_t* at36 = list.data() + kListRecordsOffset + 36 * kRecordBytes;
	CHECK(Record(list, swan, 36) == at30);
	CHECK(Record(list, swan, 30) == nullptr);
	WriteU32(at30, 0xFFFFFFFF); // never filled: the position is the slot's again
	CHECK(Record(list, swan, 30) == at30 && Record(list, swan, 36) == at36);
}

// An export from anyone: a record whose fields the game would not show is refused.
static void TestImportRefusesAnImplausibleRecord() {
	Bytes replay = Replay(0x00, 8000), exported, back;
	WriteU32(replay.data() + 0x20, 38); WriteU32(replay.data() + 0x170, 1);
	const std::uint64_t filetime = 116444736000000000ull + 1791302248ull * 10000000ull;
	WriteU32(replay.data() + 0x10, static_cast<std::uint32_t>(filetime)); WriteU32(replay.data() + 0x14, static_cast<std::uint32_t>(filetime >> 32));
	CHECK(ExportFromReplayAlone(replay, exported));
	const auto refused = [&](std::size_t at, std::uint8_t value) {
		Bytes bad = exported, list = EmptyList(), swan = EmptySwan();
		bad[8 + at] = value;
		return !Import(bad, 300, 1800000000, list, swan, back);
	};
	Bytes list = EmptyList(), swan = EmptySwan();
	CHECK(Import(exported, 300, 1800000000, list, swan, back));
	CHECK(refused(71, 200)); // a fighter outside the roster
	CHECK(refused(105, 64));
	CHECK(refused(53, 13)); // month 13
	CHECK(refused(54, 0)); // day 0
	CHECK(refused(123, 24)); // hour 24
	CHECK(refused(22, 22)); // a title longer than its field
}

int main() {
	TestARecordNeedsAWholeReplay();
	TestAnImportThatFailsLeavesTheFilesAsTheyWere();
	TestASlotWhoseIndexIsBehind();
	TestRecordNeverTakesAnotherSlots();
	TestImportRefusesAnImplausibleRecord();
	TestASlotFileAheadOfItsRecord();
	TestAnImportedReplayIsKnownByItsCrc();
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
