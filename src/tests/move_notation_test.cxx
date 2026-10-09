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
        const auto follow = Synthesize({"236P", "~ 5LP@+2"}, true);
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
    {
        // A dash is two taps, not one hold.
        const auto dash = Synthesize({"66"}, true);
        CHECK(dash.size() >= 7 && dash[0].raw == Right && dash[3].raw == 0 && dash[4].raw == Right);
    }
    // A mash: five presses, several buttons cycled a frame each, one button on and off.
    {
        const auto first = [](const std::vector<sf4e::training::Input>& f) { std::size_t i = 0; while (i < f.size() && !f[i].raw) ++i; return i; };
        const auto piano = Synthesize({"5P(mash)"}, true); const auto p = first(piano);
        CHECK(p + 5 < piano.size() && piano[p].raw == LP && piano[p + 1].raw == MP && piano[p + 2].raw == HP && piano[p + 3].raw == LP && piano[p + 4].raw == MP && piano[p + 5].raw == 0);
        const auto one = Synthesize({"5HP(mash)"}, true); const auto o = first(one);
        CHECK(o + 9 < one.size() && one[o].raw == HP && one[o + 1].raw == 0 && one[o + 8].raw == HP && one[o + 9].raw == 0);
        const auto two = Synthesize({"5LP+MP(mash)"}, true); const auto t = first(two);
        CHECK(two[t].raw == LP && two[t + 1].raw == MP && two[t + 4].raw == LP);
        // An order of its own sets the presses and their count; a repeat gets a frame off between.
        CHECK(ParseStep("5P(mash HP-MP-LP-LP)", step, error) && step.mash && step.mashOrder.size() == 4 && Canonical(step) == "5P(mash HP-MP-LP-LP)");
        CHECK(!ParseStep("5P(mash HP-XX)", step, error) && !ParseStep("5P(mash -)", step, error) && !ParseStep("5P(mash LP-LP-LP-LP-LP-LP-LP-LP-LP-LP-LP)", step, error));
        // Each press may carry its own direction; the dashes are optional.
        CHECK(ParseStep("5K(mash 1MK 1MK 3MK)", step, error) && step.mashOrder.size() == 3 && step.mashOrder[0].direction == '1' && step.mashOrder[2].direction == '3' && Canonical(step) == "5K(mash 1MK-1MK-3MK)");
        const auto legs = Synthesize({"5K(mash 1MK-1MK-3MK)"}, true); const auto l = first(legs);
        CHECK(legs[l].raw == (Down | Left | MK) && legs[l + 1].raw == (Down | Left) && legs[l + 2].raw == (Down | Left | MK) && legs[l + 3].raw == (Down | Right) && legs[l + 4].raw == (Down | Right | MK));
        const auto ordered = Synthesize({"5P(mash HP-MP-LP-LP)"}, true); const auto q = first(ordered);
        CHECK(ordered[q].raw == HP && ordered[q + 1].raw == MP && ordered[q + 2].raw == LP && ordered[q + 3].raw == 0 && ordered[q + 4].raw == LP && ordered[q + 5].raw == 0);
    }
    // The red focus cancel: LP+MP+MK, then the dash.
    CHECK(ParseStep("xx RFADC", step, error) && step.cancel && step.buttons == (LP | MP | MK) && step.need == 3 && step.motion == "66" && Canonical(step) == "xx RFADC");
    CHECK(ParseStep("rfadc44@+1", step, error) && step.motion == "44" && Canonical(step) == "RFADC44@+1");
    {
        // The red focus is tapped, then waits for its own hit before the dash.
        const auto frames = Synthesize({"5HP", "xx RFADC"}, true);
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
    CHECK(!ParseSteps("2MK xx", steps, error));
    CHECK(ParseStep("[2]8LP(mash)", step, error) && step.mash && step.charge && step.buttons == LP);
    CHECK(!ParseStep("236(mash)", step, error) && !ParseStep("FADC(mash)", step, error));

    // Typed steps become pad input, timed by the fight: a cancelled move does
    // its motion at once but the last direction, which it presses with the
    // button on the hit; a linked move holds its first direction through the
    // wait for the free frame.
    {
        const auto frames = Synthesize(Steps("2MK xx 236HP@+1 > 2HP@+3 > [4]6P@+1"), true);
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
        CHECK(held == 50 && frames[0].mapped == frames[0].raw);
        // Facing left mirrors forward and back.
        const auto mirrored = Synthesize(Steps("236HP > 5LP"), false);
        CHECK(mirrored[3].raw == (Down | Left) && mirrored[9].raw == (Left | HP) && mirrored[11].wait == sf4e::training::WaitActionable && mirrored[12].raw == LP);
        CHECK(Synthesize({}, true).size() == 1 && Synthesize({"nonsense"}, true).size() == 1);
        // Moves on frames: the press lands on its frame, no cue is waited for,
        // and a frame already passed starts the move at once.
        const auto timed = Synthesize(Steps("5LP#10 > 2MK#30 > xx 236HP#42 > 5HP#43"), true);
        CHECK(timed[10].raw == LP && timed[9].raw == 0 && timed[30].raw == (Down | MK) && timed[27].raw == Down && timed[42].raw == (Right | HP));
        for (const auto& frame : timed) CHECK(!frame.wait);
        CHECK(timed[29].raw == Down && timed[31].raw == Down && timed[32].raw == 0 && timed[36].raw == Down);
        CHECK(timed.size() > 44 && timed.back().raw == 0);
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
    return 0;
}
