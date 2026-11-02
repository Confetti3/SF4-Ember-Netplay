// Random on the fighter grid: which fighters a roll may land on, that every
// one is as likely, and when the pick rolls again (once per game).
#include "../common/RandomFighter.hxx"
#include "../session/RandomFighterWatch.hxx"

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include "test_support.hxx"

using namespace sf4e;
using room::Snapshot;
using room::TablePhase;

namespace {
// A source that hands out `values` in turn.
std::function<std::uint32_t()> Sequence(std::vector<std::uint32_t> values) {
    auto at = std::make_shared<std::size_t>(0);
    return [values, at] { return values[(*at)++ % values.size()]; };
}
selection::Availability Owned(bool dlc) {
    selection::Availability availability; availability.ready = true; availability.costumes = dlc ? 1u : 0u;
    return availability;
}
bool Contains(const std::vector<int>& fighters, int id) { return std::find(fighters.begin(), fighters.end(), id) != fighters.end(); }

// Member 1 (this PC) at table 0's first seat, member 2 opposite, between games.
Snapshot Seated() {
    Snapshot room;
    room.roomEpoch = 7; room.localMember = 1;
    for (std::size_t i = 0; i < room.tables.size(); ++i) room.tables[i].id = static_cast<std::uint8_t>(i);
    room::Member local, other;
    local.id = 1; local.table = 0; local.seat = 0; local.fighter = 4;
    other.id = 2; other.table = 0; other.seat = 1; other.fighter = 9;
    room.members = {local, other};
    room.tables[0].p1 = 1; room.tables[0].p2 = 2; room.tables[0].phase = TablePhase::Waiting;
    return room;
}
void Play(Snapshot& room, std::uint64_t generation) {
    room.tables[0].matchGeneration = generation; room.tables[0].phase = TablePhase::Playing;
}
void ResultIn(Snapshot& room) { room.tables[0].phase = TablePhase::Waiting; }

void Candidates() {
    // Without DLC the sold-separately fighters (35-43 when unowned) are out; the rest are in.
    const auto base = selection::RandomCandidates([](int) { return Owned(false); }, false);
    EXPECT(!base.empty() && !Contains(base, 35) && !Contains(base, 43) && Contains(base, 0) && Contains(base, 34));
    const auto all = selection::RandomCandidates([](int) { return Owned(true); }, false);
    EXPECT(all.size() == static_cast<std::size_t>(selection::FighterCount));
    // Unknown availability (the game not read yet) locks nobody.
    EXPECT(selection::RandomCandidates({}, true).size() == static_cast<std::size_t>(selection::FighterCount));
    // Every candidate has a valid pick under the edition rule it was asked for.
    for (const bool editionSelect : {false, true})
        for (const int id : selection::RandomCandidates({}, editionSelect)) {
            selection::Pick pick; pick.fighter = id; selection::Normalize(pick, editionSelect);
            EXPECT(selection::Valid(pick, editionSelect));
        }
}

void Draw() {
    // The source picks the candidate: value % count, past the uneven low values.
    const std::vector<int> three{5, 8, 13};
    EXPECT(selection::DrawFighter(three, Sequence({7})) == 8);
    EXPECT(selection::DrawFighter(three, Sequence({9})) == 5);
    EXPECT(selection::DrawFighter({}, Sequence({1})) == -1);
    // 2^32 mod 3 is 1: a 0 is drawn again rather than favour the first candidate.
    EXPECT(selection::DrawFighter(three, Sequence({0, 5})) == 13);
    // Over many draws each candidate comes up about as often.
    std::mt19937 engine(2026);
    const auto fighters = selection::RandomCandidates({}, true);
    std::map<int, int> counts;
    for (int i = 0; i < 44000; ++i) ++counts[selection::DrawFighter(fighters, [&] { return static_cast<std::uint32_t>(engine()); })];
    EXPECT(counts.size() == fighters.size());
    for (const auto& count : counts) EXPECT_MSG(count.second > 800 && count.second < 1200, "fighter %d drawn %d times", count.first, count.second);
}

void Look() {
    // The rolled fighter plays in its remembered look, fitted to the rule.
    selection::Pick remembered; remembered.fighter = 3; remembered.costume = 1; remembered.color = 4; remembered.edition = 2;
    const auto withSelect = selection::RolledPick(3, remembered, true, nullptr);
    EXPECT(withSelect.fighter == 3 && withSelect.costume == 1 && withSelect.color == 4 && withSelect.edition == 2);
    const auto ultraOnly = selection::RolledPick(3, remembered, false, nullptr);
    EXPECT(ultraOnly.edition == selection::UltraEdition && selection::Valid(ultraOnly, false));
}

void WatchRolls() {
    room::RandomFighterWatch watch;
    auto room = Seated();
    // Chosen (or loaded): one roll, then none while the roll is unplayed.
    EXPECT(watch.Update(true, room, false));
    EXPECT(!watch.Update(true, room, false));
    // A game is played: no roll during it; one as soon as its WIN is on screen,
    // and none again while the WIN plays out or when the result comes in.
    Play(room, 11);
    EXPECT(!watch.Update(true, room, false));
    EXPECT(watch.Update(true, room, true));
    EXPECT(!watch.Update(true, room, true));
    ResultIn(room);
    EXPECT(!watch.Update(true, room, false));
    // The rematch is a game of its own: it rolls again.
    Play(room, 12);
    EXPECT(!watch.Update(true, room, false));
    ResultIn(room);
    EXPECT(watch.Update(true, room, false));
    // Off, nothing rolls; on again, it rolls at once.
    EXPECT(!watch.Update(false, room, false));
    EXPECT(watch.Update(true, room, false));
    // Training from Home plays the fighter in hand.
    watch.Played();
    EXPECT(watch.Update(true, room, false));
    // The result that rotates the player out of the seat still ends the game.
    Play(room, 13);
    EXPECT(!watch.Update(true, room, false));
    ResultIn(room); room.members[0].seat = room.members[0].table = -1; room.tables[0].p1 = 3;
    EXPECT(watch.Update(true, room, false));
}

void WatchKeeps() {
    // An Arcade room whose winners keep their fighters, first to 2.
    room::RandomFighterWatch watch;
    auto room = Seated();
    room.arcade = room::ArcadeRoom;
    room.tables[0].rules.winnerKeepsFighter = true; room.tables[0].rules.format = room::SetFormat::Ft2;
    EXPECT(watch.Update(true, room, false));
    // At 1-1 the game may end the set: the WIN waits for the result.
    room.tables[0].score[0] = room.tables[0].score[1] = 1;
    Play(room, 21);
    EXPECT(!watch.Update(true, room, true));
    // The result makes this player the keeper: the rolled fighter stays.
    ResultIn(room);
    room.tables[0].keeper = 1; room.tables[0].keptFighter = 4;
    room.tables[0].score[0] = room.tables[0].score[1] = 0;
    EXPECT(!watch.Update(true, room, false));
    // A game that cannot end the set is known at its WIN: still the keeper, no roll.
    Play(room, 22);
    EXPECT(!watch.Update(true, room, true));
    // Losing the set ends the keep: the next game's end rolls.
    ResultIn(room);
    room.tables[0].keeper = 2; room.tables[0].keptFighter = 9;
    Play(room, 23);
    EXPECT(!watch.Update(true, room, false));
    ResultIn(room);
    EXPECT(watch.Update(true, room, false));
}

void WatchNewRoom() {
    // Another room numbers its games afresh: its first game is not taken for one already over.
    room::RandomFighterWatch watch;
    auto room = Seated();
    EXPECT(watch.Update(true, room, false));
    Play(room, 1);
    EXPECT(!watch.Update(true, room, false));
    EXPECT(watch.Update(true, room, true));
    room.roomEpoch = 8; ResultIn(room);
    EXPECT(!watch.Update(true, room, false));
    Play(room, 1);
    EXPECT(!watch.Update(true, room, false));
    EXPECT(watch.Update(true, room, true));
}

void Show() {
    // The roll goes to the room host while the player sits between games, not ready.
    auto room = Seated();
    EXPECT(room::RandomRollToShow(room, 7));
    // The room already shows it: nothing to send.
    EXPECT(!room::RandomRollToShow(room, 4));
    // Readied (the Ready carried the pick), playing, or not seated: nothing.
    room.tables[0].ready[0] = true;
    EXPECT(!room::RandomRollToShow(room, 7));
    room.tables[0].ready[0] = false; Play(room, 3);
    EXPECT(!room::RandomRollToShow(room, 7));
    ResultIn(room); room.members[0].seat = -1;
    EXPECT(!room::RandomRollToShow(room, 7));
    // An opponent already ready (they readied before the roll arrived) does not
    // hold it back: this player's Ready would carry it anyway, and the room says
    // now, not then, that the matchup changed.
    room = Seated(); room.tables[0].ready[1] = true;
    EXPECT(room::RandomRollToShow(room, 7));
}
}

int main() {
    Candidates();
    Draw();
    Look();
    WatchRolls();
    WatchKeeps();
    WatchNewRoom();
    Show();
    return TEST_EXIT_CODE();
}
