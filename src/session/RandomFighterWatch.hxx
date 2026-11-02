#pragma once
#include "RoomModel.hxx"
#include <algorithm>
#include <cstdint>

namespace sf4e { namespace room {
// When a Random pick (common/RandomFighter.hxx) rolls. The fighter in hand is
// always one not yet played: Random rolls when it is chosen or loaded with
// the saved picks, and again as soon as each game this player fights at a
// room table is over, so a seat taken finds a fresh roll and the next game's
// fighter is known before anyone can Ready for it.
//
// A game is over for the roll when its WIN is on screen or the room has its
// result. Where winners keep their fighters (KeepsFighter), a game that may
// end the set waits for the result, which says who keeps theirs; a keeper's
// fighter stays for the next game instead of rolling.
class RandomFighterWatch {
public:
    // True when the pick should roll now; the caller rolls, and the roll is
    // the fresh fighter from then on. `random`: the pick is Random.
    // `winShown`: the WIN of the game this PC fights is on screen.
    bool Update(bool random, const Snapshot& room, bool winShown) {
        if (room.roomEpoch != epoch_) { epoch_ = room.roomEpoch; game_ = ended_ = 0; }
        if (!random) { fresh_ = false; game_ = 0; return false; }
        const auto place = PlaceOf(room, room.localMember);
        if (place.kind == Place::Kind::Seat) {
            const Table& seat = room.tables[place.table];
            if (Live(seat) && seat.matchGeneration != ended_) { game_ = seat.matchGeneration; table_ = seat.id; }
        }
        if (game_ && Over(room, winShown)) { ended_ = game_; game_ = 0; fresh_ = false; }
        // A keeper plays on with the fighter they won with.
        if (KeepsFighter(room, room.localMember)) fresh_ = true;
        if (fresh_) return false;
        fresh_ = true;
        return true;
    }
    // The fighter in hand was played outside a room (Training from Home):
    // the next Update rolls.
    void Played() { fresh_ = false; }

private:
    static bool Live(const Table& table) { return table.phase == TablePhase::Playing || table.phase == TablePhase::Paused; }
    bool Over(const Snapshot& room, bool winShown) const {
        if (table_ >= room.tables.size()) return true;
        const Table& table = room.tables[table_];
        if (table.matchGeneration != game_ || !Live(table)) return true;
        if (!winShown) return false;
        // Over the WIN, a keeper is known unless this game may have ended the set.
        if (!room.arcade || !table.rules.winnerKeepsFighter) return true;
        const auto goal = static_cast<std::uint32_t>(table.rules.format);
        return table.rules.format != SetFormat::Unlimited && (std::max)(table.score[0], table.score[1]) + 1 < goal;
    }
    bool fresh_ = false;
    std::uint64_t epoch_ = 0;
    // The game this PC fights while it is not over for the roll, its table,
    // and the last game that was.
    std::uint64_t game_ = 0, ended_ = 0;
    std::uint8_t table_ = 0;
};

// Whether the room host should be shown `fighter` for the local player now
// (SessionClient::PreBattle_SetChara): they sit at a table between games, have
// not readied, and the room shows another fighter for them. A Random roll goes
// out then, so a Ready the opponent gives after it is never taken back by the
// change (RoomAuthority::SetMemberChara).
inline bool RandomRollToShow(const Snapshot& room, int fighter) {
    const auto place = PlaceOf(room, room.localMember);
    const Member* local = FindMember(room, room.localMember);
    if (place.kind != Place::Kind::Seat || !local || local->fighter == fighter) return false;
    const Table& table = room.tables[place.table];
    return table.phase == TablePhase::Waiting && !table.ready[place.seat];
}
} }
