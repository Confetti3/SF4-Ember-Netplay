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

// The Training battle as the game thread sees it now: running in Training
// and not leaving, and its generation.
struct CallBattle { bool running = false; std::uint64_t generation = 0; };
// What the battle is told of the call, as state it reads before every
// countdown tick (training::CallControl): the call that stands, or none, and
// whether go now was chosen for it.
struct CallState { CallIdentity call; bool hurry = false; };

// The call's lifecycle, one game tick at a time. The call that stands is
// the battle's to follow, and so is a go now chosen for it: both are state
// the caller publishes each tick, never orders a full queue could refuse, so
// a call that ended stops its countdown and a press taken for a call takes
// effect once that call's countdown runs, however busy the battle's command
// queue is. The call is offered to go now's gate only while the battle it was
// sent to still runs in Training (not leaving, not closed, not another one);
// otherwise Enter and View are the game's again. A press taken for exactly
// the offered call is that call's go now; any other is dropped.
class CallLifecycle {
public:
    CallState Tick(const CallIdentity& call, const CallBattle& battle, GoNowGate& gate) {
        if (call.serial != state_.call.serial || !call.Live()) { state_.call = call.Live() ? call : CallIdentity{}; state_.hurry = false; }
        CallIdentity offer;
        if (call.Live() && battle.running && battle.generation == call.generation) offer = call;
        gate.Offer(offer);
        for (const auto& request : gate.Take()) {
            if (offer.Live() && request.call == offer) state_.hurry = true;
            else ++dropped_;
        }
        return state_;
    }
    // Presses taken for a call that was no longer offered, for the log.
    std::uint64_t Dropped() const { return dropped_; }
private:
    CallState state_;
    std::uint64_t dropped_ = 0;
};

// The game's one gate, shared by the window procedure, the pad and the runtime.
inline GoNowGate& TrainingGoNow() { static GoNowGate gate; return gate; }

} }
