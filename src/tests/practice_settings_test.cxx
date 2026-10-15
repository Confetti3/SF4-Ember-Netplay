#include "../training/PracticeSettings.hxx"
#include "test_support.hxx"
#include <cctype>
#include <string>

using namespace sf4e::training;
using nlohmann::json;

int main() {
    // Decode the complete resulting pair, including defaults, before use.
    const auto keys = [](const char* text, int reset, int save) {
        const auto result = ReadPositionKeys(json::parse(text));
        CHECK(result[0] == reset && result[1] == save);
        CHECK(result[0] < 0 || result[0] != result[1]);
    };
    keys("{}", 1, 10);
    keys(R"({"reset_position":10})", 10, 1);
    keys(R"({"reset_position":10,"save_position":"invalid"})", 10, 1);
    keys(R"({"reset_position":10,"save_position":10})", 10, 1);
    keys(R"({"save_position":1})", 1, 10);
    keys(R"({"reset_position":3,"save_position":3})", 3, 10);
    keys(R"({"reset_position":3,"save_position":8})", 3, 8);
    keys(R"({"reset_position":9,"save_position":4})", 1, 10);
    keys(R"({"reset_position":false,"save_position":1.5})", 1, 10);
    keys(R"({"reset_position":-2,"save_position":12})", 1, 10);
    keys(R"({"reset_position":18446744073709551615,"save_position":null})", 1, 10);
    keys(R"({"reset_position":-1,"save_position":-1})", -1, -1);
    keys(R"({"reset_position":10,"save_position":-1})", 10, -1);
    keys(R"({"reset_position":-1,"save_position":1})", -1, 1);
    keys("[0,10,2,3,4,null]", 10, 1);
    keys("[0,10,2,3,4,10]", 10, 1);
    keys("[0,8,2,3,4,3]", 8, 3);
    keys("[0,null,2,3,4,1]", 1, 10);
    keys("[0,-1,2,3,4,-1]", -1, -1);
    keys("[0,1]", 1, 10);
    for (const char* bad : {"null", "true", "42", "\"keys\""}) keys(bad, 1, 10);
    for (int reset = -1; reset < 12; ++reset) for (int save = -1; save < 12; ++save) {
        const auto pair = ReadPositionKeys(json{{"reset_position", reset}, {"save_position", save}});
        CHECK(FreePositionKey(pair[0]) && FreePositionKey(pair[1]));
        CHECK(pair[0] < 0 || pair[0] != pair[1]);
    }

    // All formerly shared aliases migrate to the same canonical inputs.
    const char* names[] = {"focus", "focusattack", "redfocus", "redfocusattack", "falv1", "falv2", "falv3",
        "focusattacklv1", "focusattacklv2", "focusattacklv3", "redfalv1", "redfalv2", "redfalv3",
        "redfocusattacklv1", "redfocusattacklv2", "redfocusattacklv3", "throw", "backthrow", "taunt",
        "dash", "backdash", "jump", "jumpforward", "jumpback"};
    const char* notation[] = {"MP+MK", "MP+MK", "LP+MP+MK", "LP+MP+MK", "MP+MK", "[MP+MK]", "[MP+MK]",
        "MP+MK", "[MP+MK]", "[MP+MK]", "LP+MP+MK", "[LP+MP+MK]", "[LP+MP+MK]",
        "LP+MP+MK", "[LP+MP+MK]", "[LP+MP+MK]", "LP+LK", "4LP+LK", "HP+HK", "66", "44", "8", "9", "7"};
    DummyPlan settings; settings.when = 4; settings.chance = 75;
    std::string error;
    for (std::size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        sf4e::combo::Step step;
        CHECK(sf4e::combo::ParseStep(notation[i], step, error));
        const auto migrated = MigrateReplyMoves(names[i]);
        CHECK(migrated == sf4e::combo::Canonical(step));
        CHECK(MigrateReplyMoves(migrated) == migrated);
        DummyPlan plan;
        CHECK(BuildReplyPlan(migrated, settings, plan, error));
        CHECK(plan.when == 4 && plan.chance == 75 && !plan.moves.empty());
        // Made once, for the dummy facing right, as a slot holds it.
        sf4e::combo::Compiled compiled;
        CHECK(sf4e::combo::Synthesize({notation[i]}, true, compiled, error));
        const auto& expected = compiled.frames;
        CHECK(plan.moves.size() == expected.size());
        for (std::size_t frame = 0; frame < expected.size() && frame < plan.moves.size(); ++frame) {
            const auto& got = plan.moves[frame]; const auto& want = expected[frame];
            CHECK(got.raw == want.raw && got.mapped == want.mapped && got.wait == want.wait && got.offset == want.offset);
        }
    }
    CHECK(MigrateReplyMoves(" Dash > back dash > THROW > Focus > jump ") == "66 > 44 > 5LP+LK > 5MP+MK > 8");
    CHECK(MigrateReplyMoves("2MK xx focus attack > dash") == "2MK > xx 5MP+MK > 66");
    CHECK(MigrateReplyMoves("2MK > xx 623HP") == "2MK > xx 623HP");
    CHECK(MigrateReplyMoves("dash > Hadoken") == "dash > Hadoken");
    // Invalid saved text (including a nonempty separator-only line) cannot
    // erase a typed reply and silently switch the command to a recording.
    DummyPlan result; result.moves = {{0x10, 0x10, 0, 0}};
    // A reply plays from its first pressed frame, so one starting with a wait
    // is refused, as are holds with no move to time the reply by and a
    // neutral "5", which presses nothing.
    for (const char* bad : {"Hadoken", "dash", "> ,", "2MK > nonsense", "xx 623HP", "10f > 5MK", "[1]", "[2]7195f > 5MK", "[4] > 10f > 5MK",
             "[2]30f", "[2]30f > [4]10f", "[2]30f > [4]10f > [1]5f", "5", "[2]30f > 5",
             "[2]30f > 3f > [4]10f > 5", "[2]30f > 5 > 5MK"}) {
        error.clear();
        CHECK(!BuildReplyPlan(bad, settings, result, error) && !error.empty());
        CHECK(result.moves.size() == 1 && result.moves[0].raw == 0x10);
    }
    CHECK(BuildReplyPlan("", settings, result, error) && result.moves.empty() && result.chance == 75);
    CHECK(BuildReplyPlan("[1] > 3HP", settings, result, error) && !result.moves.empty() && BuildReplyPlan("[2]30f > 5MK", settings, result, error));
    // The first move's own offset rides with the plan, and goes with typed moves.
    CHECK(BuildReplyPlan("[2]30f > 5MK@+3", settings, result, error) && result.offset == 3 && ValidDummyPlan(result));
    // A plan built is one the dummy takes: its frames and schedule agree.
    for (const char* line : {"5MK", "[2]30f > 8#45", "[2]30f > 66#45@+2", "[2]30f > [4] > 5MK", "5LP > [4] > 66#60", "[1] > 623HP > xx 5MP+MK"}) {
        DummyPlan built; error.clear();
        CHECK(BuildReplyPlan(line, settings, built, error) && ValidDummyPlan(built));
    }
    CHECK(BuildReplyPlan("", settings, result, error) && result.offset == 0 && result.moves.empty());

    // Every reply preset, 214K and 236P included, is notation the parser and
    // the plan builder take as typed text, and reads back as itself however
    // it is spelled; text that is no preset, or none, picks none.
    CHECK(ReplyPresetCount == 9);
    for (int i = 0; i < ReplyPresetCount; ++i) {
        DummyPlan plan; error.clear();
        CHECK(BuildReplyPlan(ReplyPresets[i], settings, plan, error) && !plan.moves.empty());
        CHECK(ReplyPresetIndex(ReplyPresets[i]) == i);
        std::string lower;
        for (const char* c = ReplyPresets[i]; *c; ++c) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        CHECK(ReplyPresetIndex("  " + lower + " ") == i);
        // Saved and loaded again, as training.json keeps the typed text.
        CHECK(ReplyPresetIndex(MigrateReplyMoves(json(ReplyPresets[i]).get<std::string>())) == i);
        for (int j = 0; j < i; ++j) CHECK(ReplyPresetIndex(ReplyPresets[j]) != i);
    }
    CHECK(ReplyPresetIndex(MigrateReplyMoves("throw")) == 3 && ReplyPresetIndex(MigrateReplyMoves("dash")) == 2);
    CHECK(ReplyPresetIndex("5LP+LK") == 3 && ReplyPresetIndex("cr.LK") == 6);
    for (const char* none : {"", "  ", "623HP", "2LK > 623P", "not a move", "8LP"}) CHECK(ReplyPresetIndex(none) < 0);

    // The meter's options: angled bars and no recovery number unless saved
    // as flat and true, and a round trip keeps each one apart from the other.
    for (const char* defaults : {"null", "{}", "[]", "true", R"({"style":"Flat","hud_recovery":"true"})", R"({"style":1,"hud_recovery":1})",
        R"({"style":"angled","hud_recovery":false})"}) {
        const auto options = ReadMeterOptions(json::parse(defaults));
        CHECK(options.style == MeterStyle::Angled && !options.recovery);
    }
    CHECK(ReadMeterOptions(json::parse(R"({"style":"flat"})")).style == MeterStyle::Flat);
    for (const char* shown : {"{}", R"({"frames_shown":30})", R"({"frames_shown":"90"})", R"({"frames_shown":90.5})", R"({"frames_shown":-60})"})
        CHECK(ReadMeterOptions(json::parse(shown)).shown == 60);
    for (const int choice : MeterShownChoices) CHECK(ReadMeterOptions(json{{"frames_shown", choice}}).shown == choice);
    for (int bits = 0; bits < 4; ++bits) {
        MeterOptions options; options.style = (bits & 1) ? MeterStyle::Flat : MeterStyle::Angled; options.recovery = (bits & 2) != 0; options.shown = bits == 3 ? 120 : 90;
        const auto read = ReadMeterOptions(json::parse(MeterOptionsJson(options).dump()));
        CHECK(read.style == options.style && read.recovery == options.recovery && read.shown == options.shown);
    }
    // The meter keeps 20 seconds unless one of the offered lengths is saved.
    for (const char* history : {"{}", R"({"history_frames":500})", R"({"history_frames":"600"})", R"({"history_frames":-240})"})
        CHECK(ReadMeterOptions(json::parse(history)).history == 1200);
    for (const int choice : MeterHistoryChoices) {
        MeterOptions saved; saved.history = choice;
        CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(saved).dump())).history == choice);
    }
    // The meter sits above the super meters at the standard bar height unless an offered place or height is saved.
    CHECK(ReadMeterOptions(json::parse(R"({"position":50,"bar_height":"22"})")).position == 82 && ReadMeterOptions(json::parse(R"({"bar_height":17})")).barHeight == 16);
    for (const int choice : MeterPositions) { MeterOptions saved; saved.position = choice; CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(saved).dump())).position == choice); }
    for (const int choice : ClassicBarHeights) { MeterOptions saved; saved.barHeight = choice; CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(saved).dump())).barHeight == choice); }
    // The bars turn pages unless "smooth" is saved.
    CHECK(!ReadMeterOptions(json::object()).smooth && !ReadMeterOptions(json::parse(R"({"scroll":true})")).smooth);
    { MeterOptions sliding; sliding.smooth = true; CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(sliding).dump())).smooth); }
    // The meter starts over after a pause unless "continue" is saved.
    CHECK(!ReadMeterOptions(json::object()).continuous && !ReadMeterOptions(json::parse(R"({"after_pause":true})")).continuous);
    { MeterOptions going; going.continuous = true; CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(going).dump())).continuous); }
    // The classic style is its own choice: only "classic" picks it, never with
    // flat, and its parts show unless saved as a literal false, each apart.
    CHECK(ReadMeterOptions(json::parse(R"({"style":"classic"})")).style == MeterStyle::Classic);
    CHECK(ReadMeterOptions(json::parse(R"({"style":"Classic"})")).style == MeterStyle::Angled);
    { MeterOptions classic; classic.style = MeterStyle::Classic; CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(classic).dump())).style == MeterStyle::Classic); }
    for (const auto& part : ClassicParts) {
        CHECK(ReadMeterOptions(json::object()).*part.shown);
        MeterOptions off; off.*part.shown = false;
        const auto read = ReadMeterOptions(json::parse(MeterOptionsJson(off).dump()));
        for (const auto& other : ClassicParts) CHECK(read.*other.shown == (other.key != part.key));
    }
    // Every part of the HUD shows unless it is saved as a literal false, and
    // a round trip keeps each one apart from the others.
    for (const auto& part : HudParts) {
        for (const json& kept : {json(), json(true), json(0), json("false")}) CHECK(ReadMeterOptions(json{{part.key, kept}}).*part.shown);
        MeterOptions off; off.*part.shown = false;
        const auto read = ReadMeterOptions(json::parse(MeterOptionsJson(off).dump()));
        for (const auto& other : HudParts) CHECK(read.*other.shown == (&other != &part));
    }
    // No input display unless one is saved by name, and a round trip keeps each.
    for (const json& inputs : {json(), json("Stick"), json(1), json(true)}) CHECK(ReadMeterOptions(json{{"hud_inputs", inputs}}).inputs == InputDisplay::Off);
    for (const auto inputs : {InputDisplay::Off, InputDisplay::Stick, InputDisplay::Arrows, InputDisplay::Both}) {
        MeterOptions saved; saved.inputs = inputs;
        CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(saved).dump())).inputs == inputs);
    }
    // The HUD is standard size unless small (0) or large (2) is saved, and keeps its size through a round trip.
    for (const json& size : {json(), json(3), json(-1), json(1.5), json("2"), json(true)}) CHECK(ReadMeterOptions(json{{"hud_size", size}}).size == 1);
    for (int size = 0; size < HudSizes; ++size) {
        MeterOptions options; options.size = size;
        CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(options).dump())).size == size);
    }
    return 0;
}
