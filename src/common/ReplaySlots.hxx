#pragma once

// The game's saved replays, as files. It keeps 310 replay slots; the last
// ones are where a Versus battle saves its own replay (ten in the stock game,
// slots 280 to 309 with sf4e::Game::Battle::WidenMatchReplayList). On disk,
// in ssf4_savedata:
//
//   LIST               the index of slots 0 to 299: a version, a count and
//                      one 125-byte slot record each
//   replays-swan.dat   the index of slots 300 to 309: a CRC-32 of the rest,
//                      an "SRL" chunk holding one "SRI" chunk, which is ten
//                      slot records and then two bytes for each of the 310
//                      slots
//   <slot>             the replay itself, magic "#BRP"
//   <slot>.0           the CRC-32 of the replay, little-endian; the two
//                      indexes have one each too
//
// A slot record is its slot number, a used flag, the replay's CRC-32, size and
// save time, and 108 bytes the menu shows (title, fighters, players), all
// little-endian. An exported replay is the record, the slot's two bytes and
// the replay in one file, so it can be put back into any slot. Everything
// here works on bytes in memory and touches no file. The game holds the slots
// in memory while it runs and writes them back on its next save, so an import
// made while it runs has to reach that table too (sf4e__ReplayStore.cxx).
// ReplaySlotsTest covers export and import on indexes built at these offsets.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>
#include <vector>

namespace sf4e { namespace replayslots {

using Bytes = std::vector<std::uint8_t>;

constexpr int kSlots = 310;
constexpr int kListSlots = 300;
constexpr std::size_t kRecordBytes = 125;
constexpr std::size_t kListRecordsOffset = 8;
constexpr std::size_t kSwanRecordsOffset = 40;
constexpr std::size_t kSwanSlotBytesOffset = kSwanRecordsOffset + (kSlots - kListSlots) * kRecordBytes;
constexpr std::size_t kListMinBytes = kListRecordsOffset + kListSlots * kRecordBytes;
constexpr std::size_t kSwanMinBytes = kSwanSlotBytesOffset + kSlots * 2;
// The game preallocates 51,200 bytes for a replay it has not written yet.
constexpr std::size_t kLargestReplay = 51200;
constexpr char kExportMagic[] = "EMBRPLY1";
constexpr std::size_t kExportHeaderBytes = 8 + kRecordBytes + 2;

inline std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
	std::uint32_t crc = 0xFFFFFFFFu;
	for (std::size_t i = 0; i < size; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

inline std::uint32_t ReadU32(const std::uint8_t* p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void WriteU32(std::uint8_t* p, std::uint32_t value) {
	for (int i = 0; i < 4; i++) p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// The four bytes of a ".0" file for these contents.
inline Bytes Sidecar(const Bytes& file) {
	Bytes out(4);
	WriteU32(out.data(), Crc32(file.data(), file.size()));
	return out;
}

inline bool ValidList(const Bytes& list) {
	return list.size() >= kListMinBytes && ReadU32(list.data()) == 1 && ReadU32(list.data() + 4) == kListSlots;
}

// Long enough, the two chunk tags in place, and the leading CRC matching the rest.
inline bool ValidSwan(const Bytes& swan) {
	return swan.size() >= kSwanMinBytes &&
		!std::memcmp(swan.data() + 8, "SRL", 4) && !std::memcmp(swan.data() + 28, "SRI", 4) &&
		ReadU32(swan.data()) == Crc32(swan.data() + 4, swan.size() - 4);
}

// The slot's record, or null when the slot is out of range or its index is
// not one the game wrote. Each record names its slot in its first four
// bytes, and a save need not keep them in slot order (a tester's LIST had
// slot 36 at position 30), so the record is the one that names the slot.
// A list that names it nowhere falls back to the slot's position, unless
// the record there names another slot: that one is the other slot's.
inline const std::uint8_t* Record(const Bytes& list, const Bytes& swan, int slot) {
	if (slot < 0 || slot >= kSlots) return nullptr;
	const bool inList = slot < kListSlots;
	if (inList ? !ValidList(list) : !ValidSwan(swan)) return nullptr;
	const std::uint8_t* const records = inList ? list.data() + kListRecordsOffset : swan.data() + kSwanRecordsOffset;
	const int first = inList ? 0 : kListSlots, count = inList ? kListSlots : kSlots - kListSlots;
	for (int at = 0; at < count; at++)
		if (ReadU32(records + at * kRecordBytes) == static_cast<std::uint32_t>(slot)) return records + at * kRecordBytes;
	const std::uint8_t* const fallback = records + (slot - first) * kRecordBytes;
	return ReadU32(fallback) < static_cast<std::uint32_t>(kSlots) ? nullptr : fallback;
}

struct SlotInfo {
	bool used;
	std::uint32_t crc, size, time; // time is seconds since 1970, UTC
};

// An unused slot when there is no record.
inline SlotInfo ReadSlot(const Bytes& list, const Bytes& swan, int slot) {
	const std::uint8_t* record = Record(list, swan, slot);
	if (!record) return {false, 0, 0, 0};
	return {record[4] != 0, ReadU32(record + 5), ReadU32(record + 9), ReadU32(record + 13)};
}

// What the menu shows for a record: the two fighters (native IDs, -1 when
// out of range) and the match's date and time, UTC. From the record's menu
// fields (offset 47): a header of 4 bytes, year, month and day, then two
// 34-byte players, each with its fighter at +16, then the hour and minute.
// Read from the game's own records (ReplaySlotsTest).
struct RecordInfo {
	int fighters[2];
	int year, month, day, hour, minute;
};

inline RecordInfo ReadRecordInfo(const std::uint8_t* record) {
	RecordInfo info;
	for (int side = 0; side < 2; side++) {
		const int fighter = record[47 + 8 + side * 34 + 16];
		info.fighters[side] = fighter < 64 ? fighter : -1;
	}
	info.year = record[51] | (record[52] << 8);
	info.month = record[53]; info.day = record[54];
	info.hour = record[123]; info.minute = record[124];
	return info;
}

// What a replay file's own header says: the two fighters (dwords at 0x20
// and 0x170, the second player's block 0x150 after the first's) and when it
// was played (a FILETIME at 0x10; the record's save time matches it for a
// record the game wrote). Read from 153 archived replays against their
// records (ReplaySlotsTest). False for anything shorter than the header.
struct ReplayHeaderInfo {
	int fighters[2];
	std::uint32_t time; // seconds since 1970, UTC
};
constexpr std::size_t kReplayHeaderBytes = 0x174;
inline bool ReadReplayHeader(const std::uint8_t* replay, std::size_t size, ReplayHeaderInfo& info) {
	if (size < kReplayHeaderBytes || std::memcmp(replay, "#BRP", 4)) return false;
	for (int side = 0; side < 2; side++) {
		const std::uint32_t fighter = ReadU32(replay + 0x20 + side * 0x150);
		info.fighters[side] = fighter < 64 ? static_cast<int>(fighter) : -1;
	}
	const std::uint64_t filetime = ReadU32(replay + 0x10) | (static_cast<std::uint64_t>(ReadU32(replay + 0x14)) << 32);
	info.time = filetime > 116444736000000000ull ? static_cast<std::uint32_t>((filetime - 116444736000000000ull) / 10000000ull) : 0;
	return true;
}
inline bool ReadReplayHeader(const Bytes& replay, ReplayHeaderInfo& info) { return ReadReplayHeader(replay.data(), replay.size(), info); }

// A record that is used and names this replay: its size and CRC, of a file
// the game could have saved, with a whole header whose two fighters are the
// record's. The one check every export and import makes.
inline bool Describes(const std::uint8_t* record, const std::uint8_t* replay, std::size_t size) {
	ReplayHeaderInfo header;
	if (!record[4] || size > kLargestReplay || !ReadReplayHeader(replay, size, header) ||
		ReadU32(record + 9) != size || ReadU32(record + 5) != Crc32(replay, size)) return false;
	const RecordInfo info = ReadRecordInfo(record);
	return header.fighters[0] >= 0 && header.fighters[1] >= 0 && header.fighters[0] == info.fighters[0] && header.fighters[1] == info.fighters[1];
}

// The export of a slot, given the contents of its replay file. False when the
// indexes are damaged, the slot is empty or the file is not the one the slot
// names: the record must be used and name the file's size and CRC, and the
// file must be one the game could have saved.
inline bool Export(const Bytes& list, const Bytes& swan, int slot, const Bytes& replay, Bytes& out) {
	const std::uint8_t* record = Record(list, swan, slot);
	if (!record || !ValidSwan(swan) || !Describes(record, replay.data(), replay.size())) return false;
	out.assign(kExportMagic, kExportMagic + 8);
	out.insert(out.end(), record, record + kRecordBytes);
	out.insert(out.end(), swan.data() + kSwanSlotBytesOffset + slot * 2, swan.data() + kSwanSlotBytesOffset + slot * 2 + 2);
	out.insert(out.end(), replay.begin(), replay.end());
	return true;
}

// What the game reads of a record it did not write: a title of at most 21
// characters, two fighters of the roster and a date and time that exist. An
// export may come from anyone, so a record outside that is not imported.
inline bool PlausibleRecord(const std::uint8_t* record) {
	const RecordInfo info = ReadRecordInfo(record);
	return ReadU32(record + 22) <= 21 && info.fighters[0] >= 0 && info.fighters[1] >= 0 &&
		info.year >= 2009 && info.year <= 2199 && info.month >= 1 && info.month <= 12 && info.day >= 1 && info.day <= 31 && info.hour < 24 && info.minute < 60;
}

// An export built from a replay file and the 121 bytes of its slot record
// from the used flag on, which is what usf4-replay-saver keeps beside each
// replay (its .index/<crc>.entry). The slot number is left for Import to
// set and the slot's two bytes are the ones every saved slot has. False
// when the entry does not describe this replay.
inline bool ExportFromReplay(const Bytes& replay, const Bytes& entry, Bytes& out) {
	if (entry.size() < kRecordBytes - 4) return false;
	Bytes record(kRecordBytes, 0);
	std::memcpy(record.data() + 4, entry.data(), kRecordBytes - 4);
	if (!Describes(record.data(), replay.data(), replay.size())) return false;
	out.assign(kExportMagic, kExportMagic + 8);
	out.insert(out.end(), record.begin(), record.end());
	out.push_back(0x0E); out.push_back(0x0E);
	out.insert(out.end(), replay.begin(), replay.end());
	return true;
}

// An export built from a replay file alone, for a saver file without its
// .index entry: the record is made up as the game writes one for a Versus
// battle (type 7 at 50, 8 at each player's +22, 0E 0E as the slot's bytes,
// as every Ember-recorded replay carries), with the fighters and the played
// date and time from the replay's own header.
inline bool ExportFromReplayAlone(const Bytes& replay, Bytes& out) {
	ReplayHeaderInfo header;
	if (!ReadReplayHeader(replay, header) || header.fighters[0] < 0 || header.fighters[1] < 0) return false;
	Bytes full(kRecordBytes, 0);
	std::uint8_t* record = full.data();
	record[4] = 1;
	WriteU32(record + 5, Crc32(replay.data(), replay.size()));
	WriteU32(record + 9, static_cast<std::uint32_t>(replay.size()));
	WriteU32(record + 13, header.time);
	const std::time_t at = header.time;
	tm utc = {};
#ifdef _WIN32
	gmtime_s(&utc, &at);
#else
	gmtime_r(&at, &utc);
#endif
	char title[22] = {};
	std::snprintf(title, sizeof(title), "%04d-%02d-%02d %02d:%02d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min);
	WriteU32(record + 22, 21);
	std::memcpy(record + 26, title, 21);
	record[47] = 0x02; record[48] = 0x0E; record[49] = 0x00; record[50] = 0x07;
	record[51] = static_cast<std::uint8_t>(utc.tm_year + 1900); record[52] = static_cast<std::uint8_t>((utc.tm_year + 1900) >> 8);
	record[53] = static_cast<std::uint8_t>(utc.tm_mon + 1); record[54] = static_cast<std::uint8_t>(utc.tm_mday);
	for (int side = 0; side < 2; side++) {
		record[71 + side * 34] = static_cast<std::uint8_t>(header.fighters[side]);
		record[77 + side * 34] = 8;
	}
	record[123] = static_cast<std::uint8_t>(utc.tm_hour); record[124] = static_cast<std::uint8_t>(utc.tm_min);
	return ExportFromReplay(replay, Bytes(full.begin() + 4, full.end()), out);
}
// A record made up above carries a title, which the game's own records of a
// Versus battle never do, and names no winner.
inline bool MadeUp(const std::uint8_t* record) { return ReadU32(record + 22) != 0; }
// The winner in a record the game wrote: 0 for player 1, 1 for player 2.
// -1 for a made-up record or any other value.
inline int RecordWinner(const std::uint8_t* record) { return !MadeUp(record) && record[49] <= 1 ? record[49] : -1; }

// The export of a slot from its file, whatever the index says of it. The
// game writes a replay's file and its ".0" when a match ends, and its indexes
// at a save of their own, which a slot's file can be ahead of (or never get:
// the file is then on disk with the record of the replay it replaced). With
// a record that names the file this is Export. Without one the file has to
// be whole, which its ".0" says, and the record is made up from its header
// (ExportFromReplayAlone); fromRecord tells which.
inline bool ExportSlotFile(const Bytes& list, const Bytes& swan, int slot, const Bytes& replay, const Bytes& sidecar, Bytes& out, bool& fromRecord) {
	fromRecord = Export(list, swan, slot, replay, out);
	return fromRecord || (sidecar.size() == 4 && sidecar == Sidecar(replay) && ExportFromReplayAlone(replay, out));
}

// An archived replay's file name, "20261005-213503-69991186.emberreplay":
// the save time (UTC, seconds since 1970) and the replay's CRC. False for any
// other name. A replay put back into the game is saved under a new time
// (Import), so the CRC alone tells whether the archive holds it.
inline bool ParseArchiveName(const std::wstring& name, std::uint64_t& time, std::uint32_t& crc) {
	if (name.size() != 36 || name[8] != L'-' || name[15] != L'-' || name.compare(24, std::wstring::npos, L".emberreplay")) return false;
	int part[6] = {};
	const int at[6] = {0, 4, 6, 9, 11, 13}, digits[6] = {4, 2, 2, 2, 2, 2};
	for (int i = 0; i < 6; i++)
		for (int d = 0; d < digits[i]; d++) {
			const wchar_t c = name[at[i] + d];
			if (c < L'0' || c > L'9') return false;
			part[i] = part[i] * 10 + (c - L'0');
		}
	crc = 0;
	for (int i = 16; i < 24; i++) {
		const wchar_t c = name[i];
		const int digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
		if (digit < 0) return false;
		crc = crc << 4 | static_cast<std::uint32_t>(digit);
	}
	if (part[0] < 1970 || part[1] < 1 || part[1] > 12 || part[2] < 1 || part[2] > 31 || part[3] > 23 || part[4] > 59 || part[5] > 60) return false;
	// Days since 1970 for a date of the proleptic Gregorian calendar.
	const int y = part[0] - (part[1] <= 2), era = y / 400, yoe = y - era * 400;
	const int doy = (153 * (part[1] + (part[1] > 2 ? -3 : 9)) + 2) / 5 + part[2] - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
	time = static_cast<std::uint64_t>(days * 86400 + part[3] * 3600 + part[4] * 60 + part[5]);
	return true;
}

// Whether an archive file is the replay its name says: an export whose record
// describes its body, with the name's CRC. The archive is a folder anyone can
// put a file in, and this is what lets a slot be written over.
inline bool WholeArchived(const Bytes& exported, std::uint32_t nameCrc) {
	if (exported.size() < kExportHeaderBytes || std::memcmp(exported.data(), kExportMagic, 8)) return false;
	const std::uint8_t* record = exported.data() + 8;
	return ReadU32(record + 5) == nameCrc && Describes(record, exported.data() + kExportHeaderBytes, exported.size() - kExportHeaderBytes);
}

// A slot whose file the index does not describe: the game writes a match's
// replay file as the match ends and its indexes at a later save, and until
// then the index still names the replay the slot held before. sidecar is the
// slot's ".0". False for a slot with no whole replay in its file.
inline bool FileAheadOfRecord(const Bytes& list, const Bytes& swan, int slot, const Bytes& replay, const Bytes& sidecar) {
	const std::uint8_t* record = Record(list, swan, slot);
	ReplayHeaderInfo header;
	if (!ReadReplayHeader(replay, header) || sidecar != Sidecar(replay)) return false;
	return !record || !Describes(record, replay.data(), replay.size());
}

// The slot a new replay goes into among first to last, the way the game picks
// one (0x67B830): the first empty slot, else the one saved longest ago. -1
// when no slot in the range has a record.
inline int SlotToReplace(const Bytes& list, const Bytes& swan, int first, int last) {
	int oldest = -1;
	for (int slot = first; slot <= last; slot++) {
		const SlotInfo info = ReadSlot(list, swan, slot);
		if (!Record(list, swan, slot)) continue;
		if (!info.used) return slot;
		if (oldest < 0 || info.time < ReadSlot(list, swan, oldest).time) oldest = slot;
	}
	return oldest;
}

// Puts an exported replay into a slot, replacing what was there: the slot's
// index gets the record with this slot number and now as its save time, swan
// gets the slot's two bytes and a new leading CRC, and replay becomes the
// contents for the slot's file. The save time decides which slot the next
// replay replaces, so a fresh one keeps the import until the slots cycle
// round. The date and time the menu shows stay the ones it was played at, so
// the menu, which lists by date, has it at its own place. The caller writes the
// changed files and their ".0" files from Sidecar. False, changing nothing,
// when the export or an index is damaged. The export may come from anyone;
// the size, CRC and magic are checked here and the record's fields are kept
// to what the game shows (PlausibleRecord); the replay itself is read by the
// game.
inline bool Import(const Bytes& exported, int slot, std::uint32_t now, Bytes& list, Bytes& swan, Bytes& replay) {
	const std::uint8_t* target = Record(list, swan, slot);
	if (!target || !ValidSwan(swan) || exported.size() < kExportHeaderBytes || std::memcmp(exported.data(), kExportMagic, 8)) return false;
	const std::uint8_t* record = exported.data() + 8;
	const std::uint8_t* body = exported.data() + kExportHeaderBytes;
	const std::size_t size = exported.size() - kExportHeaderBytes;
	if (!Describes(record, body, size) || !PlausibleRecord(record)) return false;
	std::uint8_t* out = const_cast<std::uint8_t*>(target);
	std::memcpy(out, record, kRecordBytes);
	WriteU32(out, static_cast<std::uint32_t>(slot));
	WriteU32(out + 13, now);
	std::memcpy(swan.data() + kSwanSlotBytesOffset + slot * 2, record + kRecordBytes, 2);
	WriteU32(swan.data(), Crc32(swan.data() + 4, swan.size() - 4));
	replay.assign(body, body + size);
	return true;
}

// A match as Ember noted it when it started (platform/ReplayFiles.hxx:
// NoteMatchStart): when, the two players' names, their fighters (-1 in a note
// written before fighters were) and whether this PC only watched.
struct NotedMatch {
	std::uint64_t started;
	std::string names[2];
	int fighters[2];
	bool spectated;
};

// The match a replay saved at `saved` belongs to: the last one that started
// before the save, within the hour, with the replay's two fighters. Both
// times are this PC's clock, so the start is never after the save; a replay
// is saved as its match ends, and in a room the next match has often started
// by the time it is looked at, which is why the save is the limit. Null when
// no note fits.
inline const NotedMatch* MatchOf(const std::vector<NotedMatch>& noted, std::uint64_t saved, const int fighters[2]) {
	const NotedMatch* match = nullptr;
	for (const NotedMatch& candidate : noted) {
		if (candidate.started > saved || saved >= candidate.started + 3600) continue;
		const bool named = candidate.fighters[0] >= 0 && candidate.fighters[1] >= 0;
		if (named && (candidate.fighters[0] != fighters[0] || candidate.fighters[1] != fighters[1])) continue;
		if (!match || candidate.started > match->started) match = &candidate;
	}
	return match;
}

// The files an import changes, as data: each by its name in the save folder,
// with the bytes it gets and the bytes it had before anything was changed.
// The replay and its ".0" come first and the indexes that name it last, so an
// index never names a file that is not there yet.
struct PlannedWrite {
	std::string name;
	Bytes now, before;
};
using WritePlan = std::vector<PlannedWrite>;

// The plan for putting an export into a slot (Import), from the files as they
// are now: the slot's replay and its ".0", swan and its ".0", and for a slot
// of LIST that file and its ".0". False, with an empty plan, where Import
// refuses. A file that was not there has an empty `before`.
struct SlotFiles {
	Bytes list, listSidecar, swan, swanSidecar, replay, replaySidecar;
};
inline bool PlanImport(const Bytes& exported, int slot, std::uint32_t now, const SlotFiles& files, WritePlan& plan) {
	plan.clear();
	Bytes list = files.list, swan = files.swan, replay;
	if (!Import(exported, slot, now, list, swan, replay)) return false;
	const std::string name = std::to_string(slot);
	plan.push_back({name, replay, files.replay});
	plan.push_back({name + ".0", Sidecar(replay), files.replaySidecar});
	plan.push_back({"replays-swan.dat", swan, files.swan});
	plan.push_back({"replays-swan.dat.0", Sidecar(swan), files.swanSidecar});
	if (slot < kListSlots) {
		plan.push_back({"LIST", list, files.list});
		plan.push_back({"LIST.0", Sidecar(list), files.listSidecar});
	}
	return true;
}

// Puts back the first `count` files of a plan, last written first. A file that
// was not there before stays as written: nothing names it once its index is
// back. False when a file could not be put back.
template <class Writer> bool UndoPlan(const WritePlan& plan, std::size_t count, const Writer& write) {
	bool whole = true;
	for (std::size_t at = count < plan.size() ? count : plan.size(); at-- > 0;)
		if (!plan[at].before.empty() && !write(plan[at].name, plan[at].before)) whole = false;
	return whole;
}

// Writes a plan in order. On the first write that fails, the files written so
// far are put back and it is false; `undone` then says whether they all were.
template <class Writer> bool RunPlan(const WritePlan& plan, const Writer& write, bool& undone) {
	undone = true;
	for (std::size_t at = 0; at < plan.size(); at++) {
		if (write(plan[at].name, plan[at].now)) continue;
		// The failed write may have left the file changed, so it is put back too.
		undone = UndoPlan(plan, at + 1, write);
		return false;
	}
	return true;
}

} }
