// The replay channel's two portable halves: the queue of the player's games
// waiting to go (common/ReplayChannelQueue.hxx), and the runtime's requests
// (netplay/ReplayChannel.hxx): uploads only once the choice is read as on,
// waits after failures, final failures, pages that append, and a change of
// service forgetting what was asked.
#include "../common/OrderedWorker.hxx"
#include "../common/ReplayChannelQueue.hxx"
#include "../netplay/ReplayChannel.hxx"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>

using namespace sf4e;
using netplay::channel::Requests;
using netplay::channel::Sharing;
using Kind = Requests::Kind;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

static replaychannel::Game AGame(std::uint64_t generation) {
	replaychannel::Game game;
	game.emberId = "emb1_player1";
	game.roomId = "0123456789abcdef0123456789abcdef";
	game.table = 1; game.generation = generation; game.side = 0; game.name = "Player 1"; game.country = "GB";
	return game;
}

// A replay header the export can be made from: fighters 39 and 23, played at `time`.
static replayslots::Bytes Body(std::uint32_t time, std::uint8_t mark) {
	replayslots::Bytes body(0x400, 0);
	std::memcpy(body.data(), "#BRP", 4);
	const std::uint64_t filetime = static_cast<std::uint64_t>(time) * 10000000ull + 116444736000000000ull;
	replayslots::WriteU32(body.data() + 0x10, static_cast<std::uint32_t>(filetime));
	replayslots::WriteU32(body.data() + 0x14, static_cast<std::uint32_t>(filetime >> 32));
	replayslots::WriteU32(body.data() + 0x20, 39);
	replayslots::WriteU32(body.data() + 0x170, 23);
	body.back() = mark;
	return body;
}

static bool Write(const std::filesystem::path& path, const replayslots::Bytes& bytes) {
	std::error_code error;
	if (std::filesystem::exists(path, error)) return false;
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out);
}

static void Queue() {
	const auto folder = std::filesystem::temp_directory_path() / "sf4e-replay-channel-test";
	std::error_code ignored;
	std::filesystem::remove_all(folder, ignored);
	CHECK(replaychannel::Enqueue(folder, Body(1761393600, 1), AGame(3), 100, Write) == replaychannel::Enqueued::Queued);
	CHECK(replaychannel::Enqueue(folder, Body(1761393700, 2), AGame(4), 50, Write) == replaychannel::Enqueued::Queued);
	// The same body again keeps the pair it has.
	CHECK(replaychannel::Enqueue(folder, Body(1761393600, 1), AGame(9), 200, Write) == replaychannel::Enqueued::Queued);
	auto pending = replaychannel::Pending(folder, 300);
	CHECK(pending.size() == 2);
	CHECK(pending.size() == 2 && pending[0].value.generation == 4 && pending[1].value.generation == 3);
	CHECK(pending.size() == 2 && pending[1].value.name == "Player 1" && pending[1].value.country == "GB" && pending[1].value.emberId == "emb1_player1");
	// What the helper uploads is an export the game can import.
	if (!pending.empty()) {
		std::ifstream in(pending[0].replay, std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		CHECK(bytes.size() == replayslots::kExportHeaderBytes + 0x400 && bytes.compare(0, 8, "EMBRPLY1") == 0);
	}
	// An invalid game, or a body that is no replay, is never queued.
	auto noRoom = AGame(5); noRoom.roomId = std::string(32, '0');
	auto twoLines = AGame(5); twoLines.name = "Player\n1";
	CHECK(replaychannel::Enqueue(folder, Body(1761393800, 3), noRoom, 100, Write) == replaychannel::Enqueued::Refused);
	CHECK(replaychannel::Enqueue(folder, Body(1761393800, 3), twoLines, 100, Write) == replaychannel::Enqueued::Refused);
	CHECK(replaychannel::Enqueue(folder, replayslots::Bytes(16, 0), AGame(5), 100, Write) == replaychannel::Enqueued::Refused);
	// A week later the older pair is gone, and Remove takes both files.
	pending = replaychannel::Pending(folder, 50 + replaychannel::kQueueSeconds + 1);
	CHECK(pending.size() == 1 && pending[0].value.generation == 3);
	if (!pending.empty()) {
		replaychannel::Remove(pending[0]);
		CHECK(!std::filesystem::exists(pending[0].replay) && !std::filesystem::exists(pending[0].game));
	}
	// A damaged .game is dropped with its export.
	CHECK(replaychannel::Enqueue(folder, Body(1761393900, 4), AGame(6), 100, Write) == replaychannel::Enqueued::Queued);
	for (const auto& entry : replaychannel::Pending(folder, 100)) { std::ofstream(entry.game, std::ios::trunc) << "x"; }
	CHECK(replaychannel::Pending(folder, 100).empty());
	// Stopping sharing clears that player's games, never another Ember ID's.
	auto other = AGame(8); other.emberId = "emb1_player2";
	CHECK(replaychannel::Enqueue(folder, Body(1761394000, 5), AGame(7), 100, Write) == replaychannel::Enqueued::Queued);
	CHECK(replaychannel::Enqueue(folder, Body(1761394100, 6), other, 100, Write) == replaychannel::Enqueued::Queued);
	replaychannel::Clear(folder, "emb1_player1");
	pending = replaychannel::Pending(folder, 100);
	CHECK(pending.size() == 1 && pending[0].value.emberId == "emb1_player2");
	auto noId = AGame(9); noId.emberId.clear();
	CHECK(replaychannel::Enqueue(folder, Body(1761394200, 7), noId, 100, Write) == replaychannel::Enqueued::Refused);
	std::filesystem::remove_all(folder, ignored);
}

static replaychannel::Entry Queued(const char* name, const char* emberId, std::uint64_t queuedAt) {
	replaychannel::Entry entry;
	entry.game = name; entry.replay = name; entry.value = AGame(queuedAt); entry.value.emberId = emberId; entry.queuedAt = queuedAt;
	return entry;
}

// One look as the runtime takes it: the scope first, a listing when one is
// due (made at once here, as the queue's worker would make it), then the next pair.
static const replaychannel::Entry* Look(replaychannel::Uploads& uploads, const std::string& scope, const std::string& emberId,
	std::uint64_t nowMs, std::uint64_t unixNow, const std::function<std::vector<replaychannel::Entry>()>& read) {
	uploads.Rescope(scope);
	if (uploads.ReadDue(nowMs)) { uploads.Asked(); uploads.Read(read(), nowMs); }
	return uploads.Next(emberId, unixNow);
}

// The uploads' one owner: a change of service or Ember ID forgets the pairs
// read and the one on its way together; the folder is read again while the
// first keeps failing, so a new pair is seen and one past its week goes.
static void Uploads() {
	using replaychannel::Uploads;
	std::vector<replaychannel::Entry> folder{Queued("a1", "emb1_a", 100), Queued("b1", "emb1_b", 110), Queued("a2", "emb1_a", 120)};
	int reads = 0;
	const auto read = [&] { ++reads; return folder; };
	Uploads uploads;
	const auto* next = Look(uploads, "brg_1\nemb1_a", "emb1_a", 0, 200, read);
	CHECK(next && next->game == "a1" && reads == 1 && uploads.Size() == 2);
	uploads.Sent(*next);
	// Another Ember ID: what was read and the one on its way are forgotten,
	// and only that Ember ID's pairs go.
	next = Look(uploads, "brg_1\nemb1_b", "emb1_b", 20, 200, read);
	CHECK(next && next->game == "b1" && reads == 2 && uploads.Size() == 1);
	CHECK(!uploads.Answered(true));
	// Back to the first: a failure keeps the pair; a new one is seen on the next read.
	next = Look(uploads, "brg_1\nemb1_a", "emb1_a", 30, 200, read);
	uploads.Sent(*next);
	CHECK(!uploads.Answered(false) && uploads.Size() == 2);
	folder.push_back(Queued("a3", "emb1_a", 130));
	next = Look(uploads, "brg_1\nemb1_a", "emb1_a", 30 + Uploads::kCheckMs - 1, 200, read);
	CHECK(next && next->game == "a1" && uploads.Size() == 2);
	next = Look(uploads, "brg_1\nemb1_a", "emb1_a", 30 + Uploads::kCheckMs, 200, read);
	CHECK(next && uploads.Size() == 3);
	// Past its week, the first pair never goes, even from what was read.
	next = Look(uploads, "brg_1\nemb1_a", "emb1_a", 30 + Uploads::kCheckMs, 100 + replaychannel::kQueueSeconds + 1, read);
	CHECK(next && next->game == "a2");
	// Sent: out of the queue, and answered as the pair whose files go.
	uploads.Sent(*next);
	const auto done = uploads.Answered(true);
	CHECK(done && done->game == "a2" && uploads.Size() == 1);
}

// A listing asked for before an upload finished is not taken: it may still
// hold the pair just sent, whose removal comes after it.
static void StaleListing() {
	replaychannel::Uploads uploads;
	uploads.Rescope("brg_1\nemb1_a");
	uploads.Asked();
	uploads.Read({Queued("a1", "emb1_a", 100), Queued("a2", "emb1_a", 110)}, 0);
	CHECK(!uploads.ReadDue(1));
	const auto* next = uploads.Next("emb1_a", 200);
	uploads.Sent(*next);
	// A listing is due and asked for while the upload is on its way...
	CHECK(uploads.ReadDue(replaychannel::Uploads::kCheckMs));
	uploads.Asked();
	// ...the upload finishes, then the old listing arrives with a1 in it.
	const auto done = uploads.Answered(true);
	CHECK(done && done->game == "a1");
	uploads.Read({Queued("a1", "emb1_a", 100), Queued("a2", "emb1_a", 110)}, replaychannel::Uploads::kCheckMs + 5);
	next = uploads.Next("emb1_a", 200);
	CHECK(next && next->game == "a2" && uploads.ReadDue(replaychannel::Uploads::kCheckMs + 5));
	uploads.Asked();
	uploads.Read({Queued("a2", "emb1_a", 110)}, replaychannel::Uploads::kCheckMs + 6);
	CHECK(uploads.Size() == 1 && uploads.Next("emb1_a", 200)->game == "a2");
}

// A choice read while an upload is on its way lists the folder again but
// keeps the pair: its answer still takes it out, once.
static void RelistKeepsTheUpload() {
	replaychannel::Uploads uploads;
	uploads.Rescope("brg_1\nemb1_a");
	uploads.Asked();
	uploads.Read({Queued("a1", "emb1_a", 100), Queued("a2", "emb1_a", 110)}, 0);
	uploads.Sent(*uploads.Next("emb1_a", 200));
	uploads.Relist();
	CHECK(uploads.ReadDue(1));
	const auto done = uploads.Answered(true);
	CHECK(done && done->game == "a1" && !uploads.Answered(true));
	uploads.Asked();
	uploads.Read({Queued("a2", "emb1_a", 110)}, 2);
	CHECK(uploads.Size() == 1 && uploads.Next("emb1_a", 200)->game == "a2");
	// A listing asked before the relist is set aside.
	uploads.Asked();
	uploads.Relist();
	uploads.Read({Queued("a1", "emb1_a", 100)}, 3);
	CHECK(uploads.Size() == 1 && uploads.Next("emb1_a", 200)->game == "a2" && uploads.ReadDue(3));
}

// Sharing went off: the pairs the runtime held go at once, so none of the
// cleared ones is sent before a listing made after the clear; the pair on
// its way keeps its answer.
static void DiscardOnOff() {
	replaychannel::Uploads uploads;
	uploads.Rescope("brg_1\nemb1_a");
	uploads.Asked();
	uploads.Read({Queued("a1", "emb1_a", 100), Queued("a2", "emb1_a", 110)}, 0);
	uploads.Sent(*uploads.Next("emb1_a", 200));
	uploads.Asked();
	uploads.Discard();
	CHECK(uploads.Size() == 0 && !uploads.Next("emb1_a", 200));
	// A listing asked before the clear is set aside.
	uploads.Read({Queued("a2", "emb1_a", 110)}, 1);
	CHECK(!uploads.Next("emb1_a", 200) && uploads.ReadDue(1));
	const auto done = uploads.Answered(true);
	CHECK(done && done->game == "a1");
	uploads.Asked();
	uploads.Read({}, 2);
	CHECK(!uploads.Next("emb1_a", 200));
}

// The queue's worker: answers in order; a job that throws answers `failed`;
// at Stop a job not started is dropped and its waiting caller is answered
// `failed` at once, even while another job is still running.
static void Worker() {
	{
		OrderedWorker worker;
		CHECK(worker.Ask<int>([] { return 7; }, -1) == 7);
		CHECK(worker.Ask<int>([]() -> int { throw 1; }, -1) == -1);
	}
	OrderedWorker worker;
	std::mutex gate;
	std::unique_lock<std::mutex> held(gate);
	std::atomic<bool> running{false};
	worker.Post([&] { running = true; std::lock_guard<std::mutex> wait(gate); });
	while (!running) std::this_thread::yield();
	std::atomic<int> answer{0};
	std::thread asker([&] { answer = worker.Ask<int>([] { return 7; }, -1); });
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	std::thread stopper([&] { worker.Stop(); });
	for (int i = 0; i < 200 && answer == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	CHECK(answer == -1); // answered while the first job still runs
	held.unlock();
	asker.join();
	stopper.join();
	CHECK(!worker.Post([] {}) && worker.Ask<int>([] { return 7; }, -1) == -1);
}

// The request state and the uploads together: an upload the helper never
// answers ends at its time limit, waits, and the same pair goes again.
static void UploadLostAndRetried() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.SentSharing(1, 0);
	requests.FinishSharing("", Sharing::On, 0);
	std::vector<replaychannel::Entry> folder{Queued("a1", "emb1_a", 100)};
	const auto read = [&] { return folder; };
	replaychannel::Uploads uploads;
	const std::string scope = "brg_1\nemb1_a";
	auto* next = Look(uploads, scope, "emb1_a", 0, 200, read);
	CHECK(next && requests.MayUpload(0));
	requests.SentUpload(5, 0);
	uploads.Sent(*next);
	CHECK(!requests.MayUpload(1));
	requests.Expire(Requests::GiveUpMs);
	CHECK(requests.Awaiting(5) == Kind::None);
	const auto after = Requests::GiveUpMs + Requests::FirstWaitMs;
	CHECK(!requests.MayUpload(after - 1) && requests.MayUpload(after));
	next = Look(uploads, scope, "emb1_a", after, 200, read);
	CHECK(next && next->game == "a1");
	requests.SentUpload(6, after);
	uploads.Sent(*next);
	CHECK(requests.Awaiting(6) == Kind::Upload);
	const auto done = uploads.Answered(requests.FinishUpload("", after + 10));
	CHECK(done && done->game == "a1" && uploads.Size() == 0);
}

// The choice is read again by the runtime: at once for a new scope, a while
// after a failure (soon when the helper was not up), every while it says no
// account, never while it is known, and at once after a change got no answer,
// which leaves it unknown and shares nothing.
static void SharingReads() {
	Requests requests;
	CHECK(!requests.WantsSharingRead(0));
	requests.Scope("brg_1", "emb1_a");
	CHECK(requests.WantsSharingRead(0));
	requests.SentSharing(0, 0); // the helper was not up
	CHECK(!requests.WantsSharingRead(Requests::HelperRetryMs - 1) && requests.WantsSharingRead(Requests::HelperRetryMs));
	requests.SentSharing(1, 100);
	CHECK(!requests.WantsSharingRead(200));
	requests.FinishSharing("unavailable", std::nullopt, 200);
	CHECK(!requests.WantsSharingRead(200 + Requests::SharingRetryMs - 1) && requests.WantsSharingRead(200 + Requests::SharingRetryMs));
	requests.SentSharing(2, 1000000);
	requests.FinishSharing("", Sharing::NoAccount, 1000000);
	CHECK(requests.WantsSharingRead(1000000 + Requests::SharingRetryMs));
	requests.SentSharing(3, 2000000);
	requests.FinishSharing("", Sharing::On, 2000000);
	CHECK(!requests.WantsSharingRead(9000000) && requests.SharesNow());
	// Off goes out and its answer is lost: the choice is unknown, nothing is
	// shared, and it is read again at once.
	requests.SentChoice(7, 4, 9000000);
	CHECK(!requests.SharesNow());
	requests.Expire(9000000 + Requests::GiveUpMs);
	CHECK(!requests.SharesNow() && !requests.Published(0).sharing.loaded && requests.WantsSharingRead(9000000 + Requests::GiveUpMs));
	// One the helper never took changed nothing.
	requests.SentSharing(5, 9100000);
	requests.FinishSharing("", Sharing::On, 9100000);
	requests.SentChoice(8, 0, 9100000);
	CHECK(requests.SharesNow() && !requests.WantsSharingRead(9100000));
}

// Nothing goes before the choice is read as on; off stops it.
static void UploadsWaitForTheChoice() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	CHECK(!requests.MayUpload(0));
	requests.SentSharing(11, 0);
	CHECK(requests.Awaiting(11) == Kind::Sharing && !requests.MayReadSharing());
	requests.FinishSharing("", Sharing::Off, 10);
	CHECK(!requests.MayUpload(10));
	requests.SentChoice(1, 12, 20);
	// A choice on its way holds back uploads and new games until it is answered.
	CHECK(!requests.SharesNow() && !requests.MayUpload(20));
	requests.FinishChoice("", Sharing::On, 30);
	CHECK(requests.SharesNow());
	CHECK(requests.Published(30).chose == 1 && requests.MayUpload(30));
	requests.SentUpload(13, 30);
	CHECK(requests.Awaiting(13) == Kind::Upload && !requests.MayUpload(30));
	CHECK(requests.FinishUpload("", 40));
	CHECK(requests.Published(40).uploaded == 1 && requests.MayUpload(40));
}

// A failure that may pass waits longer each time; one that never will drops the replay.
static void UploadFailures() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.SentSharing(1, 0);
	requests.FinishSharing("", Sharing::On, 0);
	requests.SentUpload(2, 0);
	CHECK(!requests.FinishUpload("unavailable", 0));
	CHECK(!requests.MayUpload(Requests::FirstWaitMs - 1) && requests.MayUpload(Requests::FirstWaitMs));
	requests.SentUpload(3, Requests::FirstWaitMs);
	CHECK(!requests.FinishUpload("unavailable", Requests::FirstWaitMs));
	CHECK(!requests.MayUpload(3 * Requests::FirstWaitMs - 1) && requests.MayUpload(3 * Requests::FirstWaitMs));
	requests.SentUpload(4, 0);
	CHECK(requests.FinishUpload("replay_not_your_game", 0));
	requests.SentUpload(5, 0);
	CHECK(!requests.FinishUpload("replay_channel_unavailable", 0));
	CHECK(!requests.MayUpload(Requests::UnavailableWaitMs - 1));
	// A request the helper never answers ends after its limit, and is tried again.
	Requests lost;
	lost.Scope("brg_1", "emb1_a");
	lost.SentSharing(1, 0);
	lost.FinishSharing("", Sharing::On, 0);
	lost.SentUpload(9, 0);
	lost.Expire(Requests::GiveUpMs);
	CHECK(lost.Awaiting(9) == Kind::None);
	// No account: the choice is read again before anything goes.
	Requests none;
	none.Scope("brg_1", "emb1_a");
	none.SentSharing(1, 0);
	none.FinishSharing("", Sharing::On, 0);
	none.SentUpload(2, 0);
	CHECK(!none.FinishUpload("no_discord_account", 0));
	CHECK(!none.MayUpload(Requests::UnavailableWaitMs) && !none.Published(0).sharing.loaded);
}

static netplay::channel::Entry Entry(char id, std::uint64_t playedAt) {
	netplay::channel::Entry entry;
	entry.id = std::string(32, id); entry.playedAt = playedAt;
	return entry;
}

// A further page appends to the one listed for the same fighter; another
// fighter's first page replaces it.
static void Pages() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	netplay::channel::Page first;
	first.entries = {Entry('b', 20), Entry('a', 10)}; first.more = true;
	requests.SentPage(1, -1, false, 0);
	requests.FinishPage("", first, 0);
	netplay::channel::Page second;
	second.entries = {Entry('a', 10), Entry('9', 5)};
	requests.SentPage(2, -1, true, 0);
	requests.FinishPage("", second, 0);
	auto status = requests.Published(0);
	CHECK(status.page.value->entries.size() == 3 && !status.page.value->more && status.fighter == -1);
	netplay::channel::Page ryu;
	ryu.entries = {Entry('c', 30)};
	requests.SentPage(3, 1, true, 0);
	requests.FinishPage("", ryu, 0);
	status = requests.Published(0);
	CHECK(status.page.value->entries.size() == 1 && status.fighter == 1);
	// A failed read keeps the page and says why.
	requests.SentPage(4, 1, false, 0);
	requests.FinishPage("unavailable", std::nullopt, 0);
	status = requests.Published(0);
	CHECK(status.page.loaded && status.page.error == "unavailable" && status.page.value->entries.size() == 1);
}

// A change of service or Ember ID answers what was on its way as unavailable and forgets the rest.
static void Scope() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.SentSharing(1, 0);
	requests.FinishSharing("", Sharing::On, 0);
	requests.SentDownload(7, 2, 0);
	requests.SentChoice(8, 3, 0);
	requests.Scope("brg_1", "emb1_b");
	auto status = requests.Published(0);
	CHECK(status.downloaded == 7 && status.downloadFailure == "unavailable");
	CHECK(status.chose == 8 && status.choiceFailure == "unavailable");
	CHECK(!status.sharing.loaded && !requests.MayUpload(0) && requests.Awaiting(2) == Kind::None);
	requests.SentDownload(9, 4, 0);
	requests.FinishDownload("", "C:\\channel\\x.emberreplay", 0);
	status = requests.Published(0);
	CHECK(status.downloaded == 9 && status.downloadFailure.empty() && status.downloadedFile == "C:\\channel\\x.emberreplay");
}

int main() {
	Queue();
	Uploads();
	UploadLostAndRetried();
	StaleListing();
	RelistKeepsTheUpload();
	DiscardOnOff();
	Worker();
	SharingReads();
	UploadsWaitForTheChoice();
	UploadFailures();
	Pages();
	Scope();
	if (failures) { std::printf("%d failure(s)\n", failures); return EXIT_FAILURE; }
	std::printf("replay channel: all checks passed\n");
	return EXIT_SUCCESS;
}
