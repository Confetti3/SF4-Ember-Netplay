#pragma once
#include "RecordingTypes.hxx"
#include <array>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

// The dummy's recording slots and the choice of which one plays. Pure, so the
// session, the runtime and the tests share it.
namespace sf4e { namespace training {
// How a slot is chosen among those that are on.
enum class Order { Random, InOrder };
// Slots as bits, slot 0 the lowest.
using SlotSet = unsigned;
inline bool InSet(SlotSet set, int slot) { return slot >= 0 && slot < SlotCount && (set >> slot & 1u) != 0; }
// The eight slots, whether each is on, the order they play in, and a
// revision that changes with any of them. A slot that gets a recording starts
// on; an empty one is off and cannot be turned on.
class RecordingBank {
public:
    const std::vector<Input>& Slot(int slot) const { return slots_[slot]; }
    bool On(int slot) const { return on_[slot]; }
    Order GetOrder() const { return order_; }
    std::uint64_t Revision() const { return revision_; }
    // The slot's frames, at most MaxFrames of them; none clears it.
    void Replace(int slot, std::vector<Input> frames) {
        if (frames.size() > static_cast<std::size_t>(MaxFrames)) frames.resize(MaxFrames);
        on_[slot] = !frames.empty(); slots_[slot] = std::move(frames); ++revision_;
    }
    void MirrorSlot(int slot) {
        for (auto& frame : slots_[slot]) frame = Mirror(frame);
        ++revision_;
    }
    bool SetOn(int slot, bool on) {
        if (slots_[slot].empty()) return false;
        if (on_[slot] != on) { on_[slot] = on; ++revision_; }
        return true;
    }
    void SetOrder(Order order) { if (order_ != order) { order_ = order; ++revision_; } }
    // Manual playback: on and not empty.
    SlotSet Playable() const {
        SlotSet set = 0;
        for (int slot = 0; slot < SlotCount; ++slot) if (on_[slot] && !slots_[slot].empty()) set |= 1u << slot;
        return set;
    }
    // A recorded reply: on, with a pressed frame to start from (ReplyStart),
    // so a recording that only stands can be played but never holds up a reply.
    SlotSet Replyable() const {
        SlotSet set = 0;
        for (int slot = 0; slot < SlotCount; ++slot) if (on_[slot] && ReplyStart(slots_[slot]) >= 0) set |= 1u << slot;
        return set;
    }
private:
    std::array<std::vector<Input>, SlotCount> slots_;
    std::array<bool, SlotCount> on_{};
    Order order_ = Order::Random;
    std::uint64_t revision_ = 0;
};
// Which slot of a set plays next, one picker per operation (manual playback,
// the dummy's reply). Peek proposes; Commit is called only once the proposed
// slot starts playing, and only it moves the in-order cursor on. At random
// each Peek is a new draw, one per decision, so a proposal that never starts
// leaves nothing behind. A different set from the last one starts the order
// again from its first slot.
class SlotPicker {
public:
    explicit SlotPicker(unsigned seed = 1) : random_(seed) {}
    // -1 when the set is empty. weights: one per slot for the random draw, each 1 without.
    int Peek(SlotSet eligible, Order order, const int* weights = nullptr) {
        if (eligible != set_) { set_ = eligible; next_ = 0; }
        if (!eligible) return -1;
        if (order == Order::InOrder) {
            for (int step = 0; step < SlotCount; ++step) {
                const int slot = (next_ + step) % SlotCount;
                if (InSet(eligible, slot)) return slot;
            }
        }
        const auto weight = [&](int slot) { return InSet(eligible, slot) ? weights ? static_cast<unsigned>(weights[slot]) : 1u : 0u; };
        unsigned total = 0;
        for (int slot = 0; slot < SlotCount; ++slot) total += weight(slot);
        unsigned pick = Draw() % total;
        for (int slot = 0; slot < SlotCount; ++slot) { if (pick < weight(slot)) return slot; pick -= weight(slot); }
        return -1;
    }
    void Commit(int slot) { if (slot >= 0 && slot < SlotCount) next_ = (slot + 1) % SlotCount; }
    void Seed(unsigned seed) { random_ = seed; }
private:
    unsigned Draw() { random_ = random_ * 1103515245u + 12345u; return random_ >> 16; }
    SlotSet set_ = 0;
    int next_ = 0;
    unsigned random_;
};
// The banks kept between battles, one per fighter the dummy was, so a battle
// restarted or the same dummy picked again finds its slots. On the runtime
// thread only, and in memory only: named recordings are how slots are kept
// on disk.
class RecordingBanks {
public:
    // A battle starts with fighter as the dummy: its bank, or an empty one.
    // An unknown fighter (-1) gets an empty bank that is not kept.
    RecordingBank Attach(int fighter) {
        attached_ = true; fighter_ = fighter;
        RecordingBank bank;
        const auto found = banks_.find(fighter);
        if (fighter >= 0 && found != banks_.end()) { bank = std::move(found->second); banks_.erase(found); }
        return bank;
    }
    // The battle closes: the bank goes back under the fighter it was attached
    // for. Once per Attach; a second close keeps nothing.
    void Detach(RecordingBank bank) {
        if (!attached_) return;
        attached_ = false;
        if (fighter_ >= 0) banks_[fighter_] = std::move(bank);
    }
    bool Holds(int fighter) const { return banks_.count(fighter) != 0; }
private:
    std::map<int, RecordingBank> banks_;
    bool attached_ = false;
    int fighter_ = -1;
};
} }
