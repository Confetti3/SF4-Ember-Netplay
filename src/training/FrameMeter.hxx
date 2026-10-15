#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
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
// What an action is, from the game's own move data: the category of the
// command (.bcm) move a Skill plays, or a dash status. Focus is the game's
// SAVING category, which its normal and red focus moves and their cancel
// versions carry; a dash out of a focus is the command move DASH or
// BACK_DASH, played as a Skill. Unknown: no sample, or a Skill that no
// command move started. TrainingRuntime resolves it (ResolveActionClass);
// docs/design/FOCUS_CANCEL_MARKER.md says how the marker was confirmed.
enum class ActionClass : std::uint8_t { Unknown, Other, Focus, ForwardDash, BackDash };
// Bits of a command move's category (its +0x28), named as the game's table names them.
struct CommandCategory { enum : std::uint32_t { Saving = 0x80, Dash = 0x80000, BackDash = 0x100000 }; };
// command: a Skill's command move was read, its category in category.
inline ActionClass ResolveActionClass(bool valid, unsigned status, bool command, std::uint32_t category) {
    if (!valid) return ActionClass::Unknown;
    if (status == ActorStatus::FrontDash) return ActionClass::ForwardDash;
    if (status == ActorStatus::BackDash) return ActionClass::BackDash;
    if (status != ActorStatus::Skill) return ActionClass::Other;
    if (!command) return ActionClass::Unknown;
    if (category & CommandCategory::Saving) return ActionClass::Focus;
    if (category & CommandCategory::Dash) return ActionClass::ForwardDash;
    if (category & CommandCategory::BackDash) return ActionClass::BackDash;
    return ActionClass::Other;
}
inline bool DashClass(ActionClass action) { return action == ActionClass::ForwardDash || action == ActionClass::BackDash; }
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
    ActionClass actionClass = ActionClass::Unknown;
    // Set by the meter, not the game: the thrower, still in a throw's
    // sequence after the fighter it threw has left it.
    bool throwRecovery = false;
    // Also the meter's: the first sample up from a knockdown. The frame a meaty attack meets is the one before it.
    bool wake = false;
    // The side's pad this frame (FightButtons), for the classic meter's input lanes; 0 when none was read.
    unsigned input = 0;
};
// The frame a move's recovery ends on: where the script says the fighter is
// free again, its interruptible frame, else its end; -1 when neither is known.
// The one boundary frame advantage's reply timing and the focus cancel share.
inline int MoveRecoveryEnd(const FighterSample& move) {
    if (move.interruptibleFrame > 0) return move.interruptibleFrame;
    return move.totalFrames > 0 ? move.totalFrames : -1;
}
// Whether a move, as last sampled, still had recovery left: the one evidence
// that a following action cut it short. A move sampled on or past its recovery
// end finished, so what follows (a buffered focus) replaced nothing; with no
// boundary known there is no evidence either way.
inline bool RecoveryCutShort(const FighterSample& move) {
    const int end = MoveRecoveryEnd(move);
    return end > 0 && move.actionFrame < static_cast<float>(end);
}
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
// The HUD's parts, each shown until it is turned off: numbers (the readings
// beside the bars), the bars, state (the line under them), shortcuts (the
// chip and its hint) and status (the strip a recording or a playback adds).
// size: the HUD's, small, standard or large (0 to HudSizes - 1).
constexpr int HudSizes = 3;
// classic: the first meter's style instead (TrainingMeter.cxx: ClassicMeter),
// with its own parts, each on until it is turned off: the data line, the
// input lanes, Street Fighter 6's colours with meaty frames, and the colour
// key, scroll markers and trim.
// The bars' style: angled, the default; Stable's flat bars; or the first meter's.
enum class MeterStyle { Angled, Flat, Classic };
constexpr int MeterStyles = 3;
// Each fighter's input display beside the meter (InputDisplayChoices).
enum class InputDisplay { Off, Stick, Arrows, Both };
constexpr int InputDisplays = 4;
struct MeterOptions { MeterStyle style = MeterStyle::Angled; bool recovery = false; int shown = 60; bool numbers = true, bars = true, state = true, shortcuts = true, status = true; int size = 1;
    InputDisplay inputs = InputDisplay::Off;
    bool classicData = true, classicInputs = true, classicColors = true, classicExtras = true, classicSlant = true;
    // The frames the meter keeps, one of MeterHistoryChoices.
    int history = 1200;
    // After a pause the next exchange goes on after the last instead of starting over.
    bool continuous = false;
    // The bars slide a frame at a time, and a held meter steps back MeterSlideStep
    // frames a notch, instead of a page at a time.
    bool smooth = false;
    // Where the meter's bottom edge sits, in percent of the screen's height
    // from the top (MeterPositions; 82 is just above the super meters), and
    // the classic bars' height (ClassicBarHeights, at the HUD's scale).
    int position = 82, barHeight = 16; };
constexpr int MeterPositions[] = {12, 25, 40, 55, 70, 82, 92};
constexpr std::size_t MeterSlideStep = 10;
constexpr int ClassicBarHeights[] = {12, 16, 22};
inline bool GroundedRecoveryState(unsigned status) {
    using S = ActorStatus;
    switch (status) {
    case S::Stand: case S::Crouch: case S::StandToCrouch: case S::CrouchToStand: case S::TurnStand: case S::TurnCrouch: case S::TurnWalk:
    case S::Forward: case S::Backward: case S::GuardStand: case S::GuardCrouch: return true;
    default: return false;
    }
}
// A focus cancel (FrameMeter::Observe): a move, a focus started before that
// move recovered, then a forward or back dash out of the focus. side: whose.
// startup, its provenance and reason, and active are the move's, taken as the
// focus began, before the focus's own replaced them. focusFrames: the frames
// the focus was seen in. dash: ForwardDash or BackDash. recoveryCancelled: the
// move's recovery was cut short, so it has no recovery to show.
struct CancelChain {
    bool valid = false;
    int side = -1;
    int startup = -1, active = 0, focusFrames = 0;
    BoundaryProvenance provenance = BoundaryProvenance::None;
    MeasurementUnavailable startupUnavailable = MeasurementUnavailable::NoAttackBoundary;
    ActionClass dash = ActionClass::Unknown;
    bool recoveryCancelled = false;
};
struct FrameAdvantage {
    std::array<int, 2> frames{};
    bool valid = false, pending = false;
    bool knockdown = false;
    // Who landed the attack this exchange is measured from (-1 before any
    // contact), and whether it was blocked.
    int attacker = -1;
    bool blocked = false;
    MeasurementUnavailable unavailable = MeasurementUnavailable::NoContact;
    // This exchange's focus cancel once its dash came out; it goes and stays
    // with the frames above, so a reading never pairs one exchange's cancel
    // with another's result.
    CancelChain chain;
};
// The focus cancel of a reading made by side, or null.
inline const CancelChain* ChainOf(const FrameAdvantage& reading, int side) {
    return reading.chain.valid && reading.chain.side == side ? &reading.chain : nullptr;
}
// A fighter's last attack in advancing frames, hitstop left out: those inside
// its attack boundary and those after it. Its startup is MeterView's
// startupFrames, including the first active frame, as the game's frame data
// counts it, so the move's total is startup - 1 + active + recovery. seen:
// an attack was made; live: it is still going.
struct MoveFrames { int active = 0, recovery = 0; bool seen = false, live = false; };
// The bars show up to MeterShown frames. The meter keeps MeterHistory frames
// unless the player chose another length on the Frame data page (4, 10, 20 or
// 60 seconds), so a held meter can step back through a whole combo or
// exchange, a page a notch.
constexpr std::size_t MeterShown = 120, MeterHistory = 10 * MeterShown;
constexpr int MeterHistoryChoices[] = {240, 600, 1200, 3600};
// Both fighters quiet this many frames end an exchange and hold the bars.
constexpr unsigned MeterIdleFrames = 4;
// The bars fill from the left, as SF6's meter does: the frame at position pos
// of the exchange (its first frame is 0) is drawn in cell pos % window of
// page pos / window, so an exchange longer than the bars goes on from their
// left edge. They show the newest frame's page, or the page back pages
// before it while a held meter is stepped back. size: the frames kept;
// length: the exchange's frames so far, the newest at length - 1 (taken as
// size when smaller, for bars made by hand). The frames' index, or -1.
inline int MeterFrameIndex(std::size_t size, std::size_t length, std::size_t cell, std::size_t window = MeterShown, std::size_t back = 0) {
    length = (std::max)(length, size);
    if (!size || !window || cell >= window || back > (length - 1) / window) return -1;
    const auto pos = ((length - 1) / window - back) * window + cell, first = length - size;
    return pos < first || pos >= length ? -1 : static_cast<int>(pos - first);
}
// Smooth scrolling (MeterOptions::smooth): the bars show the window frames
// that end back frames before the newest, the newest at their right edge once
// they are full, and fill from the left until then. The frames' index, or -1.
inline int MeterSlideIndex(std::size_t size, std::size_t cell, std::size_t window, std::size_t back = 0) {
    if (back >= size) return -1;
    const std::size_t end = size - back, start = end > window ? end - window : 0;
    return start + cell < end ? static_cast<int>(start + cell) : -1;
}
// How far a smooth held meter steps back, in frames: to its oldest at the left edge.
inline std::size_t MeterSlideLimit(std::size_t size, std::size_t window) { return size > window ? size - window : 0; }
// How many pages a held meter can step back: to the oldest with a frame kept.
inline std::size_t MeterBackLimit(std::size_t size, std::size_t length, std::size_t window) {
    length = (std::max)(length, size);
    return size && window ? (length - 1) / window - (length - size) / window : 0;
}
// What a bar cell shows, resolved once as its frame arrives, so a cell never
// changes as older frames leave the history and the bars only draw it.
// kind: the angled bars' (ClassifyMeter, except that holding back from a jump
// near an attack is a guard status that guards nothing in the air: Neutral).
// phase: the flat bars' (ClassifyStatus). newAction: a native action began on
// this frame. split: the frame starts a count of its own inside a run of
// the same kind: a cancel or a string's next attack, or each hit of a combo.
// Guarding is one run whatever plays in it. With no frame before it in the
// bars, neither is set.
struct MeterCell {
    MeterKind kind = MeterKind::Unknown;
    Phase phase = Phase::Unknown;
    bool newAction = false, split = false;
    // Set later, once, when the other fighter is seen up (FrameMeter::MarkMeaty):
    // the frame a fighter could first be hit after a knockdown, or an attack's
    // active frame that had passed before the one that met it. Only the
    // classic style draws it.
    bool meaty = false;
    // The status the guard this cell is part of began from, carried from
    // cell to cell, so a long guard stays what it began as.
    unsigned guardFrom = ~0u;
};
// sample's cell after previous, the sample before it in the bars whose cell
// was before; no previous for the first cell of the bars.
inline MeterCell ResolveMeterCell(const FighterSample& sample, const FighterSample* previous, const MeterCell& before) {
    using S = ActorStatus;
    MeterCell cell;
    cell.phase = sample.valid ? ClassifyStatus(sample.status) : Phase::Unknown;
    cell.kind = ClassifyMeter(sample);
    const auto guarding = [](const FighterSample& s) { return s.valid && (s.status == S::GuardStand || s.status == S::GuardCrouch); };
    if (guarding(sample)) cell.guardFrom = previous && guarding(*previous) ? before.guardFrom : previous && previous->valid ? previous->status : sample.status;
    if (cell.kind == MeterKind::Guard && sample.status != S::DamageGuard &&
        (sample.posture > 1 || cell.guardFrom == S::Jump || cell.guardFrom == S::StandToJump)) cell.kind = MeterKind::Neutral;
    if (!previous) return cell;
    cell.newAction = sample.valid && sample.action >= 0 && sample.action != previous->action;
    const bool action = cell.newAction && !(cell.kind == MeterKind::Guard && before.kind == MeterKind::Guard);
    const bool struck = cell.kind == MeterKind::Hit && before.kind == MeterKind::Hit && !action &&
        (sample.comboDamage > previous->comboDamage || sample.actionFrame < previous->actionFrame);
    cell.split = action || struck;
    return cell;
}
// A cell from its sample alone, with nothing before it: what the F6 key and
// tests name a kind's colour by.
inline MeterCell MeterCellOf(const FighterSample& sample) { return ResolveMeterCell(sample, nullptr, MeterCell{}); }
struct MeterFrame {
    std::array<FighterSample, 2> fighters;
    std::array<MeterCell, 2> cells;
    int frame = 0;
};
// The fighter on side `up` is first seen up on the frame after the bars'
// last. That last frame is the one it could first be hit on, and the other's
// active frames before it, if it was met with an active frame too, are the
// meaty ones (MeterCell::meaty).
inline void MarkMeaty(std::deque<MeterFrame>& bars, int up) {
    if (bars.empty()) return;
    bars.back().cells[up].meaty = true;
    const int attacker = 1 - up;
    if (bars.back().cells[attacker].kind != MeterKind::Active) return;
    for (auto at = bars.rbegin() + 1; at != bars.rend() && at->cells[attacker].kind == MeterKind::Active; ++at) at->cells[attacker].meaty = true;
}
// A frame added after the last of frames, its cells resolved after that one's
// as the meter resolves them: for bars made by hand, a preview or a test.
inline void AppendMeterFrame(std::deque<MeterFrame>& frames, const std::array<FighterSample, 2>& fighters, int frame) {
    MeterFrame added; added.fighters = fighters; added.frame = frame;
    for (int side = 0; side < 2; ++side)
        added.cells[side] = frames.empty() ? MeterCellOf(fighters[side]) :
            ResolveMeterCell(fighters[side], &frames.back().fighters[side], frames.back().cells[side]);
    frames.push_back(added);
}
struct MeterView {
    // The exchange's newest frames, and how many it has had (MeterFrameIndex).
    std::deque<MeterFrame> frames;
    std::size_t length = 0;
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
    // The bars hold the last exchange (FrameMeter::Observe).
    bool frozen = false;
    // The last reading measured, kept through whiffs, walks and the next
    // exchange until that one's reading replaces it; only a reset clears it.
    FrameAdvantage lastAdvantage;
};
// The reading the HUD and Frame data show: the exchange's own once it is
// measured, until then the last one.
inline const FrameAdvantage& ShownAdvantage(const MeterView& meter) {
    return meter.advantage.valid || !meter.lastAdvantage.valid ? meter.advantage : meter.lastAdvantage;
}
class FrameMeter {
public:
    const MeterView& View() const { return view_; }
    // Whether an exchange after a held meter goes on after what the bars hold
    // (the classic way) instead of starting on clear bars, as SF6's does.
    void SetContinuous(bool continuous) { continuous_ = continuous; }
    // The frames kept from now on; a shorter length drops the oldest at once.
    void SetHistory(std::size_t frames) {
        history_ = (std::max)(MeterShown, frames);
        while (view_.frames.size() > history_) view_.frames.pop_front();
    }
    void Reset() { view_ = MeterView{}; idleFrames_ = 0; hasFrame_ = false; observedFrames_ = contactFrame_ = 0; armed_ = {}; recovered_ = {{-1, -1}}; startupElapsed_ = {}; startupPending_ = {}; recoveryCells_ = {}; thrower_ = {}; firstActiveAt_ = wakeAt_ = {{-1, -1}}; lastCells_ = {}; aside_.clear(); asideLength_ = 0; exchangeAt_ = 0; cancelling_ = {}; }
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
        std::array<MeterCell, 2> cells;
        for (int side = 0; side < 2; ++side) {
            const auto& sample = fighters[side];
            if (sample.valid && view_.current[side].valid && sample.status == view_.current[side].status)
                ++view_.stateFrames[side];
            else view_.stateFrames[side] = sample.valid ? 1 : 0;
            const auto& previous = view_.current[side];
            const bool actionChanged = sample.action != previous.action || sample.actionFrame < previous.actionFrame;
            // Before a focus's own startup and move replace the move's below.
            WatchCancel(side, previous, sample);
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
                if (sample.wake) { wakeAt_[side] = now - 1; Meaty(1 - side); MarkMeaty(view_.frames, side); }
            }
            if (sample.valid && previous.valid && sample.action >= 0 && sample.action == previous.action &&
                sample.actionFrame >= previous.actionFrame) ++view_.actionFrames[side];
            else {
                if (previous.valid && sample.valid && previous.action >= 0 && ClassifyStatus(previous.status) == Phase::Attack)
                    view_.lastAttackFrames[side] = view_.actionFrames[side];
                view_.actionFrames[side] = sample.valid && sample.action >= 0 ? 1 : 0;
            }
            // The cell is resolved now, with the frame before it and the guard
            // it continues, which the history may no longer hold later.
            cells[side] = ResolveMeterCell(sample, previous.valid ? &previous : nullptr, lastCells_[side]);
            lastCells_[side] = cells[side];
            view_.current[side] = sample;
        }
        // Both fighters quiet, as their cells show them: standing, crouching,
        // a jump or a guard in the air, or missing.
        bool neutral = true;
        for (const auto& cell : cells) neutral = neutral && (cell.kind == MeterKind::Neutral || cell.kind == MeterKind::Unknown);
        idleFrames_ = neutral ? idleFrames_ + 1 : 0;
        // The bars run from an exchange's first active frame; idle frames
        // before it are not kept. MeterIdleFrames quiet frames hold them,
        // ending on the exchange's last active frame, and any action after
        // that starts the next exchange on clear bars. An attack still waiting
        // for its contact when they are held (ObserveAdvantage's armed
        // exchange), a projectile in flight among them, goes on in frames
        // kept aside from the frame it began; the other's guard, block or hit
        // puts them in the bars' place, so that exchange shows whole.
        const MeterFrame latest{fighters, cells, frame};
        const bool attacking = (armed_[0] || armed_[1]) && !view_.advantage.valid;
        if (view_.frozen) {
            if (neutral) {
                if (attacking && !aside_.empty()) Append(aside_, asideLength_, latest);
                else aside_.clear();
                return;
            }
            // The other's guard, block or hit while the attack is quiet (a
            // projectile on its way) goes on with the exchange kept aside.
            const int attacker = armed_[0] ? 0 : 1;
            const auto other = cells[1 - attacker].kind;
            const bool reaction = cells[attacker].kind == MeterKind::Neutral && (other == MeterKind::Guard || other == MeterKind::Hit ||
                other == MeterKind::Down || other == MeterKind::Sequence);
            if (reaction && attacking && !aside_.empty()) {
                Append(aside_, asideLength_, latest);
                // Continuing, the bars keep what they had and take on the
                // frames kept aside since they were held.
                if (continuous_) for (std::size_t at = asideKept_; at < aside_.size(); ++at) Append(view_.frames, view_.length, aside_[at]);
                else { view_.frames.swap(aside_); view_.length = asideLength_; }
            } else {
                if (!continuous_) { view_.frames.clear(); view_.length = 0; }
                Append(view_.frames, view_.length, latest);
            }
            aside_.clear(); view_.frozen = false;
            return;
        }
        if (neutral && view_.frames.empty()) return;
        Append(view_.frames, view_.length, latest);
        if (idleFrames_ < MeterIdleFrames) return;
        view_.frozen = true;
        if (attacking) {
            const auto since = (std::min)(view_.length, static_cast<std::size_t>(now - exchangeAt_ + 1));
            aside_.assign(view_.frames.end() - static_cast<std::ptrdiff_t>((std::min)(since, view_.frames.size())), view_.frames.end());
            asideLength_ = since;
            if (!aside_.empty()) for (auto& cell : aside_.front().cells) cell.newAction = cell.split = false;
        }
        // The quiet frames after the last active one leave the bars.
        unsigned quiet = 0;
        for (; quiet < idleFrames_ && view_.frames.size() > 1; ++quiet) { view_.frames.pop_back(); --view_.length; }
        // What the bars still hold of the frames kept aside; the rest are added when continuing.
        asideKept_ = aside_.size() - (std::min)(aside_.size(), static_cast<std::size_t>(quiet));
    }
private:
    void Append(std::deque<MeterFrame>& bars, std::size_t& length, MeterFrame added) const {
        // The first cell of the bars has nothing before it to part from.
        if (bars.empty()) for (auto& cell : added.cells) cell.newAction = cell.split = false;
        bars.push_back(added); ++length;
        while (bars.size() > history_) bars.pop_front();
    }
    // Whether a fighter was hit, blocked or thrown this frame.
    bool ObserveAdvantage(std::int64_t frame, const std::array<FighterSample, 2>& fighters) {
        // A cleared reading takes its focus cancel, and one still forming, with it.
        auto clear = [&](MeasurementUnavailable reason) {
            view_.advantage = {}; view_.advantage.unavailable = reason; recovered_ = {{-1, -1}}; cancelling_ = {};
        };
        for (const auto& sample : fighters) {
            if (!sample.valid || sample.action < 0 || sample.status > ActorStatus::Sequence ||
                sample.timeScale < 0 ||
                sample.posture < 0) {
                clear(MeasurementUnavailable::InvalidSample); armed_ = {}; return false;
            }
        }
        for (int side = 0; side < 2; ++side) {
            const auto& sample = fighters[side];
            const auto& previous = view_.current[side];
            const bool attackStarted = sample.status == ActorStatus::Skill && previous.valid &&
                (previous.status != ActorStatus::Skill || previous.action != sample.action || sample.actionFrame < previous.actionFrame);
            if (attackStarted) {
                if (!armed_[side] || view_.advantage.valid) {
                    clear(MeasurementUnavailable::NoContact); armed_ = {}; armed_[side] = true; exchangeAt_ = frame;
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
        const bool contact = contacts[0] || contacts[1];
        // A trade/interruption cannot inherit the earlier attack's recovery.
        if ((contacts[0] && contacts[1]) || (contacts[0] && armed_[0]) || (contacts[1] && armed_[1])) {
            clear(MeasurementUnavailable::Interrupted); armed_ = {}; return contact;
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
            armed_ = {}; return contact;
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
            view_.lastAdvantage = view_.advantage;
        }
        return contact;
    }
    // A focus cancel forms only as a move, a focus that began while the move
    // still played (no frame between them out of Skill, and the move short of
    // the end of its recovery: RecoveryCutShort), then a dash straight
    // out of the focus. The move's readings are taken as the focus begins;
    // the dash puts the chain in this exchange's reading. Anything else after
    // the focus drops it, and another attack after the dash takes it out of
    // the reading, which no longer ends on the cancel.
    void WatchCancel(int side, const FighterSample& previous, const FighterSample& sample) {
        auto& chain = cancelling_[side];
        const bool focus = sample.valid && sample.status == ActorStatus::Skill && sample.actionClass == ActionClass::Focus;
        const bool attack = sample.valid && sample.status == ActorStatus::Skill && !DashClass(sample.actionClass) && previous.valid &&
            (previous.status != ActorStatus::Skill || sample.action != previous.action || sample.actionFrame < previous.actionFrame);
        if (attack && ChainOf(view_.advantage, side)) view_.advantage.chain = CancelChain{};
        if (chain.side >= 0) {
            if (focus) { ++chain.focusFrames; return; }
            if (sample.valid && DashClass(sample.actionClass)) {
                chain.dash = sample.actionClass; chain.recoveryCancelled = chain.valid = true;
                view_.advantage.chain = chain;
            }
            chain = {};
            return;
        }
        if (!focus || !previous.valid || previous.status != ActorStatus::Skill || previous.actionClass != ActionClass::Other ||
            !RecoveryCutShort(previous)) return;
        chain.side = side; chain.focusFrames = 1;
        chain.startup = view_.startupFrames[side]; chain.provenance = view_.startupBoundaryProvenance[side];
        chain.startupUnavailable = view_.startupUnavailable[side]; chain.active = view_.moves[side].active;
    }
    void Meaty(int attacker) {
        const auto active = firstActiveAt_[attacker], wake = wakeAt_[1 - attacker];
        if (active < 0 || wake < 0 || active - wake > 60) return;
        view_.meatyFrames[attacker] = static_cast<int>(active - wake); view_.meatyValid[attacker] = true;
    }
    MeterView view_;
    std::array<bool, 2> thrower_{};
    // The last cell each side was given (ResolveMeterCell).
    std::array<MeterCell, 2> lastCells_{};
    // What a held meter has seen of an exchange still waiting for its
    // contact, and that exchange's frames so far (Observe).
    std::deque<MeterFrame> aside_;
    std::size_t asideLength_ = 0;
    std::size_t history_ = MeterHistory, asideKept_ = 0;
    bool continuous_ = false;
    // When the armed exchange began, on the clock of accepted observations.
    std::int64_t exchangeAt_ = 0;
    // On the clock of accepted observations; -1 none.
    std::array<std::int64_t, 2> firstActiveAt_{{-1, -1}}, wakeAt_{{-1, -1}};
    std::array<bool, 2> armed_{};
    // A focus cancel each side may be forming (WatchCancel): side -1 none.
    std::array<CancelChain, 2> cancelling_{};
    std::array<bool, 2> startupPending_{};
    std::array<int, 2> startupElapsed_{};
    std::array<int, 2> recoveryCells_{};
    std::array<std::int64_t, 2> recovered_{{-1, -1}};
    std::int64_t observedFrames_ = 0, contactFrame_ = 0;
    unsigned idleFrames_ = 0;
    std::uint16_t lastFrame_ = 0;
    bool hasFrame_ = false;
};
} }
