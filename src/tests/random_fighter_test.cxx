// Random on the fighter grid: which fighters a roll may land on, that every
// one is as likely, when the pick rolls again (once per game), and what a
// Ready parked over a WIN sends once the game's result is in.
#include "../common/RandomFighter.hxx"
#include "../session/RandomFighterPick.hxx"

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

// Rolls `fighters` in turn, counting the rolls.
struct Rolls {
    std::vector<int> fighters; std::size_t made = 0;
    room::RandomFighterPick::Roll Source() { return [this] { return fighters[made++ % fighters.size()]; }; }
};
selection::Pick Picked(int fighter, int costume = 0) { selection::Pick pick; pick.fighter = fighter; pick.costume = costume; return pick; }
// What the Ready sends for a pick pressed as `pressed`, the rolled fighter's
// remembered look being costume 2.
selection::Pick Sent(const room::RandomFighterPick& pick, const selection::Pick& pressed) {
    return pick.Sent(pressed, Picked(pick.Fighter(), 2), true, nullptr);
}
// An Arcade room whose winners keep their fighters, first to 2, with no keeper.
Snapshot KeepersTable() {
    auto room = Seated();
    room.arcade = room::ArcadeRoom;
    room.tables[0].rules.winnerKeepsFighter = true; room.tables[0].rules.format = room::SetFormat::Ft2;
    return room;
}

void PickRolls() {
    room::RandomFighterPick pick; Rolls rolls{{5, 12, 20, 30}};
    auto room = Seated();
    // Chosen (or loaded): one roll, then none while it is unplayed.
    pick.Update(true, room, rolls.Source());
    EXPECT(pick.Fighter() == 5 && pick.Serial() == 1);
    pick.Update(true, room, rolls.Source());
    EXPECT(rolls.made == 1);
    // A game: no roll while it is played or its WIN is held; one when the room has its result.
    Play(room, 11);
    pick.Update(true, room, rolls.Source());
    EXPECT(rolls.made == 1);
    ResultIn(room);
    pick.Update(true, room, rolls.Source());
    EXPECT(pick.Fighter() == 12 && pick.Serial() == 2);
    pick.Update(true, room, rolls.Source());
    EXPECT(rolls.made == 2);
    // Off, no fighter; on again, a roll at once.
    pick.Update(false, room, rolls.Source());
    EXPECT(pick.Fighter() == -1);
    pick.Update(true, room, rolls.Source());
    EXPECT(pick.Fighter() == 20);
    // Training from Home plays the fighter in hand.
    pick.Played();
    pick.Update(true, room, rolls.Source());
    EXPECT(pick.Fighter() == 30);
    // The result that rotates the player out of the seat still ends the game.
    Play(room, 13);
    pick.Update(true, room, rolls.Source());
    ResultIn(room); room.members[0].seat = room.members[0].table = -1; room.tables[0].p1 = 3;
    pick.Update(true, room, rolls.Source());
    EXPECT(rolls.made == 5);
}

// The whole Rematch over a WIN: the result is held over the WIN, Rematch parks
// a Ready pressed with the fighter just played, the battle closes, the room's
// result comes in, the roll resolves, and the parked Ready sends the roll.
void ParkedRematch(const char* name, int score0, int score1, int keeper, bool rollsAgain) {
    room::RandomFighterPick pick; Rolls rolls{{5, 12}};
    auto room = KeepersTable();
    pick.Update(true, room, rolls.Source());
    room.tables[0].score[0] = 1; room.tables[0].score[1] = 0;
    Play(room, 31);
    pick.Update(true, room, rolls.Source());
    const auto parked = Picked(5, 1);
    // Held over the WIN, and while the battle closes, the room is still playing the game.
    pick.Update(true, room, rolls.Source());
    EXPECT_MSG(Sent(pick, parked).fighter == 5, "%s: rolled before the result", name);
    ResultIn(room);
    room.tables[0].score[0] = score0; room.tables[0].score[1] = score1;
    if (keeper) { room.tables[0].keeper = keeper; room.tables[0].keptFighter = keeper == 1 ? 5 : 9; }
    pick.Update(true, room, rolls.Source());
    const auto sent = Sent(pick, parked);
    if (rollsAgain) EXPECT_MSG(sent.fighter == 12 && sent.costume == 2, "%s: the parked Ready sent fighter %d", name, sent.fighter);
    else EXPECT_MSG(sent.fighter == 5 && sent.costume == 1, "%s: the parked Ready sent fighter %d", name, sent.fighter);
}

void ParkedRematches() {
    // 1-0 down, the player wins: 1-1, the set goes on, nobody keeps: a new fighter.
    ParkedRematch("trailing win", 1, 1, 0, true);
    // A draw at set point: the score stays, nobody keeps: a new fighter.
    ParkedRematch("draw at set point", 1, 0, 0, true);
    // The player wins the set and keeps the fighter they won with.
    ParkedRematch("set won", 0, 0, 1, false);
    // The opponent wins the set: they keep theirs; this player rolls.
    ParkedRematch("set lost", 0, 0, 2, true);
}

void PickSends() {
    room::RandomFighterPick pick; Rolls rolls{{5}};
    // No roll yet: the Ready sends what it was pressed with.
    EXPECT(Sent(pick, Picked(8, 1)).fighter == 8);
    pick.Update(true, Seated(), rolls.Source());
    // Pressed with the rolled fighter: its look as pressed (edited on its page).
    const auto same = Sent(pick, Picked(5, 1));
    EXPECT(same.fighter == 5 && same.costume == 1);
}

void PickNewRoom() {
    // Another room numbers its games afresh: its first game is not taken for one already over.
    room::RandomFighterPick pick; Rolls rolls{{1, 2, 3}};
    auto room = Seated();
    pick.Update(true, room, rolls.Source());
    Play(room, 1);
    pick.Update(true, room, rolls.Source());
    ResultIn(room);
    pick.Update(true, room, rolls.Source());
    EXPECT(rolls.made == 2);
    room.roomEpoch = 8; Play(room, 1);
    pick.Update(true, room, rolls.Source());
    EXPECT(rolls.made == 2);
    ResultIn(room);
    pick.Update(true, room, rolls.Source());
    EXPECT(rolls.made == 3);
}

void Show() {
    // The roll goes to the room host while the player sits between games, not ready.
    auto room = Seated();
    EXPECT(room::RandomRollToShow(room, 7) && !room::RandomRollToShow(room, -1));
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
    PickRolls();
    ParkedRematches();
    PickSends();
    PickNewRoom();
    Show();
    return TEST_EXIT_CODE();
}
