// The replay channel from Replays: the sharing choice read as soon as a
// service is usable, the channel read on opening, Share my room games on and
// off, the fighter filter, Load more, Options, a game downloaded and played
// at the main menu and only downloaded in a room, Connect Discord without an
// account, a service without the channel, and no service at all.
#include "public_rooms_journey_support.hxx"
#include "../common/FighterCatalog.hxx"
#include "../ui/SignInSites.hxx"
namespace {
using netplay::channel::Entry;
using netplay::channel::Page;
using netplay::channel::Sharing;

std::vector<const Command*> Sent(const Journey& j, Op op) {
 std::vector<const Command*> out;
 for (const auto& a : j.h.actions) if (a.tournament.op == op) out.push_back(&a.tournament);
 return out;
}

// The runtime has answered the newest read of the choice.
void AnswerSharing(Journey& j, Sharing sharing, const char* error = "") {
 auto& status = j.h.view.replayChannel;
 status.bridge = "brg_1";
 if (!*error) { status.sharing.value = sharing; status.sharing.loaded = true; ++status.sharing.revision; }
 status.sharing.loading = false; status.sharing.error = error; ++status.sharing.finished;
 j.h.Frame(0, 2);
}

Entry Game(char id, std::uint64_t playedAt, const char* first, int fighter1, const char* second, int fighter2) {
 Entry entry;
 entry.id = std::string(32, id); entry.playedAt = playedAt; entry.publicRoom = true; entry.winner = 1; entry.size = 9000;
 entry.sides[0].name = first; entry.sides[0].fighter = fighter1; entry.sides[0].country = CountryCode::Parsed("GB");
 entry.sides[0].badge = SkillBadge::OfClass(9, true);
 entry.sides[1].name = second; entry.sides[1].fighter = fighter2;
 return entry;
}

// The runtime has answered the newest page read, for `fighter`.
void AnswerPage(Journey& j, std::vector<Entry> entries, bool more, int fighter = -1, const char* error = "") {
 auto& status = j.h.view.replayChannel;
 status.bridge = "brg_1";
 if (!*error) {
  auto page = std::make_shared<Page>(); page->entries = std::move(entries); page->more = more;
  status.page.value = page; status.page.loaded = true; ++status.page.revision; status.fighter = fighter;
 }
 status.page.loading = false; status.page.error = error; ++status.page.finished;
 j.h.Frame(0, 2);
}

void AnswerChoice(Journey& j, Sharing after, const char* failure = "") {
 const auto sent = Sent(j, Op::ReplayShare);
 Check(!sent.empty(), "No sharing choice to answer");
 auto& status = j.h.view.replayChannel;
 if (!*failure) { status.sharing.value = after; ++status.sharing.revision; }
 status.chose = sent.back()->request; status.choosing = 0; status.choiceFailure = failure;
 j.h.Frame(0, 2);
}

void AnswerDownload(Journey& j, const std::string& file, const char* failure = "") {
 const auto sent = Sent(j, Op::ReplayDownload);
 Check(!sent.empty(), "No download to answer");
 auto& status = j.h.view.replayChannel;
 status.downloaded = sent.back()->request; status.downloading = 0;
 status.downloadFailure = failure; status.downloadedFile = *failure ? std::string() : file;
 j.h.Frame(0, 2);
}

std::size_t Watches(const Journey& j, const std::string& file) {
 return static_cast<std::size_t>(std::count_if(j.h.actions.begin(), j.h.actions.end(), [&](const ShellAction& a) {
  return a.replay.mode == replay::Mode::Watch && a.replay.path == file; }));
}

void Open(Journey& j) {
 j.h.view.replays.ready = true;
 j.h.Screen("replays");
 Check(j.row("replay-channel") != nullptr, "Replays has no Replay channel row");
 j.h.Choose("replay-channel"); j.h.Frame(0, 2);
 Check(j.h.shell.Navigation().Screen() == "replay-channel", "Replays does not open the Replay channel");
 j.answerIdentityUntilIdle();
}

const char* FighterNameOf(int id) { return selection::FindFighter(id)->name; }

void OpeningJourney() {
 Journey j; Open(j);
 const auto reads = Sent(j, Op::ReplaySharing);
 Check(reads.size() == 1 && reads[0]->bridgeId == "brg_1", "The sharing choice was not read once");
 const auto lists = Sent(j, Op::ReplayList);
 Check(lists.size() == 1 && lists[0]->fighter == -1 && !lists[0]->more, "Opening did not read the first page for every fighter");
 Check(j.row("rc-loading") && j.row("rc-share-reading"), "No loading rows before the answers");
 // Frames pass without answers: nothing is asked again.
 j.h.Frame(0, 30);
 Check(Sent(j, Op::ReplaySharing).size() == 1 && Sent(j, Op::ReplayList).size() == 1, "Requests were sent again every frame");
 AnswerSharing(j, Sharing::On);
 AnswerPage(j, {Game('b', 200, "Player 1", 23, "Player 2", 39), Game('a', 100, "Player 3", 1, "Player 4", 2)}, true);
 std::vector<std::string> ids;
 for (const auto& row : j.rows) ids.push_back(row.id);
 Check(ids == std::vector<std::string>{"rc-share", "rc-fighter", "rc:" + std::string(32, 'b'), "rc:" + std::string(32, 'a'), "rc-more"},
  "The channel's rows are not sharing, filter, games, Load more");
 Check(j.row("rc-share")->value == loc::T("common.on"), "Sharing does not show On");
 Check(j.row("rc-fighter")->value == loc::T("channel.all_fighters"), "The filter does not start on every fighter");
 const auto* game = j.row("rc:" + std::string(32, 'b'));
 Check(game->fighter1 == 23 && game->fighter2 == 39, "A game row does not carry both fighters for its portraits");
 Check(game->label.find("Player 1") != std::string::npos && game->label.find("Player 2") != std::string::npos, "A game row does not name both players");
 Check(game->value == loc::Tf("channel.won", "Player 2"), "A game row does not name the winner");
 Check(game->detail.find(FighterNameOf(23)) != std::string::npos && game->detail.find(ClassName(9)) != std::string::npos,
  "A game's detail does not name a fighter and class");
 Check(game->enabled, "A game row cannot be chosen at the main menu");
}

void SharingJourney() {
 Journey j; Open(j);
 AnswerSharing(j, Sharing::On);
 AnswerPage(j, {Game('b', 200, "Player 1", 23, "Player 2", 39)}, false);
 j.h.FocusOn("rc-share"); j.h.Press(MenuInput::Left);
 auto shares = Sent(j, Op::ReplayShare);
 Check(shares.size() == 1 && !shares[0]->sharing && shares[0]->bridgeId == "brg_1" && shares[0]->request, "Left did not turn sharing off");
 j.h.view.replayChannel.choosing = shares[0]->request; j.h.Frame();
 Check(!j.row("rc-share")->enabled, "Sharing can be changed again while a choice is on its way");
 AnswerChoice(j, Sharing::Off);
 Check(j.row("rc-share")->value == loc::T("common.off"), "Sharing does not show Off");
 Check(j.status == loc::T("channel.shared_off"), "Turning sharing off was not said");
 j.h.FocusOn("rc-share"); j.h.Press(MenuInput::Right);
 shares = Sent(j, Op::ReplayShare);
 Check(shares.size() == 2 && shares[1]->sharing && shares[1]->request > shares[0]->request, "Right did not turn sharing back on");
 AnswerChoice(j, Sharing::On);
 Check(j.row("rc-share")->value == loc::T("common.on"), "Sharing does not show On again");
 // A refused choice is said.
 j.h.FocusOn("rc-share"); j.h.Press(MenuInput::Left);
 AnswerChoice(j, Sharing::On, "replay_channel_unavailable");
 Check(j.status == loc::T("channel.failure.unavailable"), "A refused choice was not said");
}

void FilterJourney() {
 Journey j; Open(j);
 AnswerSharing(j, Sharing::On);
 AnswerPage(j, {Game('b', 200, "Player 1", 23, "Player 2", 39)}, true);
 const int first = selection::RosterDisplayOrder[0], second = selection::RosterDisplayOrder[1];
 j.h.FocusOn("rc-fighter"); j.h.Press(MenuInput::Right);
 auto lists = Sent(j, Op::ReplayList);
 Check(lists.size() == 2 && lists[1]->fighter == first && !lists[1]->more, "Right on the filter did not read the first fighter's games");
 Check(j.row("rc-fighter")->value == FighterNameOf(first), "The filter does not name its fighter");
 Check(j.row("rc-loading") != nullptr, "The previous fighter's games stayed listed under the new filter");
 // A second step before the first answer: once that answer comes, the page
 // for the fighter shown is read.
 j.h.view.replayChannel.page.loading = true;
 j.h.FocusOn("rc-fighter"); j.h.Press(MenuInput::Right);
 Check(j.row("rc-fighter")->value == FighterNameOf(second), "The filter did not step to the next fighter");
 AnswerPage(j, {Game('c', 300, "Player 5", first, "Player 6", 3)}, false, first);
 lists = Sent(j, Op::ReplayList);
 Check(lists.back()->fighter == second, "The page for the fighter shown was not read after the earlier one answered");
 AnswerPage(j, {Game('d', 400, "Player 7", second, "Player 8", 4)}, false, second);
 Check(j.row("rc:" + std::string(32, 'd')) && !j.row("rc:" + std::string(32, 'c')), "The fighter shown does not list its own games");
 // Back to every fighter.
 j.h.FocusOn("rc-fighter"); j.h.Press(MenuInput::Left); j.h.Press(MenuInput::Left);
 Check(j.row("rc-fighter")->value == loc::T("channel.all_fighters"), "Left did not come back to every fighter");
}

void MoreAndRefreshJourney() {
 Journey j; Open(j);
 AnswerSharing(j, Sharing::On);
 AnswerPage(j, {Game('b', 200, "Player 1", 23, "Player 2", 39)}, true);
 j.h.Choose("rc-more");
 auto lists = Sent(j, Op::ReplayList);
 Check(lists.size() == 2 && lists[1]->more && lists[1]->fighter == -1, "Load more did not ask for the next page");
 AnswerPage(j, {Game('b', 200, "Player 1", 23, "Player 2", 39), Game('a', 100, "Player 3", 1, "Player 4", 2)}, false);
 Check(!j.row("rc-more"), "Load more stayed after the last page");
 j.h.Press(MenuInput::Options);
 lists = Sent(j, Op::ReplayList);
 Check(lists.size() == 3 && !lists[2]->more, "Options did not read the first page again");
 // A failed read keeps the games and says why.
 AnswerPage(j, {}, false, -1, "unavailable");
 Check(j.row("rc:" + std::string(32, 'a')) != nullptr, "A failed refresh dropped the games");
}

void WatchJourney() {
 Journey j; Open(j);
 AnswerSharing(j, Sharing::On);
 AnswerPage(j, {Game('b', 200, "Player 1", 23, "Player 2", 39)}, false);
 const std::string row = "rc:" + std::string(32, 'b'), file = "C:\\Users\\Player 1\\AppData\\Roaming\\sf4e\\replay-channel\\x.emberreplay";
 j.h.Choose(row.c_str());
 auto downloads = Sent(j, Op::ReplayDownload);
 Check(downloads.size() == 1 && downloads[0]->replayId == std::string(32, 'b') && downloads[0]->request, "Select did not download the game");
 Check(j.status == loc::T("channel.downloading"), "The download is not shown under way");
 j.h.Choose(row.c_str());
 Check(Sent(j, Op::ReplayDownload).size() == 1, "A second download went while one was on its way");
 AnswerDownload(j, file);
 Check(Watches(j, file) == 1, "The downloaded game was not played");
 // In a room it is only downloaded, and said so.
 j.h.view.session.room = netplay::RoomState::Joined; j.h.Frame();
 j.h.Screen("replay-channel"); j.h.Frame(0, 2);
 AnswerPage(j, {Game('b', 200, "Player 1", 23, "Player 2", 39)}, false);
 Check(j.row(row)->hint == loc::T("channel.download"), "A room's row does not say it only downloads");
 j.h.Choose(row.c_str());
 AnswerDownload(j, file);
 Check(Watches(j, file) == 1 && j.status == loc::T("channel.downloaded"), "A room's download was played or not said");
 // A game no longer listed is said.
 j.h.view.session.room = netplay::RoomState::Idle; j.h.Frame();
 j.h.Choose(row.c_str());
 AnswerDownload(j, file, "replay_not_found");
 Check(j.status == loc::T("channel.failure.not_found") && Watches(j, file) == 1, "A missing game was not said");
}

void NoAccountJourney() {
 Journey j; Open(j);
 AnswerSharing(j, Sharing::NoAccount);
 AnswerPage(j, {}, false);
 Check(j.row(DiscordConnectScreen) && !j.row("rc-share"), "No Connect Discord for a player without an account");
 Check(j.row("rc-empty") != nullptr, "An empty channel has no card");
 j.h.Choose(DiscordConnectScreen);
 Check(j.h.shell.Navigation().Screen() == DiscordConnectScreen, "Connect Discord did not open");
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "replay-channel", "Back from Connect Discord did not return to the channel");
}

void UnavailableJourney() {
 Journey j; Open(j);
 AnswerSharing(j, Sharing::On, "replay_channel_unavailable");
 AnswerPage(j, {}, false, -1, "replay_channel_unavailable");
 Check(j.row("rc-unavailable") != nullptr, "A service without the channel shows no card");
}

void NoServiceJourney() {
 Journey j(false);
 j.h.view.replays.ready = true;
 j.h.Screen("replays"); j.h.Choose("replay-channel"); j.h.Frame(0, 2);
 j.answerIdentityUntilIdle();
 Check(Sent(j, Op::ReplaySharing).empty() && Sent(j, Op::ReplayList).empty(), "A request went without a usable service");
 Check(j.row("rc-needs-setup") && j.row("rc-setup"), "No way to the setup without a service");
 j.h.Choose("rc-setup");
 Check(j.h.shell.Navigation().Screen() == "public-rooms", "The setup row does not open Public rooms");
}

// The choice is read wherever the player is, since uploads wait for it.
void ReadAnywhereJourney() {
 Journey j;
 j.h.Screen("home"); j.answerIdentityUntilIdle(); j.h.Frame(0, 4);
 Check(Sent(j, Op::ReplaySharing).size() == 1 && Sent(j, Op::ReplayList).empty(), "The choice was not read away from the channel, or the channel was");
}
}

int main() {
 try {
  OpeningJourney(); SharingJourney(); FilterJourney(); MoreAndRefreshJourney(); WatchJourney();
  NoAccountJourney(); UnavailableJourney(); NoServiceJourney(); ReadAnywhereJourney();
  std::cout << "Replay channel journeys passed.\n";
  return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
