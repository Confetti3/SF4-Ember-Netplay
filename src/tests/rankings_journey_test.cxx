// Rankings from Online play: the standing read on opening, the listing sent
// once when the board lists the player otherwise, taking part or not behind a
// confirmation and the once-a-day wait, Connect Discord for a player without
// an account, a service without rankings, and the board by class.
#include "public_rooms_journey_support.hxx"
#include <imgui_internal.h>
#include "../ui/SignInSites.hxx"
namespace {
using netplay::rankings::Standing;
using netplay::rankings::BoardEntry;

std::vector<const Command*> Sent(const Journey& j, Op op) {
 std::vector<const Command*> out;
 for (const auto& a : j.h.actions) if (a.tournament.op == op) out.push_back(&a.tournament);
 return out;
}

// The runtime has the newest standing read's answer.
// Time passes on the runtime's clock, which the status carries.
void Pass(Journey& j, double seconds) {
 j.h.view.rankings.nowMs += static_cast<std::uint64_t>(seconds * 1000);
 j.h.Frame();
}

// A failure keeps the standing already there, and when it arrived, as the runtime does.
void AnswerStanding(Journey& j, Standing standing, const char* error = "") {
 auto& status = j.h.view.rankings;
 status.bridge = "brg_1";
 if (!*error) {
  status.standing.value = standing; status.standing.loaded = true; status.standing.installedMs = status.nowMs; ++status.standing.revision;
 }
 status.standing.loading = false; status.standing.error = error; ++status.standing.finished;
 j.h.Frame(0, 2);
}

// ... and the newest choice's: the standing after it, or its refusal.
// `frame` false leaves drawing to the caller (Ember hidden).
void AnswerChoice(Journey& j, const Standing& after, const char* failure = "", bool frame = true) {
 const auto sent = Sent(j, Op::RankingChoose);
 Check(!sent.empty(), "No choice to answer");
 auto& status = j.h.view.rankings;
 if (!*failure) { status.standing.value = after; status.standing.installedMs = status.nowMs; ++status.standing.revision; }
 status.chose = sent.back()->request; status.choosing = 0; status.choiceFailure = failure;
 if (frame) j.h.Frame(0, 2);
}

// One frame with Ember hidden.
void Hidden(Journey& j) {
 ImGui::GetIO().DeltaTime = 1.f / 60; ImGui::NewFrame();
 j.h.shell.Background(j.h.view, j.h.view.room, [&](ShellAction a) { j.h.actions.push_back(a); return true; });
 ImGui::Render();
}

Standing Ranked(std::uint8_t band, bool settled, bool listed = false) {
 Standing standing;
 standing.kind = Standing::Kind::Ranked; standing.classesShown = true; standing.badge = band ? SkillBadge::OfClass(band, settled) : SkillBadge::Placing();
 if (listed) { standing.listed = true; standing.listing = {"Player 1", CountryCode::Parsed("GB")}; }
 return standing;
}

void Open(Journey& j) {
 j.h.view.preferences.displayName = " Player 1 ";
 j.h.view.automaticCountry = CountryCode::Parsed("GB");
 j.h.Screen("online"); j.h.Choose("rankings"); j.h.Frame(0, 2);
 Check(j.h.shell.Navigation().Screen() == "rankings", "Online play does not open Rankings");
 j.answerIdentityUntilIdle();
}

void StandingJourney() {
 Journey j; Open(j);
 const auto reads = Sent(j, Op::RankingStanding);
 Check(reads.size() == 1 && reads[0]->bridgeId == "brg_1", "Opening Rankings did not read the standing once");
 Check(j.row("rk-loading") != nullptr, "No loading card before the standing");
 // A class, unlisted: the name and flag rooms show go to the board, once.
 AnswerStanding(j, Ranked(6, true));
 const auto chosen = Sent(j, Op::RankingChoose);
 Check(chosen.size() == 1 && chosen[0]->ranked && chosen[0]->listingName == "Player 1" && chosen[0]->listingCountry == "GB",
  "The listing was not sent as rooms show it");
 Check(j.row("rk-standing") && j.row("rk-standing")->label == "B+", "The card does not name the class");
 for (const char* id : {"rk-board-as", "rk-players", "rk-stop", "rk-how"}) Check(j.row(id) != nullptr, (std::string("Missing row ") + id).c_str());
 AnswerChoice(j, Ranked(6, true, true));
 Check(j.row("rk-board-as")->value == "Player 1", "On the board as does not show the bridge's listing");
 // A refresh of a listed standing sends nothing more.
 j.h.Press(MenuInput::Options);
 AnswerStanding(j, Ranked(6, true, true));
 Check(Sent(j, Op::RankingStanding).size() == 2 && Sent(j, Op::RankingChoose).size() == 1, "Options did not read again, or the listing went twice");
 // Online play shows the class at a glance.
 j.h.Screen("online");
 Check(j.row("rankings") && j.row("rankings")->value == "B+", "Online play does not show the class");
}

void ChoiceJourney() {
 Journey j; Open(j);
 AnswerStanding(j, Ranked(3, false, true));
 Check(Sent(j, Op::RankingChoose).empty(), "A listed player's listing was sent again");
 j.h.Choose("rk-stop");
 Check(j.h.shell.Navigation().Confirming(), "Stop taking part did not ask first");
 j.h.shell.Navigation().ConfirmSelected(true); j.h.Press(MenuInput::Select);
 const auto stop = Sent(j, Op::RankingChoose);
 Check(stop.size() == 1 && !stop[0]->ranked && stop[0]->listingName.empty(), "Stopping sent a listing or did not stop");
 Standing out; out.kind = Standing::Kind::OptedOut; out.changeIn = 24 * 3600;
 AnswerChoice(j, out);
 const auto* join = j.row("rk-join");
 Check(join && !join->enabled && join->detail.find("24") != std::string::npos, "Take part is not held for the day");
 // A day later the row is free again, and taking part sends the listing it named.
 Pass(j, 24 * 3600);
 Check(j.row("rk-join")->enabled, "Take part did not come back after the wait");
 j.h.Choose("rk-join"); j.h.shell.Navigation().ConfirmSelected(true); j.h.Press(MenuInput::Select);
 const auto join2 = Sent(j, Op::RankingChoose);
 Check(join2.size() == 2 && join2[1]->ranked && join2[1]->listingName == "Player 1", "Take part did not send the listing it named");
 // Refused as too soon: said, and the standing read again for the wait.
 const auto reads = Sent(j, Op::RankingStanding).size();
 AnswerChoice(j, out, "choice_too_soon");
 Check(Sent(j, Op::RankingStanding).size() == reads + 1, "A refused change did not read the standing again");
}

void DeveloperJourney() {
 Journey j; Open(j);
 Standing developer = Ranked(0, false);
 developer.badge = SkillBadge::Developer();
 AnswerStanding(j, developer);
 // The mark stands in place of a class: no listing is sent and the board does not list them.
 Check(Sent(j, Op::RankingChoose).empty() && !j.row("rk-board-as"), "A developer was offered a place on the board");
 Check(j.row("rk-standing") && j.row("rk-players") && j.row("rk-stop"), "A developer's Rankings lost its rows");
 j.h.Screen("online");
 Check(j.row("rankings") && j.row("rankings")->value == "Dev", "Online play does not show the developer mark");
}

// Leaving Rankings while a choice is on its way: its answer still lands, is
// said, and counts its wait from when it arrived; a refusal reads the standing again.
void LeaveDuringChoiceJourney() {
 Journey j; Open(j);
 AnswerStanding(j, Ranked(5, true, true));
 j.h.Choose("rk-stop"); j.h.shell.Navigation().ConfirmSelected(true); j.h.Press(MenuInput::Select);
 Check(Sent(j, Op::RankingChoose).size() == 1, "Stop taking part was not sent");
 j.h.view.rankings.choosing = Sent(j, Op::RankingChoose).back()->request;
 // Back and in again before the answer: the read waits for the choice, which answers with the standing.
 const auto readsBeforeReopen = Sent(j, Op::RankingStanding).size();
 j.h.Screen("online");
 j.h.Choose("rankings");
 Check(Sent(j, Op::RankingStanding).size() == readsBeforeReopen, "A read went while the choice was on its way");
 j.h.Screen("online");
 Pass(j, 3600);
 Standing out; out.kind = Standing::Kind::OptedOut; out.changeIn = 24 * 3600;
 AnswerChoice(j, out);
 j.h.Choose("rankings");
 const auto* join = j.row("rk-join");
 Check(join && join->detail.find("24") != std::string::npos, "The day's wait did not count from the choice's answer");
 // The same, refused while away: the next visit reads the standing.
 Pass(j, 24 * 3600);
 j.h.Choose("rk-join"); j.h.shell.Navigation().ConfirmSelected(true); j.h.Press(MenuInput::Select);
 j.h.Screen("online");
 const auto reads = Sent(j, Op::RankingStanding).size();
 AnswerChoice(j, out, "choice_too_soon");
 j.h.Choose("rankings");
 Check(Sent(j, Op::RankingStanding).size() == reads + 1, "A refusal while away did not read the standing again");
}

// A failed refresh keeps the standing and how old it is; so does a restart of the interface clock.
void FreshnessJourney() {
 Journey j; Open(j);
 Standing out; out.kind = Standing::Kind::OptedOut; out.changeIn = 24 * 3600;
 AnswerStanding(j, out);
 Pass(j, 3600);
 j.h.Press(MenuInput::Options);
 AnswerStanding(j, out, "bridge_unreachable");
 Check(j.row("rk-join") && j.row("rk-join")->detail.find("23") != std::string::npos, "A failed refresh restarted the day's wait");
 // A DX9 reset recreates ImGui, whose clock starts again.
 ImGui::GetCurrentContext()->Time = 0; j.h.Frame(0, 2);
 Check(j.row("rk-join")->detail.find("23") != std::string::npos, "The day's wait did not survive the interface clock restarting");
}

// Hidden while a choice is on its way: a refusal is said once Ember shows
// again, and a choice that went through counts its wait from its answer.
void HiddenDuringChoiceJourney() {
 Journey j; Open(j);
 AnswerStanding(j, Ranked(5, true, true));
 j.h.Choose("rk-stop"); j.h.shell.Navigation().ConfirmSelected(true); j.h.Press(MenuInput::Select);
 Hidden(j);
 AnswerChoice(j, Standing(), "choice_too_soon", false);
 for (int i = 0; i < 3; ++i) Hidden(j);
 j.h.Frame(0, 2);
 Check(j.status == loc::T("rankings.failure.too_soon"), "A refusal while hidden was not said");
 // Went through while hidden, an hour before Ember shows again.
 j.h.Screen("online"); j.h.Choose("rankings");
 AnswerStanding(j, Ranked(5, true, true));
 j.h.Choose("rk-stop"); j.h.shell.Navigation().ConfirmSelected(true); j.h.Press(MenuInput::Select);
 Hidden(j);
 Standing out; out.kind = Standing::Kind::OptedOut; out.changeIn = 24 * 3600;
 AnswerChoice(j, out, "", false);
 j.h.view.rankings.nowMs += 3600 * 1000;
 Hidden(j);
 j.h.Frame(0, 2);
 Check(j.row("rk-join") && j.row("rk-join")->detail.find("23") != std::string::npos, "Time hidden was not counted in the day's wait");
}

void NoAccountJourney() {
 Journey j; Open(j);
 Standing none;
 AnswerStanding(j, none);
 Check(j.row(DiscordConnectScreen) != nullptr && !j.row("rk-stop"), "No Connect Discord for a player without an account");
 j.h.Choose(DiscordConnectScreen);
 Check(j.h.shell.Navigation().Screen() == DiscordConnectScreen, "Connect Discord did not open");
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "rankings", "Back from Connect Discord did not return to Rankings");
}

void UnavailableJourney() {
 Journey j; Open(j);
 AnswerStanding(j, Standing(), "ranking_unavailable");
 Check(j.row("rk-unavailable") && j.rows.size() == 1, "A service without rankings shows more than its card");
}

void BoardJourney() {
 Journey j; Open(j);
 AnswerStanding(j, Ranked(7, true, true));
 j.h.Choose("rk-players");
 Check(j.h.shell.Navigation().Screen() == "rankings-board" && Sent(j, Op::RankingBoard).size() == 1, "Players by class did not open and read the board");
 auto board = std::make_shared<std::vector<BoardEntry>>();
 board->push_back({{"Player 2", CountryCode::Parsed("JP")}, 7, false});
 board->push_back({{"Player 1", CountryCode::Parsed("GB")}, 7, true});
 board->push_back({{"Player 3", CountryCode()}, 3, false});
 auto& status = j.h.view.rankings;
 status.board.value = board; status.board.loaded = true; ++status.board.finished; j.h.Frame(0, 2);
 std::vector<std::string> ids;
 for (const auto& row : j.rows) ids.push_back(row.id);
 Check(ids == std::vector<std::string>{"rkb-you", "rkb-class:7", "rkb-p:0", "rkb-p:1", "rkb-class:3", "rkb-p:2"}, "The board is not grouped by class");
 Check(j.row("rkb-class:7")->value == "2" && j.row("rkb-p:1")->valueHighlighted, "Counts or your own row are wrong");
 Check(j.row("rkb-p:0")->left == "rkb-class:7" && j.row("rkb-p:0")->right == "rkb-class:3" && j.row("rkb-class:7")->left == "rkb-you",
  "Left and Right do not move between classes");
 // Back on Rankings, nothing is read again; Options on the board reads the board.
 j.h.Press(MenuInput::Options);
 Check(Sent(j, Op::RankingBoard).size() == 2, "Options did not read the board again");
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "rankings" && Sent(j, Op::RankingStanding).size() == 1, "Coming back from the board read the standing again");
}
}

int main() {
 try {
  StandingJourney(); ChoiceJourney(); DeveloperJourney(); LeaveDuringChoiceJourney(); FreshnessJourney(); HiddenDuringChoiceJourney(); NoAccountJourney(); UnavailableJourney(); BoardJourney();
  std::cout << "Rankings journeys passed.\n";
  return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
