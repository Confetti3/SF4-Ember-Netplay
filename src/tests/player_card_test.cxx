// The player's card from their fighter levels (docs/design/PLAYER_CARD.md,
// View models): which fighters the strip shows and in what order, the main's
// progress and the facts, the states without levels, and how far a level is.
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

void Play(Own& own, int fighter, int level, std::uint64_t games, std::uint64_t wins, std::uint64_t floor, std::uint64_t next) {
    auto& l = own.fighters[fighter];
    l.level = static_cast<std::uint8_t>(level); l.games = games; l.wins = wins; l.floor = floor; l.next = next;
}

Answered<Own> Counted(Own own, bool shown = true) {
    own.kind = Own::Kind::Counted;
    own.shown = shown;
    Answered<Own> answer;
    answer.value = std::move(own);
    answer.loaded = true;
    return answer;
}

// Highest level first, then most games, then the lower fighter.
void PlayedOrder() {
    Own own;
    Play(own, 9, 12, 100, 40, 0, 0);
    Play(own, 4, 30, 10, 5, 0, 0);
    Play(own, 7, 12, 150, 60, 0, 0);
    Play(own, 2, 12, 150, 70, 0, 0);
    const auto played = ui::player_card::PlayedFighters(own);
    Check(played == std::vector<int>({4, 2, 7, 9}), "the played fighters are not by level, then games, then fighter");
    Check(ui::player_card::PlayedFighters(Own{}).empty(), "a fighter with no level is listed as played");
}

// The strip is the main first, then the two highest others, whatever the main's own level.
void Strip() {
    Own own;
    Play(own, 0, 5, 40, 20, 0, 0);
    Play(own, 1, 42, 600, 300, 0, 0);
    Play(own, 2, 25, 300, 100, 0, 0);
    Play(own, 3, 13, 80, 40, 0, 0);
    ui::PlayerCardView view;
    ui::player_card::Fill(view, Counted(own), 0);
    Check(view.levels == ui::CardLevels::Counted && view.fighters.size() == 3, "the strip is not three fighters");
    Check(view.fighters[0].fighter == 0 && view.fighters[1].fighter == 1 && view.fighters[2].fighter == 2, "the strip is not the main, then the two highest others");
    Check(view.fighters[0].level == 5 && view.fighters[1].games == 600, "the strip's levels or games are wrong");
    // A main nobody has played is still first, at level 0.
    ui::player_card::Fill(view, Counted(own), 9);
    Check(view.fighters.size() == 3 && view.fighters[0].fighter == 9 && view.fighters[0].level == 0 && view.fighters[1].fighter == 1 && view.fighters[2].fighter == 2,
        "an unplayed main is not first at level 0");
    Check(view.progress.level == 0, "an unplayed main has progress");
    // The main among the highest is not shown twice.
    ui::player_card::Fill(view, Counted(own), 1);
    Check(view.fighters.size() == 3 && view.fighters[0].fighter == 1 && view.fighters[1].fighter == 2 && view.fighters[2].fighter == 3, "the main was shown twice");
    // Fewer fighters than places: only those.
    Own one;
    Play(one, 6, 3, 10, 5, 0, 0);
    ui::player_card::Fill(view, Counted(one), 6);
    Check(view.fighters.size() == 1, "the strip is padded past the fighters played");
}

// The main's progress and the facts.
void ProgressAndFacts() {
    Own own;
    // Level 42 begins at 41 * 45 = 1845 points and level 43 at 42 * 46 = 1932.
    Play(own, 0, 42, 1300, 600, 1845, 1932);
    Play(own, 5, 7, 30, 12, 0, 0);
    ui::PlayerCardView view;
    ui::player_card::Fill(view, Counted(own), 0);
    Check(view.progress.level == 42 && view.progress.points == 1900 && view.progress.floor == 1845 && view.progress.next == 1932, "the main's progress is wrong");
    Check(view.facts.games == 1330 && view.facts.fightersPlayed == 2, "the counted games or fighters played are wrong");
    // 612 wins in 1330 games rounds to 46 %.
    Check(view.facts.winPercent == 46, "the win rate is wrong");
    Check(view.levelsShown, "levels shown to others were not carried");
    ui::player_card::Fill(view, Counted(own, false), 0);
    Check(!view.levelsShown, "levels hidden from others were not carried");
}

// The states without a level, and the words that say so.
void States() {
    ui::PlayerCardView view;
    Answered<Own> answer;
    ui::player_card::Fill(view, answer, 0);
    Check(view.levels == ui::CardLevels::Pending && std::string(ui::player_card::NoLevelWords(view)) == "level.loading", "no answer yet is not pending");
    answer.loading = true;
    ui::player_card::Fill(view, answer, 0);
    Check(view.levels == ui::CardLevels::Pending, "a read in flight is not pending");
    answer.loading = false; answer.error = "bridge_unreachable";
    ui::player_card::Fill(view, answer, 0);
    Check(view.levels == ui::CardLevels::Unavailable && view.fighters.empty(), "a read that failed with nothing before it is not unavailable");
    Own own;
    own.kind = Own::Kind::NoAccount;
    answer = {}; answer.value = own; answer.loaded = true;
    ui::player_card::Fill(view, answer, 0);
    Check(view.levels == ui::CardLevels::NoAccount && std::string(ui::player_card::NoLevelWords(view)) == "level.no_account", "no account is not said");
    own.kind = Own::Kind::Unavailable;
    answer.value = own;
    ui::player_card::Fill(view, answer, 0);
    Check(view.levels == ui::CardLevels::Unavailable && std::string(ui::player_card::NoLevelWords(view)) == "level.unavailable", "a service without levels is not said");
    // Counted with nothing played says so; with something played it says nothing.
    ui::player_card::Fill(view, Counted(Own{}), 0);
    Check(std::string(ui::player_card::NoLevelWords(view)) == "level.none" && view.progress.level == 0, "nothing played yet is not said");
    Own played;
    Play(played, 0, 1, 2, 1, 0, 5);
    ui::player_card::Fill(view, Counted(played), 0);
    Check(!ui::player_card::NoLevelWords(view), "a card with levels says why it has none");
}

// How far a level is, and how many games remain at the fighter's own pace.
void Distances() {
    using ui::player_card::GamesAway;
    using ui::player_card::Part;
    netplay::levels::FighterLevel level;
    level.level = 5; level.games = 20; level.wins = 10; level.floor = 20; level.next = 45;
    // 30 points of 20 to 45: two fifths through.
    Check(Part(level) > .39f && Part(level) < .41f, "the part of a level is wrong");
    // 15 points to go at 1.5 points a game: ten games.
    Check(GamesAway(level) == 10, "the games to the next level are wrong");
    level.games = 30; level.wins = 0;
    // At a point a game: 15 to go is 15 games.
    Check(GamesAway(level) == 15, "the games at one point a game are wrong");
    level.next = 0;
    Check(Part(level) == 1.f && GamesAway(level) == 0, "the top level has a way to go");
    level = {};
    Check(Part(level) == 1.f && GamesAway(level) == 0, "an unplayed fighter has a way to go");
    level.level = 2; level.floor = 5; level.next = 12; level.games = 3; level.wins = 0;
    Check(Part(level) == 0.f, "a level below its floor has a part");
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
