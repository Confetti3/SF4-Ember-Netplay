#include <algorithm>
#include "../training/ComboTrial.hxx"
#include "game_file_test_support.hxx"
#include "test_support.hxx"
#include <map>

using namespace sf4e;
using Ids = std::vector<std::int32_t>;

// A command file with only what the conversion reads: motions and moves.
struct MoveRow {
    const char* name;
    std::uint16_t input, flags;
    int motion, script;
    std::uint16_t position = 0;
    std::uint32_t category = 0;
    std::int16_t meter = 0, revenge = 0;
    std::uint16_t rules = 0;
    std::int8_t ultra = 0;
};
static Bytes Commands() {
    // Each motion: {input, match} per step.
    const std::vector<std::vector<std::array<std::uint16_t, 2>>> motions = {
        {{{0x04, 2}}, {{0x14, 2}}, {{0x10, 2}}},              // 236
        {{{0x04, 2}}, {{0x10, 1}}, {{0x04, 2}}, {{0x10, 2}}}, // 236236
        {{{0x10, 1}}, {{0x01, 1}}, {{0x10, 2}}}};             // 66
    const MoveRow moves[] = {
        {"5LP", 0x40, 0x1010, -1, 256, 2, 0x1},
        {"5LPF", 0x40, 0x1010, -1, 262, 1, 0x1},
        {"DASH", 0, 0, 2, 18, 0, 0x80000},
        {"HADO_L", 0x40, 0x3010, 0, 384, 0, 0x200},
        {"HADO_M", 0x80, 0x3010, 0, 385, 0, 0x200},
        {"HADO_H", 0x100, 0x3010, 0, 386, 0, 0x200},
        {"HADO_EX", 0x1C0, 0x3050, 0, 387, 0, 0x1200, 250},
        {"SUPER_L", 0x40, 0x3020, 1, 420, 0, 0x400, 1000},
        {"ULTRA_1", 0x1C0, 0x3020, 1, 436, 0, 0x800, 0, 200, 0x81, 0},
        {"ULTRA_2", 0xE00, 0x3020, 1, 440, 0, 0x800, 0, 200, 0x81, 1},
        {"SAVING", 0x480, 0x1020, -1, 322, 0, 0x80},
        {"SAVING_EX", 0x480, 0x1020, -1, 325, 0, 0x1080, 500}};
    Bytes out(0x38), names, strings;
    std::memcpy(out.data(), "#BCM", 4);
    out[0x12] = static_cast<std::uint8_t>(motions.size());
    out[0x14] = static_cast<std::uint8_t>(sizeof moves / sizeof *moves);
    Set32(out, 0x20, static_cast<std::uint32_t>(out.size()));
    for (const auto& motion : motions) {
        const std::size_t at = out.size();
        Put32(out, static_cast<std::uint32_t>(motion.size()));
        for (const auto& step : motion) { Put16(out, 0); Put16(out, 12); Put16(out, step[0]); Put16(out, 0); Put16(out, step[1]); Put16(out, 0); }
        out.resize(at + 0xC4);
    }
    Set32(out, 0x28, static_cast<std::uint32_t>(out.size()));
    const std::size_t stringsAt = out.size() + (sizeof moves / sizeof *moves) * (0x54 + 4);
    for (const auto& row : moves) {
        const std::size_t at = out.size();
        Put16(out, row.input); Put16(out, row.flags); Put16(out, row.position); Put16(out, row.rules);
        Put16(out, 0); Put16(out, 3); Put16(out, 0);
        out.push_back(0); out.push_back(0); out.push_back(static_cast<std::uint8_t>(row.ultra)); out.push_back(0);
        Put16(out, 0); Put32(out, 0);
        Put16(out, static_cast<std::uint16_t>(row.meter)); Put16(out, 0);
        Put16(out, static_cast<std::uint16_t>(row.revenge)); Put16(out, 0);
        Put32(out, static_cast<std::uint32_t>(row.motion)); Put32(out, static_cast<std::uint32_t>(row.script));
        Put32(out, row.category);
        out.resize(at + 0x54);
        Put32(names, static_cast<std::uint32_t>(stringsAt + strings.size()));
        strings.insert(strings.end(), row.name, row.name + std::strlen(row.name) + 1);
    }
    Set32(out, 0x2C, static_cast<std::uint32_t>(out.size()));
    out.insert(out.end(), names.begin(), names.end());
    out.insert(out.end(), strings.begin(), strings.end());
    return out;
}

// A script file laid out like a stock one: header, both offset tables, then
// each script as its header, its command lists and their records.
struct ScriptRow {
    Ids hits, effects;
    std::vector<std::array<int, 2>> flows; // {type, target}
    Ids guards;                            // hitboxes of type 0, which never hit
};
static Bytes Scripts(const std::map<int, ScriptRow>& scripts, const std::map<int, ScriptRow>& effects) {
    const std::size_t counts[] = {scripts.empty() ? 0 : static_cast<std::size_t>(scripts.rbegin()->first) + 1,
        effects.empty() ? 0 : static_cast<std::size_t>(effects.rbegin()->first) + 1};
    Bytes out(0x28 + 4 * (counts[0] + counts[1]));
    std::memcpy(out.data(), "#BAC", 4);
    std::size_t table = 0x28;
    for (int bank = 0; bank < 2; ++bank) {
        out[0x0C + 2 * bank] = static_cast<std::uint8_t>(counts[bank]);
        out[0x0D + 2 * bank] = static_cast<std::uint8_t>(counts[bank] >> 8);
        Set32(out, 0x14 + 4 * bank, static_cast<std::uint32_t>(table));
        for (const auto& entry : bank ? effects : scripts) {
            const ScriptRow& row = entry.second;
            Set32(out, table + 4 * entry.first, static_cast<std::uint32_t>(out.size()));
            Bytes lists, data;
            const std::size_t listCount = 4;
            const auto list = [&](unsigned type, std::size_t count) {
                Put16(lists, type); Put16(lists, static_cast<std::uint32_t>(count)); Put32(lists, 0);
                Put32(lists, static_cast<std::uint32_t>(listCount * 12 - (lists.size() - 8) + data.size()));
            };
            list(0, row.flows.size());
            for (const auto& flow : row.flows) { Put16(data, flow[0]); Put16(data, 0); Put16(data, flow[1]); Put16(data, 0); }
            list(7, row.hits.size() + row.guards.size());
            for (std::size_t i = 0; i < row.hits.size() + row.guards.size(); ++i) {
                const std::size_t at = data.size();
                data.resize(at + 44);
                data[at + 26] = i < row.hits.size() ? 1 : 0;
                Set32(data, at + 40, static_cast<std::uint32_t>(i < row.hits.size() ? row.hits[i] : row.guards[i - row.hits.size()]));
            }
            list(10, row.effects.size() + 1);
            data.resize(data.size() + 32); // An effect command of another kind.
            data[data.size() - 32] = 5;
            for (const auto effect : row.effects) { Put16(data, 0); Put16(data, 2); Put32(data, effect); data.resize(data.size() + 24); }
            list(9, 3); // A list the reader skips.
            const std::size_t at = out.size();
            out.resize(at + 0x18);
            out[at + 0x12] = static_cast<std::uint8_t>(listCount);
            out.insert(out.end(), lists.begin(), lists.end());
            out.insert(out.end(), data.begin(), data.end());
        }
        table += 4 * counts[bank];
    }
    return out;
}
static ScriptRow Script(Ids hits, Ids effects = {}, std::vector<std::array<int, 2>> flows = {}, Ids guards = {}) {
    return ScriptRow{std::move(hits), std::move(effects), std::move(flows), std::move(guards)};
}
static Bytes SampleScripts() {
    return Scripts({{18, Script({})}, {256, Script({10}, {}, {}, {999})}, {262, Script({12})}, {322, Script({60}, {}, {{{0, 325}}})}, {325, Script({})},
        {384, Script({}, {0})}, {385, Script({}, {1})}, {386, Script({}, {2})}, {387, Script({}, {3})}, {420, Script({128})},
        // The ultra hits from the script it passes into on hit, never from the one that waits for a button.
        {436, Script({}, {}, {{{1, 437}}, {{12, 256}}})}, {437, Script({130}, {}, {{{0, 436}}})}, {440, Script({137})}},
        {{0, Script({100})}, {1, Script({101})}, {2, Script({102})}, {3, Script({103, 104})}});
}


static clg::Step Row(Ids criteria, const char* name, std::int16_t type = clg::Attack) {
    clg::Step row;
    row.type = type; row.criteria = std::move(criteria);
    row.screen[2] = name; row.help[2] = std::string(name) + "H";
    return row;
}
static bool Named(const std::vector<std::string>& issues, const std::string& part) {
    return std::any_of(issues.begin(), issues.end(), [&](const std::string& issue) { return issue.find(part) != std::string::npos; });
}

int main() {
    std::string error;
    // The script file: hits, effects, passes, and what a damaged file does.
    const Bytes scriptBytes = SampleScripts();
    bac::File scripts;
    CHECK(ReadExact(scriptBytes, scriptBytes.size(), scripts, error));
    CHECK(scripts.scripts.size() == 441 && scripts.effects.size() == 4);
    CHECK(bac::ScriptHits(scripts, 256) == Ids{10});
    CHECK(bac::ScriptHits(scripts, 384) == Ids{100});
    CHECK((bac::ScriptHits(scripts, 387) == Ids{103, 104}));
    CHECK(bac::ScriptHits(scripts, 436) == Ids{130});
    CHECK(bac::ScriptHits(scripts, 300).empty() && bac::ScriptHits(scripts, -1).empty() && bac::ScriptHits(scripts, 9999).empty());
    for (std::size_t size = 0; size < scriptBytes.size(); ++size) {
        bac::File model;
        if (!ReadExact(scriptBytes, size, model, error)) CHECK(model.scripts.empty());
    }
    {
        bac::File model;
        CHECK(!bac::Read(nullptr, 64, model, error));
        Bytes bad = scriptBytes;
        bad[1] = 'X';
        CHECK(!ReadExact(bad, bad.size(), model, error));
        // Every script aimed at the same bytes is refused, not read 65535 times over.
        bad = scriptBytes;
        bad[0x0C] = bad[0x0D] = 0xFF;
        Set32(bad, 0x14, 0x28);
        CHECK(!ReadExact(bad, bad.size(), model, error));
        Bytes same(0x28 + 4 * 2000 + 0x18 + 12 + 44);
        std::memcpy(same.data(), "#BAC", 4);
        same[0x0C] = 2000 & 0xFF; same[0x0D] = 2000 >> 8;
        Set32(same, 0x14, 0x28);
        const std::size_t one = 0x28 + 4 * 2000;
        for (int i = 0; i < 2000; ++i) Set32(same, 0x28 + 4 * i, static_cast<std::uint32_t>(one));
        same[one + 0x12] = 1; same[one + 0x18] = 7; same[one + 0x1A] = 1; same[one + 0x20] = 12;
        CHECK(!ReadExact(same, same.size(), model, error) && error == "the scripts overlap");
        // One byte at a time: read or refused, never out of range.
        for (std::size_t at = 0; at < scriptBytes.size(); ++at) for (const std::uint8_t value : {std::uint8_t(0x7F), std::uint8_t(0xFF)}) {
            bad = scriptBytes;
            bad[at] = value;
            if (ReadExact(bad, bad.size(), model, error)) for (std::int32_t s = 0; s < 441; s += 20) bac::ScriptHits(model, s);
        }
    }

    const Bytes commandBytes = Commands();
    bcm::File moves;
    CHECK(bcm::Read(commandBytes.data(), commandBytes.size(), moves, error));
    const combo::Fighter fighter = combo::MakeFighter(moves, scripts);
    CHECK((fighter.known == Ids{10, 12, 60, 100, 101, 102, 103, 104, 128, 130, 137}));

    // What a typed step starts with full bars.
    const auto actions = [&](const char* text, int ultra = -1) {
        combo::Step step;
        CHECK(combo::ParseStep(text, step, error));
        return combo::StepActions(fighter.moves, step, ultra);
    };
    CHECK((actions("236P") == Ids{384, 385, 386}));
    CHECK(actions("236236LP") == Ids{420});       // not the Hadoken inside it
    CHECK(actions("236236PPP") == Ids{436});      // not the EX Hadoken
    CHECK(actions("236236KKK", 1) == Ids{440} && actions("236236KKK", 0).empty());
    CHECK(actions("FADC") == Ids{325} && actions("FADC44") == Ids{325});
    CHECK((actions("MP+MK") == Ids{322, 325}));
    CHECK(actions("66") == Ids{18} && actions("623P").empty());

    // A combo as Ember's own trial: the action ids to see, step by step.
    {
        const auto sorted = [](std::vector<int> ids) { std::sort(ids.begin(), ids.end()); return ids; };
        const training::Trial made = combo::PracticeTrial(fighter, {"", "RYU", "", {"cl.LP", "xx 236P", "FADC", "MP+MK", "236236PPP", "623P", "what"}});
        CHECK(made.steps.size() == 7);
        CHECK(made.steps[0].ids == Ids{256} && made.steps[0].mustHit);
        CHECK((sorted(made.steps[1].ids) == Ids{384, 385, 386}) && made.steps[1].mustHit); // a projectile hits too
        // FADC is its dash, and the focus attack it cancels is no move of this trial.
        CHECK(made.steps[2].ids == Ids{18} && !made.steps[2].mustHit);
        CHECK((sorted(made.moves) == Ids{18, 256, 262, 322, 384, 385, 386, 387, 420, 436, 440}));
        // The focus attack passes into its next level by itself; 437 is no move.
        CHECK((sorted(made.steps[3].ids) == Ids{322, 325}) && made.steps[3].mustHit);
        CHECK(made.steps[4].ids == Ids{436} && made.steps[4].mustHit);
        // A move the fighter does not have and a step that does not parse are not checked.
        CHECK(made.steps[5].ids.empty() && made.steps[6].ids.empty());
        const training::Trial dash = combo::PracticeTrial(fighter, {"", "RYU", "", {"66", "5LP"}});
        CHECK(dash.steps[0].ids == Ids{18} && !dash.steps[0].mustHit && (sorted(dash.steps[1].ids) == Ids{256, 262}));
        CHECK(std::find(dash.moves.begin(), dash.moves.end(), 325) != dash.moves.end());
        CHECK(combo::PracticeTrial(fighter, {}).steps.empty());

        // Played through the trial: jab, Hadoken, focus cancel, dash.
        training::TrialSession session;
        CHECK(session.Load(combo::PracticeTrial(fighter, {"", "RYU", "", {"cl.LP", "xx 236P", "FADC"}}), error));
        training::TrialObservation now;
        now.valid = true;
        const auto frame = [&](int action, unsigned defender, float damage) {
            now.attackerActionFrame = action == now.attackerAction ? now.attackerActionFrame + 1 : 0;
            now.attackerAction = action; now.attackerStatus = 16; now.defenderStatus = defender; now.defenderComboDamage = damage;
            session.Observe(now);
        };
        frame(0, 0, 0); frame(256, 0, 0); frame(256, 21, 30); frame(385, 21, 30); frame(385, 21, 90);
        CHECK(session.GetView().current == 2 && !session.GetView().complete);
        frame(325, 21, 90); frame(18, 21, 90);
        CHECK(session.GetView().complete && session.GetView().successes == 1 && session.Unmatched().empty());
    }

    // The fighter's own trials: 24 levels, as the game's menu lists them.
    clg::File stock;
    stock.levels.resize(clg::GameLevels);
    for (std::size_t i = 0; i < stock.levels.size(); ++i) {
        stock.levels[i].name = "Lv_" + std::to_string(i + 1);
        stock.levels[i].menuA = 88;
        stock.levels[i].steps = {Row({100, 101, 102}, "ID_CMD_RYU_0008")};
    }
    // The game lists ids no hitbox uses (11, 13) beside the real ones.
    stock.levels[1].steps = {Row({10, 11, 12, 13}, "ID_CMD_CMN_0006"), Row({325}, "ID_CMD_CMN_0001", clg::Script)};
    stock.levels[2].steps = {Row({102}, "ID_CMD_RYU_0008"), Row({103, 104}, "ID_CMD_RYU_0008"), Row({777}, "ID_CMD_RYU_0099")};
    stock.levels[3].steps = {Row({130}, "ID_CMD_RYU_0018"), Row({18}, "ID_CMD_RYU_0030", clg::Script)};
    stock.levels[3].ultra = 1;

    // Trials to combos.
    std::vector<std::string> issues;
    const combo::Pack game = combo::ReadTrials(stock, fighter, "RYU", "Ryu", issues);
    CHECK(game.name == "Ryu" && game.combos.size() == 23);
    CHECK(issues.size() == 1 && issues[0] == "Trial 3: step 3 is a move the notation cannot say");
    CHECK(game.combos[0].name == "Trial 1" && game.combos[0].character == "RYU" && game.combos[0].steps == std::vector<std::string>{"236P"});
    CHECK((game.combos[1].steps == std::vector<std::string>{"5LP", "FADC"}));
    CHECK((game.combos[2].name == "Trial 4" && game.combos[2].steps == std::vector<std::string>{"236236PPP", "66"}));
    CHECK(game.combos[0].notes.empty() && game.combos[2].notes == "Ultra 2");
    {
        // Two strengths of three have no word: the one sharing most is taken, and said to be a guess.
        combo::Step step;
        bool guessed = false;
        CHECK(combo::RowStep(fighter, Row({100, 101}, ""), step, guessed) && guessed && combo::Canonical(step) == "236LP");
        CHECK(combo::RowStep(fighter, Row({60}, ""), step, guessed) && !guessed && combo::Canonical(step) == "5MP+MK");
        CHECK(!combo::RowStep(fighter, Row({}, ""), step, guessed));
    }

    // The player's own combos go before trials read back from the game, whatever the pack order.
    {
        combo::Pack mixed{"Ryu", {{"Trial 1", "RYU", "", {"5LP"}}, {"Trial 2", "RYU", "", {"5LP", "5LP"}}, {"Mine", "RYU", "", {"5LP", "5LP", "5LP"}}, {"Trial 10", "RYU", "", {"66"}}}};
        clg::File made; std::vector<std::string> notes;
        CHECK(combo::BuildTrials(mixed, "RYU", fighter, stock, made, notes) == 4);
        CHECK(!combo::FromGameTrial(mixed.combos[2]) && combo::FromGameTrial(mixed.combos[3]) && !combo::FromGameTrial({"Trial", "RYU", "", {}}));
        CHECK(made.levels[0].steps.size() == 3 && made.levels[1].steps.size() == 1 && made.levels[2].steps.size() == 2 && made.levels[3].steps.size() == 1);
    }
    // A move the game reported comes back as its step, cancelled when asked.
    {
        combo::Step probe; std::string ignored;
        CHECK(combo::ParseStep("5LP", probe, ignored));
        int jab = -1;
        for (const auto& move : fighter.moves.moves) if (combo::Satisfies(fighter.moves, move, probe)) { jab = move.script; break; }
        // The script is the close jab's, so that is how it is spelled.
        CHECK(jab >= 0 && combo::ActionStep(fighter.moves, jab, false) == "cl.LP" && combo::ActionStep(fighter.moves, jab, true) == "xx cl.LP");
        CHECK(combo::ActionStep(fighter.moves, 99999, false).empty());
        // A script shared by an any-button command and a named one is spelled by the named one.
        {
            bcm::File shared; bcm::Move any, named;
            any.name = "HADOKEN"; any.spelled = true; any.motion = "236"; any.buttons = combo::Punches; any.need = 1; any.press = true; any.script = 7;
            named = any; named.name = "HADOKEN_H"; named.buttons = combo::HP;
            shared.moves = {any, named};
            CHECK(combo::ActionStep(shared, 7, false) == "236HP" && combo::ActionStep(shared, 7, true) == "xx 236HP");
        }
    }
    // Combos to trials.
    combo::Pack pack{"Mine", {
        {"Bread and butter", "RYU", "", {"5LP", "xx 236HP", "FADC", "236236PPP"}},
        {"Not Ryu", "KEN", "", {"5LP"}},
        {"Too long", "RYU", "", std::vector<std::string>(9, "5LP")},
        {"No such move", "RYU", "", {"5LP", "623P"}},
        {"", "RYU", "", {"236236LP", "66", "236236KKK", "MP+MK", "236PP"}}}};
    clg::File trials;
    issues.clear();
    CHECK(combo::BuildTrials(pack, "RYU", fighter, stock, trials, issues) == 3);
    CHECK(trials.levels.size() == clg::GameLevels);
    // A long combo is written whole and the player is told what the game shows.
    CHECK(Named(issues, "Too long: the game shows only the first 8 of its 9 steps"));
    CHECK(Named(issues, "No such move: step 2 \"623P\": no move of the fighter is started by it"));
    CHECK(Named(issues, "\"236236LP\" is shown by its kind of move") && Named(issues, "\"236236KKK\" is shown by its kind of move"));
    CHECK(issues.size() == 4);
    // The game's own task list gets a combo's rows as text ids, or nothing when a step has no row.
    {
        const auto texts = combo::TrialTexts(fighter, stock, pack.combos[0]);
        CHECK(texts.size() == 4 && texts[0] == trials.levels[0].steps[0].screen && texts[3] == trials.levels[0].steps[3].screen);
        CHECK(combo::TrialTexts(fighter, stock, pack.combos[3]).empty());
    }
    {
        const auto& level = trials.levels[0];
        // What no reader is known for stays the game's own.
        CHECK(level.name == "Lv_1" && level.menuA == 88 && level.ultra == 0 && level.steps.size() == 4);
        CHECK(level.steps[0] == stock.levels[1].steps[0]);                 // the game's own row, unused ids and all
        CHECK(level.steps[1] == stock.levels[2].steps[0]);
        CHECK(level.steps[2] == stock.levels[1].steps[1]);
        CHECK(level.steps[3] == stock.levels[3].steps[0]);
        // The long combo takes the second level whole; the ultra combo follows it.
        CHECK(trials.levels[1].steps.size() == 9);
        const auto& second = trials.levels[2];
        CHECK(second.ultra == 1 && second.steps.size() == 5);
        CHECK(second.steps[0].criteria == Ids{128} && second.steps[0].screen[2] == "ID_CMD_CMN_0016" && second.steps[0].screen[1] == "ID_CMD_CMN_0023");
        CHECK(second.steps[1] == stock.levels[3].steps[1]);
        CHECK(second.steps[2].criteria == Ids{137} && second.steps[2].help[2] == "ID_CMD_CMN_0019");
        CHECK(second.steps[3].type == clg::Attack && second.steps[3].criteria == Ids{60});
        CHECK(second.steps[3].screen[2] == "ID_CMD_CMN_0000" && second.steps[3].help[2] == "ID_CMD_CMN_0032");
        CHECK(second.steps[4] == stock.levels[2].steps[1]);
        for (std::size_t i = 3; i < clg::GameLevels; ++i) CHECK(trials.levels[i] == stock.levels[i]);
    }
    {
        // A normal the game's trials never show gets the shared row for normals.
        clg::File bare = stock;
        bare.levels[1].steps = {Row({100}, "ID_CMD_RYU_0008")};
        issues.clear();
        CHECK(combo::BuildTrials({"", {{"", "RYU", "", {"far.LP", "FADC"}}}}, "RYU", fighter, bare, trials, issues) == 1 && issues.empty());
        const auto& jab = trials.levels[0].steps[0];
        CHECK(jab.criteria == Ids{12});
        // A row borrowed from a wider one of the game's says which strength.
        combo::Step typed;
        clg::Step borrowed;
        bool vague = false;
        CHECK(combo::ParseStep("236MP", typed, error) && combo::StepRow(fighter, bare, typed, -1, borrowed, vague, error) && !vague);
        CHECK(borrowed.criteria == Ids{101} && borrowed.screen[2] == "ID_CMD_RYU_0008" && borrowed.help[2] == "ID_CMD_RYU_0008H");
        CHECK(borrowed.screen[1] == "ID_CMD_CMN_0024" && borrowed.help[1] == "ID_CMD_CMN_0024");
        CHECK(jab.screen[0] == "ID_CMD_CMN_0003" && jab.screen[1] == "ID_CMD_CMN_0023" && jab.screen[2] == "ID_CMD_CMN_0006" && jab.help[2] == "ID_CMD_CMN_0042");
        const auto& cancel = trials.levels[0].steps[1];
        CHECK(cancel.type == clg::Script && cancel.criteria == Ids{325} && cancel.screen[0] == "ID_CMD_CMN_0022" && cancel.screen[2] == "ID_CMD_CMN_0001");
        // The notes choose the ultra where the moves do not.
        CHECK(combo::BuildTrials({"", {{"", "RYU", "Ultra 2", {"5LP"}}, {"", "RYU", "Ultra 1", {"236236KKK"}}}}, "RYU", fighter, stock, trials, issues) == 1);
        CHECK(trials.levels[0].ultra == 1 && issues.size() == 1);
        issues.clear();
        // More combos than the menu has levels: the rest are named.
        combo::Pack many{"", std::vector<combo::Combo>(26, combo::Combo{"Jab", "RYU", "", {"5LP"}})};
        CHECK(combo::BuildTrials(many, "RYU", fighter, stock, trials, issues) == 24 && issues.size() == 2);
        bare.levels.pop_back();
        issues.clear();
        CHECK(combo::BuildTrials(many, "RYU", fighter, bare, trials, issues) == 0 && issues.size() == 1);
    }

    // The files: written beside the book, read back, and never by a made-up code.
    namespace fs = std::filesystem;
    CHECK(combo::IsFighterCode("RYU") && !combo::IsFighterCode("..") && !combo::IsFighterCode("ry/") && !combo::IsFighterCode("RYUU"));
    {
        std::error_code ignored;
        const fs::path folder = fs::temp_directory_path() / "sf4e-combo-trial-test";
        fs::remove_all(folder, ignored);
        // The program's choice of balance folder first, then the other layers
        // newest first down to the SF4 one, which stands when none has the fighter.
        const auto layer = [&](const char* path, const char* code) {
            const fs::path made = folder / path / code;
            fs::create_directories(made, ignored);
            std::ofstream(made / (std::string(code) + ".bcm")).put('#');
            return made;
        };
        CHECK(combo::CommandFolder(folder, "RYU") == folder / "resource/battle/chara" / "RYU");
        CHECK(combo::CommandFolder(folder, "RYU") == layer("resource/battle/chara", "RYU"));
        CHECK(combo::CommandFolder(folder, "RYU") == layer("dlc/03_character_free/battle/regulation/latest", "RYU"));
        CHECK(combo::CommandFolder(folder, "RYU") == layer("patch_ae2_tu2/battle/regulation/ae2_110", "RYU"));
        CHECK(combo::CommandFolder(folder, "RYU") == layer("patch_ae2_tu3/battle/regulation/ae2_111", "RYU"));
        // Ken takes 1.10 in the program, so a 1.11 folder is passed over.
        layer("patch_ae2_tu3/battle/regulation/ae2_111", "KEN");
        CHECK(combo::CommandFolder(folder, "KEN") == layer("patch_ae2_tu1/battle/regulation/ae2_109", "KEN"));
        CHECK(combo::CommandFolder(folder, "KEN") == layer("patch_ae2_tu2/battle/regulation/ae2_110", "KEN"));
        CHECK(!combo::SaveTrials(folder, "../x", stock, error));
        CHECK(combo::SaveTrials(folder / "trials", "RYU", stock, error));
        Bytes bytes;
        clg::File back;
        CHECK(combo::ReadFile(folder / "trials" / "RYU_swan.clg", clg::MaxBytes, bytes, error) && clg::Read(bytes.data(), bytes.size(), back, error) && back == stock);
        CHECK(!combo::ReadFile(folder / "trials" / "RYU_swan.clg", 16, bytes, error) && !combo::ReadFile(folder / "none.clg", 16, bytes, error));
        combo::Fighter none;
        CHECK(!combo::LoadFighter(folder, "RYU", none, error) && !combo::LoadFighter(folder, "..", none, error) && !combo::LoadTrials(folder, "RYU", back, error));
        fs::remove_all(folder, ignored);
    }

    // The installed game, when there is one: every one of the 44 fighters
    // has files, every fighter's trials become combos, and those combos become
    // a trial file the reader accepts.
    if (const char* installed = std::getenv("SF4E_GAME_DIR")) {
        static const char* const codes[] = {"ADN", "AGL", "BLK", "BLR", "BOS", "BSN", "CDY", "CHB", "CMY", "CNL", "DAN", "DCP", "DDL", "DJY", "DSM",
            "ELN", "FLN", "GEN", "GKI", "GKN", "GKX", "GUL", "GUY", "HKN", "HND", "HUG", "HWK", "IBK", "JHA", "JRI", "KEN", "MKT", "PSN", "RIC",
            "RLN", "ROS", "RYU", "RYX", "SGT", "SKR", "VEG", "YAN", "YUN", "ZGF"};
        int fighters = 0, rows = 0, same = 0, levels = 0, combos = 0, steps = 0, said = 0, practised = 0, judged = 0;
        for (const std::string code : codes) {
            combo::Fighter real;
            clg::File theirs, rebuilt, back;
            CHECK(combo::LoadFighter(installed, code, real, error));
            std::printf("%s: %s\n", code.c_str(), combo::CommandFolder(installed, code).lexically_relative(installed).generic_string().c_str());
            if (!combo::LoadTrials(installed, code, theirs, error)) continue;
            issues.clear();
            const combo::Pack read = combo::ReadTrials(theirs, real, code, code, issues);
            combos += static_cast<int>(read.combos.size());
            for (const auto& level : theirs.levels) for (const auto& row : level.steps) {
                combo::Step step;
                bool guessed = false;
                ++steps; said += combo::RowStep(real, row, step, guessed);
            }
            CHECK(combo::BuildTrials(read, code, real, theirs, rebuilt, issues) == static_cast<int>(read.combos.size()));
            std::vector<unsigned char> written;
            CHECK(clg::Write(rebuilt, written, error) && clg::Read(written.data(), written.size(), back, error) && back == rebuilt);
            // How many rows come back as the game wrote them.
            for (std::size_t i = 0; i < read.combos.size(); ++i) {
                const auto& original = theirs.levels[static_cast<std::size_t>(std::stoi(read.combos[i].name.substr(6))) - 1].steps;
                CHECK(rebuilt.levels[i].steps.size() == original.size());
                for (std::size_t j = 0; j < original.size(); ++j) { ++rows; same += rebuilt.levels[i].steps[j] == original[j]; }
            }
            // The same combos as Ember's own trial: every step of the game's has a move to look for.
            for (const auto& made : read.combos) {
                training::TrialSession session;
                const training::Trial practice = combo::PracticeTrial(real, made);
                CHECK(practice.steps.size() == made.steps.size() && session.Load(practice, error));
                for (const auto& step : practice.steps) { ++practised; judged += !step.ids.empty(); }
            }
            levels += static_cast<int>(theirs.levels.size());
            ++fighters;
        }
        CHECK(fighters == 44);
        std::printf("%d fighters: %d of %d trial rows said as notation, %d of %d trials whole; %d of %d rows rebuilt as the game wrote them\n",
            fighters, said, steps, combos, levels, same, rows);
        // Not worse than the figures of the 1.11/1.10 folders the program names.
        CHECK(said >= 3211 && steps == 3226 && combos >= 1042 && levels == 1056);
        std::printf("%d of %d steps of those trials have action ids for Ember's own trial\n", judged, practised);
    }
    return 0;
}
