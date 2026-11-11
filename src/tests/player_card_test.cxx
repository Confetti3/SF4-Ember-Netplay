// The player's card from their fighter points (docs/design/PLAYER_CARD.md,
// View models): which fighters the strip shows and in what order, the main's
// progress and the facts, the states without points, and how far the next tier is.
#include "../ui/PlayerCard.hxx"
#include <cstdio>
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

Answered<Own> Counted(Own own, bool shown = true) {
    own.kind = Own::Kind::Counted;
    own.shown = shown;
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
    Check(view.progress.points == 0, "an unplayed main has progress");
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
    ui::PlayerCardView view;
    ui::player_card::Fill(view, Counted(own), 0);
    Check(view.progress.points == 1900 && view.progress.tier == 3 && view.progress.next == 2500, "the main's progress is wrong");
    Check(view.facts.games == 1330 && view.facts.fightersPlayed == 2, "the counted games or fighters played are wrong");
    // 612 wins in 1330 games rounds to 46 %.
    Check(view.facts.winPercent == 46, "the win rate is wrong");
    Check(view.levelsShown, "points shown to others were not carried");
    ui::player_card::Fill(view, Counted(own, false), 0);
    Check(!view.levelsShown, "points hidden from others were not carried");
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
    Check(view.levels == ui::CardLevels::Unavailable && view.fighters.size() == 1 && view.fighters[0].points == 0,
        "a read that failed with nothing before it is not unavailable with the main at 0");
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
    Check(std::string(ui::player_card::NoLevelWords(view)) == "level.none" && view.progress.points == 0, "nothing played yet is not said");
    Check(view.fighters.size() == 1 && view.fighters[0].fighter == 0 && view.fighters[0].points == 0, "a card with nothing played does not show the main at 0");
    Own played;
    Play(played, 0, 2, 1, 0, 200);
    ui::player_card::Fill(view, Counted(played), 0);
    Check(!ui::player_card::NoLevelWords(view), "a card with points says why it has none");
}

// How far the points are from their tier's start to the next tier's.
void Distances() {
    using ui::player_card::Part;
    netplay::levels::FighterLevel level;
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

}

int main() {
    PlayedOrder();
    Strip();
    ProgressAndFacts();
    States();
    Distances();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("player card tests passed\n");
    return 0;
}
