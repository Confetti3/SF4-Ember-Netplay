#pragma once
#include "RoomModel.hxx"
#include "../common/TrainingCallInput.hxx"
#include <algorithm>
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
//   A player who goes back into a Training battle while the window is open
//   is called again, and keeps the time they had left.
// - Open: they are back at the main menu. The ready window starts, or goes
//   on, and the table is to be shown.
// - Ready: the window is open, readying is possible and the player has asked
//   for it to be done for them.
// - Forfeit: the window ran out without a Ready. The seat is to be given up.
// The window's time runs only while the player is at the main menu, out of
// Ember's fighter selector and with no Ready of theirs on its way: choosing a
// fighter or a Ready the room is still taking costs them nothing. However the
// time is held, the seat is given up WindowCapMs after they were first back.
// The window ends without a step when the player readies, when the opponent
// leaves, or when the table starts or stops being theirs.
class TrainingCall {
public:
    static constexpr std::uint64_t ReadyWindowMs = 15000;
    // The most a window can be held, from the first time the player is back
    // at the main menu, so the one waiting is never kept there for long.
    static constexpr std::uint64_t WindowCapMs = 60000;
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
        bool choosingFighter = false;  // Ember's fighter selector is on screen
        bool readyInFlight = false;    // a Ready of theirs is sent or parked, not yet in the room
        std::uint64_t generation = 0;  // the Training battle's (training::View)
    };
    // Whether the local player may go to Training from the room: a member
    // who is not readied, not fighting or about to, and whom nobody is
    // waiting for yet. A place in a queue or a seat with the other one empty
    // is no obstacle; that is what the call is for. With a fighter already
    // opposite, the call would come the moment the battle began. An Arcade
    // fighter who asked for a new opponent in a full room (Table::wantsNew)
    // still has the old one opposite, but waits for someone else: Training
    // is theirs too, and the call comes once the new opponent sits there.
    static bool MayTrain(const Snapshot& room) {
        if (!room.localMember) return false;
        int seat = 0;
        const Table* at = LocalTable(room, seat);
        if (!at) return true;
        const MemberId opponent = seat ? at->p1 : at->p2;
        return (!opponent || AskedNew(*at, seat)) && !at->ready[seat] && (at->phase == TablePhase::Idle || at->phase == TablePhase::Waiting);
    }
    static bool AskedNew(const Table& table, int seat) { return table.deciding && table.wantsNew[seat]; }
    Step Update(const Snapshot& room, const Input& in, std::uint64_t nowMs) {
        int seat = 0;
        const Table* at = LocalTable(room, seat);
        const MemberId opponent = at ? (seat ? at->p1 : at->p2) : 0;
        const bool open = at && (at->phase == TablePhase::Idle || at->phase == TablePhase::Waiting);
        const bool waiting = opponent && open && !at->ready[seat] && !AskedNew(*at, seat);
        if (state_ != State::Idle && (!waiting || at->id != table_ || opponent != opponent_ || room.roomEpoch != roomEpoch_)) Reset();
        if (!waiting) return Step::None;
        // Once the player has been back at the main menu, the cap ends the
        // call whatever it is doing: called again from Training or waiting.
        if (openedMs_ && nowMs - openedMs_ >= WindowCapMs) { Reset(); return Step::Forfeit; }
        switch (state_) {
        case State::Idle:
            if (!in.inTraining) return Step::None;
            state_ = State::Called; table_ = at->id; opponent_ = opponent; sinceMs_ = nowMs;
            roomEpoch_ = room.roomEpoch; generation_ = in.generation; serial_ = ++calls_;
            return Step::Call;
        case State::Called:
            if (in.atMainMenu) {
                state_ = State::Window; lastMs_ = nowMs; readied_ = false;
                if (!openedMs_) openedMs_ = nowMs;
                return Step::Open;
            }
            // Called again from the window, the call is never forgotten: only
            // the cap above ends it.
            if (!openedMs_ && nowMs - sinceMs_ >= ArrivalMs) Reset();
            return Step::None;
        case State::Window:
            if (in.inTraining) {
                state_ = State::Called; sinceMs_ = nowMs; generation_ = in.generation; serial_ = ++calls_;
                return Step::Call;
            }
            if (in.atMainMenu && !in.choosingFighter && !in.readyInFlight) spentMs_ += nowMs - lastMs_;
            lastMs_ = nowMs;
            if (spentMs_ >= ReadyWindowMs) { Reset(); return Step::Forfeit; }
            if (in.autoAccept && in.canReady && !readied_) { readied_ = true; return Step::Ready; }
            return Step::None;
        }
        return Step::None;
    }
    // Milliseconds left to ready, 0 when no window is open.
    std::uint64_t Remaining(std::uint64_t nowMs) const {
        if (state_ != State::Window) return 0;
        const std::uint64_t open = nowMs - openedMs_;
        if (spentMs_ >= ReadyWindowMs || open >= WindowCapMs) return 0;
        return (std::min)(ReadyWindowMs - spentMs_, WindowCapMs - open);
    }
    bool Called() const { return state_ == State::Called; }
    // The call while it stands, from Call until Open or until it ends
    // without one: who it is for and the Training battle it was sent to.
    input::CallIdentity Identity() const {
        input::CallIdentity call;
        if (state_ != State::Called) return call;
        call.roomEpoch = roomEpoch_; call.table = table_; call.opponent = opponent_; call.generation = generation_; call.serial = serial_;
        return call;
    }
    void Reset() {
        state_ = State::Idle; table_ = NoTable; opponent_ = 0; sinceMs_ = 0; readied_ = false; roomEpoch_ = 0; generation_ = 0; serial_ = 0;
        openedMs_ = 0; lastMs_ = 0; spentMs_ = 0;
    }
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
    // Each call has its own number, never reused: serial_ is the standing
    // call's, calls_ the last given.
    std::uint64_t serial_ = 0, calls_ = 0;
    // When the call was made (Called).
    std::uint64_t sinceMs_ = 0;
    // The window: when the player was first back at the main menu, the last
    // update, and the window time spent so far.
    std::uint64_t openedMs_ = 0, lastMs_ = 0, spentMs_ = 0;
    bool readied_ = false;
};

// The call's one owner: the call itself, and what the battle is told of it.
// Reconcile reads the room as it stands now, then sets the call that stands
// and any go now taken for it as the state the battle's countdown follows
// (TrainingSession.hxx: LeaveCountdown::Follow). The runtime reconciles once
// every outer tick, at one place: after the room's incoming updates
// (SessionClient::Step) and before the next battle update. So the countdown
// never runs for a call the room has already ended or replaced, and a go now
// taken for a call that was replaced is dropped rather than hurrying the next.
class CallOwner {
public:
    struct Result { TrainingCall::Step step = TrainingCall::Step::None; input::CallState state; };
    Result Reconcile(const Snapshot& room, const TrainingCall::Input& in, std::uint64_t nowMs, input::GoNowGate& gate) {
        Result result;
        result.step = call_.Update(room, in, nowMs);
        input::CallBattle battle; battle.running = in.inTraining; battle.generation = in.generation;
        result.state = lifecycle_.Tick(call_.Identity(), battle, gate);
        return result;
    }
    std::uint64_t Remaining(std::uint64_t nowMs) const { return call_.Remaining(nowMs); }
    input::CallIdentity Identity() const { return call_.Identity(); }
    // Presses taken for a call that was no longer offered, for the log.
    std::uint64_t Dropped() const { return lifecycle_.Dropped(); }
private:
    TrainingCall call_;
    input::CallLifecycle lifecycle_;
};
} }
