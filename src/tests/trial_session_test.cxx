#include "../training/TrialSession.hxx"
#include "../training/ComboCapture.hxx"
#include "test_support.hxx"

using namespace sf4e::training;

namespace {
// Action ids as a fighter shows them: basic actions low, moves from 256 up.
constexpr int Stand = 0, Crouching = 2, PreJump = 7, Jump = 8, Landing = 9, Walk = 16, Dash = 18;
constexpr int Jab = 256, Strong = 301, Low = 356, Fireball = 386, Uppercut = 395, UppercutFall = 396, Focus = 327, JumpKick = 282;

// Plays both fighters one frame at a time, the way the adapter samples them.
struct Lab {
    TrialSession session;
    TrialObservation now;
    Lab() { now.valid = true; now.attackerStatus = 0; now.attackerAction = Stand; now.defenderStatus = 0; }
    void Frames(int count = 1) {
        for (int i = 0; i < count; ++i) { session.Observe(now); now.attackerActionFrame += 1; }
    }
    // The attacker starts an action; a move runs in AS_SKILL.
    void Act(int action, unsigned status = 16) {
        now.attackerAction = action; now.attackerStatus = status; now.attackerActionFrame = 0;
        Frames(3);
    }
    void Hit(float damage, unsigned status = 21) {
        now.defenderStatus = status; now.defenderComboDamage += damage;
        Frames(3);
    }
    // The defender is free again; the game leaves the combo damage standing.
    void Recover() { now.defenderStatus = 0; Frames(2); }
    void Idle() { now.attackerStatus = 0; Frames(2); }
    void Load(Trial trial) { std::string error; CHECK(session.Load(std::move(trial), error)); Frames(); }
    const TrialView& View() const { return session.GetView(); }
};
TrialStep Move(std::vector<int> ids, bool mustHit = true) { TrialStep step; step.ids = std::move(ids); step.mustHit = mustHit; return step; }
Trial Combo(std::vector<TrialStep> steps) {
    Trial trial; trial.steps = std::move(steps);
    trial.moves = {Jab, Strong, Low, Fireball, Uppercut, Focus, JumpKick};
    return trial;
}
using S = TrialStepState;
}

int main() {
    std::string error;
    {
        // A trial needs steps, a bounded count, and at least one known move.
        TrialSession session;
        CHECK(!session.Load(Trial{}, error) && !error.empty());
        Trial unknown; unknown.steps.resize(3);
        CHECK(!session.Load(unknown, error) && error.find("known") != std::string::npos);
        Trial big; big.steps.assign(MaxTrialSteps + 1, Move({Jab}));
        CHECK(!session.Load(big, error));
        session.Observe(TrialObservation{}); // Nothing loaded: nothing to judge.
        CHECK(session.GetView().steps.empty() && session.GetView().attempts == 0);
        big.steps.pop_back();
        CHECK(session.Load(big, error) && session.GetView().steps.size() == MaxTrialSteps);
    }
    {
        // Success: each move comes out and hits in order, linked or cancelled,
        // with walking and crouching between them ignored.
        Lab lab; lab.Load(Combo({Move({Jab}), Move({Strong}), Move({Low}), Move({Fireball})}));
        CHECK(lab.View().current == 0 && lab.View().steps[0] == S::Waiting);
        lab.Act(Jab);
        CHECK(lab.View().steps[0] == S::Out && lab.View().attempts == 0);
        lab.Hit(30);
        CHECK(lab.View().steps[0] == S::Done && lab.View().current == 1 && lab.View().attempts == 1);
        lab.Act(Strong); lab.Hit(60);
        lab.Act(Crouching, 3); lab.Act(Walk, 10);
        CHECK(lab.View().current == 2);
        lab.Act(Low); lab.Hit(40);
        lab.Act(Fireball);
        CHECK(!lab.View().complete && lab.View().steps[3] == S::Out);
        lab.Hit(50, 23);
        CHECK(lab.View().complete && lab.View().successes == 1 && lab.View().current == 4);
        CHECK(lab.View().lastFailure == TrialFailure::None && lab.View().failedStep == -1);
        // The finished combo stays shown while it plays out.
        lab.now.defenderStatus = 18; lab.Frames(3); lab.Recover(); lab.Idle();
        CHECK(lab.View().complete && lab.View().steps[3] == S::Done);
        // Restart after success: the first move starts a new attempt.
        lab.Act(Jab);
        CHECK(!lab.View().complete && lab.View().current == 0 && lab.View().steps[0] == S::Out && lab.View().steps[3] == S::Waiting);
        lab.Hit(30); lab.Act(Strong); lab.Hit(60); lab.Act(Low); lab.Hit(40); lab.Act(Fireball); lab.Hit(50);
        CHECK(lab.View().successes == 2 && lab.View().attempts == 2);
    }
    {
        // Dropped: the opponent recovers before the next move hits.
        Lab lab; lab.Load(Combo({Move({Jab}), Move({Strong})}));
        lab.Act(Jab); lab.Hit(30); lab.Recover();
        CHECK(lab.View().lastFailure == TrialFailure::Dropped && lab.View().failedStep == 1);
        CHECK(lab.View().current == 0 && lab.View().steps[0] == S::Waiting && lab.View().attempts == 1 && lab.View().successes == 0);
        // The late move then hits a free opponent: a new combo, not step two.
        lab.Act(Strong); lab.Hit(60);
        CHECK(lab.View().current == 0 && !lab.View().complete);
        // A move that was out when the opponent recovered is dropped as well.
        lab.Recover(); lab.Idle();
        lab.Act(Jab); lab.Hit(30); lab.Act(Strong); lab.Recover();
        CHECK(lab.View().lastFailure == TrialFailure::Dropped && lab.View().current == 0);
        // Knocked down counts as recovered: a wake-up hit is a new combo.
        lab.Idle(); lab.Act(Jab); lab.Hit(30, 23);
        lab.now.defenderStatus = 18; lab.Frames(3);
        CHECK(lab.View().current == 1);
        lab.now.defenderStatus = 19; lab.Frames(3);
        CHECK(lab.View().current == 0 && lab.View().failedStep == 1);
        lab.now.defenderStatus = 20; lab.Frames(3);
        lab.Act(Strong); lab.Hit(60);
        CHECK(lab.View().current == 0 && lab.View().attempts == 3 && lab.View().successes == 0);
        // Each failure is counted on the step it happened at.
        CHECK(lab.View().failures == 3 && lab.View().drops == (std::vector<unsigned>{0, 3}));
    }
    {
        // Wrong move: another move of the fighter, or the combo's own moves
        // out of order.
        Lab lab; lab.Load(Combo({Move({Jab}), Move({Strong}), Move({Low})}));
        lab.Act(Jab); lab.Hit(30); lab.Act(Uppercut);
        CHECK(lab.View().lastFailure == TrialFailure::WrongMove && lab.View().failedStep == 1 && lab.View().current == 0);
        lab.Recover(); lab.Idle();
        lab.Act(Jab); lab.Hit(30); lab.Act(Low);
        CHECK(lab.View().lastFailure == TrialFailure::WrongMove && lab.View().current == 0);
        // The wrong move is the first move: it starts the next attempt.
        lab.Recover(); lab.Idle();
        lab.Act(Jab); lab.Hit(30); lab.Act(Strong); lab.Hit(60); lab.Act(Jab);
        CHECK(lab.View().lastFailure == TrialFailure::WrongMove && lab.View().failedStep == 2);
        CHECK(lab.View().current == 0 && lab.View().steps[0] == S::Out);
        lab.Hit(30);
        CHECK(lab.View().current == 1);
        // A wrong move while the current one has yet to hit.
        lab.Recover(); lab.Idle();
        lab.Act(Jab); lab.Hit(30); lab.Act(Strong); lab.Act(Uppercut);
        CHECK(lab.View().lastFailure == TrialFailure::WrongMove && lab.View().failedStep == 1 && lab.View().current == 0);
        // Before the first move is done nothing fails, whatever comes out.
        Lab idle; idle.Load(Combo({Move({Jab}), Move({Strong})}));
        idle.Act(Uppercut); idle.Act(Strong); idle.Hit(60); idle.Recover();
        CHECK(idle.View().lastFailure == TrialFailure::None && idle.View().current == 0 && idle.View().attempts == 0);
        // With no move list, only the combo's own moves are told apart.
        Trial bare; bare.steps = {Move({Jab}), Move({Strong}), Move({Low})};
        Lab plain; plain.Load(bare);
        plain.Act(Jab); plain.Hit(30); plain.Act(Uppercut);
        CHECK(plain.View().current == 1 && plain.View().lastFailure == TrialFailure::None);
        plain.Act(Low);
        CHECK(plain.View().lastFailure == TrialFailure::WrongMove);
    }
    {
        // Whiffed: the next move comes out before this one hit.
        Lab lab; lab.Load(Combo({Move({Jab}), Move({Low}), Move({Fireball})}));
        lab.Act(Jab); lab.Hit(30); lab.Act(Low); lab.Act(Fireball);
        CHECK(lab.View().lastFailure == TrialFailure::Whiffed && lab.View().failedStep == 1 && lab.View().current == 0);
        // The fireball then hitting does not count for anything.
        lab.Hit(50);
        CHECK(lab.View().current == 0 && !lab.View().complete);
        // A first move that whiffs is forgotten without a failure when another
        // move comes out.
        Lab first; first.Load(Combo({Move({Jab}), Move({Strong})}));
        first.Act(Jab);
        CHECK(first.View().steps[0] == S::Out);
        first.Idle();
        CHECK(first.View().steps[0] == S::Out);
        first.Act(Uppercut);
        CHECK(first.View().steps[0] == S::Waiting);
        first.Hit(100, 23);
        CHECK(first.View().current == 0 && first.View().attempts == 0 && first.View().lastFailure == TrialFailure::None);
        // Whiffed into the second move, which hits: still nothing done.
        first.Recover(); first.Idle();
        first.Act(Jab); first.Act(Strong); first.Hit(60);
        CHECK(first.View().current == 0 && first.View().lastFailure == TrialFailure::None);
    }
    {
        // A first-step fireball that lands after its user is idle again (a
        // replay: 40 frames in AS_SKILL, 10 idle, then the hit) is its hit.
        Lab lab; lab.Load(Combo({Move({Fireball}), Move({Uppercut})}));
        lab.Act(Fireball); lab.Frames(37); lab.Idle(); lab.Frames(8);
        CHECK(lab.View().steps[0] == S::Out);
        lab.Hit(50);
        CHECK(lab.View().current == 1 && lab.View().attempts == 1 && lab.View().steps[0] == S::Done);
    }
    {
        // Several hits: the first completes the step, the rest are its own.
        Lab lab; lab.Load(Combo({Move({Fireball}), Move({Uppercut})}));
        lab.Act(Fireball); lab.Hit(24);
        CHECK(lab.View().current == 1 && lab.View().steps[1] == S::Waiting);
        lab.Hit(24); lab.Hit(24); lab.Hit(24);
        CHECK(lab.View().current == 1 && lab.View().steps[1] == S::Waiting && !lab.View().complete);
        lab.Act(Uppercut); lab.Hit(49); lab.Hit(35, 23);
        CHECK(lab.View().complete && lab.View().successes == 1 && lab.View().attempts == 1);
    }
    {
        // The same move twice in a row: two starts and two hits. The second
        // start shows only as the action frame running backwards.
        Lab lab; lab.Load(Combo({Move({Jab}), Move({Jab}), Move({Strong})}));
        lab.Act(Jab); lab.Hit(30);
        lab.Hit(30); // A second hit of one jab is not a second jab.
        CHECK(lab.View().current == 1 && lab.View().steps[1] == S::Waiting);
        lab.Act(Jab);
        CHECK(lab.View().steps[1] == S::Out);
        lab.Hit(27);
        CHECK(lab.View().current == 2);
        // A third jab continues the step before and is let through.
        lab.Act(Jab); lab.Hit(24);
        CHECK(lab.View().current == 2 && lab.View().lastFailure == TrialFailure::None);
        lab.Act(Strong); lab.Hit(40);
        CHECK(lab.View().complete);
    }
    {
        // Steps that only come out: a jump before the jump attack, and the
        // dash of an FADC, which shows the dash action in AS_SKILL.
        Lab lab; lab.Load(Combo({Move({Jump}, false), Move({JumpKick}), Move({Fireball}), Move({Dash}, false), Move({Uppercut})}));
        lab.Act(PreJump, 5);
        CHECK(lab.View().current == 0);
        lab.Act(Jump, 2);
        CHECK(lab.View().steps[0] == S::Done && lab.View().current == 1 && lab.View().attempts == 1);
        // No hit yet, so the free opponent does not drop anything.
        lab.Frames(20);
        CHECK(lab.View().current == 1);
        lab.Act(JumpKick); lab.Hit(100); lab.Act(Landing, 6);
        lab.Act(Fireball); lab.Hit(56);
        // The focus attack is left out of this trial's moves, as the dash step passes through it.
        Trial trial = Combo({Move({Jab}), Move({Dash}, false), Move({Uppercut})});
        trial.moves = {Jab, Strong, Low, Fireball, Uppercut};
        Lab fadc; fadc.Load(trial);
        fadc.Act(Jab); fadc.Hit(30); fadc.Act(Focus); fadc.Act(Dash);
        CHECK(fadc.View().steps[1] == S::Done && fadc.View().current == 2);
        fadc.Act(Crouching, 3); fadc.Act(Uppercut); fadc.Hit(49);
        CHECK(fadc.View().complete);
        // Listed as a move, the focus attack is a wrong move.
        lab.Act(Focus);
        CHECK(lab.View().lastFailure == TrialFailure::WrongMove && lab.View().failedStep == 3);
    }
    {
        // A move that passes through a second script: listed in the step it
        // continues the step; unlisted it is a transition. Neither fails.
        Trial trial = Combo({Move({Uppercut, UppercutFall}), Move({Dash}, false), Move({Fireball})});
        trial.moves.push_back(UppercutFall);
        Lab lab; lab.Load(trial);
        lab.Act(Uppercut); lab.Act(UppercutFall);
        CHECK(lab.View().steps[0] == S::Out);
        lab.Hit(100, 23);
        CHECK(lab.View().current == 1);
        lab.Act(UppercutFall);
        CHECK(lab.View().current == 1 && lab.View().lastFailure == TrialFailure::None);
        lab.Act(Dash); lab.Act(Fireball); lab.Hit(30, 23);
        CHECK(lab.View().complete);
        // The same in the middle of a combo, before the move has hit.
        trial.steps.insert(trial.steps.begin(), Move({Jab}));
        lab.Load(trial);
        lab.Act(Jab); lab.Hit(30); lab.Act(Uppercut); lab.Act(UppercutFall);
        CHECK(lab.View().steps[1] == S::Out && lab.View().lastFailure == TrialFailure::None);
    }
    {
        // A step with no known ids is passed unjudged and says so, and its
        // real move coming out is not called wrong.
        Lab lab; lab.Load(Combo({Move({}), Move({Jab}), Move({}), Move({Low}), Move({})}));
        CHECK(lab.View().steps[0] == S::Unchecked && lab.View().current == 1);
        lab.Act(Jab);
        lab.Act(Strong); // May be the unknown move after the jab: not judged.
        CHECK(lab.View().steps[1] == S::Out && lab.View().lastFailure == TrialFailure::None);
        lab.Act(Jab); lab.Hit(30);
        CHECK(lab.View().steps[2] == S::Unchecked && lab.View().current == 3 && lab.View().attempts == 1);
        lab.Act(Strong); lab.Hit(60);
        CHECK(lab.View().current == 3 && lab.View().lastFailure == TrialFailure::None);
        lab.Act(Low); lab.Hit(40);
        CHECK(lab.View().complete && lab.View().steps[4] == S::Unchecked && lab.View().current == 5);
        // The combo must still hold together around the unknown step.
        lab.Recover(); lab.Idle();
        lab.Act(Jab); lab.Hit(30); lab.Recover();
        CHECK(lab.View().lastFailure == TrialFailure::Dropped && lab.View().failedStep == 3);
        CHECK(lab.View().steps[0] == S::Unchecked && lab.View().current == 1);
    }
    {
        // A throw or cinematic puts the defender in AS_SEQUENCE: that is a hit.
        Lab lab; lab.Load(Combo({Move({Jab}), Move({Uppercut})}));
        lab.Act(Jab); lab.Hit(0, 24);
        CHECK(lab.View().current == 1);
        lab.Act(Uppercut); lab.Hit(130, 24);
        CHECK(lab.View().complete);
        // A stunned opponent has not recovered.
        lab.now.defenderStatus = 17; lab.Frames(3);
        lab.Act(Jab); lab.Hit(30);
        lab.now.defenderStatus = 17; lab.Frames(3);
        CHECK(lab.View().current == 1 && lab.View().lastFailure == TrialFailure::None);
        // Rising into a stun is no hit.
        lab.Recover(); lab.Idle();
        lab.Act(Jab); lab.now.defenderStatus = 20; lab.Frames(2); lab.now.defenderStatus = 17; lab.Frames(2);
        CHECK(lab.View().steps[0] == S::Out && lab.View().attempts == 2);
        // A blocked move is no hit either, though the combo damage changes.
        lab.now.defenderStatus = 22; lab.now.defenderComboDamage = 0; lab.Frames(2);
        CHECK(lab.View().steps[0] == S::Out);
    }
    {
        // Broken frames: an invalid sample or Restart abandons the attempt
        // without a failure and never pairs frames across the gap.
        Lab lab; lab.Load(Combo({Move({Jab}), Move({Strong})}));
        lab.Act(Jab); lab.Hit(30);
        lab.now.valid = false; lab.Frames();
        CHECK(lab.View().current == 0 && lab.View().lastFailure == TrialFailure::None && lab.View().attempts == 1);
        lab.now.valid = true; lab.now.attackerAction = Jab; lab.now.attackerActionFrame = 0;
        lab.now.defenderStatus = 21; lab.now.defenderComboDamage = 500;
        lab.Frames(3); // First frame after the gap: neither a start nor a hit.
        CHECK(lab.View().steps[0] == S::Waiting);
        lab.Recover(); lab.Idle(); lab.Act(Jab); lab.Hit(30);
        lab.session.Restart();
        CHECK(lab.View().current == 0 && lab.View().attempts == 2 && lab.View().lastFailure == TrialFailure::None);
        lab.Frames();
        // Loading again clears the counters and the last failure.
        lab.Act(Jab); lab.Hit(30); lab.Recover();
        CHECK(lab.View().lastFailure == TrialFailure::Dropped);
        lab.Load(Combo({Move({Jab})}));
        CHECK(lab.View().attempts == 0 && lab.View().lastFailure == TrialFailure::None && lab.View().steps.size() == 1);
        // The adapter's conversion keeps the attacker and defender apart.
        FighterSample attacker, defender;
        attacker.valid = defender.valid = true;
        attacker.status = 16; attacker.action = Jab; attacker.actionFrame = 3; attacker.comboDamage = 9;
        defender.status = 21; defender.action = 131; defender.comboDamage = 27;
        const TrialObservation seen = ObserveTrial(attacker, defender);
        CHECK(seen.valid && seen.attackerStatus == 16 && seen.attackerAction == Jab && seen.attackerActionFrame == 3);
        CHECK(seen.defenderStatus == 21 && seen.defenderComboDamage == 27);
        defender.valid = false;
        CHECK(!ObserveTrial(attacker, defender).valid);
    }
    {
        // A step whose ids are wrong never ticks; the log line names the ids it
        // waits for and the moves that did come out.
        Lab lab;
        CHECK(lab.session.Unmatched().empty());
        lab.Load(Combo({Move({Jab}), Move({}), Move({999, 998})}));
        CHECK(lab.session.Unmatched().empty());
        lab.Act(Walk, 0);
        CHECK(lab.session.Unmatched().empty());
        lab.Act(Jab); lab.Hit(30); lab.Act(Uppercut); lab.Act(Uppercut); lab.Recover();
        CHECK(lab.session.Unmatched() == "step 3 waits for 999 998; seen 256 395");
        // Coming out off-step is not a match.
        lab.Act(999);
        CHECK(lab.session.Unmatched() == "step 3 waits for 999 998; seen 256 395 999");
        lab.Act(Jab); lab.Hit(30); lab.Act(998);
        CHECK(lab.session.Unmatched().empty());
        // Bounded however many moves come out, and cleared by a new trial.
        for (int action = 400; action < 500; ++action) lab.Act(action);
        lab.Load(Combo({Move({Jab})}));
        lab.Act(1000);
        CHECK(lab.session.Unmatched() == "step 1 waits for 256; seen 1000");
        Trial wide;
        wide.steps.assign(MaxTrialSteps, Move(std::vector<int>(40, 123456)));
        lab.Load(wide);
        for (int action = 400; action < 500; ++action) lab.Act(action);
        CHECK(!lab.session.Unmatched().empty() && lab.session.Unmatched().size() < 1600);
    }
    {
        // A performed combo is written down move by move: a new attack while
        // still attacking is a cancel, and the capture ends on its own after idling.
        ComboCapture capture; FighterSample fighter; fighter.valid = true;
        auto frames = [&](unsigned status, int action, int count) { fighter.status = status; fighter.action = action; for (int i = 0; i < count; ++i) capture.Observe(fighter); };
        capture.Start();
        frames(0, 0, 5); frames(16, 300, 8); frames(16, 310, 20); frames(0, 0, 10); frames(16, 300, 12); frames(0, 0, 10);
        CHECK(capture.Active() && capture.Events().size() == 3);
        CHECK(capture.Events()[0].action == 300 && !capture.Events()[0].cancel);
        CHECK(capture.Events()[1].action == 310 && capture.Events()[1].cancel);
        CHECK(capture.Events()[2].action == 300 && !capture.Events()[2].cancel);
        frames(0, 0, CaptureIdleFrames);
        CHECK(!capture.Active() && capture.Events().size() == 3);
        // The same move again, after a neutral frame, is a second move.
        capture.Start(); frames(16, 300, 5); frames(0, 0, 1); frames(16, 300, 5);
        CHECK(capture.Events().size() == 2 && !capture.Events()[1].cancel);
    }
    return 0;
}
