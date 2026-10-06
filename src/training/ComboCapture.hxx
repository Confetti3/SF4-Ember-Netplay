#pragma once
#include "FrameMeter.hxx"
#include <vector>

// Watches one fighter and writes down the moves that come out, so a combo a
// player performs once becomes steps: each new attack is a move, a cancel
// when it starts before the one before it recovered. The capture ends by
// itself after a while with no attack.
namespace sf4e { namespace training {
// frame: frames since the capture started when the move began.
struct CaptureEvent { int action = -1; bool cancel = false; int frame = 0; };
constexpr int CaptureIdleFrames = 90;
class ComboCapture {
public:
    void Start() { events_.clear(); active_ = true; idle_ = 0; frame_ = 0; attacking_ = false; lastAction_ = -1; }
    void Stop() { active_ = false; }
    bool Active() const { return active_; }
    const std::vector<CaptureEvent>& Events() const { return events_; }
    void Observe(const FighterSample& fighter) {
        if (!active_) return;
        const bool attack = fighter.valid && ClassifyStatus(fighter.status) == Phase::Attack;
        if (attack && fighter.action != lastAction_) {
            // Still attacking when a new move starts: it was cancelled into.
            events_.push_back({fighter.action, attacking_ && !events_.empty(), frame_});
            lastAction_ = fighter.action;
        }
        if (!attack) lastAction_ = -1;
        attacking_ = attack;
        idle_ = attack ? 0 : idle_ + 1; ++frame_;
        if (!events_.empty() && idle_ >= CaptureIdleFrames) active_ = false;
    }
private:
    std::vector<CaptureEvent> events_;
    bool active_ = false, attacking_ = false;
    int idle_ = 0, frame_ = 0, lastAction_ = -1;
};
} }
