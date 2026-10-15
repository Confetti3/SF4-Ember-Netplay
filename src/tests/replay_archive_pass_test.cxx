#include "replay_slots_support.hxx"
#include "../platform/ReplayArchivePass.hxx"
#include "../platform/ReplayArchiveWorker.hxx"
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace fs = std::filesystem;
namespace files = sf4e::replayfiles;

// The stock game's match list: ten slots that a Versus battle saves into
// round and round.
constexpr int kFirst = 300, kLast = 309;

static void Save(const fs::path& path, const Bytes& bytes) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	out.close(); CHECK(!out.fail());
}

// What a process keeps between passes: the archive's index and the slot
// cache. A process that starts afresh has its own.
struct Owner {
	files::ArchiveCandidates index;
	files::SlotFileCache slots;
};

// A game's save folder, an archive and an outbox beside it, with the owner
// and writer a pass is given. While archiveBroken is set no file can be
// written into the archive.
struct Folders {
	fs::path root, saves, archive, outbox;
	std::size_t settleBudget = 1000, settleFrom = 0;
	Bytes list = EmptyList(), swan = EmptySwan();
	Owner own;
	std::atomic<bool> archiveBroken{false};
	explicit Folders(const char* name) {
		root = fs::temp_directory_path() / (std::string(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		saves = root / "ssf4_savedata"; archive = root / "replays"; outbox = root / "replay-outbox";
		CHECK(fs::create_directories(saves));
		WriteIndexes();
		Refresh(own);
	}
	~Folders() { std::error_code ignored; fs::remove_all(root, ignored); }
	void WriteIndexes() { Save(saves / "LIST", list); Save(saves / "replays-swan.dat", swan); }
	// Reads the archive into an owner's index, as the game and the launcher do.
	void Refresh(Owner& owner) {
		owner.index.Refresh([&] { return files::ScanArchive(archive, [](const fs::path&, const files::ArchiveFile&) {}); });
	}
	// The pass's view of the archive, from the owner's index.
	files::ArchiveTarget Target(Owner& owner, files::ArchiveView& view) {
		return {archive, outbox, settleBudget, settleFrom, view, owner.slots, [this](const fs::path& path, const Bytes& bytes) {
			if (archiveBroken && *path.lexically_relative(archive).begin() != "..") return false;
			if (fs::exists(path)) return false;
			Save(path, bytes);
			return true;
		}};
	}
	// What the game writes as a match ends: the replay and its ".0", and,
	// unless its index is behind, the slot's record.
	void SaveMatch(int slot, const Bytes& replay, std::uint32_t savedAt, bool indexed) {
		const fs::path file = saves / std::to_wstring(slot);
		Save(file, replay);
		Save(file.wstring() + L".0", Sidecar(replay));
		if (indexed) Index(slot, replay, savedAt);
	}
	// The slot's record as the game writes it, naming player 2 the winner.
	void Index(int slot, const Bytes& replay, std::uint32_t savedAt) {
		Fill(list, swan, slot, replay, 0x44);
		std::uint8_t* record = const_cast<std::uint8_t*>(Record(list, swan, slot));
		WriteU32(record + 13, savedAt);
		record[49] = 1;
		Seal(swan);
		WriteIndexes();
	}
	files::ArchivePass Pass(Owner& owner, const std::vector<fs::path>& accounts) {
		files::ArchiveView view(owner.index);
		return files::RunPass(Target(owner, view), accounts, kFirst, kLast);
	}
	files::ArchivePass Pass() { return Pass(own, {saves}); }
	// The archive's replay files, each a different replay, and their replays.
	std::vector<fs::path> Archived() const {
		std::vector<fs::path> paths;
		const std::size_t bodies = Bodies(&paths).size();
		CHECK(bodies == paths.size());
		return paths;
	}
	std::set<Bytes> Bodies(std::vector<fs::path>* paths = nullptr) const {
		std::set<Bytes> bodies;
		std::error_code missing;
		for (fs::directory_iterator at(archive, missing), end; !missing && at != end; at.increment(missing)) {
			if (at->path().extension() != L".emberreplay") continue;
			files::ArchiveFile read;
			if (files::ReadArchive(at->path(), true, read) != files::ArchiveState::Valid) continue;
			bodies.insert(read.Body());
			if (paths) paths->push_back(at->path());
		}
		return bodies;
	}
	std::size_t Outboxed() const {
		std::size_t entries = 0;
		std::error_code missing;
		for (fs::directory_iterator at(outbox, missing), end; !missing && at != end; at.increment(missing)) entries++;
		return entries;
	}
};

// A replay played at `played` (seconds since 1970), which its header holds.
static Bytes Match(int match, std::uint32_t played) {
	Bytes replay = Replay(static_cast<std::uint8_t>(match + 1), 2000 + match);
	SetPlayed(replay, played);
	return replay;
}

// Twenty-five matches through ten slots while the game's index never lists
// one. A pass as each is saved puts every one in the archive at once; one
// pass after the sitting finds only the last ten.
static void TestASittingWithTheIndexBehind() {
	Folders asSaved("ember-archive-each"), atExit("ember-archive-exit");
	for (int match = 0; match < 25; match++) {
		const Bytes replay = Match(match, 1791600000u + match * 180);
		asSaved.SaveMatch(kFirst + match % 10, replay, 0, false);
		atExit.SaveMatch(kFirst + match % 10, replay, 0, false);
		const auto pass = asSaved.Pass();
		CHECK(pass.copied == 1 && !pass.retry);
		CHECK(asSaved.Archived().size() == static_cast<std::size_t>(match + 1));
	}
	CHECK(atExit.Pass().copied == 10 && atExit.Archived().size() == 10);
}

// A slot caught between its replay and its ".0" asks for another pass, which
// takes it; a replay whose archive file cannot be written waits in the
// outbox.
static void TestRetries() {
	Folders folders("ember-archive-retry");
	const Bytes first = Match(1, 1791600000u), second = Match(2, 1791600200u);
	Save(folders.saves / "300", first);
	Save(folders.saves / "300.0", Sidecar(second));
	auto pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.retry);
	Save(folders.saves / "300.0", Sidecar(first));
	folders.archiveBroken = true;
	pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.waiting == 1 && !pass.retry && folders.Outboxed() == 1);
	folders.archiveBroken = false;
	pass = folders.Pass();
	CHECK(pass.copied == 1 && pass.waiting == 0 && !pass.retry && folders.Outboxed() == 0 && folders.Archived().size() == 1);
}

// A replay whose archive file cannot be written waits in the outbox, so the
// next match taking its slot does not lose it: both reach the archive, byte
// for byte, once it can be written.
static void TestTheOutboxOutlivesTheSlot() {
	Folders folders("ember-archive-held");
	const Bytes first = Match(1, 1791600000u), second = Match(2, 1791600200u);
	folders.archiveBroken = true;
	folders.SaveMatch(kFirst, first, 0, false);
	auto pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.failed == 1 && pass.waiting == 1);
	folders.SaveMatch(kFirst, second, 0, false);
	CHECK(folders.Pass().waiting == 2);
	folders.archiveBroken = false;
	pass = folders.Pass();
	CHECK(pass.copied == 2 && pass.waiting == 0 && folders.Outboxed() == 0);
	CHECK(folders.Bodies() == std::set<Bytes>({first, second}));
}

// An archive file with the same CRC as a new replay cannot be read, so it is
// not known whether the archive holds the replay. The replay goes into the
// outbox all the same, and its slot being taken loses nothing.
static void TestAnUnreadableCandidateKeepsTheReplay() {
	Folders folders("ember-archive-candidate");
	const Bytes earlier = Match(1, 1791600000u);
	Bytes later = Match(1, 1791600600u); // the same size, played later
	ForgeCrc(later, Crc32(earlier.data(), earlier.size()));
	folders.SaveMatch(kFirst, earlier, 0, false);
	CHECK(folders.Pass().copied == 1);
	const fs::path copy = folders.Archived().at(0);
	const Bytes kept = *files::ReadFile(copy);
	CHECK(fs::remove(copy) && fs::create_directory(copy)); // there, but no file to read
	folders.SaveMatch(kFirst + 1, later, 0, false);
	// Neither replay can be told from the unreadable file: both are kept.
	auto pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.waiting == 2 && folders.Outboxed() == 2);
	const Bytes next = Match(3, 1791600900u);
	folders.SaveMatch(kFirst + 1, next, 0, false);
	CHECK(folders.Pass().copied == 1);
	CHECK(fs::remove(copy));
	Save(copy, kept);
	pass = folders.Pass();
	CHECK(pass.copied == 1 && pass.waiting == 0 && folders.Outboxed() == 0);
	CHECK(folders.Bodies() == std::set<Bytes>({earlier, later, next}));
}

// The outbox has no limit: forty matches through ten slots while the archive
// cannot be written all wait there, and all reach the archive once it can.
static void TestManyReplaysAllWait() {
	Folders folders("ember-archive-many");
	folders.archiveBroken = true;
	folders.settleBudget = 0; // nothing can be settled; listing is enough
	std::set<Bytes> all;
	for (int match = 0; match < 40; match++) {
		const Bytes replay = Match(match, 1791600000u + match * 180);
		all.insert(replay);
		folders.SaveMatch(kFirst + match % 10, replay, 0, false);
		CHECK(folders.Pass().waiting == match + 1);
	}
	folders.archiveBroken = false;
	folders.settleBudget = files::kSettleAll;
	const auto pass = folders.Pass();
	CHECK(pass.copied == 40 && pass.waiting == 0 && folders.Outboxed() == 0 && folders.Bodies() == all);
}

// A game that closes with replays in the outbox leaves them there. The next
// process archives them with an index and a slot cache of its own, and needs
// no game's saves to find them.
static void TestTheOutboxOutlivesTheProcess() {
	Folders folders("ember-archive-closed");
	const Bytes first = Match(1, 1791600000u), second = Match(2, 1791600200u);
	folders.archiveBroken = true;
	folders.SaveMatch(kFirst, first, 0, false);
	CHECK(folders.Pass().waiting == 1);
	folders.SaveMatch(kFirst, second, 0, false);
	folders.archiveBroken = false;
	Owner fresh;
	folders.Refresh(fresh);
	auto pass = folders.Pass(fresh, {});
	CHECK(pass.copied == 1 && pass.waiting == 0 && folders.Outboxed() == 0);
	pass = folders.Pass(fresh, {folders.saves});
	CHECK(pass.copied == 1 && folders.Bodies() == std::set<Bytes>({first, second}));
}

// An outbox that cannot be listed counts as waiting, not as empty.
static void TestAnOutboxThatCannotBeListed() {
	Folders folders("ember-archive-unlisted");
	folders.archiveBroken = true;
	folders.SaveMatch(kFirst, Match(1, 1791600000u), 0, false);
	CHECK(folders.Pass().waiting == 1);
	fs::rename(folders.outbox, folders.root / "away");
	Save(folders.outbox, Bytes(8, 1)); // a file where the outbox was
	folders.archiveBroken = false;
	Owner fresh;
	folders.Refresh(fresh);
	CHECK(folders.Pass(fresh, {}).waiting > 0);
}

// Nine replays of one size and one CRC, played at different times, through
// one slot while the archive cannot be written: each has its own outbox
// entry, named after its bytes, and all nine reach the archive.
static void TestNineReplaysWithOneCrc() {
	Folders folders("ember-archive-one-crc");
	folders.archiveBroken = true;
	const Bytes first = Match(1, 1791600000u);
	std::set<Bytes> all;
	for (int match = 0; match < 9; match++) {
		Bytes replay = Match(1, 1791600000u + match * 180);
		if (match) ForgeCrc(replay, Crc32(first.data(), first.size()));
		all.insert(replay);
		folders.SaveMatch(kFirst, replay, 0, false);
		CHECK(folders.Pass().waiting == match + 1);
	}
	CHECK(all.size() == 9);
	folders.archiveBroken = false;
	const auto pass = folders.Pass();
	CHECK(pass.copied == 9 && pass.waiting == 0 && folders.Bodies() == all);
}

// The launcher's sweep settles the whole outbox in one call, more than a
// Sidecar pass settles, with no game's saves to read.
static void TestTheLauncherSettlesEverything() {
	Folders folders("ember-archive-launcher");
	folders.archiveBroken = true;
	folders.settleBudget = 0;
	for (int match = 0; match < 40; match++) {
		folders.SaveMatch(kFirst + match % 10, Match(match, 1791600000u + match * 180), 0, false);
		folders.Pass();
	}
	CHECK(folders.Outboxed() == 40);
	folders.archiveBroken = false;
	folders.settleBudget = files::kSettleAll;
	Owner fresh;
	folders.Refresh(fresh);
	const auto pass = folders.Pass(fresh, {});
	CHECK(pass.copied == 40 && pass.waiting == 0 && !pass.more && folders.Outboxed() == 0);
}

// The game's index is never read for a replay's record, even when it names
// the replay: a record that names the replay's CRC, size and fighters may be
// an older match's. Every archived replay has a record made from its header.
static void TestTheIndexIsNeverTaken() {
	Folders folders("ember-archive-index");
	folders.SaveMatch(kFirst, Match(1, 1791600000u), 1791600000u, true);
	CHECK(folders.Pass().copied == 1);
	const Bytes contents = *files::ReadFile(folders.Archived().at(0));
	CHECK(MadeUp(contents.data() + 8) && RecordWinner(contents.data() + 8) == -1);
}

// The archive's index, refreshed while a copy cannot be read, still knows the
// copy may hold its replay, so a second export of that replay waits in the
// outbox instead of being archived beside it, and goes once the copy can be
// read again: the archive ends with the one copy.
static void TestARefreshKeepsAnUnreadableCopy() {
	Folders folders("ember-archive-refresh");
	const Bytes replay = Match(1, 1791600000u);
	folders.SaveMatch(kFirst, replay, 0, false);
	CHECK(folders.Pass().copied == 1);
	const fs::path copy = folders.Archived().at(0);
	const Bytes kept = *files::ReadFile(copy);
	CHECK(fs::remove(copy) && fs::create_directory(copy));
	int listed = 0;
	folders.own.index.Refresh([&] { return files::ScanArchive(folders.archive, [&](const fs::path&, const files::ArchiveFile&) { listed++; }); });
	CHECK(folders.own.index.Current()->complete && listed == 0 && folders.own.index.Candidates(Crc32(replay.data(), replay.size())).size() == 1);
	// The same replay under a later save time, so another name.
	Bytes later = kept;
	WriteU32(later.data() + 8 + 13, ReadU32(later.data() + 8 + 13) + 600);
	files::ArchiveView view(folders.own.index);
	CHECK(files::Enqueue(folders.Target(folders.own, view), replay, later));
	auto pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.waiting == 1 && folders.Outboxed() == 1);
	CHECK(fs::remove(copy));
	Save(copy, kept);
	pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.waiting == 0 && folders.Outboxed() == 0);
	CHECK(folders.Archived().size() == 1);
}

// A scan of the archive says whether it missed anything: a replay file that
// cannot be read and gives no CRC to keep it under makes it incomplete; an
// Ember file that cannot be read is kept under its name's CRC; no archive yet
// is complete.
static void TestAScanSaysWhatItMissed() {
	Folders folders("ember-archive-scan");
	const auto none = [](const fs::path&, const files::ArchiveFile&) {};
	auto scan = files::ScanArchive(folders.archive, none);
	CHECK(scan.files.empty() && scan.complete);
	folders.SaveMatch(kFirst, Match(1, 1791600000u), 0, false);
	CHECK(folders.Pass().copied == 1);
	scan = files::ScanArchive(folders.archive, none);
	CHECK(scan.files.size() == 1 && scan.complete);
	CHECK(fs::create_directories(folders.archive / "saver" / "20260101.usf4replay")); // a replay file that cannot be read
	CHECK(!files::ScanArchive(folders.archive, none).complete);
}

// While the archive is not listed whole, a pass still puts each slot's replay
// into the outbox as the slot comes round, and archives nothing: it cannot be
// sure the archive does not hold them. Once it is listed whole, both go in.
static void TestAnIncompleteIndexStillKeepsTheSlots() {
	Folders folders("ember-archive-incomplete");
	folders.own.index.Refresh([] { return files::ArchiveCandidates::Snapshot{}; }); // a refresh that missed something
	const Bytes first = Match(1, 1791600000u), second = Match(2, 1791600200u);
	folders.SaveMatch(kFirst, first, 0, false);
	auto pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.waiting == 1 && folders.Outboxed() == 1);
	folders.SaveMatch(kFirst, second, 0, false);
	pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.waiting == 2 && folders.Archived().empty());
	folders.Refresh(folders.own);
	pass = folders.Pass();
	CHECK(pass.copied == 2 && pass.waiting == 0 && folders.Bodies() == std::set<Bytes>({first, second}));
}

// A pass takes one generation of the archive's index and keeps to it: a
// refresh that lands while it runs, here one that missed the archive's copy,
// changes nothing for it, so it neither counts the copy as unknown nor puts
// the replay in the outbox again. The next pass takes the new generation.
static void TestAPassKeepsToOneGeneration() {
	Folders folders("ember-archive-generation");
	folders.SaveMatch(kFirst, Match(1, 1791600000u), 0, false);
	CHECK(folders.Pass().copied == 1);
	files::ArchiveView view(folders.own.index);
	folders.own.index.Refresh([] { return files::ArchiveCandidates::Snapshot{}; });
	auto pass = files::RunPass(folders.Target(folders.own, view), {folders.saves}, kFirst, kLast);
	CHECK(pass.copied == 0 && pass.waiting == 0 && folders.Outboxed() == 0);
	pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.waiting == 1 && folders.Outboxed() == 1);
	folders.Refresh(folders.own);
	pass = folders.Pass();
	CHECK(pass.waiting == 0 && folders.Outboxed() == 0 && folders.Archived().size() == 1);
}

// A backlog in the outbox is settled a few entries a pass, after the slots
// are read: a match saved while the backlog waits is in the outbox at the end
// of the next pass, and passes go on until everything is archived.
static void TestABacklogWaitsBehindTheSlots() {
	Folders folders("ember-archive-backlog");
	folders.archiveBroken = true;
	folders.settleBudget = 0;
	std::set<Bytes> all;
	for (int match = 0; match < 12; match++) {
		const Bytes replay = Match(match, 1791600000u + match * 180);
		all.insert(replay);
		folders.SaveMatch(kFirst + match % 10, replay, 0, false);
		folders.Pass();
	}
	folders.archiveBroken = false;
	folders.settleBudget = 3;
	const Bytes latest = Match(20, 1791700000u);
	all.insert(latest);
	folders.SaveMatch(kFirst, latest, 0, false);
	auto pass = folders.Pass();
	CHECK(pass.copied == 3 && pass.more && pass.waiting == 10);
	CHECK(folders.Outboxed() + folders.Archived().size() == 13);
	for (int passes = 0; passes < 10 && pass.waiting; passes++) pass = folders.Pass();
	CHECK(pass.waiting == 0 && !pass.more && folders.Bodies() == all);
}

// An archive file that may hold the slot's replay but cannot be read is not
// taken as a copy, and the replay is not copied again beside it: it waits in
// the outbox until the file can be read.
static void TestAnUnreadableCopy() {
	Folders folders("ember-archive-unreadable");
	folders.SaveMatch(kFirst, Match(3, 1791600000u), 0, false);
	CHECK(folders.Pass().copied == 1);
	const fs::path copy = folders.Archived().at(0);
	CHECK(fs::remove(copy) && fs::create_directory(copy)); // there, but no file to read
	const auto pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.waiting == 1 && folders.Outboxed() == 1 && folders.Archived().empty());
	CHECK(fs::remove(copy));
	CHECK(folders.Pass().copied == 1 && folders.Archived().size() == 1 && folders.Outboxed() == 0);
}

// A replay put back into the game (Watch) is saved under a new time in
// another slot. The archive holds it already, so it is not copied again, and
// an import's backup of that slot is the copy there.
static void TestAReplayPutBackAndItsBackup() {
	Folders folders("ember-archive-back");
	const Bytes replay = Match(7, 1791600000u);
	folders.SaveMatch(kFirst, replay, 1791600000u, true);
	CHECK(folders.Pass().copied == 1);
	Bytes exported, list = folders.list, swan = folders.swan, body;
	CHECK(Export(list, swan, kFirst, replay, exported));
	CHECK(Import(exported, kFirst + 5, 1791700000u, list, swan, body));
	folders.list = list; folders.swan = swan; folders.WriteIndexes();
	Save(folders.saves / std::to_wstring(kFirst + 5), body);
	Save(folders.saves / (std::to_wstring(kFirst + 5) + L".0"), Sidecar(body));
	const auto pass = folders.Pass();
	CHECK(pass.copied == 0 && pass.failed == 0 && !pass.retry);
	CHECK(folders.Archived().size() == 1);
	// A pass ran before the import's backup: the backup is that copy.
	files::BackupEvidence evidence;
	files::ArchiveView view(folders.own.index);
	CHECK(files::EnsureBackup(folders.Target(folders.own, view), folders.saves, kFirst + 5, body, evidence) && !evidence.path.empty());
	// No pass ran: the backup makes the copy, even of a replay the index is behind.
	const Bytes other = Match(8, 1791600300u);
	folders.SaveMatch(kFirst + 2, other, 0, false);
	files::ArchiveView later(folders.own.index);
	CHECK(files::EnsureBackup(folders.Target(folders.own, later), folders.saves, kFirst + 2, other, evidence));
	CHECK(folders.Archived().size() == 2);
}

// Waits up to two seconds for `done`.
template<class Done> static bool Eventually(const Done& done) {
	for (int waited = 0; !done() && waited < 2000; waited += 2) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	return done();
}
// Waits for the count to reach `least`, then a little longer to see that it
// stays there.
static int Settled(const std::atomic<int>& passes, int least) {
	Eventually([&] { return passes >= least; });
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	return passes;
}

// The worker's schedule. A retry is followed by a run of passes without
// being asked again, and a run ends; asked again, another run follows. After
// a match ends passes run until its window closes, whatever they find, and
// then stop.
static void TestArchiveWorkerSchedule() {
	{
		std::atomic<int> passes{0};
		files::ArchiveWorker worker([&] { ++passes; files::ArchivePass pass; pass.retry = true; return pass; },
			{std::chrono::milliseconds(1), std::chrono::milliseconds(4), 3, {}, {}});
		worker.Want();
		CHECK(Settled(passes, 4) == 4);
		worker.Want();
		CHECK(Settled(passes, 8) == 8);
	}
	{
		std::atomic<int> passes{0};
		files::ArchiveWorker worker([&] { ++passes; files::ArchivePass pass; pass.copied = 1; return pass; },
			{std::chrono::milliseconds(1), std::chrono::milliseconds(4), 3, std::chrono::milliseconds(60), {}});
		worker.Want();
		CHECK(Settled(passes, 1) == 1);
		worker.MatchEnded();
		const int afterWindow = Settled(passes, 6);
		CHECK(afterWindow >= 6 && Settled(passes, afterWindow) == afterWindow);
	}
}

// The worker driving real passes. A match ends; the first pass copies an
// older replay, the save folder changes twenty times, each change asking for
// a pass that finds nothing new, and then the match's own replay is saved
// with no further ask: the window after the match end archives it. A copy
// that cannot be read is looked at again until it can, without a change to
// the save folder.
static void TestArchiveWorkerWithPasses() {
	// This fixture waits for twenty real filesystem passes before saving the
	// second replay. Leave enough window for disk contention; bounded expiry
	// is checked separately by TestArchiveWorkerSchedule without filesystem IO.
	const files::ArchiveWorker::Schedule schedule{std::chrono::milliseconds(10), std::chrono::milliseconds(40), 3, std::chrono::seconds(10), std::chrono::seconds(1)};
	Folders folders("ember-archive-worker");
	folders.SaveMatch(kFirst, Match(1, 1791600000u), 1791600000u, true);
	std::atomic<int> passes{0};
	files::ArchiveWorker worker([&] { const auto pass = folders.Pass(); ++passes; return pass; }, schedule);
	worker.MatchEnded();
	CHECK(Eventually([&] { return passes >= 1; }));
	for (int change = 0; change < 20; change++) {
		const int before = passes;
		worker.Want();
		CHECK(Eventually([&] { return passes > before; }));
	}
	folders.SaveMatch(kFirst + 1, Match(2, 1791600200u), 0, false);
	CHECK(Eventually([&] { return folders.Archived().size() == 2; }));
	worker.Stop();

	Folders unreadable("ember-archive-worker-unreadable");
	unreadable.SaveMatch(kFirst, Match(3, 1791600000u), 0, false);
	CHECK(unreadable.Pass().copied == 1);
	const fs::path copy = unreadable.Archived().at(0);
	CHECK(fs::remove(copy) && fs::create_directory(copy));
	std::atomic<int> looks{0};
	files::ArchiveWorker again([&] { const auto pass = unreadable.Pass(); ++looks; return pass; },
		{std::chrono::milliseconds(5), std::chrono::milliseconds(40), 8, {}, std::chrono::milliseconds(20)});
	again.Want();
	CHECK(Eventually([&] { return looks >= 2; }));
	CHECK(fs::remove(copy));
	CHECK(Eventually([&] { return unreadable.Archived().size() == 1; }));
	again.Stop();

	// The archive cannot be written past every retry and match window: the
	// outbox keeps the worker coming back until it can, unasked, though no
	// account is signed in any more.
	Folders broken("ember-archive-worker-broken");
	broken.archiveBroken = true;
	broken.SaveMatch(kFirst, Match(4, 1791600000u), 0, false);
	CHECK(broken.Pass().waiting == 1);
	std::atomic<int> tries{0};
	files::ArchiveWorker steady([&] { const auto pass = broken.Pass(broken.own, {}); ++tries; return pass; },
		{std::chrono::milliseconds(1), std::chrono::milliseconds(2), 2, std::chrono::milliseconds(5), std::chrono::milliseconds(20)});
	steady.MatchEnded();
	CHECK(Eventually([&] { return tries >= 12; }));
	broken.archiveBroken = false;
	CHECK(Eventually([&] { return broken.Archived().size() == 1 && broken.Outboxed() == 0; }));
	steady.Stop();

	// An outbox that cannot be listed keeps the worker coming back too, until
	// it can be.
	Folders unlisted("ember-archive-worker-unlisted");
	unlisted.archiveBroken = true;
	unlisted.SaveMatch(kFirst, Match(5, 1791600000u), 0, false);
	CHECK(unlisted.Pass().waiting == 1);
	fs::rename(unlisted.outbox, unlisted.root / "away");
	Save(unlisted.outbox, Bytes(8, 1));
	unlisted.archiveBroken = false;
	std::atomic<int> lists{0};
	// Held while the outbox is put back, so no pass sees it half done.
	std::mutex restoring;
	files::ArchiveWorker listing([&] {
		std::lock_guard<std::mutex> lock(restoring);
		const auto pass = unlisted.Pass(unlisted.own, {}); ++lists; return pass;
	}, {std::chrono::milliseconds(1), std::chrono::milliseconds(2), 2, {}, std::chrono::milliseconds(20)});
	listing.Want();
	CHECK(Eventually([&] { return lists >= 4; }));
	{
		std::lock_guard<std::mutex> lock(restoring);
		fs::remove(unlisted.outbox);
		fs::rename(unlisted.root / "away", unlisted.outbox);
	}
	CHECK(Eventually([&] { return unlisted.Archived().size() == 1 && unlisted.Outboxed() == 0; }));
}

int main() {
	TestASittingWithTheIndexBehind();
	TestRetries();
	TestTheOutboxOutlivesTheSlot();
	TestAnUnreadableCandidateKeepsTheReplay();
	TestManyReplaysAllWait();
	TestTheOutboxOutlivesTheProcess();
	TestAnOutboxThatCannotBeListed();
	TestNineReplaysWithOneCrc();
	TestTheIndexIsNeverTaken();
	TestARefreshKeepsAnUnreadableCopy();
	TestAScanSaysWhatItMissed();
	TestAnIncompleteIndexStillKeepsTheSlots();
	TestAPassKeepsToOneGeneration();
	TestABacklogWaitsBehindTheSlots();
	TestTheLauncherSettlesEverything();
	TestAnUnreadableCopy();
	TestAReplayPutBackAndItsBackup();
	TestArchiveWorkerSchedule();
	TestArchiveWorkerWithPasses();
	return 0;
}
