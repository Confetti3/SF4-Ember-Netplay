#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

// Go now on the call back from Training (docs/design/TRAINING_IN_ROOMS.md).
// The call is the room's call owner's (session/TrainingCall.hxx), and so is
// the decision to carry a go now out; a press is taken for go now once, where
// it arrives, and only for the call that stands then.
namespace sf4e { namespace input {

// One call: the room's epoch, the table, the opponent who sat down there and
// the Training battle it was sent to (its generation), and its own serial,
// which no other call shares. Two calls are the same only when all five are;
// no opponent is no call.
struct CallIdentity {
    std::uint64_t roomEpoch = 0, opponent = 0, generation = 0, serial = 0;
    std::uint8_t table = 0xff;
    bool Live() const { return opponent != 0; }
    bool operator==(const CallIdentity& other) const {
        return serial == other.serial && roomEpoch == other.roomEpoch && opponent == other.opponent && generation == other.generation && table == other.table;
    }
    bool operator!=(const CallIdentity& other) const { return !(*this == other); }
};

// Where go now is pressed. The game thread offers the call that stands each
// tick. A fresh press of go now's key (the window procedure's Enter) or button
// (the pad's View, TrainingPad.hxx) is taken in the same step that asks for
// it: taken, it is the call's alone, kept from the game and from Ember's menus,
// and it leaves a request tagged with that call. The game thread takes the
// requests and carries out only those for the call that still stands, so a
// press can neither go now for a call that has gone nor be kept from the game
// without asking for it.
class GoNowGate {
public:
    enum class Source : std::uint8_t { Keyboard, Pad };
    struct Request { CallIdentity call; Source source = Source::Keyboard; };
    static constexpr std::size_t MostRequests = 8;

    // Game thread: the call that stands, or none.
    void Offer(const CallIdentity& call) { std::lock_guard<std::mutex> lock(mutex_); offered_ = call; }
    CallIdentity Offered() const { std::lock_guard<std::mutex> lock(mutex_); return offered_; }
    // Any thread, for a fresh press. free: the press is nobody else's now (the
    // game's window has the focus, Ember's menu takes no input and the game's
    // pause menu is closed). True when it is taken for the call that stands.
    bool Press(Source source, bool free) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!free || !offered_.Live() || requests_.size() >= MostRequests) return false;
        requests_.push_back({offered_, source});
        return true;
    }
    // Game thread: the requests taken since the last call, in order.
    std::vector<Request> Take() {
        std::vector<Request> taken;
        std::lock_guard<std::mutex> lock(mutex_);
        taken.swap(requests_);
        return taken;
    }
private:
    mutable std::mutex mutex_;
    CallIdentity offered_;
    std::vector<Request> requests_;
};
// What the call's lifecycle asks the Training battle for, each for one call:
// Leave starts its countdown, Stay ends it, LeaveNow shortens it.
enum class CallOrder : std::uint8_t { Leave, Stay, LeaveNow };
// The Training battle as the game thread sees it now: running in Training
// and not leaving, and its generation.
struct CallBattle { bool running = false; std::uint64_t generation = 0; };

// The call's lifecycle, one game tick at a time and in one order:
// 1. a call that ended, or was replaced by another, has its countdown ended;
// 2. the call that stands has its countdown started, once;
// 3. only once that start is queued, and only while the battle it was sent
//    to still runs in Training (not leaving, not closed, not another one),
//    is the call offered to go now's gate; otherwise nothing is offered and
//    Enter and View are the game's again;
// 4. the presses taken for exactly the offered call go now.
// So a press is never taken for a countdown not yet queued, a battle that
// has gone, or a call that is not the one counting. order(kind, call) queues
// one order and says whether it was queued; a start not queued is asked
// again next tick. opened: the call ended by reaching the main menu, where
// its battle has left and there is nothing to end.
class CallLifecycle {
public:
    template<class Order>
    void Tick(const CallIdentity& before, const CallIdentity& after, bool opened, const CallBattle& battle, GoNowGate& gate, const Order& order) {
        if (before.Live() && before != after && !opened) order(CallOrder::Stay, before);
        if (after.Live() && started_ != after.serial && order(CallOrder::Leave, after)) started_ = after.serial;
        CallIdentity offer;
        if (after.Live() && started_ == after.serial && battle.running && battle.generation == after.generation) offer = after;
        gate.Offer(offer);
        for (const auto& request : gate.Take()) {
            if (offer.Live() && request.call == offer) order(CallOrder::LeaveNow, offer);
            else ++dropped_;
        }
    }
    // Presses taken for a call that was no longer offered, for the log.
    std::uint64_t Dropped() const { return dropped_; }
private:
    std::uint64_t started_ = 0, dropped_ = 0;
};

// The game's one gate, shared by the window procedure, the pad and the runtime.
inline GoNowGate& TrainingGoNow() { static GoNowGate gate; return gate; }

} }
