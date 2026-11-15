#include "../training/FighterMoves.hxx"
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
        ReplyChoice plan;
        CHECK(BuildReply(migrated, plan, error) && !plan.moves.empty());
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
    ReplyChoice result; result.moves = {{0x10, 0x10, 0, 0}};
    // A reply plays from its first pressed frame, so one starting with a wait
    // is refused, as are holds with no move to time the reply by and a
    // neutral "5", which presses nothing.
    for (const char* bad : {"Hadoken", "dash", "> ,", "2MK > nonsense", "xx 623HP", "10f > 5MK", "[1]", "[2]7195f > 5MK", "[4] > 10f > 5MK",
             "[2]30f", "[2]30f > [4]10f", "[2]30f > [4]10f > [1]5f", "5", "[2]30f > 5",
             "[2]30f > 3f > [4]10f > 5", "[2]30f > 5 > 5MK"}) {
        error.clear();
        CHECK(!BuildReply(bad, result, error) && !error.empty());
        CHECK(result.moves.size() == 1 && result.moves[0].raw == 0x10);
    }
    CHECK(BuildReply("", result, error) && result.moves.empty());
    CHECK(BuildReply("[1] > 3HP", result, error) && !result.moves.empty() && BuildReply("[2]30f > 5MK", result, error));
    // The first move's own offset rides with the plan, and goes with typed moves.
    CHECK(BuildReply("[2]30f > 5MK@+3", result, error) && result.offset == 3 && ValidChoice(result));
    // A plan built is one the dummy takes: its frames and schedule agree.
    for (const char* line : {"5MK", "[2]30f > 8#45", "[2]30f > 66#45@+2", "[2]30f > [4] > 5MK", "5LP > [4] > 66#60", "[1] > 623HP > xx 5MP+MK", "[KK]210f > ]KK["}) {
        ReplyChoice built; error.clear();
        CHECK(BuildReply(line, built, error) && ValidChoice(built));
    }
    CHECK(BuildReply("", result, error) && result.offset == 0 && result.moves.empty());

    // Every move the pick list offers is a reply the plan builder takes: the
    // same ones for every fighter, then each fighter's own.
    for (const auto& move : UniversalMoves) {
        ReplyChoice reply; error.clear();
        CHECK(BuildReply(move.moves, reply, error) && !reply.moves.empty());
    }
    for (const auto& move : FighterMoves) {
        ReplyChoice reply; error.clear();
        CHECK(BuildReply(move.moves, reply, error) && !reply.moves.empty());
    }
    CHECK(MovesOf("CDY").size() >= 20 && MovesOf("RYU").size() >= 10 && MovesOf("XXX").empty() && MovesOf(nullptr).empty());
    {
        // The slots that are on make the plan's choices, with their weights and the settings' order.
        ReplySlots slots;
        slots[0] = {"]HP[", true, 3}; slots[1] = {"623P", true, 2}; slots[2] = {"44", false, 5}; slots[3] = {"", true, 1};
        DummyPlan plan; error.clear(); settings.order = Order::InOrder;
        CHECK(BuildSlotPlan(slots, settings, plan, error) && plan.choices.size() == 2 && plan.choices[0].weight == 3 && plan.choices[1].weight == 2 && plan.order == Order::InOrder && ValidDummyPlan(plan));
        slots[0].on = slots[1].on = false;
        CHECK(BuildSlotPlan(slots, settings, plan, error) && plan.choices.empty());
        slots[2] = {"not a move", true, 1};
        plan.choices.clear(); plan.when = 7;
        CHECK(!BuildSlotPlan(slots, settings, plan, error) && plan.when == 7);
    }
    CHECK(MigrateReplyMoves("throw") == "5LP+LK" && MigrateReplyMoves("dash") == "66");

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
    // The Data panel is off, on each fighter's side, 80% solid and with every
    // part unless saved otherwise; a round trip keeps each choice and each part apart.
    {
        const auto plain = ReadMeterOptions(json::parse(R"({"data":"true","data_fighters":"P1","data_opacity":70})"));
        CHECK(!plain.data && plain.dataFighters == DataFighter::Both && plain.dataOpacity == 80);
        for (int fighters = 0; fighters < DataFighters; ++fighters) for (const int opacity : DataOpacities) {
            MeterOptions saved; saved.data = true; saved.dataFighters = static_cast<DataFighter>(fighters); saved.dataOpacity = opacity;
            const auto read = ReadMeterOptions(json::parse(MeterOptionsJson(saved).dump()));
            CHECK(read.data && read.dataFighters == saved.dataFighters && read.dataOpacity == opacity);
        }
        for (const auto& part : DataParts) {
            CHECK(ReadMeterOptions(json::object()).*part.shown);
            MeterOptions off; off.*part.shown = false;
            const auto read = ReadMeterOptions(json::parse(MeterOptionsJson(off).dump()));
            for (const auto& other : DataParts) CHECK(read.*other.shown == (&other != &part));
        }
        // Hakan's oil is left alone unless it is saved as a literal true.
        MeterOptions oil; oil.oilFull = true;
        CHECK(!ReadMeterOptions(json::object()).oilFull && !ReadMeterOptions(json{{"oil_full", "yes"}}).oilFull &&
            ReadMeterOptions(json::parse(MeterOptionsJson(oil).dump())).oilFull);
    }
    // No input display unless one is saved by name, and a round trip keeps each.
    for (const json& inputs : {json(), json("Stick"), json(1), json(true)}) CHECK(ReadMeterOptions(json{{"hud_inputs", inputs}}).inputs == InputDisplay::Off);
    for (const auto inputs : {InputDisplay::Off, InputDisplay::Stick, InputDisplay::Arrows, InputDisplay::Both}) {
        MeterOptions saved; saved.inputs = inputs;
        CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(saved).dump())).inputs == inputs);
    }
    // No hitboxes unless saved as a literal true, and a round trip keeps them on.
    for (const json& boxes : {json(), json(1), json("true")}) CHECK(!ReadMeterOptions(json{{"hitboxes", boxes}}).hitboxes);
    MeterOptions boxed; boxed.hitboxes = true;
    CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(boxed).dump())).hitboxes);
    // Its parts are on unless saved off, and stay off.
    CHECK(boxed.hurtBoxes && boxed.throwBoxes && boxed.skeleton);
    boxed.skeleton = false;
    const MeterOptions read = ReadMeterOptions(json::parse(MeterOptionsJson(boxed).dump()));
    CHECK(!read.skeleton && read.hurtBoxes && read.throwBoxes);
    // The HUD is standard size unless small (0) or large (2) is saved, and keeps its size through a round trip.
    for (const json& size : {json(), json(3), json(-1), json(1.5), json("2"), json(true)}) CHECK(ReadMeterOptions(json{{"hud_size", size}}).size == 1);
    for (int size = 0; size < HudSizes; ++size) {
        MeterOptions options; options.size = size;
        CHECK(ReadMeterOptions(json::parse(MeterOptionsJson(options).dump())).size == size);
    }
    return 0;
}
