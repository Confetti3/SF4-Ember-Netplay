#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include "FrameMeter.hxx"

// Ember's own trial mode: checks, frame by frame, that the player performs a
// combo's moves in order as one combo. Like the game's Trial mode it judges
// which move came out and whether it connected, never which buttons were
// pressed. It knows nothing of the game; the adapter feeds it one observation
// per simulated frame.
namespace sf4e { namespace training {
constexpr std::size_t MaxTrialSteps = 64; // As many as a combo may hold.

// ids: the action ids the attacker may show for this move, every script the
//   move passes through that is also listed in Trial::moves included. Empty
//   means the move is unknown: the step is passed unjudged and shown Unchecked.
// mustHit: the move has to connect. False for a move that only has to come
//   out (a dash, the dash of an FADC, a jump, a focus charge).
// Cancel against link is not a field: both show as one action following
// another, and a finished action keeps its id while the fighter stands idle,
// so the samples cannot tell the two apart.
struct TrialStep { std::vector<int> ids; bool mustHit = true; };
// moves: every action id that counts as a move of this fighter. One of them
//   coming out off-step fails the attempt; an action outside it (walking,
//   landing, a move's follow-up script) is a transition and is never judged.
//   The ids of the steps always count, so empty still catches the combo's own
//   moves done out of order.
struct Trial { std::vector<TrialStep> steps; std::vector<int> moves; };

// One simulated frame, attacker and defender, straight from FighterSample.
struct TrialObservation {
    bool valid = false;               // both samples valid
    unsigned attackerStatus = ~0u;    // Actor::GetStatus
    int attackerAction = -1;          // Actor::GetActionID
    float attackerActionFrame = 0;    // Actor::GetActionFrame
    unsigned defenderStatus = ~0u;    // Actor::GetStatus
    float defenderComboDamage = 0;    // Actor::GetComboDamage, read on the defender
};
inline TrialObservation ObserveTrial(const FighterSample& attacker, const FighterSample& defender) {
    TrialObservation now;
    now.valid = attacker.valid && defender.valid;
    now.attackerStatus = attacker.status; now.attackerAction = attacker.action;
    now.attackerActionFrame = attacker.actionFrame;
    now.defenderStatus = defender.status; now.defenderComboDamage = defender.comboDamage;
    return now;
}

// Out: the move came out and has yet to hit. Unchecked: passed unjudged.
enum class TrialStepState { Waiting, Out, Done, Unchecked };
enum class TrialFailure { None, WrongMove, Whiffed, Dropped };
// steps: one state per step, in order; the caller supplies the labels.
// current: the step being waited for; equals steps.size() while complete.
// complete: the last attempt succeeded; it stays shown until the first move
//   comes out again.
// failedStep: the step that was current when lastFailure happened.
// attempts: times the first judged step was completed.
struct TrialView {
    std::vector<TrialStepState> steps;
    int current = 0;
    bool complete = false;
    TrialFailure lastFailure = TrialFailure::None;
    int failedStep = -1;
    unsigned attempts = 0, successes = 0;
    // failures: attempts that ended in a failure, so a change means one just
    // did. drops: how many of them ended at each step.
    unsigned failures = 0;
    std::vector<unsigned> drops;
};

// Values are Dimps::Game::Battle::Chara::Actor::Status.
// Taking a hit: AS_DAMAGE, AS_DAMAGE__BLOW, AS_SEQUENCE (a throw or cinematic).
inline bool TrialHitStatus(unsigned status) { return status == 21 || status == 23 || status == 24; }
// Still in the combo: also AS_STUN, and 18, which FrameMeter files under Down
// on the reading that it is a bound (a bounce that more hits follow); no
// capture has shown which. AS_DOWN and AS_RISE end it, so a meaty hit on
// wake-up never continues the combo before.
inline bool TrialComboStatus(unsigned status) { return TrialHitStatus(status) || status == 17 || status == 18; }

// Rules, all judged on what the two fighters show:
// - A step's move comes out when the attacker starts one of its ids: the
//   action id changes to it, or its action frame runs backwards (the same
//   move again). So the same move twice in a row needs two starts.
// - A mustHit step is done on the first new hit after its move came out: the
//   defender enters a hit status, or its combo damage changes while in one.
//   Further hits of the same move are ignored. Other steps are done as they
//   come out.
// - Steps go strictly in order. While a step waits for its hit, the next
//   step's move coming out fails the attempt (Whiffed), and so does any other
//   id of Trial::moves or of another step (WrongMove). Ids of the step that
//   came out last are its own continuation and never fail.
// - Once a hit has landed, the defender leaving the combo statuses with steps
//   remaining fails the attempt (Dropped).
// - A failure returns to the first step, and the move that caused it may
//   itself start the next attempt. Nothing fails before the first judged step
//   is done: a first move that whiffs is forgotten when another move comes
//   out. It stays out while its user idles, so a projectile that lands after
//   the recovery is still its hit.
// - A step with no ids is passed without being judged, and no WrongMove is
//   called while the move of such a step may be the one coming out.
class TrialSession {
public:
    const TrialView& GetView() const { return view_; }
    // Replaces the trial and clears the counters. On failure nothing is loaded.
    bool Load(Trial trial, std::string& error) {
        trial_ = Trial{}; view_ = TrialView{}; hasLast_ = false;
        matched_.clear(); seen_.clear();
        if (trial.steps.empty()) { error = "the trial has no steps"; return false; }
        if (trial.steps.size() > MaxTrialSteps) { error = "the trial has too many steps"; return false; }
        if (std::all_of(trial.steps.begin(), trial.steps.end(), [](const TrialStep& step) { return step.ids.empty(); })) {
            error = "no move of the trial is known, so nothing can be checked"; return false;
        }
        trial_ = std::move(trial);
        matched_.assign(trial_.steps.size(), 0);
        view_.drops.assign(trial_.steps.size(), 0);
        Rewind();
        return true;
    }
    // Abandons the attempt without counting a failure. Call it whenever the
    // frames stop being consecutive: a restored checkpoint, a skipped update.
    void Restart() { hasLast_ = false; if (!trial_.steps.empty()) Rewind(); }
    void Observe(const TrialObservation& now) {
        if (trial_.steps.empty()) return;
        if (!now.valid) { Restart(); return; }
        const TrialObservation last = last_;
        const bool hadLast = hasLast_;
        last_ = now; hasLast_ = true;
        if (!hadLast) return;
        // The hit is judged before the new action: a move cannot hit on the
        // frame it starts, so a hit on that frame belongs to the move before.
        const bool hit = TrialHitStatus(now.defenderStatus) &&
            (!TrialHitStatus(last.defenderStatus) || now.defenderComboDamage != last.defenderComboDamage);
        if (hit && !view_.complete && State() == TrialStepState::Out) { connected_ = true; Finish(); }
        if (!view_.complete && connected_ && !TrialComboStatus(now.defenderStatus)) Fail(TrialFailure::Dropped);
        if (now.attackerAction >= 0 && (now.attackerAction != last.attackerAction ||
            now.attackerActionFrame < last.attackerActionFrame)) {
            if (now.attackerStatus == 16 && seen_.size() < 32 &&
                std::find(seen_.begin(), seen_.end(), now.attackerAction) == seen_.end()) seen_.push_back(now.attackerAction);
            Started(now.attackerAction);
        }
    }
    // For the log, since the ids of a step come from the game's files and a
    // wrong one only shows as a step that never ticks: every judged step whose
    // move was never seen coming out, with the ids it waits for, and the
    // actions the attacker did start in AS_SKILL. Empty when there is no such
    // step or the attacker did nothing.
    std::string Unmatched() const {
        std::string text;
        for (std::size_t step = 0; step < trial_.steps.size() && text.size() < 900; ++step) {
            if (matched_[step] || trial_.steps[step].ids.empty()) continue;
            text += "step " + std::to_string(step + 1) + " waits for";
            for (const int id : trial_.steps[step].ids) text += " " + std::to_string(id);
            text += "; ";
        }
        if (text.empty() || seen_.empty()) return {};
        text += "seen";
        for (const int id : seen_) text += " " + std::to_string(id);
        return text;
    }
private:
    Trial trial_;
    std::vector<char> matched_; // Per step: its move came out at least once.
    std::vector<int> seen_;     // The first distinct actions started in AS_SKILL.
    TrialView view_;
    TrialObservation last_;
    bool hasLast_ = false;
    bool connected_ = false; // A hit has landed in this attempt.
    int first_ = 0;          // The first step with known ids.

    TrialStepState& State() { return view_.steps[view_.current]; }
    bool Has(int step, int action) const {
        const auto& ids = trial_.steps[step].ids;
        return std::find(ids.begin(), ids.end(), action) != ids.end();
    }
    bool Known(int step) const { return !trial_.steps[step].ids.empty(); }
    bool IsMove(int action) const {
        if (std::find(trial_.moves.begin(), trial_.moves.end(), action) != trial_.moves.end()) return true;
        for (int step = 0; step < static_cast<int>(trial_.steps.size()); ++step) if (Has(step, action)) return true;
        return false;
    }
    void SkipUnknown() {
        const int count = static_cast<int>(trial_.steps.size());
        while (view_.current < count && !Known(view_.current)) view_.steps[view_.current++] = TrialStepState::Unchecked;
    }
    void Rewind() {
        view_.steps.assign(trial_.steps.size(), TrialStepState::Waiting);
        view_.current = 0; view_.complete = false; connected_ = false;
        SkipUnknown();
        first_ = view_.current;
    }
    void Finish() {
        if (view_.current == first_) ++view_.attempts;
        State() = TrialStepState::Done;
        ++view_.current;
        SkipUnknown();
        if (view_.current == static_cast<int>(trial_.steps.size())) { view_.complete = true; ++view_.successes; }
    }
    void Fail(TrialFailure reason) {
        if (view_.current > first_) { view_.lastFailure = reason; view_.failedStep = view_.current; ++view_.failures; ++view_.drops[view_.current]; }
        Rewind();
    }
    void ComeOut() {
        matched_[view_.current] = 1;
        if (trial_.steps[view_.current].mustHit) State() = TrialStepState::Out;
        else Finish();
    }
    void Started(int action) {
        const int count = static_cast<int>(trial_.steps.size());
        if (view_.complete) {
            if (!Has(first_, action)) return;
            Rewind();
        }
        const int current = view_.current;
        if (State() == TrialStepState::Waiting) {
            if (Has(current, action)) { ComeOut(); return; }
            // Nothing is in progress, or the move may belong to the unknown
            // step before, or it continues the step that came out last.
            if (current == first_ || !Known(current - 1) || Has(current - 1, action) || !IsMove(action)) return;
            Fail(TrialFailure::WrongMove);
        } else {
            if (Has(current, action)) return;
            const bool last = current + 1 == count;
            if (!last && !Known(current + 1)) return;
            if (!last && Has(current + 1, action)) Fail(TrialFailure::Whiffed);
            else if (IsMove(action)) Fail(TrialFailure::WrongMove);
            else return;
        }
        if (Has(first_, action)) ComeOut();
    }
};
} }
