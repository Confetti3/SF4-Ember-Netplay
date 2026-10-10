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
// the Training battle it was sent to (its generation). Two calls are the same
// only when all four are; no opponent is no call.
struct CallIdentity {
    std::uint64_t roomEpoch = 0, opponent = 0, generation = 0;
    std::uint8_t table = 0xff;
    bool Live() const { return opponent != 0; }
    bool operator==(const CallIdentity& other) const {
        return roomEpoch == other.roomEpoch && opponent == other.opponent && generation == other.generation && table == other.table;
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
// The game's one gate, shared by the window procedure, the pad and the runtime.
inline GoNowGate& TrainingGoNow() { static GoNowGate gate; return gate; }

} }
