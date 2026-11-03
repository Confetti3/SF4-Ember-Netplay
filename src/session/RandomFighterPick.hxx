#pragma once
#include "RoomModel.hxx"
#include "../common/RandomFighter.hxx"
#include <cstdint>
#include <functional>

namespace sf4e { namespace room {
// The next game's fighter for a Random pick (common/RandomFighter.hxx), owned
// in one place: the runtime keeps one, every Ready it sends takes its fighter
// from it (Sent), and the overlay shows the fighter it rolled.
//
// The fighter is always one not yet played. It rolls when Random is chosen or
// loaded, and again once each game this player fights at a room table has its
// result in the room, judged by the room's own settled state: a winner who
// now keeps their fighter (KeepsFighter) keeps it instead of rolling.
class RandomFighterPick {
public:
    // A fresh fighter, or -1 when none can be played.
    using Roll = std::function<int()>;
    // Each tick, with whether the pick is Random and the room as it is now.
    void Update(bool random, const Snapshot& room, const Roll& roll) {
        if (room.roomEpoch != epoch_) { epoch_ = room.roomEpoch; game_ = ended_ = 0; }
        if (!random) { fighter_ = -1; game_ = 0; return; }
        const auto place = PlaceOf(room, room.localMember);
        if (place.kind == Place::Kind::Seat) {
            const Table& seat = room.tables[place.table];
            if (Live(seat) && seat.matchGeneration != ended_) { game_ = seat.matchGeneration; table_ = seat.id; }
        }
        bool owed = fighter_ < 0;
        if (game_ && ResultIn(room)) { ended_ = game_; game_ = 0; owed = true; }
        // A keeper plays on with the fighter they won with.
        if (!owed || (fighter_ >= 0 && KeepsFighter(room, room.localMember))) return;
        fighter_ = roll();
        ++serial_;
    }
    // The fighter was played outside a room (Training from Home): the next
    // Update rolls.
    void Played() { fighter_ = -1; }
    // The rolled fighter (-1 before a roll), and a count of rolls that moves
    // with each new one.
    int Fighter() const { return fighter_; }
    std::uint64_t Serial() const { return serial_; }
    // What a Ready sends: `pressed`, the pick it was pressed with, while that
    // is the rolled fighter; else the rolled fighter in its remembered look
    // (RolledPick). A Ready parked over a WIN thus sends the roll made once
    // the game's result came in, not the fighter it was pressed with.
    selection::Pick Sent(const selection::Pick& pressed, const selection::Pick& remembered, bool editionSelect,
                         const selection::Availability* availability, const selection::InstalledContent* installed = nullptr) const {
        if (fighter_ < 0 || pressed.fighter == fighter_) return pressed;
        return selection::RolledPick(fighter_, remembered, editionSelect, availability, installed);
    }

private:
    static bool Live(const Table& table) { return table.phase == TablePhase::Playing || table.phase == TablePhase::Paused; }
    // The room has the result of the game this PC fights: its table left
    // Playing and Paused, or went on to another game.
    bool ResultIn(const Snapshot& room) const {
        if (table_ >= room.tables.size()) return true;
        const Table& table = room.tables[table_];
        return table.matchGeneration != game_ || !Live(table);
    }
    int fighter_ = -1;
    std::uint64_t serial_ = 0, epoch_ = 0;
    // The game this PC fights while its result is not in, its table, and the
    // last game whose result is.
    std::uint64_t game_ = 0, ended_ = 0;
    std::uint8_t table_ = 0;
};

// Whether the room host should be shown `fighter` for the local player now
// (SessionClient::PreBattle_SetChara): they sit at a table between games, have
// not readied, and the room shows another fighter for them. A Random roll goes
// out then, before the opponent's next Ready, which the change would
// otherwise take back (RoomAuthority::SetMemberChara).
inline bool RandomRollToShow(const Snapshot& room, int fighter) {
    const auto place = PlaceOf(room, room.localMember);
    const Member* local = FindMember(room, room.localMember);
    if (fighter < 0 || place.kind != Place::Kind::Seat || !local || local->fighter == fighter) return false;
    const Table& table = room.tables[place.table];
    return table.phase == TablePhase::Waiting && !table.ready[place.seat];
}
} }
