#pragma once
#include "RoomModel.hxx"
#include <cstdint>

namespace sf4e { namespace room {
// Says when to call out that the other fighter at the local player's table
// readied while the local player has not: someone is waiting on them. Once
// per ready; a ready taken back and given again calls out again after a quiet
// time, so a player flicking it cannot spam the other.
class ReadyChime {
public:
    static constexpr std::uint64_t QuietMs = 4000;
    bool Update(const Snapshot& room, std::uint64_t nowMs) {
        const Table* at = nullptr;
        int seat = 0;
        for (const auto& table : room.tables) {
            if (!room.localMember) break;
            if (table.p1 == room.localMember || table.p2 == room.localMember) { at = &table; seat = table.p2 == room.localMember; break; }
        }
        const MemberId opponent = at ? (seat ? at->p1 : at->p2) : 0;
        const bool ready = opponent && at->ready[1 - seat];
        const bool fresh = ready && !(opponentReady_ && opponent == opponent_ && at->id == table_);
        const bool ring = fresh && !at->ready[seat] && (!rangAtMs_ || nowMs - rangAtMs_ >= QuietMs);
        opponent_ = opponent; table_ = at ? at->id : NoTable; opponentReady_ = ready;
        if (ring) rangAtMs_ = nowMs;
        return ring;
    }
private:
    static constexpr std::uint8_t NoTable = 0xff;
    MemberId opponent_ = 0;
    std::uint8_t table_ = NoTable;
    bool opponentReady_ = false;
    std::uint64_t rangAtMs_ = 0;
};
} }
