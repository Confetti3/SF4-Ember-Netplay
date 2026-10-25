// The replay channel's two portable halves: the queue of the player's games
// waiting to go (common/ReplayChannelQueue.hxx), and the runtime's requests
// (netplay/ReplayChannel.hxx): uploads only once the choice is read as on,
// waits after failures, final failures, pages that append, and a change of
// service forgetting what was asked.
#include "../common/ReplayChannelQueue.hxx"
#include "../netplay/ReplayChannel.hxx"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
	CHECK(replaychannel::Enqueue(folder, Body(1761393600, 1), AGame(3), 100, Write));
	CHECK(replaychannel::Enqueue(folder, Body(1761393700, 2), AGame(4), 50, Write));
	// The same body again keeps the pair it has.
	CHECK(replaychannel::Enqueue(folder, Body(1761393600, 1), AGame(9), 200, Write));
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
	CHECK(!replaychannel::Enqueue(folder, Body(1761393800, 3), noRoom, 100, Write));
	CHECK(!replaychannel::Enqueue(folder, Body(1761393800, 3), twoLines, 100, Write));
	CHECK(!replaychannel::Enqueue(folder, replayslots::Bytes(16, 0), AGame(5), 100, Write));
	// A week later the older pair is gone, and Remove takes both files.
	pending = replaychannel::Pending(folder, 50 + replaychannel::kQueueSeconds + 1);
	CHECK(pending.size() == 1 && pending[0].value.generation == 3);
	if (!pending.empty()) {
		replaychannel::Remove(pending[0]);
		CHECK(!std::filesystem::exists(pending[0].replay) && !std::filesystem::exists(pending[0].game));
	}
	// A damaged .game is dropped with its export.
	CHECK(replaychannel::Enqueue(folder, Body(1761393900, 4), AGame(6), 100, Write));
	for (const auto& entry : replaychannel::Pending(folder, 100)) { std::ofstream(entry.game, std::ios::trunc) << "x"; }
	CHECK(replaychannel::Pending(folder, 100).empty());
	// Stopping sharing clears that player's games, never another Ember ID's.
	auto other = AGame(8); other.emberId = "emb1_player2";
	CHECK(replaychannel::Enqueue(folder, Body(1761394000, 5), AGame(7), 100, Write));
	CHECK(replaychannel::Enqueue(folder, Body(1761394100, 6), other, 100, Write));
	replaychannel::Clear(folder, "emb1_player1");
	pending = replaychannel::Pending(folder, 100);
	CHECK(pending.size() == 1 && pending[0].value.emberId == "emb1_player2");
	auto noId = AGame(9); noId.emberId.clear();
	CHECK(!replaychannel::Enqueue(folder, Body(1761394200, 7), noId, 100, Write));
	std::filesystem::remove_all(folder, ignored);
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
	UploadsWaitForTheChoice();
	UploadFailures();
	Pages();
	Scope();
	if (failures) { std::printf("%d failure(s)\n", failures); return EXIT_FAILURE; }
	std::printf("replay channel: all checks passed\n");
	return EXIT_SUCCESS;
}
