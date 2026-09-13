#pragma once
#include <array>
#include <cstdint>
#include <deque>

namespace sf4e { namespace training {
enum class Phase { Neutral, Movement, Attack, Guard, Hit, Down, Unknown };
inline Phase ClassifyStatus(unsigned status) {
    // Values are Dimps::Game::Battle::Chara::Actor::Status. Attack is kept
    // whole: AS_SKILL does not distinguish startup, active and recovery.
    switch (status) {
    case 0: case 1: return Phase::Neutral;
    case 2: case 3: case 4: case 5: case 6: case 7: case 8:
    case 9: case 10: case 11: case 12: case 13: return Phase::Movement;
    case 16: return Phase::Attack;
    case 14: case 15: case 22: return Phase::Guard;
    case 17: case 21: case 23: return Phase::Hit;
    case 18: case 19: case 20: return Phase::Down;
    default: return Phase::Unknown;
    }
}
inline const char* PhaseName(Phase phase) {
    switch (phase) {
    case Phase::Neutral: return "Neutral";
    case Phase::Movement: return "Movement";
    case Phase::Attack: return "Attack";
    case Phase::Guard: return "Guard";
    case Phase::Hit: return "Hit / stun";
    case Phase::Down: return "Knockdown";
    default: return "Unknown";
    }
}
struct FighterSample {
    unsigned status = ~0u;
    int action = -1;
    float actionFrame = 0;
    float damage = 0, comboDamage = 0, health = 0;
    bool valid = false;
    // Native action posture: 0 standing, 1 crouching; other values include
    // airborne/downed actions. Unit time scale includes native freezes.
    int posture = -1;
    float timeScale = 0;
    bool basicActionInhibited = true;
    int firstActiveFrame = -1;
};
inline bool GroundedRecoveryState(unsigned status) {
    switch (status) {
    case 0: case 1: case 3: case 4: case 7: case 8: case 9:
    case 10: case 11: case 14: case 15: return true;
    default: return false;
    }
}
struct FrameAdvantage {
    std::array<int, 2> frames{};
    bool valid = false, pending = false;
    bool knockdown = false;
};
struct MeterFrame {
    std::array<FighterSample, 2> fighters;
    int frame = 0;
};
struct MeterView {
    std::deque<MeterFrame> frames;
    std::array<FighterSample, 2> current;
    std::array<unsigned, 2> stateFrames{};
    std::array<unsigned, 2> actionFrames{};
    std::array<unsigned, 2> lastAttackFrames{};
    FrameAdvantage advantage;
    std::array<int, 2> startupFrames{{-1, -1}};
    bool frozen = false, autoFreeze = true;
};
class FrameMeter {
public:
    const MeterView& View() const { return view_; }
    void Reset() { const bool autoFreeze = view_.autoFreeze; view_ = MeterView{}; view_.autoFreeze = autoFreeze; idleFrames_ = 0; hadActivity_ = false; hasFrame_ = false; observedFrames_ = contactFrame_ = 0; armed_ = {}; recovered_ = {{-1, -1}}; startupElapsed_ = {}; startupPending_ = {}; }
    void SetAutoFreeze(bool enabled) { view_.autoFreeze = enabled; view_.frozen = false; }
    void Observe(int frame, const std::array<FighterSample, 2>& fighters) {
        // The native fixed-point integral is a wrapping 16-bit counter. It
        // becomes negative after 32767; those values must never double as
        // the "not recovered" sentinel. Check continuity modulo 16 bits and
        // time recovery on an independent clock of accepted observations.
        const auto nativeFrame = static_cast<std::uint16_t>(frame);
        if (hasFrame_ && static_cast<std::uint16_t>(nativeFrame - lastFrame_) != 1) Reset();
        lastFrame_ = nativeFrame; hasFrame_ = true;
        ObserveAdvantage(observedFrames_++, fighters);
        bool neutral = true;
        for (int side = 0; side < 2; ++side) {
            const auto& sample = fighters[side];
            neutral = neutral && sample.valid && ClassifyStatus(sample.status) == Phase::Neutral;
            if (sample.valid && view_.current[side].valid && sample.status == view_.current[side].status)
                ++view_.stateFrames[side];
            else view_.stateFrames[side] = sample.valid ? 1 : 0;
            const auto& previous = view_.current[side];
            const bool actionChanged = sample.action != previous.action || sample.actionFrame < previous.actionFrame;
            if (!sample.valid) {
                view_.startupFrames[side] = -1; startupPending_[side] = false;
            } else if (sample.status == 16) {
                if (previous.valid && (previous.status != 16 ||
                    (actionChanged && sample.firstActiveFrame >= 0 && !startupPending_[side]))) {
                    startupElapsed_[side] = 0; startupPending_[side] = true;
                    view_.startupFrames[side] = -1;
                }
                // Count accepted advancing frames, not BAC animation ticks:
                // normals and specials commonly change animation speed.
                // A hit can enable hitstop at the end of this very step; use
                // the observed animation advance instead of the next scale.
                if (startupPending_[side] &&
                    (actionChanged || previous.status != 16 || sample.actionFrame > previous.actionFrame)) {
                    ++startupElapsed_[side];
                    if (sample.firstActiveFrame >= 0 && sample.actionFrame >= sample.firstActiveFrame) {
                        view_.startupFrames[side] = startupElapsed_[side];
                        startupPending_[side] = false;
                    }
                }
            } else startupPending_[side] = false;
            if (sample.valid && previous.valid && sample.action >= 0 && sample.action == previous.action &&
                sample.actionFrame >= previous.actionFrame) ++view_.actionFrames[side];
            else {
                if (previous.valid && sample.valid && previous.action >= 0 && ClassifyStatus(previous.status) == Phase::Attack)
                    view_.lastAttackFrames[side] = view_.actionFrames[side];
                view_.actionFrames[side] = sample.valid && sample.action >= 0 ? 1 : 0;
            }
            view_.current[side] = sample;
        }
        if (neutral) ++idleFrames_;
        else {
            if (view_.frozen) view_.frames.clear();
            view_.frozen = false; idleFrames_ = 0; hadActivity_ = true;
        }
        if (view_.autoFreeze && hadActivity_ && idleFrames_ >= 30) view_.frozen = true;
        if (!view_.frozen) {
            view_.frames.push_back({fighters, frame});
            if (view_.frames.size() > 120) view_.frames.pop_front();
        }
    }
private:
    void ObserveAdvantage(std::int64_t frame, const std::array<FighterSample, 2>& fighters) {
        auto clear = [&] { view_.advantage = {}; recovered_ = {{-1, -1}}; };
        for (const auto& sample : fighters) {
            if (!sample.valid || sample.action < 0 || sample.status > 24 ||
                sample.timeScale < 0 ||
                sample.posture < 0) {
                clear(); armed_ = {}; return;
            }
        }
        for (int side = 0; side < 2; ++side) {
            const auto& sample = fighters[side];
            const auto& previous = view_.current[side];
            const bool attackStarted = sample.status == 16 && previous.valid &&
                (previous.status != 16 || previous.action != sample.action || sample.actionFrame < previous.actionFrame);
            if (attackStarted) {
                if (!armed_[side] || view_.advantage.valid) {
                    clear(); armed_ = {}; armed_[side] = true;
                }
                // Target combos, special cancels and internal action changes
                // continue the exchange. The new action has not recovered yet.
                recovered_[side] = -1;
                contactFrame_ = frame;
            }
        }
        std::array<bool, 2> contacts{};
        for (int defender = 0; defender < 2; ++defender) {
            const auto& sample = fighters[defender];
            const auto& previous = view_.current[defender];
            contacts[defender] = (sample.status == 21 || sample.status == 22 || sample.status == 23) && previous.valid &&
                (sample.status != previous.status || sample.action != previous.action ||
                 sample.actionFrame < previous.actionFrame || sample.comboDamage > previous.comboDamage);
        }
        // A trade/interruption cannot inherit the earlier attack's recovery.
        if ((contacts[0] && contacts[1]) || (contacts[0] && armed_[0]) || (contacts[1] && armed_[1])) {
            clear(); armed_ = {}; return;
        }
        for (int defender = 0; defender < 2; ++defender) {
            if (contacts[defender] && armed_[1 - defender]) {
                view_.advantage.valid = false; view_.advantage.pending = true;
                recovered_[defender] = -1;
                // Preserve an already recovered attacker's timestamp: a
                // projectile may connect after its owner's recovery ends.
                contactFrame_ = frame;
            }
        }
        if (frame - contactFrame_ > 600) {
            if (!view_.advantage.valid) clear();
            armed_ = {}; return;
        }
        for (int side = 0; side < 2; ++side) {
            const auto& sample = fighters[side];
            if (view_.advantage.pending && (ClassifyStatus(sample.status) == Phase::Down || sample.status == 23))
                view_.advantage.knockdown = true;
            if ((armed_[side] || view_.advantage.pending) && recovered_[side] < 0 &&
                GroundedRecoveryState(sample.status) && sample.posture <= 1 && sample.timeScale > 0 && !sample.basicActionInhibited)
                recovered_[side] = frame;
        }
        if (view_.advantage.pending && recovered_[0] >= 0 && recovered_[1] >= 0) {
            // Positive means this fighter recovered first. Both values are
            // latched together, so the HUD never mixes different exchanges.
            view_.advantage.frames[0] = static_cast<int>(recovered_[1] - recovered_[0]);
            view_.advantage.frames[1] = -view_.advantage.frames[0];
            view_.advantage.valid = true; view_.advantage.pending = false;
        }
    }
    MeterView view_;
    std::array<bool, 2> armed_{};
    std::array<bool, 2> startupPending_{};
    std::array<int, 2> startupElapsed_{};
    std::array<std::int64_t, 2> recovered_{{-1, -1}};
    std::int64_t observedFrames_ = 0, contactFrame_ = 0;
    unsigned idleFrames_ = 0;
    bool hadActivity_ = false;
    std::uint16_t lastFrame_ = 0;
    bool hasFrame_ = false;
};
} }
