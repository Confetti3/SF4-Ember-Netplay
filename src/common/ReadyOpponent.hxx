#pragma once
#include <cstdint>

namespace sf4e { namespace room {
// A Ready press consents to this opponent, not to whichever character happens
// to occupy the table when a delayed request finally arrives. Room revisions,
// our own fighter selection and cosmetic changes do not change that consent.
struct ReadyOpponent {
    std::uint64_t epoch = 0, local = 0, opponent = 0;
    int table = -1, seat = -1, fighter = -1;
    bool operator==(const ReadyOpponent& other) const {
        return epoch == other.epoch && local == other.local && opponent == other.opponent &&
            table == other.table && seat == other.seat && fighter == other.fighter;
    }
    bool operator!=(const ReadyOpponent& other) const { return !(*this == other); }
};
// Also usable with the small, game-independent snapshot fixtures. A legacy
// lobby has no room epoch and retains its existing Ready behaviour.
template<class Snapshot> ReadyOpponent CaptureReadyOpponent(const Snapshot& snapshot) {
    ReadyOpponent result;
    if (!snapshot.roomEpoch) return result;
    result.epoch = snapshot.roomEpoch; result.local = snapshot.localMember;
    if (!result.local) return result;
    for (const auto& table : snapshot.tables) {
        if (table.p1 != result.local && table.p2 != result.local) continue;
        result.table = table.id; result.seat = table.p1 == result.local ? 0 : 1;
        result.opponent = result.seat == 0 ? table.p2 : table.p1;
        for (const auto& member : snapshot.members)
            if (member.id == result.opponent) { result.fighter = member.fighter; break; }
        break;
    }
    return result;
}
} }
