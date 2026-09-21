#pragma once

#include <stdint.h>

namespace sf4e {
namespace netplay {

// Scheduling only: GGPO still owns input history, framing and acknowledgments.
// The host calls OnInputSent for normal sends as well as repairs, so a healthy
// 60 Hz stream needs no extra traffic. Times use unsigned millisecond deltas,
// matching GGPO's wrapping timeGetTime clock (not wall time).
class InputRepair {
public:
    InputRepair()
        : enabled_(false), newest_(-1), acknowledged_(-1),
          last_send_ms_(0), retry_ms_(33), history_{}, repairs_(0) {}

    void SetEnabled(bool enabled) { enabled_ = enabled; }
    uint32_t Repairs() const { return repairs_; }

    void OnInputSent(uint32_t now_ms, int newest_frame) {
        if (newest_frame > newest_) {
            newest_ = newest_frame;
            retry_ms_ = 33;
        }
        last_send_ms_ = now_ms;
    }

    void OnAck(int frame) {
        // A stale or impossible ACK cannot reset backoff or invent progress.
        if (frame > acknowledged_ && frame <= newest_) {
            acknowledged_ = frame;
            retry_ms_ = 33;
        }
    }

    bool TryRepair(uint32_t now_ms, bool transport_idle) {
        // Nothing sent yet means newest_ == acknowledged_ == -1.
        if (!enabled_ || !transport_idle || newest_ <= acknowledged_ ||
            uint32_t(now_ms - last_send_ms_) < retry_ms_) {
            return false;
        }
        // A rolling one-second window, not a resettable fixed bucket. Even
        // advancing ACKs cannot refill this budget. Extra traffic is bounded
        // to four packets times the existing admitted gameplay packet size.
        uint32_t& oldest = history_[repairs_ % 4];
        if (repairs_ >= 4 && uint32_t(now_ms - oldest) < 1000) {
            return false;
        }
        oldest = now_ms;
        ++repairs_;
        last_send_ms_ = now_ms;
        retry_ms_ = retry_ms_ < 100 ? retry_ms_ * 2 : 200;
        return true;
    }

private:
    bool enabled_;
    int newest_;
    int acknowledged_;
    uint32_t last_send_ms_;
    uint32_t retry_ms_;
    uint32_t history_[4];
    uint32_t repairs_;
};

} // namespace netplay
} // namespace sf4e
