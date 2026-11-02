#include "replay_slots_support.hxx"
#include "../common/ReplayFileSafety.hxx"
#include "../common/ReplayProvenance.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/ReplaySets.hxx"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#include "../platform/ReplayPath.hxx"
#include "../platform/DurableFile.hxx"
#include <winioctl.h>
#include <cstdio>
#include <cstring>
#endif

static void TestImportWriteRecovery() {
	namespace files = sf4e::replayfiles;
	const std::vector<files::Change> changes = {
		{"280", {1, 2, 3}, {9}, true}, {"280.0", {4, 3, 2, 1}, {8}, true},
		{"replays-swan.dat", {5, 6}, {7}, true}, {"replays-swan.dat.0", {}, {6}, false},
		{"LIST", {}, {5}, true}, {"LIST.0", {7, 8}, {4}, true}
	};
	std::map<std::string, Bytes> original;
	for (const auto& change : changes) if (change.existed) original[change.name] = change.before;
	// Each failed write has already changed its file, including a missing file
	// and a sidecar that did not hold its body's checksum before the import.
	for (int thrown = 0; thrown < 2; thrown++) for (int failure = 0; failure <= static_cast<int>(changes.size()); failure++) {
		auto disk = original;
		int attempts = 0, publications = 0;
		bool undoing = false;
		std::vector<std::string> undone;
		const auto write = [&](const std::string& name, const Bytes& bytes) {
			disk[name] = bytes;
			if (undoing) { undone.push_back(name); return true; }
			if (attempts++ == failure) {
				undoing = true;
				if (thrown) throw std::runtime_error("write stopped");
				return false;
			}
			return true;
		};
		const auto remove = [&](const std::string& name) { undone.push_back(name); disk.erase(name); return true; };
		const auto publish = [&] {
			++publications;
			undoing = true;
			for (const auto& change : changes) CHECK(disk.at(change.name) == change.after);
			if (thrown) throw std::runtime_error("table refused");
			return false;
		};
		CHECK(files::Apply(changes, [] { return true; }, write, remove, publish) == files::ApplyOutcome::FailedRestored);
		CHECK(disk == original);
		CHECK(publications == (failure == static_cast<int>(changes.size()) ? 1 : 0));
		const std::size_t count = (std::min)(static_cast<std::size_t>(failure + 1), changes.size());
		CHECK(undone.size() == count);
		for (std::size_t at = 0; at < count; at++) CHECK(undone[at] == changes[count - 1 - at].name);
	}
	auto disk = original;
	CHECK(files::Apply(changes, [] { return true; }, [&](const std::string& name, const Bytes& bytes) { disk[name] = bytes; return true; },
		[&](const std::string& name) { disk.erase(name); return true; }, [] { return true; }) == files::ApplyOutcome::Done);
	for (const auto& change : changes) CHECK(disk.at(change.name) == change.after);
	int undoAttempts = 0;
	CHECK(files::Apply(changes, [] { return true; }, [&](const std::string&, const Bytes&) { return ++undoAttempts <= static_cast<int>(changes.size()); },
		[&](const std::string&) -> bool { ++undoAttempts; throw std::runtime_error("delete stopped"); }, [] { return false; }) == files::ApplyOutcome::RecoveryIncomplete);
	CHECK(undoAttempts == static_cast<int>(changes.size() * 2));
}

static void TestVerifiedArchiveAndSnapshots() {
	namespace fs = std::filesystem;
	namespace files = sf4e::replayfiles;
	const fs::path folder = fs::temp_directory_path() / ("ember-replay-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	CHECK(fs::create_directory(folder));
	const auto save = [](const fs::path& path, const Bytes& bytes) {
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		out.close(); CHECK(!out.fail());
	};
	files::Change snapshot;
	CHECK(files::Snapshot(folder / "missing", 100, snapshot) && !snapshot.existed);
	CHECK(!files::Snapshot(folder, 100, snapshot));
	save(folder / "empty", {});
	CHECK(files::Snapshot(folder / "empty", 100, snapshot) && snapshot.existed && snapshot.before.empty());
	save(folder / "large", Bytes(101, 0x42));
	CHECK(!files::Snapshot(folder / "large", 100, snapshot));
	save(folder / "exact", {3, 2, 1});
	CHECK(files::Snapshot(folder / "exact", 3, snapshot) && snapshot.before == Bytes({3, 2, 1}));
	Bytes list = EmptyList(), swan = EmptySwan(), exported;
	const Bytes body = Replay(0, 8000);
	Fill(list, swan, 280, body, 0x44);
	CHECK(Export(list, swan, 280, body, exported));
	char name[64] = {};
	std::snprintf(name, sizeof name, "20261005-213503-%08x.emberreplay", Crc32(body.data(), body.size()));
	const fs::path archive = folder / name;
	files::ArchiveFile read;
	CHECK(fs::create_directory(archive));
	CHECK(files::ReadArchive(archive, true, read) != files::ArchiveState::Valid);
#ifdef _WIN32
 CHECK(sf4e::platform::ResolveReplayFile(sf4e::platform::WideToUtf8(archive.wstring())).empty());
#endif
	CHECK(fs::remove(archive));
	save(archive, exported);
	CHECK(files::ReadArchive(archive, true, read) == files::ArchiveState::Valid && read.crc == Crc32(body.data(), body.size()));
#ifdef _WIN32
 const auto resolved=sf4e::platform::ResolveReplayFile(sf4e::platform::WideToUtf8(archive.wstring()));
 CHECK(!resolved.empty() && fs::equivalent(fs::u8path(resolved),archive));
 const auto alias=folder/"alias.emberreplay";
 std::error_code linkError;
 fs::create_symlink(archive,alias,linkError);
 if(!linkError) {
  const auto target=sf4e::platform::ResolveReplayFile(sf4e::platform::WideToUtf8(alias.wstring()));
  CHECK(!target.empty() && fs::equivalent(fs::u8path(target),archive));
  fs::remove(alias);
 }
#endif
	Bytes bad = exported; bad.resize(bad.size() - 1); save(archive, bad);
	CHECK(files::ReadArchive(archive, true, read) != files::ArchiveState::Valid);
	bad = exported; bad.back() ^= 1; save(archive, bad);
	CHECK(files::ReadArchive(archive, true, read) != files::ArchiveState::Valid);
	bad = exported; bad[0] = 'X'; save(archive, bad);
	CHECK(files::ReadArchive(archive, true, read) != files::ArchiveState::Valid);
	bad = exported; bad[8 + 5] ^= 1; save(archive, bad);
	CHECK(files::ReadArchive(archive, true, read) != files::ArchiveState::Valid);
	save(folder / "20261005-213503-00000000.emberreplay", exported);
	CHECK(files::ReadArchive(folder / "20261005-213503-00000000.emberreplay", true, read) != files::ArchiveState::Valid);
	save(folder / "saver.usf4replay", body);
	CHECK(files::ReadArchive(folder / "saver.usf4replay", false, read) == files::ArchiveState::Valid && read.crc == Crc32(body.data(), body.size()));
	save(folder / "saver.usf4replay", {'#', 'B', 'R', 'P'});
	CHECK(files::ReadArchive(folder / "saver.usf4replay", false, read) != files::ArchiveState::Valid);
 // The index located a valid candidate; it is removed/truncated before import.
 save(archive, exported);
 const std::vector<fs::path> indexed{archive};
 files::BackupEvidence proof;
 CHECK(files::VerifyBackup(indexed, folder, body, proof) && proof.replay.SameBody(body));
 CHECK(fs::remove(archive));
 CHECK(!files::VerifyBackup(indexed, folder, body, proof));
 save(archive, Bytes(exported.begin(), exported.end()-1));
 CHECK(!files::VerifyBackup(indexed, folder, body, proof));
 // Same named CRC cannot authorize replacing a different body's bytes.
 save(archive, exported);
 const Bytes other = SameCrcDifferentBody(body);
 CHECK(!files::VerifyBackup(indexed, folder, other, proof));
 // A valid replacement with the same size, timestamp and CRC must still
 // produce its current body, for both listing provenance and import proof.
 const auto timestamp = fs::last_write_time(archive);
 Bytes replaced = exported;
 std::copy(other.begin(), other.end(), replaced.begin() + kExportHeaderBytes);
 save(archive, replaced); fs::last_write_time(archive, timestamp);
 CHECK(files::ReadArchive(archive, true, read) == files::ArchiveState::Valid && read.SameBody(other));
 CHECK(!files::VerifyBackup(indexed, folder, body, proof));
 CHECK(files::VerifyBackup(indexed, folder, other, proof));
 sf4e::replayfiles::ReplayNames names{{"Ann", "Bob"}, files::Provenance::Played}, named;
 CHECK(!files::NamesForBody(files::BindNames(body, names), read.Body(), named));
 save(archive, exported);
 CHECK(files::VerifyBackup(indexed, folder, body, proof));
 fs::remove(archive);
 CHECK(proof.replay.SameBody(body)); // memory survives, but no durable backup does
 CHECK(!files::ImportFilesUnchanged({}, proof));
 save(archive, exported);
 CHECK(files::ImportFilesUnchanged({}, proof));
 // Even an export-header change with the identical body invalidates the
 // complete archive artifact captured by preparation.
 replaced = exported; replaced[8 + 17] ^= 1;
 save(archive, replaced);
 CHECK(!files::ImportFilesUnchanged({}, proof));
	fs::remove_all(folder);
}


static void TestBodyBoundProvenance() {
 namespace files = sf4e::replayfiles;
 Bytes old = Replay(1, 800), body = Replay(2, 800);
 const std::uint64_t nativeTime = 116444736000000000ull + 5000ull * 10000000ull;
 WriteU32(body.data()+0x10, static_cast<std::uint32_t>(nativeTime));
 WriteU32(body.data()+0x14, static_cast<std::uint32_t>(nativeTime >> 32));
 const int fighters[2] = {2, 3};
 std::vector<files::RecordedSlot> before{{old, Sidecar(old)}, {old, Sidecar(old)}};
 auto after = before; after[0] = {body, Sidecar(body)};
 CHECK(files::RecordedBody(before, after, 1000, 5001, fighters) == &after[0].body); // over an hour
 CHECK(!files::RecordedBody(before, before, 1000, 5001, fighters));
 CHECK(!files::RecordedBody(before, after, 5001, 6000, fighters)); // wrong recording window
 const int other[2] = {2, 4};
 CHECK(!files::RecordedBody(before, after, 1000, 5001, other));
 after[0].checksum.clear(); CHECK(!files::RecordedBody(before, after, 1000, 5001, fighters));
 after[0].checksum = Sidecar(body);
 after[1] = after[0]; CHECK(!files::RecordedBody(before, after, 1000, 5001, fighters)); // ambiguous
 files::ReplayNames names{{"Ann", "Bob"}, files::Provenance::Spectated}, read;
 const Bytes note = files::BindNames(body, names);
 CHECK(files::NamesForBody(note, body, read) && read.players[1] == "Bob" && read.provenance == files::Provenance::Spectated);
 // Only a note says how this PC took part: played is its own state, and
 // names that know neither are never written down as either.
 files::ReplayNames played = names; played.provenance = files::Provenance::Played;
 CHECK(files::NamesForBody(files::BindNames(body, played), body, read) && read.provenance == files::Provenance::Played);
 played.provenance = files::Provenance::Unknown; CHECK(files::BindNames(body, played).empty());
 CHECK(files::ReplayNames{}.provenance == files::Provenance::Unknown);
 // Same fighters and timestamp, or a renamed external/account file, cannot
 // borrow this note unless its complete body is identical.
 const Bytes unrelated = SameCrcDifferentBody(body);
 CHECK(!files::NamesForBody(note, unrelated, read));
 CHECK(!files::NamesForBody(Bytes(note.begin(),note.end()-1), body, read));
 Bytes corrupt = note; corrupt[20] = 2;
 CHECK(!files::NamesForBody(corrupt, body, read));
 CHECK(!files::NamesForBody({}, body, read)); // old time-only notes have no identity
 CHECK(!read.set.Tagged() && read.set.revision == 0);
 // A set's game keeps its bridge, match and room revision.
 names.set = {"brg_bridge", "emt_set", (std::uint64_t{1} << 32) + 3};
 CHECK(files::NamesForBody(files::BindNames(body, names), body, read) && read.set.bridgeId == "brg_bridge" &&
  read.set.matchId == "emt_set" && read.set.revision == names.set.revision && read.players[1] == "Bob");
 names.set.bridgeId.assign(files::kMostId + 1, 'x'); CHECK(files::BindNames(body, names).empty());
 // A version 1 note, written before sets were tagged, still reads.
 Bytes v1{'E','M','B','R','N','A','M','1'}; v1.resize(20);
 WriteU32(v1.data()+8, static_cast<std::uint32_t>(body.size())); WriteU32(v1.data()+12, 3); WriteU32(v1.data()+16, 3);
 v1.push_back(1); v1.insert(v1.end(), body.begin(), body.end());
 for (char c : std::string("AnnBob")) v1.push_back(static_cast<std::uint8_t>(c));
 CHECK(files::NamesForBody(v1, body, read) && read.players[0] == "Ann" && read.players[1] == "Bob" && read.provenance == files::Provenance::Spectated && !read.set.Tagged() && read.set.revision == 0);
 v1[7] = '3'; CHECK(!files::NamesForBody(v1, body, read));
}
static void TestNamesPublicationRefusesExistingTargets() {
 namespace fs = std::filesystem;
 namespace files = sf4e::replayfiles;
 const auto folder = fs::temp_directory_path() / ("ember-names-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 CHECK(fs::create_directory(folder));
 const Bytes body = Replay(2, 800), other = SameCrcDifferentBody(body);
 files::ReplayNames names{{"Ann", "Bob"}, files::Provenance::Played};
 const auto path = folder / "body.names";
 const auto save = [&](const Bytes& bytes) { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); CHECK(file.good()); };
 unsigned writes = 0;
 const auto create = [&](const Bytes&) { ++writes; return false; };
 save({'d','a','m','a','g','e','d'});
 CHECK(files::PublishBodyNames(path, body, names, create) == files::NamePublication::Refused && writes == 0);
 save(files::BindNames(other, names));
 CHECK(files::PublishBodyNames(path, body, names, create) == files::NamePublication::Refused && writes == 0);
 save(files::BindNames(body, names));
 CHECK(files::PublishBodyNames(path, body, names, create) == files::NamePublication::Published && writes == 0);
#ifdef _WIN32
 HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
 CHECK(held != INVALID_HANDLE_VALUE);
 CHECK(files::PublishBodyNames(path, body, names, create) == files::NamePublication::Refused && writes == 0);
 CHECK(CloseHandle(held));
#endif
 CHECK(fs::remove(path));
 CHECK(files::PublishBodyNames(path, body, names, create) == files::NamePublication::Retryable && writes == 1);
 CHECK(files::PublishBodyNames(path, body, names, [&](const Bytes&) { save({'r','a','c','e'}); return false; }) == files::NamePublication::Refused);
 CHECK(files::ReadFile(path).value() == Bytes({'r','a','c','e'}));
 fs::remove_all(folder);
}
static void TestImportNoticesPreserveOutcomes() {
 using namespace sf4e::platform::replays;
 const auto notice = [](ImportResult result, const char* id) { CHECK(std::string(ImportNotice(result)) == id); };
 notice(ImportResult::IndexBehind, "replays.not_added_yet");
 notice(ImportResult::ArchiveFailed, "replays.not_added_archive");
 notice(ImportResult::RejectedBeforeWrite, "replays.not_added_files");
 notice(ImportResult::FailedRestored, "replays.not_added_restored");
 notice(ImportResult::RecoveryIncomplete, "replays.not_added_recovery");
 notice(ImportResult::IndexDamaged, "replays.not_added_files");
 notice(ImportResult::NoFolder, "replays.not_added_files");
 notice(ImportResult::NotAReplay, "replays.not_added");
 notice(ImportResult::Done, "");
}
static void TestArchivePublicationPolicy() {
 namespace files = sf4e::replayfiles;
 const Bytes body = Replay(0, 8000), other = SameCrcDifferentBody(body), exported{1, 2, 3};
 unsigned writes = 0;
 const auto create = [&](const Bytes&) { ++writes; return true; };
 for (auto state : {files::ArchiveState::Unreadable, files::ArchiveState::Invalid}) {
  CHECK(!files::PublishArchive(body, exported, [&](files::ArchiveFile&) { return state; }, create));
  CHECK(writes == 0);
 }
 CHECK(files::PublishArchive(body, exported, [](files::ArchiveFile&) { return files::ArchiveState::Missing; }, create));
 CHECK(writes == 1);
 const auto existing = [&](const Bytes& bytes, files::ArchiveFile& out) { out.contents = bytes; return files::ArchiveState::Valid; };
 CHECK(files::PublishArchive(body, exported, [&](files::ArchiveFile& out) { return existing(body, out); }, create));
 CHECK(!files::PublishArchive(body, exported, [&](files::ArchiveFile& out) { return existing(other, out); }, create));
 CHECK(writes == 1); // equal CRC does not authorize replacement
 Bytes destination;
 CHECK(!files::PublishArchive(body, exported, [](files::ArchiveFile&) { return files::ArchiveState::Missing; }, [&](const Bytes&) {
  destination = other; // another publisher won after the read
  return false; // create-only publication refuses the now-existing target
 }));
 CHECK(destination == other);
}
#ifdef _WIN32
static void TestProductionArchivePublicationRefusal() {
 namespace fs = std::filesystem;
 namespace files = sf4e::replayfiles;
 // The production writer: create-only publication through a fresh partial file.
 const auto PublishFile = [](const fs::path& path, const Bytes& bytes) { return static_cast<bool>(sf4e::durable::PublishCreateOnly(path, sf4e::durable::PartialPath(path), bytes.data(), bytes.size())); };
 const auto folder = fs::temp_directory_path() / ("ember-publication-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 CHECK(fs::create_directory(folder));
 Bytes list = EmptyList(), swan = EmptySwan(), exported;
 const Bytes body = Replay(0, 8000);
 Fill(list, swan, 280, body, 0x44); CHECK(Export(list, swan, 280, body, exported));
 char name[64] = {};
 std::snprintf(name, sizeof name, "20261005-213503-%08x.emberreplay", Crc32(body.data(), body.size()));
 const auto target = folder / name;
 const auto read = [&](files::ArchiveFile& out) { return files::ReadArchive(target, true, out); };
 files::ArchiveFile out;
 CHECK(read(out) == files::ArchiveState::Missing);
 CHECK(PublishFile(target, exported));
 // This handle permits deletion but refuses other readers, exactly the
 // sharing failure that previously enabled unconditional replacement.
 HANDLE held = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
 CHECK(held != INVALID_HANDLE_VALUE);
 CHECK(read(out) == files::ArchiveState::Unreadable);
 unsigned writes = 0;
 CHECK(!files::PublishArchive(body, exported, read, [&](const Bytes& bytes) { ++writes; return PublishFile(target, bytes); }));
 CHECK(writes == 0); CHECK(CloseHandle(held));
 CHECK(files::ReadFile(target).value() == exported);
 CHECK(fs::remove(target));
 const Bytes competitor{'p','r','e','s','e','r','v','e'};
 CHECK(!files::PublishArchive(body, exported, read, [&](const Bytes& bytes) {
  CHECK(PublishFile(target, competitor)); // destination appeared after Missing
  return PublishFile(target, bytes);
 }));
 CHECK(files::ReadFile(target).value() == competitor);
 CHECK(read(out) == files::ArchiveState::Invalid);
 CHECK(!files::PublishArchive(body, exported, read, [&](const Bytes& bytes) { ++writes; return PublishFile(target, bytes); }));
 CHECK(writes == 0);
 // Failed publication also removes only its own temporary file.
 CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator{}) == 1);
 fs::remove_all(folder);
}
// Show in folder (ApplicationServices: ShowReplayFile) opens only a folder of
// the archive as a handle reaches it when asked: a junction or link out of
// it, a path that climbs out, a missing archive, a network path and a folder
// swapped for a junction after the listing are all refused, and a name in
// other case is the same folder.
static std::wstring Junction(const std::filesystem::path& link, const std::filesystem::path& target) {
 namespace fs = std::filesystem;
 std::wstring command=L"cmd.exe /c mklink /J \""+link.wstring()+L"\" \""+target.wstring()+L"\" >nul";
 STARTUPINFOW startup={sizeof startup}; PROCESS_INFORMATION process={};
 if(!CreateProcessW(nullptr,&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return {};
 WaitForSingleObject(process.hProcess,10000);
 CloseHandle(process.hThread); CloseHandle(process.hProcess);
 return fs::is_directory(link)?link.wstring():std::wstring();
}
// Turns an empty folder into a junction to target in place, as another
// program could, with no move or replacement of the folder itself. False when
// Windows refuses, as it does for a folder that is not empty.
static bool JunctionInPlace(const std::filesystem::path& folder, const std::filesystem::path& target) {
 struct MountPoint {
  DWORD tag; WORD dataLength, reserved;
  WORD substituteOffset, substituteLength, printOffset, printLength;
  wchar_t paths[1024];
 } buffer={};
 const std::wstring substitute=L"\\??\\"+target.wstring(), print=target.wstring();
 if(substitute.size()+print.size()+2>1024)return false;
 buffer.tag=IO_REPARSE_TAG_MOUNT_POINT;
 buffer.substituteOffset=0;buffer.substituteLength=static_cast<WORD>(substitute.size()*sizeof(wchar_t));
 buffer.printOffset=static_cast<WORD>((substitute.size()+1)*sizeof(wchar_t));buffer.printLength=static_cast<WORD>(print.size()*sizeof(wchar_t));
 std::memcpy(buffer.paths,substitute.c_str(),substitute.size()*sizeof(wchar_t));
 std::memcpy(buffer.paths+substitute.size()+1,print.c_str(),print.size()*sizeof(wchar_t));
 buffer.dataLength=static_cast<WORD>(8+(substitute.size()+print.size()+2)*sizeof(wchar_t));
 const HANDLE handle=CreateFileW(folder.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
  FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(handle==INVALID_HANDLE_VALUE)return false;
 DWORD written=0;
 const bool set=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&buffer,8+buffer.dataLength,nullptr,0,&written,nullptr)!=0;
 CloseHandle(handle);
 return set;
}
// Makes an empty folder case-sensitive, as WSL's folders are; false where
// this account or this Windows cannot.
static bool CaseSensitive(const std::filesystem::path& folder) {
 const HANDLE handle=CreateFileW(folder.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
 if(handle==INVALID_HANDLE_VALUE)return false;
 FILE_CASE_SENSITIVE_INFO info={};info.Flags=FILE_CS_FLAG_CASE_SENSITIVE_DIR;
 const bool set=SetFileInformationByHandle(handle,FileCaseSensitiveInfo,&info,sizeof info)!=0;
 CloseHandle(handle);
 return set;
}
// A name put in a list is appended; one taken out leaves no line behind.
static void TestNotes() {
 namespace fs = std::filesystem;
 const fs::path list=fs::temp_directory_path()/("ember-notes-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".txt");
 const auto lines=[&] { std::ifstream in(list); return std::string(std::istreambuf_iterator<char>(in), {}); };
 sf4e::replayfiles::Note(list,"a.emberreplay",true); sf4e::replayfiles::Note(list,"b.emberreplay",true);
 CHECK(lines()=="a.emberreplay\nb.emberreplay\n");
 sf4e::replayfiles::Note(list,"a.emberreplay",false);
 CHECK(lines()=="b.emberreplay\n");
 fs::remove(list);
}
static void TestArchiveFolderContainment() {
 namespace fs = std::filesystem;
 using sf4e::platform::ArchiveFolderOf;
 const fs::path folder=fs::temp_directory_path()/("ember-archive-folder-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 const fs::path archive=folder/"archive", outside=folder/"outside";
 CHECK(fs::create_directories(archive/"sub")&&fs::create_directories(outside));
 const auto touch=[](const fs::path& path) { std::ofstream(path,std::ios::binary)<<"replay"; };
 for(const auto& file:{archive/"a.emberreplay",archive/"sub"/"b.emberreplay",outside/"b.emberreplay",outside/"c.emberreplay"})touch(file);
 const auto shown=[&](const fs::path& file,const fs::path& root) { return ArchiveFolderOf(file.wstring(),root.wstring()).Path(); };
 const auto same=[](const std::wstring& opened,const fs::path& want) { return !opened.empty()&&fs::equivalent(fs::path(opened),want); };
 CHECK(same(shown(archive/"a.emberreplay",archive),archive));
 CHECK(same(shown(archive/"sub"/"b.emberreplay",archive),archive/"sub"));
 // Case does not make another folder, of the archive or the file.
 std::wstring upper=archive.wstring(), lower=(archive/"SUB"/"B.EMBERREPLAY").wstring();
 for(auto& c:upper)c=static_cast<wchar_t>(towupper(c));
 CHECK(same(shown(archive/"a.emberreplay",upper),archive));
 CHECK(same(shown(lower,archive),archive/"sub"));
 CHECK(shown(archive/".."/"outside"/"c.emberreplay",archive).empty());
 CHECK(shown(outside/"c.emberreplay",archive).empty());
 CHECK(shown(archive/"a.emberreplay",fs::path()).empty());
 CHECK(shown(archive/"a.emberreplay",folder/"missing").empty());
 CHECK(shown(archive/"a.txt",archive).empty());
 // A network path, even to this PC, is not a drive of this PC.
 const std::wstring drive=archive.wstring().substr(0,1);
 CHECK(shown(L"\\\\localhost\\"+drive+L"$"+archive.wstring().substr(2)+L"\\a.emberreplay",archive).empty());
 CHECK(shown(L"\\\\?\\"+(archive/"a.emberreplay").wstring(),archive).empty());
 // A junction beneath the archive that leads out of it.
 CHECK(!Junction(archive/"linked",outside).empty());
 CHECK(shown(archive/"linked"/"c.emberreplay",archive).empty());
 // A link out of the archive, where this account may make one.
 std::error_code linkError;
 fs::create_directory_symlink(outside,archive/"symlinked",linkError);
 if(!linkError)CHECK(shown(archive/"symlinked"/"c.emberreplay",archive).empty());
 // Listed while it was a folder of the archive, swapped for a junction out
 // of it before Show in folder: refused, since it is checked when asked.
 CHECK(same(shown(archive/"sub"/"b.emberreplay",archive),archive/"sub"));
 fs::rename(archive/"sub",folder/"moved");
 CHECK(!Junction(archive/"sub",outside).empty());
 CHECK(shown(archive/"sub"/"b.emberreplay",archive).empty());
 fs::remove(archive/"sub");
 CHECK(fs::exists(outside/"b.emberreplay"));
 fs::rename(folder/"moved",archive/"sub");
 // While held, through the shell's handoff, the folder cannot be moved, and
 // the replay can be neither deleted nor moved, so the folder cannot be
 // emptied and turned into a junction in place either. Let go, all of it can.
 {
  const auto held=ArchiveFolderOf((archive/"sub"/"b.emberreplay").wstring(),archive.wstring());
  CHECK(!held.Empty());
  CHECK(!MoveFileW((archive/"sub").c_str(),(folder/"moved").c_str()));
  CHECK(!DeleteFileW((archive/"sub"/"b.emberreplay").c_str())&&!MoveFileW((archive/"sub"/"b.emberreplay").c_str(),(folder/"b.emberreplay").c_str()));
  CHECK(!JunctionInPlace(archive/"sub",outside)&&!(GetFileAttributesW((archive/"sub").c_str())&FILE_ATTRIBUTE_REPARSE_POINT));
 }
 CHECK(DeleteFileW((archive/"sub"/"b.emberreplay").c_str())&&JunctionInPlace(archive/"sub",outside));
 CHECK(shown(archive/"sub"/"b.emberreplay",archive).empty());
 fs::remove(archive/"sub");
 CHECK(fs::exists(outside/"b.emberreplay"));
 // Two folders whose names differ only in case, in a case-sensitive folder:
 // the archive's name finds where to look, but only the archive itself, by
 // identity, holds its replays, not its sibling, nor through a junction.
 const fs::path sensitive=folder/"sensitive";
 fs::create_directories(sensitive);
 if(!CaseSensitive(sensitive))std::printf("replay_files_test: no case-sensitive folder here; that check was skipped\n");
 else {
  const fs::path kept=sensitive/"archive", other=sensitive/"Archive";
  CHECK(fs::create_directory(kept)&&fs::create_directory(other)&&!fs::equivalent(kept,other));
  touch(kept/"d.emberreplay");touch(other/"e.emberreplay");
  CHECK(same(shown(kept/"d.emberreplay",kept),kept));
  CHECK(shown(other/"e.emberreplay",kept).empty());
  CHECK(!Junction(kept/"linked",other).empty());
  CHECK(shown(kept/"linked"/"e.emberreplay",kept).empty());
  fs::remove(kept/"linked");
 }
 fs::remove(archive/"linked"); fs::remove(archive/"symlinked",linkError);
 CHECK(fs::exists(outside/"c.emberreplay"));
 fs::remove_all(folder);
}
#endif
// A sender as SendSetReplays keeps it; with `archives` false, one told that
// none of the player's bridges keeps an archive.
static sf4e::platform::replays::ReplaySender Sending(bool archives) {
 sf4e::platform::replays::ReplaySender sending;
 if (!archives) { sending.Sent(1, "x", false, 0); sending.Take(1, false, "replays_unsupported", 0); }
 return sending;
}
// A set's games: sent oldest first, only ones this PC played, and grouped
// by set with the rooms they took; a pack splits back into them.
static void TestSetReplays() {
 namespace sets = sf4e::platform::replays;
 using sf4e::replayfiles::Provenance;
 const auto game = [](const char* match, std::uint64_t revision, const char* file) {
  sets::ArchivedReplay replay; replay.path = file; replay.set = {"brg_bridge", match, revision}; replay.provenance = Provenance::Played; return replay;
 };
 // A listing is newest first.
 std::vector<sets::ArchivedReplay> archive{game("emt_b", 1, "b1"), sets::ArchivedReplay{"casual"}, game("emt_a", 2, "a3"), game("emt_a", 1, "a2"), game("emt_a", 1, "a1"), game("emt_c", 1, "c1")};
 // A set imported from a pack is listed, and never sent.
 archive[5].set.bridgeId.clear();
 archive[4].sent = true; archive[3].provenance = Provenance::Spectated;
 CHECK(sets::NextToSend(archive) == &archive[2]);
 // One whose note is not read (Unknown) is not taken as played.
 archive[2].provenance = Provenance::Unknown; CHECK(sets::NextToSend(archive) == &archive[0]);
 archive[2].provenance = Provenance::Played;
 archive[2].sent = true; CHECK(sets::NextToSend(archive) == &archive[0]);
 archive[0].sent = true; CHECK(!sets::NextToSend(archive));
 // Games outside a set go to the replay archive, Ember's own files only,
 // only ones this PC played, only those saved since sharing came on, and
 // none while it is off (0).
 CHECK(!sets::NextToArchive(archive, 1));
 archive.push_back(sets::ArchivedReplay{"old.emberreplay"}); archive.insert(archive.begin(), sets::ArchivedReplay{"new.emberreplay"});
 archive.back().time = 100; archive[0].time = 200;
 CHECK(!sets::NextToArchive(archive, 1)); // no note says either was played
 archive.back().provenance = archive[0].provenance = Provenance::Played;
 CHECK(sets::NextToArchive(archive, 1) == &archive.back() && !sets::NextToArchive(archive, 0));
 CHECK(sets::NextToArchive(archive, 150) == &archive[0] && !sets::NextToArchive(archive, 201));
 archive.back().provenance = Provenance::Spectated; CHECK(sets::NextToArchive(archive, 1) == &archive[0]);
 // A set's game goes first, sharing or not; the others only while some
 // bridge keeps an archive.
 CHECK(sets::NextReplay(archive, 1, Sending(true), 0) == &archive[0] && !sets::NextReplay(archive, 1, Sending(false), 0) && !sets::NextReplay(archive, 0, Sending(true), 0));
 archive[1].sent = false;
 CHECK(sets::NextReplay(archive, 1, Sending(true), 0) == &archive[1] && sets::NextReplay(archive, 0, Sending(false), 0) == &archive[1]);
 archive[1].sent = true;
 archive[0].sent = true; CHECK(!sets::NextToArchive(archive, 1));
 archive.erase(archive.begin()); archive.pop_back();
 CHECK(sets::SendSettled("not_found") && sets::SendSettled("replays_unsupported") && !sets::SendSettled("bridge_unreachable") && !sets::SendSettled("unauthenticated"));
 const auto listed = sets::Sets(archive);
 CHECK(listed.size() == 3 && listed[0].matchId == "emt_b" && listed[0].rooms == 1 && listed[2].matchId == "emt_c");
 CHECK(listed[1].games.size() == 3 && listed[1].games[0] == &archive[4] && listed[1].games[2] == &archive[2] && listed[1].rooms == 2);
 Bytes one, two;
 CHECK(ExportFromReplayAlone(Replay(2, 800), one) && ExportFromReplayAlone(Replay(3, 900), two));
 Bytes pack{'E','M','B','R','P','A','K','1',2,0,0,0,'{','}'};
 pack.insert(pack.end(), one.begin(), one.end()); pack.insert(pack.end(), two.begin(), two.end());
 std::vector<Bytes> games;
 std::string header;
 CHECK(sets::UnpackSet(pack, games, header) && games.size() == 2 && games[0] == one && games[1] == two && header == "{}");
 CHECK(!sets::UnpackSet(Bytes(pack.begin(), pack.end() - 1), games, header) && games.size() == 2);
 Bytes torn = pack; torn.back() ^= 1; CHECK(!sets::UnpackSet(torn, games, header));
 Bytes huge = pack; WriteU32(huge.data() + 8, 0xFFFFFFFFu); CHECK(!sets::UnpackSet(huge, games, header));
 const auto named = sets::PackNames(R"({"match_id":"emt_b","names":[null,"Bob"]})");
 CHECK(named.set.matchId == "emt_b" && named.players[0].empty() && named.players[1] == "Bob" && named.provenance == Provenance::Spectated);
 CHECK(sets::PackNames("[1]").set.matchId.empty() && sets::PackNames(R"({"names":["A"]})").players[0].empty());
}
// A game listed before its note is written down is not yet known to be
// played here, and is not shared until a later listing reads the note.
static void TestListingAheadOfItsNote() {
 namespace fs = std::filesystem;
 namespace files = sf4e::replayfiles;
 namespace sets = sf4e::platform::replays;
 const auto archive = fs::temp_directory_path() / ("ember-note-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 CHECK(fs::create_directory(archive));
 const Bytes body = Replay(2, 800);
 const auto listing = [&] {
  sets::ArchivedReplay listed; listed.path = archive / "a.emberreplay"; listed.time = 10;
  listed.provenance = files::ReadBodyNames(archive, body).provenance;
  return std::vector<sets::ArchivedReplay>{listed};
 };
 const auto write = [&](const Bytes& note) {
  std::ofstream file(files::NamesPath(archive, body), std::ios::binary); file.write(reinterpret_cast<const char*>(note.data()), note.size()); CHECK(file.good());
 };
 CHECK(listing()[0].provenance == files::Provenance::Unknown && !sets::NextToArchive(listing(), 1));
 CHECK(fs::create_directory(archive / ".names"));
 write({'E','M','B','R','N','A','M','2'}); // torn
 CHECK(listing()[0].provenance == files::Provenance::Unknown && !sets::NextToArchive(listing(), 1));
 write(files::BindNames(body, {{"Ann", "Bob"}, files::Provenance::Played}));
 const auto later = listing();
 CHECK(later[0].provenance == files::Provenance::Played && sets::NextToArchive(later, 1) == &later[0]);
 fs::remove_all(archive);
}
// One replay request out at a time: never replaced while it may be live,
// given up only after the helper's own budget is long spent, and its late
// answer ignored.
static void TestReplaySender() {
 using Sender = sf4e::platform::replays::ReplaySender;
 Sender sending;
 CHECK(sending.Poll(1000) == Sender::Step::Send);
 sending.Sent(7, "a.emberreplay", false, 1000);
 // Long after the minute between looks, it is still out.
 CHECK(sending.Poll(1000 + Sender::kAgainMs) == Sender::Step::Wait && sending.Poll(1000 + Sender::kAnswerMs - 1) == Sender::Step::Wait);
 CHECK(Sender::kAnswerMs >= 3 * 45000); // the helper's REPLAY_BUDGET
 // An answer to some other request settles nothing.
 CHECK(sending.Take(8, true, "", 2000) == Sender::Answer::NotOurs && sending.Poll(3000) == Sender::Step::Wait);
 // A failure is tried again a minute after its answer.
 CHECK(sending.Take(7, false, "bridge_unreachable", 50000) == Sender::Answer::Again);
 CHECK(sending.Poll(50000 + Sender::kAgainMs - 1) == Sender::Step::Wait && sending.Poll(50000 + Sender::kAgainMs) == Sender::Step::Send);
 // No answer in kAnswerMs: given up, as a failure; its late answer is ignored.
 sending.Sent(9, "a.emberreplay", false, 200000);
 CHECK(sending.Poll(200000 + Sender::kAnswerMs) == Sender::Step::GaveUp && sending.Path() == "a.emberreplay");
 CHECK(sending.Poll(200000 + Sender::kAnswerMs + 1) == Sender::Step::Wait);
 CHECK(sending.Take(9, true, "", 200000 + Sender::kAnswerMs + 2) == Sender::Answer::NotOurs);
 const std::uint64_t again = 200000 + Sender::kAnswerMs + Sender::kAgainMs;
 CHECK(sending.Poll(again) == Sender::Step::Send);
 // Taken, or refused for good: the next one shortly.
 sending.Sent(10, "b.emberreplay", true, again);
 CHECK(sending.Take(0, true, "", again) == Sender::Answer::NotOurs);
 CHECK(sending.Take(10, false, "not_found", again + 1) == Sender::Answer::Done && sending.Set() && sending.Path() == "b.emberreplay");
 CHECK(sending.Take(10, true, "", again + 2) == Sender::Answer::NotOurs);
 CHECK(sending.Poll(again + Sender::kNextMs) == Sender::Step::Wait && sending.Poll(again + 1 + Sender::kNextMs) == Sender::Step::Send);
 // No bridge keeps an archive: only sets' games go from then on.
 CHECK(sending.Archives());
 sending.Sent(11, "c.emberreplay", false, again + Sender::kAgainMs);
 CHECK(sending.Take(11, false, "replays_unsupported", again + Sender::kAgainMs) == Sender::Answer::NoArchive && !sending.Archives());
 // A set's game the bridge is busy with is tried again a minute later, never
 // settled; a closed set settles it for the set alone.
 const std::uint64_t set = again + 2 * Sender::kAgainMs;
 for (const char* retried : {"service_unavailable", "http_503"}) {
  CHECK(!sf4e::platform::replays::SendSettled(retried));
  sending.Sent(12, "d.emberreplay", true, set);
  CHECK(sending.Take(12, false, retried, set) == Sender::Answer::Again);
  CHECK(sending.Poll(set + Sender::kAgainMs - 1) == Sender::Step::Wait && sending.Poll(set + Sender::kAgainMs) == Sender::Step::Send);
 }
 sending.Sent(13, "d.emberreplay", true, set);
 CHECK(sending.Take(13, false, "set_closed", set) == Sender::Answer::SetClosed && sending.Path() == "d.emberreplay");
 CHECK(sending.Poll(set + Sender::kNextMs) == Sender::Step::Send);
}
// A set's game its bridge refused for a closed set is done for the set and
// goes to the replay archive as any other game would: while sharing is on,
// and only one saved since it came on. A set's other final refusals keep it
// the set's alone.
static void TestClosedSetGoesToTheArchive() {
 namespace sets = sf4e::platform::replays;
 sets::ArchivedReplay game; game.path = "g.emberreplay"; game.time = 200; game.set = {"brg_bridge", "emt_a", 1};
 game.provenance = sf4e::replayfiles::Provenance::Played;
 std::vector<sets::ArchivedReplay> archive{game};
 CHECK(sets::ForItsSet(archive[0]) && sets::NextToSend(archive) == &archive[0] && !sets::NextToArchive(archive, 1));
 archive[0].setClosed = true;
 CHECK(!sets::ForItsSet(archive[0]) && !sets::NextToSend(archive));
 CHECK(sets::NextToArchive(archive, 100) == &archive[0] && sets::NextReplay(archive, 100, Sending(true), 0) == &archive[0]);
 CHECK(!sets::NextToArchive(archive, 0) && !sets::NextToArchive(archive, 201) && !sets::NextReplay(archive, 100, Sending(false), 0));
 archive[0].sent = true; CHECK(!sets::NextReplay(archive, 100, Sending(true), 0));
 archive[0] = game; archive[0].sent = true; // refused for good otherwise
 CHECK(!sets::NextToSend(archive) && !sets::NextToArchive(archive, 1));
}
// A set's game its bridge has not confirmed yet (held for review) is put
// aside for ten minutes and the others go meanwhile, a later set's game and
// online games; then it is offered again. A plain retryable failure keeps
// the same replay, a minute later.
static void TestUnconfirmedReplayWaits() {
 namespace sets = sf4e::platform::replays;
 using Sender = sets::ReplaySender;
 const auto game = [](const char* file, std::uint64_t time, bool set) {
  sets::ArchivedReplay replay; replay.path = file; replay.time = time; replay.provenance = sf4e::replayfiles::Provenance::Played;
  if (set) replay.set = {"brg_bridge", "emt_a", 1};
  return replay;
 };
 // Newest first: an online game, a later set game, the held one.
 std::vector<sets::ArchivedReplay> archive{game("online.emberreplay", 300, false), game("later.emberreplay", 200, true), game("held.emberreplay", 100, true)};
 Sender sending;
 const std::uint64_t at = 1000;
 CHECK(sets::NextReplay(archive, 1, sending, at) == &archive[2]);
 sending.Sent(1, archive[2].path, true, at);
 CHECK(sending.Take(1, false, "replay_unconfirmed", at) == Sender::Answer::Unconfirmed && sending.Aside(archive[2].path, at));
 CHECK(sending.Poll(at + Sender::kNextMs) == Sender::Step::Send);
 CHECK(sets::NextReplay(archive, 1, sending, at + Sender::kNextMs) == &archive[1]);
 archive[1].sent = true;
 CHECK(sets::NextReplay(archive, 1, sending, at + Sender::kNextMs) == &archive[0]);
 archive[0].sent = true;
 CHECK(!sets::NextReplay(archive, 1, sending, at + Sender::kUnconfirmedMs - 1));
 CHECK(sets::NextReplay(archive, 1, sending, at + Sender::kUnconfirmedMs) == &archive[2] && !sending.Aside(archive[2].path, at + Sender::kUnconfirmedMs));
 // Any other retryable failure keeps that replay first, a minute later.
 const std::uint64_t later = at + Sender::kUnconfirmedMs;
 sending.Sent(2, archive[2].path, true, later);
 CHECK(sending.Take(2, false, "bridge_unreachable", later) == Sender::Answer::Again && !sending.Aside(archive[2].path, later));
 CHECK(sending.Poll(later + Sender::kAgainMs - 1) == Sender::Step::Wait && sending.Poll(later + Sender::kAgainMs) == Sender::Step::Send);
 CHECK(sets::NextReplay(archive, 1, sending, later + Sender::kAgainMs) == &archive[2]);
 // Unconfirmed on the replay archive's route is no set's word: a plain failure.
 sending.Sent(3, archive[0].path, false, later);
 CHECK(sending.Take(3, false, "replay_unconfirmed", later) == Sender::Answer::Again && !sending.Aside(archive[0].path, later));
}
int main() {
 TestSetReplays();
 TestListingAheadOfItsNote();
 TestReplaySender();
 TestClosedSetGoesToTheArchive();
 TestUnconfirmedReplayWaits();
 TestNamesPublicationRefusesExistingTargets();
 TestImportWriteRecovery();
 TestVerifiedArchiveAndSnapshots();
 TestBodyBoundProvenance();
 TestImportNoticesPreserveOutcomes();
 TestArchivePublicationPolicy();
#ifdef _WIN32
 TestProductionArchivePublicationRefusal();
 TestNotes();
 TestArchiveFolderContainment();
#endif
 printf("replay_files_test: all tests passed\n");
 return 0;
}
