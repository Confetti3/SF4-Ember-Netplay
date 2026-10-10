#pragma once
#include <array>
#include <cstdint>
#include <deque>

namespace sf4e { namespace training {
enum class Phase { Neutral, Movement, Attack, Guard, Hit, Down, Unknown };
enum class BoundaryProvenance : std::uint8_t { None, BacActionHeader, BacEffectSpawn };
enum class MeasurementUnavailable : std::uint8_t {
    None, WaitingForAttackBoundary, NoAttackBoundary, NoContact, MeasuringRecovery,
    Interrupted, InvalidSample
};
inline const char* MeasurementUnavailableName(MeasurementUnavailable reason) {
    switch (reason) {
    case MeasurementUnavailable::WaitingForAttackBoundary: return "waiting for the authored attack boundary";
    case MeasurementUnavailable::NoAttackBoundary: return "this native action has no usable attack boundary";
    case MeasurementUnavailable::NoContact: return "waiting for a hit or block";
    case MeasurementUnavailable::MeasuringRecovery: return "measuring recovery";
    case MeasurementUnavailable::Interrupted: return "the exchange was interrupted";
    case MeasurementUnavailable::InvalidSample: return "native action data is unavailable";
    default: return "available";
    }
}
// Dimps::Game::Battle::Chara::Actor::Status (AS_*), copied so the meter needs
// no native header; TrainingRuntime.cxx checks that the two agree.
struct ActorStatus {
    enum : unsigned {
        Stand = 0, Crouch = 1, Jump = 2, StandToCrouch = 3, CrouchToStand = 4, StandToJump = 5, JumpToStand = 6,
        TurnStand = 7, TurnCrouch = 8, TurnWalk = 9, Forward = 10, Backward = 11, FrontDash = 12, BackDash = 13,
        GuardStand = 14, GuardCrouch = 15, Skill = 16, Stun = 17, Bound = 18, Down = 19, Rise = 20,
        Damage = 21, DamageGuard = 22, DamageBlow = 23, Sequence = 24
    };
};
inline Phase ClassifyStatus(unsigned status) {
    // Attack is kept whole: Skill does not distinguish startup, active and recovery.
    using S = ActorStatus;
    switch (status) {
    case S::Stand: case S::Crouch: return Phase::Neutral;
    case S::Jump: case S::StandToCrouch: case S::CrouchToStand: case S::StandToJump: case S::JumpToStand: case S::TurnStand:
    case S::TurnCrouch: case S::TurnWalk: case S::Forward: case S::Backward: case S::FrontDash: case S::BackDash: return Phase::Movement;
    case S::Skill: return Phase::Attack;
    case S::GuardStand: case S::GuardCrouch: case S::DamageGuard: return Phase::Guard;
    case S::Stun: case S::Damage: case S::DamageBlow: return Phase::Hit;
    case S::Bound: case S::Down: case S::Rise: return Phase::Down;
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
    int lastActiveFrame = -1;
    // Native BAC header: the frame the action can be interrupted from, and
    // its total frames; -1 unknown.
    int interruptibleFrame = -1, totalFrames = -1;
    BoundaryProvenance boundaryProvenance = BoundaryProvenance::None;
    // Set by the meter, not the game: the thrower, still in a throw's
    // sequence after the fighter it threw has left it.
    bool throwRecovery = false;
    // Also the meter's: the first sample up from a knockdown. The frame a meaty attack meets is the one before it.
    bool wake = false;
};
// What a meter cell shows. An attack is told apart by the script's attack
// boundary: before it startup, inside it active, after it recovery; Attack
// alone when the script gives none.
// The cells follow the frame data's own numbers, for a player who reads the
// two side by side: a crouching jab with a startup of 3 has three startup
// cells. Frame data counts the frame a move first hits on as startup, so that
// third cell is its first active frame, the active cells stand one frame
// late, and the recovery has one cell less than its number (3, 2 and 6 cells
// for 3, 2 and 7). A completed MoveFrames::recovery is the number, not the cell count.
// Checked in the game by the in-game self-test.
// Limitation: one active stretch per action, so a multi-hit move's gaps
// between hits read as active. Read every hit box if they have to show.
// Sequence is ActorStatus::Sequence: both fighters while a throw or a cinematic plays;
// what the thrower has left of it once the other is let go is its recovery.
// Down is bouncing or lying, Rise getting up. Meaty is the bars' own, never
// ClassifyMeter's: the frame a fighter is first up, and an attack active on it.
enum class MeterKind { Neutral, Movement, Startup, Active, Recovery, Attack, Guard, Hit, Down, Rise, Sequence, Meaty, Unknown };
inline MeterKind ClassifyMeter(const FighterSample& sample) {
    using S = ActorStatus;
    if (!sample.valid) return MeterKind::Unknown;
    if (sample.status == S::Sequence) return sample.throwRecovery ? MeterKind::Recovery : MeterKind::Sequence;
    switch (ClassifyStatus(sample.status)) {
    case Phase::Neutral: return MeterKind::Neutral;
    // A jump shows as nothing: its frames are the same every time. Walks and dashes show.
    case Phase::Movement:
        return sample.status == S::Jump || sample.status == S::StandToJump || sample.status == S::JumpToStand ? MeterKind::Neutral : MeterKind::Movement;
    case Phase::Guard: return MeterKind::Guard;
    case Phase::Hit: return MeterKind::Hit;
    case Phase::Down: return sample.status == S::Rise ? MeterKind::Rise : MeterKind::Down;
    case Phase::Attack:
        if (sample.firstActiveFrame < 0 || sample.lastActiveFrame <= sample.firstActiveFrame) return MeterKind::Attack;
        return sample.actionFrame - 1 < sample.firstActiveFrame ? MeterKind::Startup :
            sample.actionFrame - 1 < sample.lastActiveFrame ? MeterKind::Active : MeterKind::Recovery;
    default: return MeterKind::Unknown;
    }
}
// How the bars are drawn, chosen on the Frame data page and kept in
// training.json. Angled bars, the default, colour an attack's cells by
// ClassifyMeter's startup, active and recovery and print each run's length;
// flat is Stable's bars. recovery adds the last move's recovery to each row.
// shown: the frames across the bar's width, 60, 90 or 120. Fewer frames make
// each cell wider and easier to count; 120 is the whole history at once.
constexpr int MeterShownChoices[] = {60, 90, 120};
struct MeterOptions { bool flat = false, recovery = false; int shown = 60; };
inline bool GroundedRecoveryState(unsigned status) {
    using S = ActorStatus;
    switch (status) {
    case S::Stand: case S::Crouch: case S::StandToCrouch: case S::CrouchToStand: case S::TurnStand: case S::TurnCrouch: case S::TurnWalk:
    case S::Forward: case S::Backward: case S::GuardStand: case S::GuardCrouch: return true;
    default: return false;
    }
}
struct FrameAdvantage {
    std::array<int, 2> frames{};
    bool valid = false, pending = false;
    bool knockdown = false;
    // Who landed the attack this exchange is measured from (-1 before any
    // contact), and whether it was blocked.
    int attacker = -1;
    bool blocked = false;
    MeasurementUnavailable unavailable = MeasurementUnavailable::NoContact;
};
// A fighter's last attack in advancing frames, hitstop left out: those inside
// its attack boundary and those after it. Its startup is MeterView's
// startupFrames, including the first active frame, as the game's frame data
// counts it, so the move's total is startup - 1 + active + recovery. seen:
// an attack was made; live: it is still going.
struct MoveFrames { int active = 0, recovery = 0; bool seen = false, live = false; };
// The bars show MeterShown frames. The meter keeps one more, so the leftmost
// cell can still tell whether a new action began on it.
constexpr std::size_t MeterShown = 120, MeterHistory = MeterShown + 1;
// Short histories align right; longer ones show only the newest cells.
// window: the cells across the bar; back: how many of the newest frames lie
// to the right of it, while a held meter is scrolled back.
inline int MeterFrameIndex(std::size_t size, std::size_t cell, std::size_t window = MeterShown, std::size_t back = 0) {
    const auto end = size > back ? size - back : 0;
    const auto shown = (std::min)(end, window);
    const int offset = static_cast<int>(cell) - static_cast<int>(window - shown);
    return cell >= window || offset < 0 ? -1 : static_cast<int>(end - shown) + offset;
}
// How far back a held meter can scroll with window cells across the bar: to
// the oldest of the MeterShown frames it keeps.
inline std::size_t MeterBackLimit(std::size_t size, std::size_t window) {
    const auto kept = (std::min)(size, MeterShown);
    return kept > window ? kept - window : 0;
}
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
    std::array<MoveFrames, 2> moves;
    FrameAdvantage advantage;
    std::array<int, 2> startupFrames{{-1, -1}};
    std::array<MeasurementUnavailable, 2> startupUnavailable{{MeasurementUnavailable::NoAttackBoundary,
        MeasurementUnavailable::NoAttackBoundary}};
    std::array<BoundaryProvenance, 2> startupBoundaryProvenance{};
    // Meaty timing: the frame this fighter's attack first became active,
    // counted from the frame the other could first be hit after a knockdown,
    // one before the first sample that shows them up. 0 meets
    // that frame; -N came N frames early, a meaty with N active frames passed
    // while N is less than the attack's active frames; +N left the other N
    // frames to act in.
    std::array<int, 2> meatyFrames{};
    std::array<bool, 2> meatyValid{};
    bool frozen = false;
};
class FrameMeter {
public:
    const MeterView& View() const { return view_; }
    void Reset() { view_ = MeterView{}; idleFrames_ = 0; hadActivity_ = false; hasFrame_ = false; observedFrames_ = contactFrame_ = 0; armed_ = {}; recovered_ = {{-1, -1}}; startupElapsed_ = {}; startupPending_ = {}; recoveryCells_ = {}; thrower_ = {}; firstActiveAt_ = wakeAt_ = {{-1, -1}}; }
    void Observe(int frame, const std::array<FighterSample, 2>& observed) {
        // The native fixed-point integral is a wrapping 16-bit counter. It
        // becomes negative after 32767; those values must never double as
        // the "not recovered" sentinel. Check continuity modulo 16 bits and
        // time recovery on an independent clock of accepted observations.
        const auto nativeFrame = static_cast<std::uint16_t>(frame);
        if (hasFrame_ && static_cast<std::uint16_t>(nativeFrame - lastFrame_) != 1) Reset();
        lastFrame_ = nativeFrame; hasFrame_ = true;
        auto fighters = observed;
        // Whoever came into a sequence out of an attack is the one throwing.
        for (int side = 0; side < 2; ++side) {
            const auto& previous = view_.current[side];
            if (!fighters[side].valid || fighters[side].status != ActorStatus::Sequence) thrower_[side] = false;
            else if (!previous.valid || previous.status != ActorStatus::Sequence) thrower_[side] = previous.valid && previous.status == ActorStatus::Skill;
        }
        for (int side = 0; side < 2; ++side)
            fighters[side].throwRecovery = thrower_[side] && fighters[1 - side].valid && fighters[1 - side].status != ActorStatus::Sequence;
        for (int side = 0; side < 2; ++side)
            fighters[side].wake = fighters[side].valid && view_.current[side].valid && view_.current[side].status == ActorStatus::Rise && fighters[side].status != ActorStatus::Rise;
        const std::int64_t now = observedFrames_++;
        ObserveAdvantage(now, fighters);
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
                view_.startupUnavailable[side] = MeasurementUnavailable::InvalidSample;
                view_.startupBoundaryProvenance[side] = BoundaryProvenance::None;
            } else if (sample.status == ActorStatus::Skill) {
                if (previous.valid && (previous.status != ActorStatus::Skill ||
                    (actionChanged && sample.firstActiveFrame >= 0 && !startupPending_[side]))) {
                    startupElapsed_[side] = 0; startupPending_[side] = true;
                    view_.startupFrames[side] = -1;
                    view_.startupUnavailable[side] = sample.firstActiveFrame >= 0 ?
                        MeasurementUnavailable::WaitingForAttackBoundary : MeasurementUnavailable::NoAttackBoundary;
                    view_.startupBoundaryProvenance[side] = sample.boundaryProvenance;
                }
                // Count accepted advancing frames, not BAC animation ticks:
                // normals and specials commonly change animation speed.
                // A hit can enable hitstop at the end of this very step; use
                // the observed animation advance instead of the next scale.
                if (startupPending_[side] &&
                    (actionChanged || previous.status != ActorStatus::Skill || sample.actionFrame > previous.actionFrame)) {
                    ++startupElapsed_[side];
                    if (sample.firstActiveFrame >= 0 && sample.actionFrame >= sample.firstActiveFrame) {
                        view_.startupFrames[side] = startupElapsed_[side];
                        view_.startupBoundaryProvenance[side] = sample.boundaryProvenance;
                        view_.startupUnavailable[side] = MeasurementUnavailable::None;
                        startupPending_[side] = false;
                    }
                }
            } else startupPending_[side] = false;
            auto& move = view_.moves[side];
            if (sample.valid && sample.status == ActorStatus::Skill) {
                const auto kind = ClassifyMeter(sample);
                const bool began = !previous.valid || previous.status != ActorStatus::Skill;
                // A cancel into another attack is a new move; a move's own later scripts are not.
                if (began || (actionChanged && kind == MeterKind::Startup)) { move = MoveFrames{}; recoveryCells_[side] = 0; }
                move.seen = move.live = true;
                if (began || actionChanged || sample.actionFrame > previous.actionFrame) {
                    // The attack's first active frame is what a meaty is timed by.
                    if (kind == MeterKind::Active && !move.active) { firstActiveAt_[side] = now; Meaty(side); }
                    if (kind == MeterKind::Active) ++move.active;
                    else if (kind == MeterKind::Recovery) move.recovery = ++recoveryCells_[side];
                }
            } else if (sample.valid && sample.status == ActorStatus::Sequence && thrower_[side] && move.seen) {
                // A throw that connected goes on in the sequence.
                move.live = true;
                if (sample.throwRecovery) move.recovery = ++recoveryCells_[side];
            } else {
                // Startup includes the first active frame, shifting the cells
                // one frame late. At normal completion count the recovery
                // frame with no cell, even when no Recovery cell was seen.
                // Throws count their sequence directly; interruptions do not
                // establish a completed move's recovery.
                if (move.live && sample.valid && previous.valid && previous.status == ActorStatus::Skill && move.active > 0 &&
                    GroundedRecoveryState(sample.status)) move.recovery = recoveryCells_[side] + 1;
                move.live = false;
            }
            if (sample.valid && previous.valid) {
                const bool down = ClassifyStatus(sample.status) == Phase::Down;
                // A knockdown starts a new meaty reading; the attack that caused it is not one.
                if (down && ClassifyStatus(previous.status) != Phase::Down) {
                    wakeAt_[side] = firstActiveAt_[1 - side] = -1; view_.meatyValid[1 - side] = false;
                }
                // The frame a meaty meets is the one before the fighter is seen up: a status is read
                // after its update, so the fighter could already be hit in the frame that ended the rise.
                if (sample.wake) { wakeAt_[side] = now - 1; Meaty(1 - side); }
            }
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
            // After a pause the bars start again from their left edge.
            if (view_.frozen || idleFrames_ >= 30) view_.frames.clear();
            view_.frozen = false; idleFrames_ = 0; hadActivity_ = true;
        }
        if (hadActivity_ && idleFrames_ >= 30) view_.frozen = true;
        if (!view_.frozen) {
            view_.frames.push_back({fighters, frame});
            if (view_.frames.size() > MeterHistory) view_.frames.pop_front();
        }
    }
private:
    void ObserveAdvantage(std::int64_t frame, const std::array<FighterSample, 2>& fighters) {
        auto clear = [&](MeasurementUnavailable reason) {
            view_.advantage = {}; view_.advantage.unavailable = reason; recovered_ = {{-1, -1}};
        };
        for (const auto& sample : fighters) {
            if (!sample.valid || sample.action < 0 || sample.status > ActorStatus::Sequence ||
                sample.timeScale < 0 ||
                sample.posture < 0) {
                clear(MeasurementUnavailable::InvalidSample); armed_ = {}; return;
            }
        }
        for (int side = 0; side < 2; ++side) {
            const auto& sample = fighters[side];
            const auto& previous = view_.current[side];
            const bool attackStarted = sample.status == ActorStatus::Skill && previous.valid &&
                (previous.status != ActorStatus::Skill || previous.action != sample.action || sample.actionFrame < previous.actionFrame);
            if (attackStarted) {
                if (!armed_[side] || view_.advantage.valid) {
                    clear(MeasurementUnavailable::NoContact); armed_ = {}; armed_[side] = true;
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
            contacts[defender] = (sample.status == ActorStatus::Damage || sample.status == ActorStatus::DamageGuard || sample.status == ActorStatus::DamageBlow) && previous.valid &&
                (sample.status != previous.status || sample.action != previous.action ||
                 sample.actionFrame < previous.actionFrame || sample.comboDamage > previous.comboDamage);
            // A throw connects as its sequence takes both; only the thrown fighter is hit by it.
            if (sample.status == ActorStatus::Sequence && previous.valid && previous.status != ActorStatus::Sequence && armed_[1 - defender]) contacts[defender] = true;
        }
        // A trade/interruption cannot inherit the earlier attack's recovery.
        if ((contacts[0] && contacts[1]) || (contacts[0] && armed_[0]) || (contacts[1] && armed_[1])) {
            clear(MeasurementUnavailable::Interrupted); armed_ = {}; return;
        }
        for (int defender = 0; defender < 2; ++defender) {
            if (contacts[defender] && armed_[1 - defender]) {
                view_.advantage.valid = false; view_.advantage.pending = true;
                view_.advantage.unavailable = MeasurementUnavailable::MeasuringRecovery;
                view_.advantage.attacker = 1 - defender;
                view_.advantage.blocked = ClassifyStatus(fighters[defender].status) == Phase::Guard;
                recovered_[defender] = -1;
                // Preserve an already recovered attacker's timestamp: a
                // projectile may connect after its owner's recovery ends.
                contactFrame_ = frame;
            }
        }
        if (frame - contactFrame_ > 600) {
            if (!view_.advantage.valid) clear(MeasurementUnavailable::Interrupted);
            armed_ = {}; return;
        }
        for (int side = 0; side < 2; ++side) {
            const auto& sample = fighters[side];
            if (view_.advantage.pending && (ClassifyStatus(sample.status) == Phase::Down || sample.status == ActorStatus::DamageBlow))
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
            view_.advantage.unavailable = MeasurementUnavailable::None;
        }
    }
    void Meaty(int attacker) {
        const auto active = firstActiveAt_[attacker], wake = wakeAt_[1 - attacker];
        if (active < 0 || wake < 0 || active - wake > 60) return;
        view_.meatyFrames[attacker] = static_cast<int>(active - wake); view_.meatyValid[attacker] = true;
    }
    MeterView view_;
    std::array<bool, 2> thrower_{};
    // On the clock of accepted observations; -1 none.
    std::array<std::int64_t, 2> firstActiveAt_{{-1, -1}}, wakeAt_{{-1, -1}};
    std::array<bool, 2> armed_{};
    std::array<bool, 2> startupPending_{};
    std::array<int, 2> startupElapsed_{};
    std::array<int, 2> recoveryCells_{};
    std::array<std::int64_t, 2> recovered_{{-1, -1}};
    std::int64_t observedFrames_ = 0, contactFrame_ = 0;
    unsigned idleFrames_ = 0;
    bool hadActivity_ = false;
    std::uint16_t lastFrame_ = 0;
    bool hasFrame_ = false;
};
} }
