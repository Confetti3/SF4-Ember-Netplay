#include "../training/MoveInputs.hxx"
#include "../training/RecordingFile.hxx"
#include "test_support.hxx"

using namespace sf4e::combo;

static std::vector<std::string> Steps(const char* line) {
    std::vector<std::string> steps;
    std::string error;
    CHECK(ParseSteps(line, steps, error));
    return steps;
}

// The frames of whole moves; every line synthesized here fits a slot.
static std::vector<sf4e::training::Input> Synth(const std::vector<std::string>& steps, bool facingRight) {
    Compiled compiled;
    std::string error;
    CHECK(Synthesize(steps, facingRight, compiled, error));
    return compiled.frames;
}

int main() {
    std::vector<std::string> steps;
    std::string error;
    Step step;

    // A step is the inputs it stands for, however it was spelled.
    CHECK(ParseStep("cr.mk", step, error) && step.motion == "2" && step.buttons == MK && step.need == 1 && !step.air);
    CHECK(ParseStep("HP", step, error) && step.motion == "5" && step.buttons == HP);
    CHECK(ParseStep("j.HK", step, error) && step.air && step.motion.empty() && step.buttons == HK);
    CHECK(ParseStep("236236ppp", step, error) && step.motion == "236236" && step.buttons == Punches && step.need == 3);
    CHECK(ParseStep("623PP", step, error) && step.buttons == Punches && step.need == 2);
    CHECK(ParseStep("[4]6HP", step, error) && step.charge && step.motion == "46");
    CHECK(ParseStep("LP + LK", step, error) && step.buttons == (LP | LK) && step.need == 2);
    CHECK(ParseStep("360HP", step, error) && step.motion == "360");
    CHECK(ParseStep("66", step, error) && step.motion == "66" && !step.buttons);
    CHECK(ParseStep("FADC", step, error) && step.motion == "66" && step.buttons == (MP | MK) && step.need == 2);
    // Cancels, both focus dashes, held and released buttons, close and far normals.
    CHECK(ParseStep("xx 236HP", step, error) && step.cancel && step.motion == "236");
    CHECK(ParseStep("FADC44", step, error) && step.motion == "44" && step.buttons == (MP | MK));
    CHECK(ParseStep("[MP+MK]", step, error) && step.edge == Edge::Hold && step.buttons == (MP | MK) && step.need == 2);
    CHECK(ParseStep("236]lp[", step, error) && step.edge == Edge::Release && step.motion == "236" && step.buttons == LP);
    CHECK(ParseStep("[4]6[PP]", step, error) && step.charge && step.edge == Edge::Hold && step.need == 2);
    CHECK(ParseStep("cl.HP", step, error) && step.range == Range::Close && step.motion == "5");
    CHECK(ParseStep("far.MK", step, error) && step.range == Range::Far);
    CHECK(ParseStep("st.MK", step, error) && step.range == Range::Any);
    // A follow-up: "~" before the move, a cancel with no cue of its own.
    CHECK(ParseStep("~ LP", step, error) && step.follow && step.cancel && step.buttons == LP && Canonical(step) == "~ 5LP");
    CHECK(ParseStep("~lp@+2", step, error) && step.follow && Canonical(step) == "~ 5LP@+2");
    CHECK((Tokens("236P ~ LP > 5HP") == std::vector<std::string>{"236P", "~ LP", "5HP"}));
    // Moves typed a line each read as the same moves on one line.
    CHECK((Tokens("2MK\r\nxx 236HP\n\n5HP >\n FADC\n~ LP") == std::vector<std::string>{"2MK", "xx 236HP", "5HP", "FADC", "~ LP"}));
    CHECK((Steps("2MK\nxx 236HP\n5HP") == Steps("2MK xx 236HP > 5HP")));
    {
        const auto follow = Synth({"236P", "~ 5LP@+2"}, true);
        for (const auto& frame : follow) CHECK(!frame.wait);
        std::size_t press = 0;
        while (press < follow.size() && follow[press].raw != LP) ++press;
        CHECK(press == 18 + FollowDelay - 2 && follow[12].raw == 0);
    }
    // A timing offset rides on any move and is kept in its canonical form.
    CHECK(ParseStep("cr.MK@-1", step, error) && step.offset == -1 && Canonical(step) == "2MK@-1");
    CHECK(ParseStep("xx 236HP(mash)@+3", step, error) && step.offset == 3 && step.mash && Canonical(step) == "xx 236HP(mash)@+3");
    CHECK(ParseStep("FADC@+2", step, error) && step.offset == 2 && Canonical(step) == "FADC@+2");
    CHECK(ParseStep("5LP@+0", step, error) && step.offset == 0 && Canonical(step) == "5LP");
    // A frame to land on; kept before the offset.
    CHECK(ParseStep("2MK#45", step, error) && step.at == 45 && step.offset == 0 && Canonical(step) == "2MK#45");
    // A direction held alone: until the next move, or for a count of frames;
    // nothing held for a count is a wait. The count is digits before an F.
    CHECK(ParseStep("[1]", step, error) && step.hold && step.motion == "1" && !step.frames && !step.buttons && Canonical(step) == "[1]");
    CHECK(ParseStep("[2] 30F", step, error) && step.hold && step.motion == "2" && step.frames == 30 && Canonical(step) == "[2]30f");
    CHECK(ParseStep("20f", step, error) && step.hold && step.motion == "5" && step.frames == 20 && Canonical(step) == "20f");
    CHECK(ParseStep("[5]8f", step, error) && step.hold && Canonical(step) == "8f");
    CHECK(ParseStep("qcf", step, error) && !step.hold && step.motion == "236" && ParseStep("[4]6P", step, error) && !step.hold && step.charge);
    for (const char* bad : {"[5]", "[1]0f", "0f", "f", "[0]", "[12]", "[1]@+1", "20f#5", "xx [1]", "~ 20f", "[1]7201f", "[1]20fHP", "[HP]20f"})
        CHECK(!ParseStep(bad, step, error));
    // A hold kept until the next move needs one after it.
    CHECK(ParseSteps("[1] > 3hp", steps, error) && steps == (std::vector<std::string>{"[1]", "3HP"}));
    CHECK(!ParseSteps("2LK > [1]", steps, error) && steps.empty() && !error.empty() && ParseSteps("2LK > [1]10f", steps, error));
    // It ends as the next step begins, so that step is a move: a hold or a wait after it is refused.
    for (const char* line : {"[4] > [2]30f > 5MK", "[4] > 10f > 5MK", "[4] > [2] > 5MK", "5LP > [4] > 10f > 6P"}) CHECK(!ParseSteps(line, steps, error) && steps.empty());
    CHECK(ParseSteps("[2]30f > [4] > 5MK", steps, error) && ParseSteps("10f > [4]20f > 5MK", steps, error));
    {
        // A dash is two taps, not one hold.
        const auto dash = Synth({"66"}, true);
        CHECK(dash.size() >= 7 && dash[0].raw == Right && dash[3].raw == 0 && dash[4].raw == Right);
    }
    // A mash: five presses, several buttons cycled a frame each, one button on and off.
    {
        const auto first = [](const std::vector<sf4e::training::Input>& f) { std::size_t i = 0; while (i < f.size() && !f[i].raw) ++i; return i; };
        const auto piano = Synth({"5P(mash)"}, true); const auto p = first(piano);
        CHECK(p + 5 < piano.size() && piano[p].raw == LP && piano[p + 1].raw == MP && piano[p + 2].raw == HP && piano[p + 3].raw == LP && piano[p + 4].raw == MP && piano[p + 5].raw == 0);
        const auto one = Synth({"5HP(mash)"}, true); const auto o = first(one);
        CHECK(o + 9 < one.size() && one[o].raw == HP && one[o + 1].raw == 0 && one[o + 8].raw == HP && one[o + 9].raw == 0);
        const auto two = Synth({"5LP+MP(mash)"}, true); const auto t = first(two);
        CHECK(two[t].raw == LP && two[t + 1].raw == MP && two[t + 4].raw == LP);
        // An order of its own sets the presses and their count; a repeat gets a frame off between.
        CHECK(ParseStep("5P(mash HP-MP-LP-LP)", step, error) && step.mash && step.mashOrder.size() == 4 && Canonical(step) == "5P(mash HP-MP-LP-LP)");
        CHECK(!ParseStep("5P(mash HP-XX)", step, error) && !ParseStep("5P(mash -)", step, error) && !ParseStep("5P(mash LP-LP-LP-LP-LP-LP-LP-LP-LP-LP-LP)", step, error));
        // Each press may carry its own direction; the dashes are optional.
        CHECK(ParseStep("5K(mash 1MK 1MK 3MK)", step, error) && step.mashOrder.size() == 3 && step.mashOrder[0].direction == '1' && step.mashOrder[2].direction == '3' && Canonical(step) == "5K(mash 1MK-1MK-3MK)");
        const auto legs = Synth({"5K(mash 1MK-1MK-3MK)"}, true); const auto l = first(legs);
        CHECK(legs[l].raw == (Down | Left | MK) && legs[l + 1].raw == (Down | Left) && legs[l + 2].raw == (Down | Left | MK) && legs[l + 3].raw == (Down | Right) && legs[l + 4].raw == (Down | Right | MK));
        const auto ordered = Synth({"5P(mash HP-MP-LP-LP)"}, true); const auto q = first(ordered);
        CHECK(ordered[q].raw == HP && ordered[q + 1].raw == MP && ordered[q + 2].raw == LP && ordered[q + 3].raw == 0 && ordered[q + 4].raw == LP && ordered[q + 5].raw == 0);
    }
    // The red focus cancel: LP+MP+MK, then the dash.
    CHECK(ParseStep("xx RFADC", step, error) && step.cancel && step.buttons == (LP | MP | MK) && step.need == 3 && step.motion == "66" && Canonical(step) == "xx RFADC");
    CHECK(ParseStep("rfadc44@+1", step, error) && step.motion == "44" && Canonical(step) == "RFADC44@+1");
    {
        // The red focus is tapped, then waits for its own hit before the dash.
        const auto frames = Synth({"5HP", "xx RFADC"}, true);
        std::size_t press = 0; while (press < frames.size() && frames[press].raw != (LP | MP | MK)) ++press;
        CHECK(press + 4 < frames.size() && frames[press + 2].raw == 0 && frames[press + 4].wait == sf4e::training::WaitHit && frames[press + 5].raw == Right);
    }
    CHECK(ParseStep("5HP@-1#14", step, error) && step.at == 14 && step.offset == -1 && Canonical(step) == "5HP#14@-1");
    CHECK(!ParseStep("5HP#14#15", step, error) && !ParseStep("5HP@+1@+2", step, error));
    CHECK(ParseStep("xx 236HP#120@+2", step, error) && step.at == 120 && step.offset == 2 && Canonical(step) == "xx 236HP#120@+2");
    CHECK(ParseStep("FADC#0", step, error) && step.at == 0 && Canonical(step) == "FADC#0");
    CHECK(ParseStep("5LP", step, error) && step.at == -1);
    for (const char* bad : {"2MK#", "2MK#-1", "2MK#7201", "2MK#12345", "2MK#a", "#45"}) CHECK(!ParseStep(bad, step, error));
    for (const char* bad : {"2MK@", "2MK@1", "2MK@-121", "2MK@+121", "2MK@-a", "@-1", "xx", "FADC4", "FADC6", "[]", "][", "[HP", "]HP]", "[LP+]", "cl.", "xx xx 5LP", "cr.", "hadouken", "236H", "236HPP", "LP+", "LP+LP", "LPLK", "+LP", "PPPP",
        "[4]HP", "[46]HP", "cr.236HP", "0HP", "12345678912LP", "236 HP x"})
        CHECK(!ParseStep(bad, step, error));

    // Typed notation becomes canonical steps; one bad step refuses the line.
    CHECK(Steps("  cr.MK >236hp ,, fadc  >  st.HP > j.hk > lk+lp > [2]8K ") ==
        (std::vector<std::string>{"2MK", "236HP", "FADC", "5HP", "j.HK", "5LP+LK", "[2]8K"}));
    CHECK(Steps("cl.hp XX 623]HP[ xx fadc44 > far.mk, [mp+mk] > ]MP+MK[") ==
        (std::vector<std::string>{"cl.HP", "xx 623]HP[", "xx FADC44", "far.MK", "5[MP+MK]", "5]MP+MK["}));
    CHECK(Steps(" > , ").empty());
    CHECK(!ParseSteps("cr.MK > shoryu", steps, error) && steps.empty() && error == "\"shoryu\" is not a move");
    CHECK(!ParseSteps("xx 236HP", steps, error) && error == "the first move cannot be a cancel");
    // Leading holds and waits are no move to cancel or follow up.
    for (const char* line : {"[1] > xx 236HP", "[2]10f > xx 236HP", "~ LP", "[1] > ~ LP", "236]HP[", "[1] > ]HP[ > 5MK"}) CHECK(!ParseSteps(line, steps, error) && steps.empty());
    CHECK(ParseSteps("5LP > 236]HP[", steps, error) && ParseSteps("[1] > [HP]", steps, error));
    CHECK(!ParseSteps("2MK xx", steps, error));
    CHECK(ParseStep("[2]8LP(mash)", step, error) && step.mash && step.charge && step.buttons == LP);
    CHECK(!ParseStep("236(mash)", step, error) && !ParseStep("FADC(mash)", step, error));

    // Typed steps become pad input, timed by the fight: a cancelled move does
    // its motion at once but the last direction, which it presses with the
    // button on the hit; a linked move holds its first direction through the
    // wait for the free frame.
    {
        const auto frames = Synth(Steps("2MK xx 236HP@+1 > 2HP@+3 > [4]6P@+1"), true);
        CHECK(!frames.empty() && frames.size() < 200);
        std::vector<std::string> seen;
        for (const auto& frame : frames) {
            const std::string text = (frame.wait == sf4e::training::WaitHit ? "hit:" : frame.wait == sf4e::training::WaitActionable ? "free:" : "") +
                std::to_string(frame.raw) + (frame.wait ? "@" + std::to_string(frame.offset) : "");
            if (seen.empty() || seen.back() != text) seen.push_back(text);
        }
        const std::vector<std::string> expected{
            std::to_string(Down), std::to_string(Down | MK), std::to_string(Down), "0",
            std::to_string(Down), std::to_string(Down | Right), "hit:" + std::to_string(Down | Right) + "@1", std::to_string(Right | HP), std::to_string(Right),
            "free:" + std::to_string(Down) + "@3", std::to_string(Down | HP), std::to_string(Down),
            "free:" + std::to_string(Left) + "@1", std::to_string(Left), std::to_string(Right), std::to_string(Right | LP), std::to_string(Right), "0"};
        CHECK(seen == expected);
        int held = 0;
        for (const auto& frame : frames) held += frame.raw == Left && !frame.wait;
        CHECK(held == ChargeFrames && frames[0].mapped == frames[0].raw);
        // Facing left mirrors forward and back.
        const auto mirrored = Synth(Steps("236HP > 5LP"), false);
        CHECK(mirrored[3].raw == (Down | Left) && mirrored[9].raw == (Left | HP) && mirrored[11].wait == sf4e::training::WaitActionable && mirrored[12].raw == LP);
        {
            Compiled compiled; compiled.frames = {{1, 1, 0, 0}};
            CHECK(Synth({}, true).empty() && !Synthesize({"nonsense"}, true, compiled, error) && compiled.frames.empty() && !error.empty());
        }
        // Moves on frames: the press lands on its frame, no cue is waited for,
        // and a frame already passed starts the move at once.
        const auto timed = Synth(Steps("[2]2f > 5LP#10 > 2MK#30 > xx 236HP#42 > 5HP#43"), true);
        CHECK(timed[10].raw == LP && timed[9].raw == 0 && timed[30].raw == (Down | MK) && timed[27].raw == Down && timed[42].raw == (Right | HP));
        for (const auto& frame : timed) CHECK(!frame.wait);
        CHECK(timed[29].raw == Down && timed[31].raw == Down && timed[32].raw == 0 && timed[36].raw == Down);
        CHECK(timed.size() > 44 && timed.back().raw == 0);
        // A hold for frames is that many frames. One kept until the next move
        // is held through that move's wait, or a follow-up's delay; first in
        // the line it is a reply's leading hold, carrying the move's offset.
        // After either, a lone direction's press follows it at once, a
        // motion's first direction after it. A wait is frames with nothing held.
        const auto runs = [](const std::vector<sf4e::training::Input>& frames) {
            std::vector<std::string> seen;
            for (const auto& frame : frames) {
                const std::string text = (frame.wait == sf4e::training::WaitActionable ? "free:" : frame.wait == sf4e::training::WaitReply ? "hold:" : "") +
                    std::to_string(frame.raw);
                if (seen.empty() || seen.back() != text) seen.push_back(text);
            }
            return seen;
        };
        const auto text = [](unsigned bits) { return std::to_string(bits); };
        // Holds and waits with a count before the first move lead into it: no
        // move to recover from, so it plays straight after them.
        CHECK(runs(Synth(Steps("[1]4f > 3HP"), true)) ==
            (std::vector<std::string>{text(Down | Left), text(Down | Right), text(Down | Right | HP), text(Down | Right), "0"}));
        const auto fixed = Synth(Steps("[1]4f > 3HP"), true);
        CHECK(fixed.size() == 10 && fixed[3].raw == (Down | Left) && std::none_of(fixed.begin(), fixed.end(), [](const sf4e::training::Input& frame) { return frame.wait; }));
        CHECK(runs(Synth(Steps("[1] > 3HP"), true)) ==
            (std::vector<std::string>{"hold:" + text(Down | Left), text(Down | Right | HP), text(Down | Right), "0"}));
        CHECK(runs(Synth(Steps("[1] > 623P"), true)) ==
            (std::vector<std::string>{"hold:" + text(Down | Left), text(Right), text(Down), text(Down | Right), text(Down | Right | LP), text(Down | Right), "0"}));
        CHECK(runs(Synth(Steps("[1] > 8"), true)) == (std::vector<std::string>{"hold:" + text(Down | Left), text(Up), "0"}));
        CHECK(runs(Synth(Steps("5LP > [1] > 3HP"), true)) ==
            (std::vector<std::string>{text(LP), "0", "free:" + text(Down | Left), text(Down | Right | HP), text(Down | Right), "0"}));
        {
            // The move's own offset is the reply's, not its hold's.
            Compiled compiled;
            CHECK(Synthesize(Steps("[1] > 3HP@+3"), true, compiled, error) && compiled.offset == 3 && compiled.frames[0].offset == 0);
        }
        // Facing left, back is Right.
        CHECK(Synth(Steps("[1] > 3HP"), false)[0].raw == (Down | Right));
        const auto follow = Synth(Steps("236P > [4] > ~ LP"), true);
        const auto press = std::find_if(follow.begin(), follow.end(), [](const sf4e::training::Input& frame) { return frame.raw == LP; });
        CHECK(press != follow.end() && std::count_if(follow.begin(), press, [](const sf4e::training::Input& frame) { return frame.raw == Left; }) == FollowDelay + 1);
        // Holds and waits play in the order typed; a hold with no count after
        // ones with a count is still the reply's leading hold.
        CHECK(runs(Synth(Steps("[2]3f > [4] > 5MK"), true)) ==
            (std::vector<std::string>{text(Down), "hold:" + text(Left), text(MK), "0"}));
        CHECK(runs(Synth(Steps("[2]3f > 2f > [4]2f > 5MK"), true)) ==
            (std::vector<std::string>{text(Down), "0", text(Left), "0", text(MK), "0"}));
        // The moves come out whole or not at all: the most that fits a slot, one frame more, and holds that add up past it.
        {
            Compiled compiled;
            CHECK(Synthesize(Steps("[2]7194f > 5MK"), true, compiled, error) && compiled.frames.size() == sf4e::training::MaxFrames &&
                compiled.frames[7197].raw == MK);
            CHECK(!Synthesize(Steps("[2]7195f > 5MK"), true, compiled, error) && compiled.frames.empty() && !error.empty());
            CHECK(!Synthesize(Steps("[2]3000f > [4]3000f > [1]1500f > 5MK"), true, compiled, error) && compiled.frames.empty());
        }
        // The first move's own offset is the reply's, whatever leads into it;
        // nothing is played before the first input.
        {
            Compiled compiled;
            CHECK(Synthesize(Steps("[2]30f > 5MK@+3"), true, compiled, error) && compiled.offset == 3 && compiled.frames.size() == 36 && compiled.frames[33].raw == MK);
            CHECK(Synthesize(Steps("[2]30f > [4] > 5MK@-2"), true, compiled, error) && compiled.offset == -2 && compiled.frames[30].wait == sf4e::training::WaitReply &&
                compiled.frames[30].offset == 0);
            CHECK(Synthesize(Steps("5MK@+2 > 5LP@+1"), true, compiled, error) && compiled.offset == 2 && compiled.frames[0].raw == MK);
            // Where the first move takes effect: its button; with none, its last
            // direction (a jump, a dash); a jumping attack, its jump. And the leading hold.
            const std::pair<const char*, std::pair<int, int>> anchors[] = {{"5MK", {0, -1}}, {"[2]30f > 8", {30, -1}}, {"[2]30f > 66 > 5HP", {34, -1}},
                {"[2]30f > [4] > 5MK", {31, 30}}, {"[1] > 623HP", {10, 0}}, {"j.HK", {0, -1}}, {"[2]3f > j.HK", {3, -1}}, {"5LP > 5MK", {0, -1}}};
            for (const auto& expected : anchors)
                CHECK(Synthesize(Steps(expected.first), true, compiled, error) && compiled.anchor == expected.second.first && compiled.hold == expected.second.second);
            // "#N": a frame of the reply, counted from its first played frame, reached
            // only through frames of a known length; a hold with no count before it
            // holds through the frames up to the move.
            for (const char* line : {"5MK#45", "[1] > 3HP#45", "[2]30f > [1] > 3HP#45", "5LP > 5MK > 2HP#40", "2LP > xx 236P > 5HP#80"})
                CHECK(!Synthesize(Steps(line), true, compiled, error) && compiled.frames.empty() && !error.empty());
            // A later "#N" move's "@N" counts from its frame; the first move's is the reply's.
            CHECK(Synthesize(Steps("[2]2f > 5LP#10@+2 > 5MK#30@-3"), true, compiled, error) && compiled.offset == 2 && compiled.frames[10].raw == LP &&
                compiled.frames[27].raw == MK && compiled.frames[9].raw == 0);
            for (const char* line : {"5LP > [4] > 3HP#60", "5LP > [4] > 623HP#60"}) {
                CHECK(Synthesize(Steps(line), true, compiled, error));
                const auto& frames = compiled.frames;
                const int motion = std::string(line).find("623") != std::string::npos ? 9 : 3;
                CHECK(frames[0].raw == LP && (frames[60].raw & HP) && std::all_of(frames.begin() + 2, frames.begin() + 60 - motion,
                    [](const sf4e::training::Input& frame) { return frame.raw == Left && !frame.wait; }));
            }
            // A "#N" move with no button, or a jumping one, lands where it takes
            // effect (its last direction, its jump), not where its input starts;
            // the first one's anchor is that same frame, its "@N" the reply's.
            const auto all = [](const std::vector<sf4e::training::Input>& frames, int from, int to, unsigned bits) {
                return std::all_of(frames.begin() + from, frames.begin() + to, [&](const sf4e::training::Input& frame) { return frame.raw == bits && !frame.wait; });
            };
            CHECK(Synthesize(Steps("[2]30f > 8#45"), true, compiled, error) && compiled.anchor == 45 && all(compiled.frames, 30, 45, 0) &&
                all(compiled.frames, 45, 48, Up) && compiled.frames[29].raw == Down);
            CHECK(Synthesize(Steps("[2]30f > 66#45@+2"), true, compiled, error) && compiled.anchor == 45 && compiled.offset == 2 &&
                all(compiled.frames, 41, 44, Right) && compiled.frames[44].raw == 0 && all(compiled.frames, 45, 48, Right) && all(compiled.frames, 30, 41, 0));
            CHECK(Synthesize(Steps("[2]2f > j.HK#20"), true, compiled, error) && compiled.anchor == 20 && compiled.frames[20].raw == Up && compiled.frames[32].raw == HK);
            // Later ones, a hold before them held up to their own input, their "@N" from their frame.
            CHECK(Synthesize(Steps("5LP > [4] > 8#60"), true, compiled, error) && all(compiled.frames, 2, 60, Left) && all(compiled.frames, 60, 63, Up));
            CHECK(Synthesize(Steps("5LP > [4] > 8#60@+3"), true, compiled, error) && all(compiled.frames, 2, 63, Left) && compiled.frames[63].raw == Up);
            CHECK(Synthesize(Steps("5LP > [4] > 66#60"), true, compiled, error) && all(compiled.frames, 2, 56, Left) && all(compiled.frames, 56, 59, Right) &&
                compiled.frames[59].raw == 0 && compiled.frames[60].raw == Right);
            CHECK(Synthesize(Steps("[2]2f > 5LP#10 > j.HK#30"), true, compiled, error) && compiled.frames[30].raw == Up && compiled.frames[29].raw == 0 &&
                compiled.frames[42].raw == HK);
            // Holds and waits with no move after them: nothing to time the reply
            // by. A neutral "5" presses nothing, so it is no move wherever it
            // stands, whatever comes before it: not the first move, nor one a
            // later move recovers from.
            for (const char* line : {"[2]30f", "[2]30f > [4]10f", "[2]30f > 5f > [4]10f", "5", "5 > 5", "[2]30f > 5", "[2]30f > 2f > [4]10f > 5",
                     "[1] > 5", "[2]30f > 5 > 5MK", "5LP > 5 > 5MK", "5MK > 5"})
                CHECK(!Synthesize(Steps(line), true, compiled, error) && compiled.frames.empty() && compiled.anchor == -1 && !error.empty());
            // A linked jump is a move: its wait holds its direction to the free frame.
            CHECK(Synthesize(Steps("5LP > 8"), true, compiled, error) && std::any_of(compiled.frames.begin(), compiled.frames.end(),
                [](const sf4e::training::Input& frame) { return frame.wait == sf4e::training::WaitActionable && frame.raw == Up; }));
        }
        const auto spaced = Synth(Steps("5LP > 10f > 5MK"), true);
        const auto lp = std::find_if(spaced.begin(), spaced.end(), [](const sf4e::training::Input& frame) { return frame.raw == LP; });
        CHECK(lp != spaced.end() && std::all_of(lp + 1, lp + 12, [](const sf4e::training::Input& frame) { return frame.raw == 0 && !frame.wait; }) &&
            (lp + 12)->wait == sf4e::training::WaitActionable);
    }

    // A recording slot as a file and back; bad files are refused.
    {
        std::vector<sf4e::training::Input> frames{{9, 9, 0, 0}, {0, 0, sf4e::training::WaitHit, 3}, {0x410, 0x410, 0, 0}}, back;
        std::string reason;
        CHECK(sf4e::training::ImportRecording(sf4e::training::ExportRecording(frames), back, reason) && back.size() == 3);
        CHECK(back[1].wait == sf4e::training::WaitHit && back[1].offset == 3 && back[2].raw == 0x410);
        for (const char* bad : {"", "{}", "{\"format\":\"sf4e-recording\",\"frames\":[]}", "{\"format\":\"sf4e-recording\",\"frames\":[[1,2,3]]}", "{\"format\":\"x\",\"frames\":[[1,1,0,0]]}"})
            CHECK(!sf4e::training::ImportRecording(bad, back, reason) && back.empty());
        for (const char* format : {"1", "-1", "1.5", "true", "false", "null", "[]", "{}"}) {
            back = frames; reason.clear();
            CHECK(!sf4e::training::ImportRecording(std::string("{\"format\":") + format + ",\"frames\":[[0,0,0,0]]}", back, reason));
            CHECK(back.empty() && reason == "this is not a recording");
        }
        for (const char* bad : {"[]", "null", "1", "\"sf4e-recording\"", R"({"frames":[[0,0,0,0]]})"}) {
            back = frames; reason.clear();
            CHECK(!sf4e::training::ImportRecording(bad, back, reason) && back.empty() && !reason.empty());
        }
        std::vector<sf4e::training::Input> earlyFrames{{2, 2, sf4e::training::WaitHit, -3}};
        CHECK(sf4e::training::ImportRecording(sf4e::training::ExportRecording(earlyFrames), back, reason) && back.size() == 1 && back[0].offset == -3);
    }
    // Motions named in words, any case, read as their digits, the button
    // straight after, after a space or after "+".
    {
        const char* const words[][2] = {{"qcf", "236"}, {"qcb", "214"}, {"dp", "623"}, {"rdp", "421"}, {"hcf", "41236"}, {"hcb", "63214"}};
        for (const auto& word : words) {
            const std::string name = word[0], digits = word[1];
            for (const std::string& spelled : {name + "LP", name + " LP", name + "+LP", name + " + lp", Fold(name, std::toupper) + "lp"}) {
                CHECK(ParseStep(spelled, step, error) && step.motion == digits && step.buttons == LP && step.need == 1);
                CHECK(Canonical(step) == digits + "LP");
            }
        }
        CHECK(Steps("qcfP") == Steps("236P") && Steps("qcbP") == Steps("214P") && Steps("dpP") == Steps("623P") && Steps("hcbP") == Steps("63214P"));
        CHECK(Steps("2MK xx qcf+HP > dp HP") == Steps("2MK xx 236HP > 623HP"));
        CHECK(ParseStep("j.qcbKK", step, error) && step.air && step.motion == "214" && step.buttons == Kicks && step.need == 2);
        CHECK(ParseStep("qcf[HP]", step, error) && step.motion == "236" && step.edge == Edge::Hold);
        CHECK(ParseStep("dp", step, error) && step.motion == "623" && !step.buttons);
        CHECK(!ParseStep("qcz P", step, error) && !ParseStep("qcfqcf", step, error) && !ParseStep("dp++LP", step, error));
    }
    // A recording file holds its frames for a fighter facing right (version
    // 2) and reads back as written; a version 1 file held the pad's own
    // directions on the dummy, facing left, so its Left and Right are swapped
    // on reading. A version not known yet is refused.
    {
        using sf4e::training::Input;
        const std::vector<Input> frames{{4, 4, 0, 0}, {10, 10, sf4e::training::WaitActionable, -2}, {0x412, 0x416, 0, 0}};
        std::vector<Input> back;
        std::string reason;
        const auto file = sf4e::training::ExportRecording(frames);
        CHECK(file.find("\"version\":2") != std::string::npos);
        CHECK(sf4e::training::ImportRecording(file, back, reason) && back.size() == 3);
        for (std::size_t i = 0; i < back.size(); ++i)
            CHECK(back[i].mapped == frames[i].mapped && back[i].raw == frames[i].raw && back[i].wait == frames[i].wait && back[i].offset == frames[i].offset);
        const std::string first = R"({"format":"sf4e-recording","version":1,"frames":[[4,4,0,0],[6,10,1,-2],[1040,1046,0,0]]})";
        CHECK(sf4e::training::ImportRecording(first, back, reason) && back.size() == 3);
        CHECK(back[0].raw == 8 && back[0].mapped == 8 && back[1].mapped == 10 && back[1].raw == 6 && back[1].wait == 1 && back[1].offset == -2);
        CHECK(back[2].mapped == 0x410 && back[2].raw == (0x410 | 2 | 8));
        // A file written before versions were read counts as version 1.
        CHECK(sf4e::training::ImportRecording(R"({"format":"sf4e-recording","frames":[[8,8,0,0]]})", back, reason) && back.size() == 1 && back[0].raw == 4);
        for (const char* version : {"3", "0", "-1", "\"2\"", "1.5", "null"}) {
            back = frames; reason.clear();
            CHECK(!sf4e::training::ImportRecording(std::string(R"({"format":"sf4e-recording","version":)") + version + R"(,"frames":[[0,0,0,0]]})", back, reason));
            CHECK(back.empty() && !reason.empty());
        }
    }
    return 0;
}
