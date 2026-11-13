// The player's card from their fighter points (docs/design/PLAYER_CARD.md,
// View models): which fighters the strip shows and in what order, the main's
// progress and the facts, the states without points, and how far the next tier is.
#include "../ui/CardSets.hxx"
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

// ---- The card's view models (CardModel.hxx, CardSets.hxx) ----

namespace pc = ui::player_card;
namespace aw = netplay::awards;
using pc::Chip;

// No text on any size is under 13 px at 1.00x, and the sizes are the design's.
void Metrics() {
    for (const auto size : {pc::Size::Wide, pc::Size::Compact, pc::Size::Narrow})
        Check(pc::MetricsOf(size).SmallestText() >= pc::TextFloor, "a card size has text under the floor");
    Check(pc::TextFloor == 13.f, "the text floor is not 13 px");
    Check(pc::MetricsOf(pc::Size::Wide).setRows == 5 && pc::MetricsOf(pc::Size::Compact).setRows == 3 && pc::MetricsOf(pc::Size::Narrow).setRows == 3, "the set rows are not five, three, three");
    Check(pc::MetricsOf(pc::Size::Wide).hero == 440 && pc::MetricsOf(pc::Size::Wide).pip == 7, "the wide card's hero or pips are not the design's");
    Check(pc::SizeOf(false, 838) == pc::Size::Wide && pc::SizeOf(false, 510) == pc::Size::Compact && pc::SizeOf(true, 900) == pc::Size::Narrow, "the size class is wrong");
}

// Plays and Looking for are two groups of chips; the Panel gives Region way first, then Connection, never the device or Looking for.
void Chips() {
    netplay::profiles::Style style;
    style.device = netplay::profiles::Device::Stick;
    style.link = netplay::profiles::Link::Wired;
    style.region = "euc1";
    style.looking = netplay::profiles::LookingLongSets | netplay::profiles::LookingTeach;
    const auto plays = pc::PlaysChips(style);
    Check(plays.size() == 3 && plays[0].kind == Chip::Kind::Device && plays[1].kind == Chip::Kind::Link && plays[2].kind == Chip::Kind::Region, "the Plays chips are not device, connection, region");
    const auto looking = pc::LookingChips(style);
    Check(looking.size() == 2 && looking[0].text == loc::T("style.long_sets") && looking[1].text == loc::T("style.teach"), "the Looking for chips are wrong");
    Check(pc::PlaysChips({}).empty() && pc::LookingChips({}).empty(), "a style with nothing has chips");
    const auto room = [](std::size_t most) { return [most](const std::vector<Chip>& chips) { return chips.size() <= most; }; };
    Check(pc::GiveWay(plays, room(3)).size() == 3, "chips that fit gave way");
    const auto two = pc::GiveWay(plays, room(2));
    Check(two.size() == 2 && two[0].kind == Chip::Kind::Device && two[1].kind == Chip::Kind::Link, "Region did not give way first");
    const auto one = pc::GiveWay(plays, room(1));
    Check(one.size() == 1 && one[0].kind == Chip::Kind::Device, "Connection did not give way second");
    Check(pc::GiveWay(plays, room(0)).size() == 1, "the device gave way");
}

aw::Counters Reached(std::uint64_t games, std::uint64_t wins, std::uint32_t streak, std::uint32_t peakClass) {
    aw::Counters counters;
    counters.games = games; counters.wins = wins; counters.bestStreak = streak; counters.peakClass = peakClass;
    return counters;
}

// The shown families: the ones chosen that have a level, filled by tier, then progress, then the catalog; never one with no level.
void Families() {
    auto counters = Reached(1000, 250, 10, 8);
    counters.fightersPlayed = 5; counters.topFighterTier = 3;
    const auto automatic = pc::ShownFamilies(counters, {});
    Check(automatic.size() == 4, "the showcase is not four families");
    for (std::size_t i = 1; i < automatic.size(); ++i) Check(automatic[i - 1].tier >= automatic[i].tier, "the automatic families are not by tier, highest first");
    for (const auto& tile : automatic) Check(tile.level > 0 && tile.family && !tile.title.empty(), "a family with no level is shown");
    // A chosen family with a level goes first; one with none is skipped and the gap filled in the automatic order.
    const auto chosen = pc::ShownFamilies(counters, {"cast", "friends", "streak"});
    Check(chosen.size() == 4 && chosen[0].family->id == std::string("cast") && chosen[1].family->id == std::string("streak"), "the chosen families did not lead");
    for (const auto& tile : chosen) Check(tile.family->id != std::string("friends"), "a pinned family with no level was shown");
    // A card that has earned nothing shows nothing; the level is the counters', whatever is claimed.
    Check(pc::ShownFamilies(aw::Counters{}, {"wins", "matches"}).empty(), "a family nobody reached is shown");
    const auto few = pc::ShownFamilies(Reached(1, 0, 0, 0), {"wins"});
    Check(few.size() == 1 && few[0].family->id == std::string("matches") && few[0].level == 1, "the level shown is not the counters'");
    // A worn title or icon is drawn at the level the counters earn, and not at all at level 0.
    netplay::profiles::Worn worn;
    worn.title = "matches"; worn.icon = "rank";
    const auto drawn = pc::WornOnCard(Reached(1000, 0, 0, 0), worn);
    Check(drawn.title == pc::LevelTitle(aw::MatchLevels[3]) && drawn.titleTier == ui::v2::FrameTier(aw::MatchLevels[3].tier) && drawn.icon < 0, "a worn family is not drawn at the level the counters earn");
    netplay::profiles::Worn nonsense;
    nonsense.title = "nonsense"; nonsense.icon = "x";
    Check(!pc::WornOnCard(aw::Counters{}, worn).Any() && !pc::WornOnCard(counters, nonsense).Any(), "a claim nobody earned is drawn");
    // The totals: the friends family counts on the player's own card only.
    const auto own = pc::AwardsOf(counters, {}, true), theirs = pc::AwardsOf(counters, {}, false);
    const int friends = static_cast<int>(aw::Find("friends")->count);
    Check(own.total == static_cast<int>(aw::LevelTotal()) && theirs.total == own.total - friends, "the awards' total is wrong for another's card");
    counters.friends = 15;
    Check(pc::AwardsOf(counters, {}, true).earned == pc::AwardsOf(counters, {}, false).earned + 3, "a friends count was counted on another's card");
}

// The record cells, own and another's, and the meter words at 0, mid and the top.
void Record() {
    auto counters = Reached(2840, 1164, 12, 7);
    counters.fightersPlayed = 12;
    const auto own = pc::RecordCells(counters, true), theirs = pc::RecordCells(counters, false);
    Check(own.size() == 4 && own[0].value == "2840" && own[1].key == loc::T("card.win_rate") && own[1].value == "41%" && own[2].value == "12" && own[3].value == loc::Tf("card.of", 12, selection::FighterCount),
        "the own record cells are wrong");
    Check(theirs[1].key == loc::T("card.wins") && theirs[1].value == "1164", "another's record has a win rate");
    Check(pc::RecordLine(counters) == loc::Tf("card.record_line", 2840, 1164, 12), "the Panel's record line is wrong");
    netplay::levels::Level level;
    Check(pc::MeterLine(level, false).words == loc::T("level.not_played"), "a fighter at 0 has tier words");
    level.points = 1986; level.tier = 3; level.floor = 1500; level.next = 2500;
    const auto mid = pc::MeterLine(level, false);
    Check(mid.words == loc::Tf("card.tier_to", ui::v2::TierName(ui::v2::FrameTier(3)), 514, ui::v2::TierName(ui::v2::FrameTier(4))) && mid.meter && mid.part > .48f && mid.part < .49f,
        "the meter words at mid are wrong");
    level.tier = 4; level.floor = 2500; level.next = 0;
    Check(pc::MeterLine(level, false).words == loc::Tf("card.tier_top", ui::v2::TierName(ui::v2::FrameTier(4))), "the meter words at the top are wrong");
}

netplay::levels::Card Another() {
    netplay::levels::Card card;
    card.name = "Player 2";
    card.hasMain = true; card.main = 16;
    card.account = {3120, 2};
    card.fighters = {{3, 310, 1}, {16, 1640, 3}, {20, 720, 2}, {30, 90, 0}};
    card.awards = Reached(1712, 941, 9, 6);
    card.hasVersus = true; card.versus.won = 3; card.versus.lost = 5; card.versus.lastAt = 100;
    card.hasStyle = true; card.style.looking = netplay::profiles::LookingRanked;
    card.worn.title = "wins"; card.worn.icon = "streak";
    return card;
}

// The Full card from a subject: the strip, Most played or You and them, and what differs by data.
void FullCard() {
    Check(pc::TopFighters({4, 7, 9}, 9) == std::vector<int>({9, 4, 7}) && pc::TopFighters({4, 7, 9, 2}, -1) == std::vector<int>({4, 7, 9}) && pc::TopFighters({}, 5) == std::vector<int>({5}),
        "the top fighters are wrong");
    // Another's card: the fighter they play now is the hero, the strip leads with the main marked, no games, You and them.
    netplay::profiles::Known known;
    const auto them = pc::CardSubject(Another(), known, 20, true, true);
    const auto view = pc::FullOf(them, pc::Size::Wide);
    Check(view.hero == 20 && view.heroWord == loc::T("card.playing") && view.heroPoints == 720, "another's hero is not the fighter they play now");
    Check(view.strip.size() == 3 && view.strip[0].fighter == 16 && view.strip[0].isMain && view.strip[1].fighter == 20 && view.strip[1].games.empty(),
        "another's strip is not their main, then the highest others, without games");
    Check(view.points.shown && !view.points.hero.meter && !view.points.account.meter, "another's points have meters");
    Check(view.record[1].key == loc::T("card.wins") && view.rival.kind == pc::Rival::Kind::Versus && view.rival.cells.size() == 3 && view.rival.cells[0].value.compare(0, 1, "3") == 0 &&
        view.rival.cells[2].value == loc::T("card.not_friends"), "the record between you is wrong");
    Check(view.awards.total == static_cast<int>(aw::LevelTotal()) - static_cast<int>(aw::Find("friends")->count) && view.worn.title == pc::LevelTitle(aw::WinLevels[2]) && view.worn.icon >= 0,
        "another's awards or worn title are wrong");
    Check(view.looking.size() == 1 && view.plays.empty(), "another's chips are wrong");
    // A card from a service without profiles has no chips, record or awards.
    const auto old = pc::FullOf(pc::CardSubject(Another(), known, -1, false, false), pc::Size::Wide);
    Check(old.record.empty() && old.awards.total == 0 && old.plays.empty() && old.looking.empty() && old.rival.kind == pc::Rival::Kind::None && old.heroWord == loc::T("profile.main_badge"),
        "a card without profiles shows more than its points");

    // The own card: meters, games under a tile, a win rate, the most played opponent in its three states.
    Own own;
    Play(own, 0, 1300, 600, 3, 2500);
    Play(own, 5, 30, 12, 0, 200);
    own.account.points = 1300; own.account.games = 1400; own.account.wins = 700; own.account.tier = 1; own.account.floor = 500; own.account.next = 2000;
    own.awards = Reached(1400, 700, 12, 7);
    auto answer = Counted(own);
    pc::OwnInputs in;
    in.levels = &answer; in.name = "Player 1"; in.main = 0; in.friends = 2;
    auto mine = pc::FullOf(pc::OwnSubject(in), pc::Size::Wide);
    Check(mine.points.hero.meter && mine.points.account.meter && !mine.strip.empty() && mine.strip[0].games == ui::player_card::GamesWords(1300) && mine.record[1].key == loc::T("card.win_rate"),
        "the own card has no meters, games or win rate");
    Check(mine.rival.kind == pc::Rival::Kind::None, "the most played part shows with no most played opponent while games are counted");
    answer = Counted(Own{});
    Check(pc::FullOf(pc::OwnSubject(in), pc::Size::Wide).rival.kind == pc::Rival::Kind::MostPlayedNone, "nothing counted yet is not said");
    own.hasMostPlayed = true;
    own.mostPlayed.card = "c1.x"; own.mostPlayed.name = "Player 7"; own.mostPlayed.won = 41; own.mostPlayed.lost = 33;
    answer = Counted(own);
    mine = pc::FullOf(pc::OwnSubject(in), pc::Size::Wide);
    Check(mine.rival.kind == pc::Rival::Kind::MostPlayed && mine.rival.player.name == "Player 7" && mine.rival.line == loc::Tf("card.most_played_line", 41, 33, 74), "the most played opponent is not shown");
    Check(mine.awards.total == static_cast<int>(aw::LevelTotal()), "the own card's awards leave out the friends family");
    // The card is whole in every state: only the words change.
    const Answered<Own> unread{};
    in.levels = &unread;
    const auto empty = pc::FullOf(pc::OwnSubject(in), pc::Size::Wide);
    Check(empty.levels == ui::CardLevels::Pending && !empty.points.shown && empty.levelWords && empty.record.size() == 4 && empty.hero == 0 && empty.awards.total > 0,
        "the card is not whole before its points are read");
}

// The rows the card's own parts are: the sets rows by size, All sets only where the service keeps sets.
void CardRows() {
    pc::FullCardView view;
    view.setRows = 5;
    view.sets.state = pc::SetsView::State::Rows;
    for (int i = 0; i < 6; ++i) { pc::SetRow row; row.row = "card-set:" + std::to_string(i); row.opponent = "Player 2"; view.sets.rows.push_back(row); }
    const auto count = [](const std::vector<ui::MenuEntry>& rows) { int sets = 0; for (const auto& row : rows) sets += row.id.compare(0, 9, "card-set:") == 0; return sets; };
    Check(count(pc::CardRows(view)) == 5, "five set rows are not shown at 1080");
    view.setRows = 3;
    Check(count(pc::CardRows(view)) == 3, "three set rows are not shown at 720");
    Check(pc::CardRows(view).back().id == pc::AllSetsRow, "All sets does not end the rows");
    view.sets.allSets = false;
    Check(pc::CardRows(view).back().id != pc::AllSetsRow, "All sets is offered by a service that keeps no sets");
}

netplay::archive::Game GameOf(std::uint64_t at, const char* a, const char* b, int fighterA, int fighterB, const char* set = "") {
    netplay::archive::Game game;
    game.digest = std::string(64, static_cast<char>(0x61 + (at / 100) % 20)); game.url = "https://archive.example/g"; game.savedAt = at;
    game.names = {{a, b}}; game.fighters = {{fighterA, fighterB}}; game.countries = {{"DE", "FR"}}; game.classes = {{8, 7}};
    game.set = set;
    return game;
}

// A set as the card's row says it, from the player's side; the card's part by the listing's state.
void Sets() {
    namespace ar = netplay::archive;
    ar::Listing listing;
    const std::uint64_t now = 1800000000;
    listing.games = {GameOf(now - 7200, "Player 1", "Player 2", 0, 5), GameOf(now - 7000, "Player 2", "Player 1", 5, 0)};
    const auto online = ui::archivesets::Group(listing);
    Check(online.size() == 1 && online[0].games.size() == 2, "two games of the same two players are not one set");
    auto row = ui::card_sets::RowOf(online[0], "Player 1", now);
    Check(row.mine == 0 && row.theirs == 5 && row.opponent == "Player 2" && row.badge.HasClass() && row.badge.band == 8, "the row's fighters or opponent are wrong");
    Check(row.line == loc::Tf("archive.games", 2) + " \xC2\xB7 " + loc::T("card.set_online") + " \xC2\xB7 " + loc::Tf("friends.hours_ago", 1), "an online set's line is wrong");
    // A tournament set's score is from this player's side: Player 1 is the second named in the first game.
    ar::Listing event;
    auto first = GameOf(now - 90000, "Player 2", "Player 1", 5, 0, "emt_a");
    first.event = "Sample Cup"; first.round = "Top 8";
    event.games = {first};
    event.sets["emt_a"].score = {{2, 1}};
    const auto tournament = ui::archivesets::Group(event);
    row = ui::card_sets::RowOf(tournament[0], "Player 1", now);
    Check(row.line.compare(0, 7, "1 \xE2\x80\x93 2") == 0 && row.line.find("Sample Cup") != std::string::npos && row.mine == 0 && row.opponent == "Player 2", "a tournament set is not scored from this player's side");

    // The card's part: reading until the listing of this name is heard, then rows, none, unreachable, or no archive.
    ar::Status status;
    using State = ar::Status::State;
    using Part = pc::SetsView::State;
    Check(ui::card_sets::ViewOf(status, false, {}, "Player 1", now).state == Part::Reading, "sets not asked for are not reading");
    status.state = State::Loading;
    Check(ui::card_sets::ViewOf(status, true, {}, "Player 1", now).state == Part::Reading, "a first page on its way is not reading");
    status.state = State::Listed;
    status.listing = std::make_shared<const ar::Listing>(listing);
    const auto shown = ui::card_sets::ViewOf(status, true, online, "Player 1", now);
    Check(shown.state == Part::Rows && shown.rows.size() == 1 && shown.allSets, "the sets are not rows");
    Check(ui::card_sets::ViewOf(status, true, {}, "Player 1", now).state == Part::None, "no sets under the name is not none");
    auto more = listing;
    more.next = "cursor";
    status.listing = std::make_shared<const ar::Listing>(more);
    Check(ui::card_sets::ViewOf(status, true, {}, "Player 1", now).state == Part::Reading, "an empty first page with more to come is not reading");
    status.state = State::Failed; status.listing.reset();
    Check(ui::card_sets::ViewOf(status, true, {}, "Player 1", now).state == Part::Unreachable, "a failed listing is not unreachable");
    status.state = State::Unsupported;
    const auto none = ui::card_sets::ViewOf(status, true, {}, "Player 1", now);
    Check(none.state == Part::Unavailable && !none.allSets, "a service without an archive offers sets");
}

}

int main() {
    Metrics();
    Chips();
    Families();
    Record();
    FullCard();
    CardRows();
    Sets();
    Groups();
    Progress();
    PlayedOrder();
    Strip();
    ProgressAndFacts();
    States();
    Distances();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("player card tests passed\n");
    return 0;
}
