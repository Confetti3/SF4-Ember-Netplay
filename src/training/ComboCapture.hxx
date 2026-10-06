#pragma once
#include "FrameMeter.hxx"
#include "TrainingSession.hxx"
#include <vector>

// Watches one fighter and writes down the moves that come out, so a combo a
// player performs once becomes steps: each new attack is a move, a cancel
// when it starts before the one before it recovered. Each move also keeps
// how many frames after its cue it was pressed: a cancel after the hit of
// the move before it, a link after that move's first free frame. The
// capture ends by itself after a while with no attack.
namespace sf4e { namespace training {
// frame: frames since the capture started when the move began. offset:
// frames from the cue to the press, NoOffset when no cue was seen.
struct CaptureEvent { int action = -1; bool cancel = false; int frame = 0; int offset = NoOffset; };
constexpr int CaptureIdleFrames = 90;
class ComboCapture {
public:
    void Start() { events_.clear(); active_ = true; idle_ = 0; frame_ = 0; attacking_ = false; lastAction_ = -1; freeSeen_ = hitSeen_ = -1; otherHit_ = false; otherDamage_ = 0; }
    void Stop() { active_ = false; }
    bool Active() const { return active_; }
    const std::vector<CaptureEvent>& Events() const { return events_; }
    // other: the fighter being hit; may be invalid, then cancels get no offset.
    void Observe(const FighterSample& fighter, const FighterSample& other = FighterSample{}) {
        if (!active_) return;
        const bool attack = fighter.valid && ClassifyStatus(fighter.status) == Phase::Attack;
        const bool neutral = fighter.valid && ClassifyStatus(fighter.status) == Phase::Neutral;
        const bool hitNow = other.valid && ClassifyStatus(other.status) == Phase::Hit;
        if (other.valid && !events_.empty() && hitSeen_ < 0 && ((hitNow && !otherHit_) || other.comboDamage > otherDamage_)) hitSeen_ = frame_;
        if (neutral && !events_.empty() && freeSeen_ < 0) freeSeen_ = frame_;
        if (attack && fighter.action != lastAction_) {
            // Still attacking when a new move starts: it was cancelled into.
            const bool cancel = attacking_ && !events_.empty();
            // The press was the frame before the move began.
            const int cue = events_.empty() ? -1 : cancel ? hitSeen_ : freeSeen_;
            events_.push_back({fighter.action, cancel, frame_, cue < 0 ? NoOffset : frame_ - 1 - cue});
            lastAction_ = fighter.action; freeSeen_ = hitSeen_ = -1;
        }
        if (!attack) lastAction_ = -1;
        attacking_ = attack; otherHit_ = hitNow; if (other.valid) otherDamage_ = other.comboDamage;
        idle_ = attack ? 0 : idle_ + 1; ++frame_;
        if (!events_.empty() && idle_ >= CaptureIdleFrames) active_ = false;
    }
private:
    std::vector<CaptureEvent> events_;
    bool active_ = false, attacking_ = false, otherHit_ = false;
    int idle_ = 0, frame_ = 0, lastAction_ = -1, freeSeen_ = -1, hitSeen_ = -1;
    float otherDamage_ = 0;
};
} }
