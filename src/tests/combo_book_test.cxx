#include "../training/ComboBook.hxx"
#include "../training/ComboReplay.hxx"
#include "../training/RecordingFile.hxx"
#include "test_support.hxx"

using namespace sf4e::combo;

static std::vector<std::string> Steps(const char* line, const char* character = "RYU") {
    std::vector<std::string> steps;
    std::string error;
    CHECK(ParseSteps(line, character, steps, error));
    return steps;
}

int main() {
    std::vector<Pack> packs;
    std::vector<std::string> steps;
    std::string error;
    Step step;

    // A step is inputs a trial can check, however it was spelled.
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
    CHECK(JoinSteps({"236P", "~ 5LP", "5HP"}) == "236P ~ 5LP > 5HP");
    {
        const auto follow = Synthesize({"236P", "~ 5LP@+2"}, true, 0);
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
    // A frame to land on, as a pattern editor lays moves out; kept before the offset.
    CHECK(ParseStep("2MK#45", step, error) && step.at == 45 && step.offset == 0 && Canonical(step) == "2MK#45");
    // A recorded focus press and the dash after it fold into one FADC on the focus's frame.
    {
        std::vector<std::string> recorded{"cl.LP#0", "2MK#19", "xx 236HP#33", "xx 5MP+MK#49", "xx 66#53", "cl.HP#70", "5MP+MK#90", "2LK#120"};
        FoldFadc(recorded);
        CHECK((recorded == std::vector<std::string>{"cl.LP#0", "2MK#19", "xx 236HP#33", "xx FADC#49", "cl.HP#70", "5MP+MK#90", "2LK#120"}));
        std::vector<std::string> back{"5MP+MK", "44"}; FoldFadc(back);
        CHECK((back == std::vector<std::string>{"FADC44"}));
        // A dash is two taps, not one hold.
        const auto dash = Synthesize({"66"}, true, 0);
        CHECK(dash.size() >= 7 && dash[0].raw == Right && dash[3].raw == 0 && dash[4].raw == Right);
    }
    CHECK(ParseStep("5HP@-1#14", step, error) && step.at == 14 && step.offset == -1 && Canonical(step) == "5HP#14@-1");
    CHECK(!ParseStep("5HP#14#15", step, error) && !ParseStep("5HP@+1@+2", step, error));
    CHECK(ParseStep("xx 236HP#120@+2", step, error) && step.at == 120 && step.offset == 2 && Canonical(step) == "xx 236HP#120@+2");
    CHECK(ParseStep("FADC#0", step, error) && step.at == 0 && Canonical(step) == "FADC#0");
    CHECK(ParseStep("5LP", step, error) && step.at == -1);
    for (const char* bad : {"2MK#", "2MK#-1", "2MK#3601", "2MK#12345", "2MK#a", "#45"}) CHECK(!ParseStep(bad, step, error));
    for (const char* bad : {"2MK@", "2MK@1", "2MK@-31", "2MK@+31", "2MK@-a", "@-1", "xx", "FADC4", "FADC6", "[]", "][", "[HP", "]HP]", "[LP+]", "cl.", "xx xx 5LP", "cr.", "hadouken", "236H", "236HPP", "LP+", "LP+LP", "LPLK", "+LP", "PPPP",
        "[4]HP", "[46]HP", "cr.236HP", "0HP", "12345678912LP", "236 HP x"})
        CHECK(!ParseStep(bad, step, error));

    // Typed notation becomes canonical steps; one bad step refuses the line.
    CHECK(Steps("  cr.MK >236hp ,, fadc  >  st.HP > j.hk > lk+lp > [2]8K ") ==
        (std::vector<std::string>{"2MK", "236HP", "FADC", "5HP", "j.HK", "5LP+LK", "[2]8K"}));
    CHECK(Steps("cl.hp XX 623]HP[ xx fadc44 > far.mk, [mp+mk] > ]MP+MK[") ==
        (std::vector<std::string>{"cl.HP", "xx 623]HP[", "xx FADC44", "far.MK", "5[MP+MK]", "5]MP+MK["}));
    CHECK(Steps(" > , ").empty());
    CHECK(!ParseSteps("cr.MK > shoryu", "RYU", steps, error) && steps.empty() && error == "\"shoryu\" is not a move");
    CHECK(!ParseSteps("xx 236HP", "RYU", steps, error) && error == "the first move cannot be a cancel");
    CHECK(!ParseSteps("2MK xx", "RYU", steps, error));
    // The stored line reads back to the same steps.
    for (const char* line : {"cl.HP xx 623]HP[ xx FADC44 > far.MK > 5[MP+MK]", "2MK xx 236HP > FADC > j.236KK > [4]6P"})
        CHECK(JoinSteps(Steps(line)) == line);

    // Named moves resolve per fighter, with a strength, and stay notation after.
    CHECK(Steps("cr.MK xx HP Hadouken > FADC > EX Tatsu, shoryuken > Focus > back dash") ==
        (std::vector<std::string>{"2MK", "xx 236HP", "FADC", "214KK", "623P", "5MP+MK", "44"}));
    CHECK(Steps("Sonic Boom xx l flash kick", "GUL") == (std::vector<std::string>{"[4]6P", "xx [2]8LK"}));
    CHECK(Steps("lariat > spd", "ZGF") == (std::vector<std::string>{"5PPP", "360P"}));
    CHECK(!ParseSteps("hadoken", "ZGF", steps, error) && !ParseSteps("HP focus", "RYU", steps, error));
    // A kick on a punch move, or the other way round, names no move.
    CHECK(!ParseSteps("LK Hadoken", "RYU", steps, error) && !ParseSteps("HP Tatsu", "RYU", steps, error) && !ParseSteps("MK Shoryuken", "RYU", steps, error));

    // Every generated name is a move the notation can express, and resolves to it.
    struct Row { const char* character; const char* name; const char* notation; };
    static const Row rows[] = {
#include "../training/ComboMoves.inc"
    };
    for (const auto& row : rows) {
        CHECK(!Tokens(row.notation).empty());
        for (const auto& piece : Tokens(row.notation)) CHECK(ParseStep(piece, step, error));
        CHECK(ResolveName(row.character, row.name) == row.notation);
        // A name must never be readable as notation, or it would hide that move.
        CHECK(!ParseStep(row.name, step, error));
    }
    CHECK(Steps("EX Oil Rocket > Oil Coaster", "HKN") == (std::vector<std::string>{"360PP", "720PPP"}));
    CHECK(Steps("Collarbone Breaker > Metsu Shoryuken") == (std::vector<std::string>{"6MP", "236236KKK"}));
    // A name may be a sequence: rekka follow-ups, target combos, a cancel, Raging Demon.
    CHECK(Steps("cl.MP xx HP Rekka 3", "FLN") == (std::vector<std::string>{"cl.MP", "xx 236HP", "236P", "236P"}));
    CHECK(Steps("Target Combo 1 xx Scramble Slide", "DCP") == (std::vector<std::string>{"cl.MP", "5HK", "xx [4]6K", "5K"}));
    CHECK(Steps("Goshoryuken FADC > Raging Demon", "GKI") ==
        (std::vector<std::string>{"623P", "xx FADC", "5LP", "5LP", "6", "5LK", "5HP"}));
    // Twins under one name are told apart by their tag; mash, hold and release are inputs too.
    CHECK(Steps("Crazy Buffalo > K Crazy Buffalo > TAP 1 > Dash Swing Blow", "BSN") ==
        (std::vector<std::string>{"[4]646P", "[4]646K", "5]PPP[", "[4]3[P]"}));
    CHECK(Steps("Ashura Senku 3K", "GKI") == (std::vector<std::string>{"623KKK"}));
    CHECK(Steps("Hands > LP (mash)", "DCP") == (std::vector<std::string>{"5P(mash)", "5LP(mash)"}));
    CHECK(ParseStep("[2]8LP(mash)", step, error) && step.mash && step.charge && step.buttons == LP);
    CHECK(!ParseStep("236(mash)", step, error) && !ParseStep("FADC(mash)", step, error));

    // Typed steps become pad input, timed by the fight: a cancelled move does
    // its motion at once but the last direction, which it presses with the
    // button on the hit; a linked move holds its first direction through the
    // wait for the free frame.
    {
        const auto frames = Synthesize(Steps("2MK xx 236HP > 2HP@+2 > [4]6P"), true, 1);
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
        const auto mirrored = Synthesize(Steps("236HP > 5LP"), false, 0);
        CHECK(mirrored[3].raw == (Down | Left) && mirrored[9].raw == (Left | HP) && mirrored[11].wait == sf4e::training::WaitActionable && mirrored[12].raw == LP);
        CHECK(Synthesize({}, true, 0).size() == 1 && Synthesize({"nonsense"}, true, 0).size() == 1);
        // Moves on frames: the press lands on its frame, no cue is waited for,
        // and a frame already passed starts the move at once.
        const auto timed = Synthesize(Steps("5LP#10 > 2MK#30 > xx 236HP#42 > 5HP#43"), true, 0);
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
        std::vector<sf4e::training::Input> earlyFrames{{2, 2, sf4e::training::WaitHit, -3}};
        CHECK(sf4e::training::ImportRecording(sf4e::training::ExportRecording(earlyFrames), back, reason) && back.size() == 1 && back[0].offset == -3);
    }
    Combo bnb{"BnB", "RYU", "", Steps("cr.MK > 236HP")};
    Combo fadc{"FADC", "RYU", "two bars", Steps("cr.MK > 236HP > FADC > cr.HP")};
    Combo other{"Punish", "RYU", "", Steps("cr.HP > 623HP")};
    Combo ken{"BnB", "KEN", "", Steps("cr.MK > 236HP")};
    Pack basics{"Basics", {bnb, fadc}};
    Pack more{"More", {other, ken}};

    // Each exported shape reads back: one combo, one pack, several packs.
    CHECK(Import(Export(fadc), packs, error));
    CHECK(packs.size() == 1 && packs[0].name.empty() && packs[0].combos.size() == 1);
    CHECK(packs[0].combos[0].steps == fadc.steps && packs[0].combos[0].notes == "two bars");
    CHECK(Import(Export(basics), packs, error));
    CHECK(packs.size() == 1 && packs[0].name == "Basics" && packs[0].combos.size() == 2);
    CHECK(Import(Export(std::vector<Pack>{basics, more}), packs, error));
    CHECK(packs.size() == 2 && packs[1].combos[1].character == "KEN");

    // A combo's dummy and gauge settings travel with it; only set fields are written.
    {
        Combo set = fadc; set.setup.action = 1; set.setup.super = 7;
        CHECK(Import(Export(set), packs, error) && packs[0].combos[0].setup.action == 1 && packs[0].combos[0].setup.super == 7 && packs[0].combos[0].setup.guard == -1);
        CHECK(Export(fadc).find("setup") == std::string::npos && Export(set).find("\"guard\"") == std::string::npos);
        CHECK(!Import(R"({"character":"RYU","steps":"5LP","setup":{"action":9}})", packs, error));
        // Where the fighters stand travels with the combo too.
        Combo placed = fadc; placed.placed = true; placed.place[0] = -1.5f; placed.place[1] = 2.25f;
        CHECK(Import(Export(placed), packs, error) && packs[0].combos[0].placed && packs[0].combos[0].place[0] == -1.5f && packs[0].combos[0].place[1] == 2.25f);
        CHECK(Export(fadc).find("place") == std::string::npos && Import(Export(fadc), packs, error) && !packs[0].combos[0].placed);
        CHECK(!Import(R"({"character":"RYU","steps":"5LP","place":[1]})", packs, error) && !Import(R"({"character":"RYU","steps":"5LP","place":[1,99999]})", packs, error));
        CHECK(!Import(R"({"character":"RYU","steps":"5LP","setup":3})", packs, error));
        CHECK(Import(R"({"character":"RYU","steps":"5LP","setup":{"revenge":8}})", packs, error) && packs[0].combos[0].setup.revenge == 8);
    }
    // A hand-written combo: no header, steps as one line or a list, any spelling.
    CHECK(Import(R"({"character":" ryu ","steps":"cr.mk > 236HP"})", packs, error));
    CHECK(packs[0].combos[0].character == "RYU" && packs[0].combos[0].steps == bnb.steps);
    CHECK(Import(R"({"character":"RYU","steps":["CR.MK","236hp"]})", packs, error));
    CHECK(packs[0].combos[0].steps == bnb.steps);
    CHECK(Import(R"({"character":"ryu","steps":["cr.MK","xx HP Hadoken"]})", packs, error));
    CHECK(packs[0].combos[0].steps == (std::vector<std::string>{"2MK", "xx 236HP"}));

    // Refused, with a reason, and nothing half-read is left behind.
    for (const char* bad : {"", "not json", "[]", R"({"format":"other","steps":"5LP","character":"RYU"})",
        R"({"version":2,"steps":"5LP","character":"RYU"})", R"({"steps":"5LP"})", R"({"character":"RYU","steps":[]})",
        R"({"character":"RYU","steps":[1]})", R"({"character":"RYU","steps":"5LP","name":7})", R"({"packs":{}})",
        R"({"character":"RYU","steps":"5LP > jab"})", R"({"character":"RYU","steps":["xx 5LP"]})",
        R"({"character":"GUL","steps":"Hadoken"})", R"({"character":"RYU","steps":"HP Focus"})", R"({"character":"RYU","steps":["5LP","jab"]})",
        R"({"combos":[{"character":"RYU"}]})", R"({"packs":[{"name":"A","combos":[{"character":"RYU","steps":"5LP"}]},{"name":"B"}]})"}) {
        CHECK(!Import(bad, packs, error));
        CHECK(packs.empty() && !error.empty());
    }
    CHECK(!Import(R"({"character":"RYU","steps":"5LP","name":")" + std::string(MaxText + 1, 'a') + "\"}", packs, error));
    CHECK(!Import(std::string(MaxBytes + 1, ' '), packs, error));

    // Merging joins packs by name and skips a route the pack already holds.
    std::vector<Pack> book{basics};
    Combo sameRoute{"Renamed", "RYU", "", Steps("2mk > 236hp")};
    CHECK(Merge(book, {Pack{"Basics", {sameRoute, other}}, more}) == 3);
    CHECK(book.size() == 2 && book[0].combos.size() == 3 && book[1].combos.size() == 2);

    // Shared starts share nodes; routes branch where they differ.
    const auto tree = BuildTree({basics, more});
    CHECK(tree.size() == 2 && tree.at("RYU").children.size() == 2);
    CHECK(RenderTree(tree) ==
        "KEN\n"
        "  2MK\n"
        "    236HP  [More / BnB]\n"
        "RYU\n"
        "  2MK\n"
        "    236HP  [Basics / BnB]\n"
        "      FADC\n"
        "        2HP  [Basics / FADC]\n"
        "  2HP\n"
        "    623HP  [More / Punish]\n");
}
