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
    DummyPlan settings; settings.when = 4; settings.slot = 3;
    std::string error;
    for (std::size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        sf4e::combo::Step step;
        CHECK(sf4e::combo::ParseStep(notation[i], step, error));
        const auto migrated = MigrateReplyMoves(names[i]);
        CHECK(migrated == sf4e::combo::Canonical(step));
        CHECK(MigrateReplyMoves(migrated) == migrated);
        DummyPlan plan;
        CHECK(BuildReplyPlan(migrated, settings, plan, error));
        CHECK(plan.when == 4 && plan.slot == 3 && !plan.moves[0].empty());
        for (int side = 0; side < 2; ++side) {
            const auto expected = sf4e::combo::Synthesize({notation[i]}, side == 0);
            CHECK(plan.moves[side].size() == expected.size());
            for (std::size_t frame = 0; frame < expected.size(); ++frame) {
                const auto& got = plan.moves[side][frame]; const auto& want = expected[frame];
                CHECK(got.raw == want.raw && got.mapped == want.mapped && got.wait == want.wait && got.offset == want.offset);
            }
        }
    }
    CHECK(MigrateReplyMoves(" Dash > back dash > THROW > Focus > jump ") == "66 > 44 > 5LP+LK > 5MP+MK > 8");
    CHECK(MigrateReplyMoves("2MK xx focus attack > dash") == "2MK > xx 5MP+MK > 66");
    CHECK(MigrateReplyMoves("2MK > xx 623HP") == "2MK > xx 623HP");
    CHECK(MigrateReplyMoves("dash > Hadoken") == "dash > Hadoken");
    // Invalid saved text (including a nonempty separator-only line) cannot
    // erase a typed reply and silently switch the command to its slot.
    DummyPlan result; result.moves[0] = result.moves[1] = {{0x10, 0x10, 0, 0}};
    for (const char* bad : {"Hadoken", "dash", "> ,", "2MK > nonsense", "xx 623HP"}) {
        error.clear();
        CHECK(!BuildReplyPlan(bad, settings, result, error) && !error.empty());
        CHECK(result.moves[0].size() == 1 && result.moves[0][0].raw == 0x10 && result.moves[1][0].raw == 0x10);
    }
    CHECK(BuildReplyPlan("", settings, result, error) && result.moves[0].empty() && result.moves[1].empty() && result.slot == 3);

    // Every reply preset, 214K and 236P included, is notation the parser and
    // the plan builder take as typed text, and reads back as itself however
    // it is spelled; text that is no preset, or none, picks none.
    CHECK(ReplyPresetCount == 9);
    for (int i = 0; i < ReplyPresetCount; ++i) {
        DummyPlan plan; error.clear();
        CHECK(BuildReplyPlan(ReplyPresets[i], settings, plan, error) && !plan.moves[0].empty() && !plan.moves[1].empty());
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
        CHECK(!options.flat && !options.recovery);
    }
    CHECK(ReadMeterOptions(json::parse(R"({"style":"flat"})")).flat);
    for (const char* shown : {"{}", R"({"frames_shown":30})", R"({"frames_shown":"90"})", R"({"frames_shown":90.5})", R"({"frames_shown":-60})"})
        CHECK(ReadMeterOptions(json::parse(shown)).shown == 60);
    for (const int choice : MeterShownChoices) CHECK(ReadMeterOptions(json{{"frames_shown", choice}}).shown == choice);
    for (int bits = 0; bits < 4; ++bits) {
        MeterOptions options; options.flat = (bits & 1) != 0; options.recovery = (bits & 2) != 0; options.shown = bits == 3 ? 120 : 90;
        const auto read = ReadMeterOptions(json::parse(MeterOptionsJson(options).dump()));
        CHECK(read.flat == options.flat && read.recovery == options.recovery && read.shown == options.shown);
    }
    return 0;
}
