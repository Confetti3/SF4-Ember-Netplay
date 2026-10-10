#pragma once
#include "RoomModel.hxx"
#include "../common/TrainingCallInput.hxx"
#include <cstdint>

namespace sf4e { namespace room {
// Lets a player in a room wait in the game's Training mode. It says when a
// challenger is waiting for them, which takes them out of Training, and holds
// the time they then have to ready: a player called out of Training who does
// not ready gives the seat up, so the one waiting is never kept there.
//
// The steps, each returned once:
// - Call: the local player is in Training and another fighter now sits
//   opposite them at their table. The battle is to be sent to the main menu.
// - Open: they are back at the main menu. The ready window starts and the
//   table is to be shown.
// - Ready: the window is open, readying is possible and the player has asked
//   for it to be done for them.
// - Forfeit: the window ran out without a Ready. The seat is to be given up.
// The window ends without a step when the player readies, when the opponent
// leaves, or when the table starts or stops being theirs.
class TrainingCall {
public:
    static constexpr std::uint64_t ReadyWindowMs = 15000;
    // How long a call may wait for the game to reach the main menu before it
    // is forgotten: the battle did not leave, and nothing is taken from the
    // player for that.
    static constexpr std::uint64_t ArrivalMs = 20000;
    enum class Step { None, Call, Open, Ready, Forfeit };
    struct Input {
        bool inTraining = false;   // a Training battle is running
        bool atMainMenu = false;
        bool canReady = false;     // the runtime's own gate for a Ready
        bool autoAccept = false;   // the player's choice to be readied at once
        std::uint64_t generation = 0;  // the Training battle's (training::View)
    };
    // Whether the local player may go to Training from the room: a member
    // who is not readied, not fighting or about to, and whom nobody is
    // waiting for yet. A place in a queue or a seat with the other one empty
    // is no obstacle; that is what the call is for. With a fighter already
    // opposite, the call would come the moment the battle began.
    static bool MayTrain(const Snapshot& room) {
        if (!room.localMember) return false;
        int seat = 0;
        const Table* at = LocalTable(room, seat);
        if (!at) return true;
        const MemberId opponent = seat ? at->p1 : at->p2;
        return !opponent && !at->ready[seat] && (at->phase == TablePhase::Idle || at->phase == TablePhase::Waiting);
    }
    Step Update(const Snapshot& room, const Input& in, std::uint64_t nowMs) {
        int seat = 0;
        const Table* at = LocalTable(room, seat);
        const MemberId opponent = at ? (seat ? at->p1 : at->p2) : 0;
        const bool open = at && (at->phase == TablePhase::Idle || at->phase == TablePhase::Waiting);
        const bool waiting = opponent && open && !at->ready[seat];
        if (state_ != State::Idle && (!waiting || at->id != table_ || opponent != opponent_ || room.roomEpoch != roomEpoch_)) Reset();
        if (!waiting) return Step::None;
        switch (state_) {
        case State::Idle:
            if (!in.inTraining) return Step::None;
            state_ = State::Called; table_ = at->id; opponent_ = opponent; sinceMs_ = nowMs;
            roomEpoch_ = room.roomEpoch; generation_ = in.generation;
            return Step::Call;
        case State::Called:
            if (in.atMainMenu) { state_ = State::Window; sinceMs_ = nowMs; readied_ = false; return Step::Open; }
            if (nowMs - sinceMs_ >= ArrivalMs) Reset();
            return Step::None;
        case State::Window:
            if (nowMs - sinceMs_ >= ReadyWindowMs) { Reset(); return Step::Forfeit; }
            if (in.autoAccept && in.canReady && !readied_) { readied_ = true; return Step::Ready; }
            return Step::None;
        }
        return Step::None;
    }
    // Milliseconds left to ready, 0 when no window is open.
    std::uint64_t Remaining(std::uint64_t nowMs) const {
        if (state_ != State::Window) return 0;
        const std::uint64_t spent = nowMs - sinceMs_;
        return spent >= ReadyWindowMs ? 0 : ReadyWindowMs - spent;
    }
    bool Called() const { return state_ == State::Called; }
    // The call while it stands, from Call until Open or until it ends
    // without one: who it is for and the Training battle it was sent to.
    input::CallIdentity Identity() const {
        input::CallIdentity call;
        if (state_ != State::Called) return call;
        call.roomEpoch = roomEpoch_; call.table = table_; call.opponent = opponent_; call.generation = generation_;
        return call;
    }
    void Reset() { state_ = State::Idle; table_ = NoTable; opponent_ = 0; sinceMs_ = 0; readied_ = false; roomEpoch_ = 0; generation_ = 0; }
private:
    enum class State : std::uint8_t { Idle, Called, Window };
    static constexpr std::uint8_t NoTable = 0xff;
    // The table the local player fights at, and which of its seats is theirs.
    static const Table* LocalTable(const Snapshot& room, int& seat) {
        if (!room.localMember) return nullptr;
        for (const auto& table : room.tables)
            if (table.p1 == room.localMember || table.p2 == room.localMember) { seat = table.p2 == room.localMember; return &table; }
        return nullptr;
    }
    State state_ = State::Idle;
    std::uint8_t table_ = NoTable;
    MemberId opponent_ = 0;
    std::uint64_t roomEpoch_ = 0, generation_ = 0;
    std::uint64_t sinceMs_ = 0;
    bool readied_ = false;
};
} }
