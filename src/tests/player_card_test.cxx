// The player's card from their fighter points (docs/design/PLAYER_CARD.md,
// View models): which fighters the strip shows and in what order, the main's
// progress and the facts, the states without points, and how far the next tier is.
#include "../ui/PlayerCard.hxx"
#include "../common/Localization.hxx"
#include <cstdio>
#include <memory>
#include <string>

namespace {
using namespace sf4e;
using netplay::Answered;
using Own = netplay::levels::Own;
int failures = 0;
void Check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}

void Play(Own& own, int fighter, std::uint64_t games, std::uint64_t wins, int tier, std::uint64_t next) {
    auto& l = own.fighters[fighter];
    // The tiers begin at 0, 200, 700, 1500 and 2500 points, as the bridge decides them.
    const std::uint64_t starts[] = {0, 200, 700, 1500, 2500};
    l.points = games + wins; l.games = games; l.wins = wins; l.tier = static_cast<std::uint8_t>(tier); l.floor = starts[tier]; l.next = next;
}

Answered<Own> Counted(Own own) {
    own.kind = Own::Kind::Counted;
    Answered<Own> answer;
    answer.value = std::move(own);
    answer.loaded = true;
    return answer;
}

// Most points first, then the lower fighter.
void PlayedOrder() {
    Own own;
    Play(own, 9, 100, 40, 0, 200);
    Play(own, 4, 1000, 500, 3, 2500);
    Play(own, 7, 100, 40, 0, 200);
    Play(own, 2, 100, 41, 0, 200);
    const auto played = ui::player_card::PlayedFighters(own);
    Check(played == std::vector<int>({4, 2, 7, 9}), "the played fighters are not by points, then fighter");
    Check(ui::player_card::PlayedFighters(Own{}).empty(), "a fighter with no points is listed as played");
}

// The strip is the main first, then the two highest others, whatever the main's own points.
void Strip() {
    Own own;
    Play(own, 0, 40, 20, 0, 200);
    Play(own, 1, 600, 300, 2, 1500);
    Play(own, 2, 300, 100, 1, 700);
    Play(own, 3, 80, 40, 0, 200);
    ui::PlayerCardView view;
    ui::player_card::Fill(view, Counted(own), 0);
    Check(view.levels == ui::CardLevels::Counted && view.fighters.size() == 3, "the strip is not three fighters");
    Check(view.fighters[0].fighter == 0 && view.fighters[1].fighter == 1 && view.fighters[2].fighter == 2, "the strip is not the main, then the two highest others");
    Check(view.fighters[0].points == 60 && view.fighters[0].tier == 0 && view.fighters[1].points == 900 && view.fighters[1].tier == 2 && view.fighters[1].games == 600,
        "the strip's points, tiers or games are wrong");
    // A main nobody has played is still first, at 0 points.
    ui::player_card::Fill(view, Counted(own), 9);
    Check(view.fighters.size() == 3 && view.fighters[0].fighter == 9 && view.fighters[0].points == 0 && view.fighters[1].fighter == 1 && view.fighters[2].fighter == 2,
        "an unplayed main is not first at 0 points");
    Check(view.main.points == 0 && view.mainFighter == 9, "an unplayed main has points or is not the main");
    // The main among the highest is not shown twice.
    ui::player_card::Fill(view, Counted(own), 1);
    Check(view.fighters.size() == 3 && view.fighters[0].fighter == 1 && view.fighters[1].fighter == 2 && view.fighters[2].fighter == 3, "the main was shown twice");
    // Fewer fighters than places: only those.
    Own one;
    Play(one, 6, 10, 5, 0, 200);
    ui::player_card::Fill(view, Counted(one), 6);
    Check(view.fighters.size() == 1, "the strip is padded past the fighters played");
}

// The main's progress and the facts.
void ProgressAndFacts() {
    Own own;
    // 1900 points are in the Gold tier, which ends at 2500.
    Play(own, 0, 1300, 600, 3, 2500);
    Play(own, 5, 30, 12, 0, 200);
    // The account also counts games that named no fighter: 1400 games, 700 won.
    own.account.games = 1400; own.account.wins = 700;
    ui::PlayerCardView view;
    ui::player_card::Fill(view, Counted(own), 0);
    Check(view.mainFighter == 0 && view.main.points == 1900 && view.main.tier == 3 && view.main.next == 2500, "the main's counts are wrong");
    Check(view.facts.games == 1400 && view.facts.fightersPlayed == 2, "the counted games are not the account's, or fighters played are wrong");
    Check(view.facts.winPercent == 50, "the win rate is not the account's");
    // The account's points are carried, 0 until a game is counted.
    Check(view.account.points == 0, "an account with no points was not 0");
    own.account.points = 2331; own.account.tier = 1; own.account.floor = 500; own.account.next = 2000;
    ui::player_card::Fill(view, Counted(own), 0);
    Check(view.account.points == 2331 && view.account.tier == 1 && view.account.next == 2000, "the account's counts were not carried");
    ui::player_card::Fill(view, Answered<Own>{}, 0);
    Check(view.account.points == 0 && view.main.points == 0 && view.mainFighter == 0, "an answer not read yet kept the counts");
}

// The states without points, and the words that say so.
void States() {
    ui::PlayerCardView view;
    Answered<Own> answer;
    ui::player_card::Fill(view, answer, 0);
    Check(view.levels == ui::CardLevels::Pending && std::string(ui::player_card::NoLevelWords(view)) == "level.loading", "no answer yet is not pending");
    // Every state still shows the main, at 0 points; without a main, nobody.
    Check(view.fighters.size() == 1 && view.fighters[0].fighter == 0 && view.fighters[0].points == 0 && view.fighters[0].games == 0,
        "a card waiting for its points does not show the main at 0");
    ui::player_card::Fill(view, answer, -1);
    Check(view.fighters.empty(), "a card without a main shows a fighter");
    answer.loading = true;
    ui::player_card::Fill(view, answer, 0);
    Check(view.levels == ui::CardLevels::Pending, "a read in flight is not pending");
    answer.loading = false; answer.error = "bridge_unreachable";
    ui::player_card::Fill(view, answer, 0);
    Check(view.levels == ui::CardLevels::Unreachable && view.fighters.size() == 1 && view.fighters[0].points == 0 && std::string(ui::player_card::NoLevelWords(view)) == "level.unreachable",
        "a read that failed with nothing before it is not unreachable with the main at 0");
    // A read in flight or failed after an answer keeps that answer.
    Own earlier;
    Play(earlier, 0, 20, 10, 0, 200);
    auto kept = Counted(earlier);
    kept.loading = true;
    ui::player_card::Fill(view, kept, 0);
    Check(view.levels == ui::CardLevels::Counted && view.main.points == 30, "a read in flight after an answer did not keep the answer");
    kept.loading = false; kept.error = "bridge_unreachable";
    ui::player_card::Fill(view, kept, 0);
    Check(view.levels == ui::CardLevels::Counted && view.main.points == 30, "a failed read after an answer did not keep the answer");
    Own own;
    own.kind = Own::Kind::NoAccount;
    answer = {}; answer.value = own; answer.loaded = true;
    ui::player_card::Fill(view, answer, 0);
    Check(view.levels == ui::CardLevels::NoAccount && std::string(ui::player_card::NoLevelWords(view)) == "level.no_account", "no account is not said");
    Check(view.fighters.size() == 1 && view.fighters[0].fighter == 0 && view.fighters[0].points == 0, "a card without an account does not show the main at 0");
    own.kind = Own::Kind::Unavailable;
    answer.value = own;
    ui::player_card::Fill(view, answer, 0);
    Check(view.levels == ui::CardLevels::Unavailable && std::string(ui::player_card::NoLevelWords(view)) == "level.unavailable", "a service without points is not said");
    // Counted with nothing played says so; with something played it says nothing.
    ui::player_card::Fill(view, Counted(Own{}), 0);
    Check(std::string(ui::player_card::NoLevelWords(view)) == "level.none" && view.main.points == 0, "nothing played yet is not said");
    Check(view.fighters.size() == 1 && view.fighters[0].fighter == 0 && view.fighters[0].points == 0, "a card with nothing played does not show the main at 0");
    Own played;
    Play(played, 0, 2, 1, 0, 200);
    ui::player_card::Fill(view, Counted(played), 0);
    Check(!ui::player_card::NoLevelWords(view), "a card with points says why it has none");
}

// How far the points are from their tier's start to the next tier's.
void Distances() {
    using ui::player_card::Part;
    netplay::levels::Level level;
    level.points = 300; level.games = 200; level.wins = 100; level.tier = 1; level.floor = 200; level.next = 700;
    // 100 of the 500 from 200 to 700.
    Check(Part(level) > .19f && Part(level) < .21f, "the part of the way to the next tier is wrong");
    level.points = 200;
    Check(Part(level) == 0.f, "a tier's first point is part of the way");
    level.points = 300;
    level.next = 0;
    Check(Part(level) == 1.f, "the top tier has a way to go");
    level = {};
    Check(Part(level) == 0.f, "an unplayed fighter's meter is not empty");
    level.points = 700; level.tier = 2; level.floor = 700; level.next = 700;
    Check(Part(level) == 1.f, "points at the next tier's start are not the whole way");
}

// The Fighters tab's groups: played by points, the rest in roster order, the main first in its own.
void Groups() {
    Own own;
    Play(own, 9, 100, 40, 0, 200);
    Play(own, 4, 1000, 500, 3, 2500);
    Play(own, 7, 100, 40, 0, 200);
    auto groups = ui::player_card::Grouped(own, -1);
    Check(groups.played == std::vector<int>({4, 7, 9}), "the played group is not by points, then fighter");
    Check(groups.played.size() + groups.unplayed.size() == own.fighters.size(), "a fighter is in no group or in both");
    const int first = selection::RosterDisplayOrder[0];
    Check(groups.unplayed.front() == first, "the unplayed group does not start in roster order");
    // The main leads its group: a played main ahead of the higher-pointed, an unplayed one ahead of the roster's first.
    groups = ui::player_card::Grouped(own, 9);
    Check(groups.played == std::vector<int>({9, 4, 7}) && groups.unplayed.front() == first, "a played main does not lead the played group");
    const int last = selection::RosterDisplayOrder[selection::FighterCount - 1];
    groups = ui::player_card::Grouped(own, last);
    Check(groups.unplayed.front() == last && groups.played.size() == 3, "an unplayed main does not lead the unplayed group");
    groups = ui::player_card::Grouped(Own{}, 5);
    Check(groups.played.empty() && groups.unplayed.size() == own.fighters.size() && groups.unplayed.front() == 5, "nothing played is not all unplayed");
}

// The Card tab's progress rows: a played fighter's frame and the way to the next, the top frame, a fighter not played,
// and an account that has not started but knows its first frame.
void Progress() {
    using ui::player_card::ProgressOf;
    const std::string dot = " \xC2\xB7 ";
    Own own;
    Play(own, 0, 1300, 600, 3, 2500);
    auto row = ProgressOf("Ryu", own.fighters[0], false);
    Check(row.label == "Ryu" + dot + ui::v2::TierName(ui::v2::FrameTier(3)) && row.words == loc::Tf("level.points_to", 600, ui::v2::TierName(ui::v2::FrameTier(4))) && row.part > .39f && row.part < .41f,
        "a played fighter's progress row is wrong");
    auto top = own.fighters[0];
    top.tier = 4; top.floor = 2500; top.next = 0;
    row = ProgressOf("Ryu", top, false);
    Check(row.label == "Ryu" + dot + ui::v2::TierName(ui::v2::FrameTier(4)) && row.words == loc::T("level.top") && row.part == 1.f, "the top frame's progress row is wrong");
    // A fighter not played has no frame yet, whatever the bridge sent as next.
    netplay::levels::Level none;
    none.next = 200;
    row = ProgressOf("Ryu", none, false);
    Check(row.label == "Ryu" && row.words == loc::T("level.not_played") && row.part == 0.f, "a fighter not played has a frame or a way to one");
    // An account at 0 still says what its first frame needs.
    none.next = 500;
    row = ProgressOf(loc::T("card.account"), none, true);
    Check(row.label == loc::T("card.account") && row.words == loc::Tf("level.points_to", 500, ui::v2::TierName(ui::v2::FrameTier(1))) && row.part == 0.f, "an account at 0 does not say its first frame");
    row = ProgressOf(loc::T("card.account"), netplay::levels::Level{}, true);
    Check(row.words == loc::T("level.not_played"), "an account with no next frame is not at its start");
    // The rows of a card: the main's, then the account's; without a main, the account's alone.
    ui::PlayerCardView view;
    own.account.points = 1300; own.account.tier = 1; own.account.floor = 500; own.account.next = 2000;
    ui::player_card::Fill(view, Counted(own), 0);
    auto rows = ui::player_card::ProgressRows(view);
    Check(rows.size() == 2 && rows[0].label.compare(0, 3, "Ryu") == 0 && rows[1].label == loc::T("card.account") + dot + ui::v2::TierName(ui::v2::FrameTier(1)), "the progress rows are not the main's, then the account's");
    ui::player_card::Fill(view, Counted(own), -1);
    Check(ui::player_card::ProgressRows(view).size() == 1, "a card without a main has a row for it");
}

// The requests waiting: none until a state is read, the incoming list once listed.
void Requests() {
    netplay::friends::View view;
    Check(ui::player_card::RequestsWaiting(view) == 0, "a friends view not read has requests");
    auto state = std::make_shared<netplay::friends::State>();
    state->incoming.resize(2);
    view.state.value = state;
    Check(ui::player_card::RequestsWaiting(view) == 0, "an unlisted state has requests");
    auto listed = std::make_shared<netplay::friends::State>(*state);
    listed->kind = netplay::friends::State::Kind::Listed;
    view.state.value = listed;
    Check(ui::player_card::RequestsWaiting(view) == 2, "the waiting requests are not counted");
}

}

int main() {
    Groups();
    Progress();
    Requests();
    PlayedOrder();
    Strip();
    ProgressAndFacts();
    States();
    Distances();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("player card tests passed\n");
    return 0;
}
