#pragma once
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "BacFile.hxx"
#include "ClgFile.hxx"
#include "ComboMoveMap.hxx"
#include "TrialSession.hxx"

// Joins the combo book to the game's own Trial mode: a pack becomes a trial
// file the game can load, and the game's trials become combos. A trial names
// a move by the hit-data its hitboxes use, so both ways go through the
// fighter's command file and script file. Also finds and reads those files in
// the game folder. It knows nothing of the running game or the overlay.
namespace sf4e { namespace combo {
// One fighter's files, as USF4 loads them. reach: per move of moves.moves,
// the hit-data ids of its script and of every script that one passes into.
// known: every hit-data id some move reaches, sorted.
struct Fighter {
    bcm::File moves;
    bac::File scripts;
    std::vector<std::vector<std::int32_t>> reach;
    std::vector<std::int32_t> known;
};
inline Fighter MakeFighter(bcm::File moves, bac::File scripts) {
    Fighter fighter;
    fighter.moves = std::move(moves); fighter.scripts = std::move(scripts);
    for (const auto& move : fighter.moves.moves) fighter.reach.push_back(bac::ScriptHits(fighter.scripts, move.script));
    for (const auto& hits : fighter.reach) fighter.known.insert(fighter.known.end(), hits.begin(), hits.end());
    std::sort(fighter.known.begin(), fighter.known.end());
    fighter.known.erase(std::unique(fighter.known.begin(), fighter.known.end()), fighter.known.end());
    return fighter;
}

// "FADC" and "FADC44", which ParseStep reads as focus buttons plus a dash.
inline bool IsFadc(const Step& step) {
    return !step.air && !step.charge && !step.mash && (step.motion == "66" || step.motion == "44") &&
        step.buttons == (MP | MK) && step.need == 2 && step.edge == Edge::Press;
}
// The moves a typed step starts when both bars are full, as indices into
// file.moves. Narrower than Satisfies alone: a super or ultra takes the input
// from the special inside its motion, and with ultra 0 or 1 the other ultra
// is left out (-1 keeps both). FADC gives the focus cancel; its dash is not
// judged, as in the game's own trials.
inline std::vector<std::size_t> StepMoves(const bcm::File& file, Step step, int ultra = -1) {
    const bool fadc = IsFadc(step);
    if (fadc) step.motion = "5";
    const auto rank = [](const bcm::Move& move) { return move.rules & bcm::NeedsUltra ? 2 : move.meter >= 1000 ? 1 : 0; };
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < file.moves.size(); ++i) {
        const bcm::Move& move = file.moves[i];
        if (move.script < 0 || !Satisfies(file, move, step)) continue;
        if (fadc && (move.category & (bcm::Focus | bcm::Ex)) != (bcm::Focus | bcm::Ex)) continue;
        if (ultra >= 0 && (move.rules & bcm::NeedsUltra) && move.ultra != ultra) continue;
        found.push_back(i);
    }
    std::vector<std::size_t> kept;
    for (const auto i : found) {
        const bcm::Move& move = file.moves[i];
        // The bigger move asks for more stick, and every button that starts
        // this one starts it too. A bigger move without a motion is a
        // follow-up inside a super or ultra and takes nothing.
        const bool taken = std::any_of(found.begin(), found.end(), [&](std::size_t other) {
            const bcm::Move& bigger = file.moves[other];
            return rank(bigger) > rank(move) && bigger.motionIndex >= 0 && bigger.motionIndex != move.motionIndex &&
                !(move.buttons & step.buttons & ~bigger.buttons);
        });
        if (!taken) kept.push_back(i);
    }
    return kept;
}
// The action ids to expect for a typed step: the scripts of StepMoves, each once.
inline std::vector<std::int32_t> StepActions(const bcm::File& file, const Step& step, int ultra = -1) {
    std::vector<std::int32_t> scripts;
    for (const auto i : StepMoves(file, step, ultra)) bac::Add(scripts, file.moves[i].script);
    return scripts;
}

// A combo as Ember's own trial, which judges the action id the fighter shows:
// on the one capture checked (Ken) that id was the script index of the move
// that runs. One step per typed step, in order:
// - its ids are the scripts of the moves it starts, and every other move's
//   script those pass into by themselves (a focus attack growing a level);
// - it has to hit when one of those moves has a hitbox or a projectile;
// - FADC is its dash. The focus attack before it is left out of the moves
//   the trial judges, since a cancelled one is not a step of the combo;
// - a step that does not parse or starts no move gets no ids, which the
//   trial shows as not checked.
inline training::Trial PracticeTrial(const Fighter& fighter, const Combo& combo) {
    training::Trial trial;
    const auto& moves = fighter.moves.moves;
    std::vector<std::int32_t> all, focus;
    for (const auto& move : moves) if (move.script >= 0) bac::Add(all, move.script);
    for (const auto& text : combo.steps) {
        training::TrialStep made;
        Step step;
        std::string error;
        if (ParseStep(text, step, error)) {
            const bool fadc = IsFadc(step);
            if (fadc) {
                bac::Add(focus, StepActions(fighter.moves, step));
                Step dash;
                dash.motion = step.motion;
                step = dash;
            }
            std::vector<std::int32_t> seen, todo;
            made.mustHit = false;
            for (const auto i : StepMoves(fighter.moves, step)) {
                todo.push_back(moves[i].script);
                made.mustHit = made.mustHit || (!fadc && !fighter.reach[i].empty());
            }
            while (!todo.empty()) {
                const auto script = todo.back();
                todo.pop_back();
                if (std::find(seen.begin(), seen.end(), script) != seen.end()) continue;
                seen.push_back(script);
                if (std::find(all.begin(), all.end(), script) != all.end()) bac::Add(made.ids, script);
                if (script < 0 || static_cast<std::size_t>(script) >= fighter.scripts.scripts.size()) continue;
                const auto& next = fighter.scripts.scripts[static_cast<std::size_t>(script)].next;
                todo.insert(todo.end(), next.begin(), next.end());
            }
        }
        trial.steps.push_back(std::move(made));
    }
    for (const auto script : all)
        if (std::find(focus.begin(), focus.end(), script) == focus.end()) trial.moves.push_back(script);
    return trial;
}

namespace detail {
// The text ids every fighter shares (ID_CMD_CMN_*): 0 Focus Attack, 1 Focus
// Cancel, 3-5 Stand/Crouch/Jump, 6-7 Punch/Kick, 8 Normal Throw, 9 Unique
// Attack, 12 Target Combo, 13 Special Move, 16 Super Combo, 19 Ultra Combo,
// 22 EX, 23-25 L/M/H, 32 the focus buttons, 42-43 the punch and kick icons.
inline std::string Common(int number) {
    char text[24];
    std::snprintf(text, sizeof text, "ID_CMD_CMN_%04d", number);
    return text;
}
// The shared text id of the strength a step names: L, M or H.
inline int StrengthText(unsigned buttons) { return buttons & (LP | LK) ? 23 : buttons & (MP | MK) ? 24 : 25; }
// A row from the shared ids, for a move the fighter's own trials never show.
// False when it can only name the kind of move ("Special Move").
inline bool SharedText(const Fighter& fighter, const Step& step, const std::vector<std::size_t>& moves, clg::Step& row) {
    const auto set = [&](std::size_t slot, int screen, int help) { row.screen[slot] = Common(screen); row.help[slot] = Common(help); };
    if (IsFadc(step)) { set(0, 22, 22); set(2, 1, 32); return true; }
    std::uint32_t category = 0;
    for (const auto i : moves) category |= fighter.moves.moves[i].category;
    const bool one = Count(step.buttons) == 1, punch = (step.buttons & Punches) != 0;
    const int strength = StrengthText(step.buttons);
    const int kind = category & bcm::Ultra ? 19 : category & bcm::Super ? 16 : category & bcm::Special ? 13 : category & bcm::Throw ? 8 :
        category & bcm::Focus ? 0 : category & bcm::TargetCombo ? 12 : 9;
    if (kind == 9 && one && !step.charge && !step.mash && (step.motion == "5" || step.motion == "2" || (step.air && step.motion.empty()))) {
        const int stance = step.air ? 5 : step.motion == "2" ? 4 : 3;
        set(0, stance, stance); set(1, strength, strength); set(2, punch ? 6 : 7, punch ? 42 : 43);
        return true;
    }
    if (step.need == 2 && (step.buttons == Punches || step.buttons == Kicks)) set(0, 22, 22);
    else if (one) set(1, strength, strength);
    set(2, kind, kind ? kind : 32);
    return kind == 0;
}
// The row the fighter's own trials use for the same criteria, to borrow its
// text. Ids no move reaches are left out of the comparison: the game lists
// some beside the real ones. exact: the row is this very move.
inline const clg::Step* StockRow(const clg::File& stock, const Fighter& fighter, const clg::Step& row, bool& exact) {
    const clg::Step* best = nullptr;
    std::size_t bestShared = 0, bestTotal = 1;
    exact = false;
    for (const auto& level : stock.levels) for (const auto& theirs : level.steps) {
        if (theirs.type != row.type) continue;
        std::size_t size = 0, shared = 0;
        for (const auto id : theirs.criteria) {
            if (row.type == clg::Attack ? !std::binary_search(fighter.known.begin(), fighter.known.end(), id) :
                std::none_of(fighter.moves.moves.begin(), fighter.moves.moves.end(), [&](const bcm::Move& move) { return move.script == id; })) continue;
            ++size;
            if (std::find(row.criteria.begin(), row.criteria.end(), id) != row.criteria.end()) ++shared;
        }
        // One has to hold the other: "H Hadoken" may borrow from "Hadoken".
        if (!shared || shared != (std::min)(size, row.criteria.size())) continue;
        const std::size_t total = size + row.criteria.size() - shared;
        if (best && shared * bestTotal <= bestShared * total) continue;
        best = &theirs; bestShared = shared; bestTotal = total;
        exact = shared == total;
    }
    return best;
}
// The ultra a typed step asks for, or -1: Ultra 1 when both take the same
// input, unless only Ultra 2 is done in the air and the step is.
inline int StepUltra(const bcm::File& file, const Step& step) {
    int ultra = -1, air = -1;
    for (const auto i : StepMoves(file, step)) {
        const bcm::Move& move = file.moves[i];
        if (!(move.rules & bcm::NeedsUltra) || move.motionIndex < 0) continue;
        const int which = move.ultra ? 1 : 0;
        if (ultra < 0 || which < ultra) ultra = which;
        if (step.air && move.air && (air < 0 || which < air)) air = which;
    }
    return air >= 0 ? air : ultra;
}
// Folds the ways several moves are typed into one step when the notation has
// a word for all of them: close and far, press and release, the three
// strengths. False when more than one way is left.
inline bool FoldSteps(std::vector<Step>& steps) {
    const auto unique = [&] {
        std::vector<Step> kept;
        for (const auto& step : steps)
            if (std::none_of(kept.begin(), kept.end(), [&](const Step& k) { return Canonical(k) == Canonical(step); })) kept.push_back(step);
        steps = std::move(kept);
        return steps.size() == 1;
    };
    if (unique()) return true;
    for (auto& step : steps) step.range = Range::Any;
    if (unique()) return true;
    if (std::any_of(steps.begin(), steps.end(), [](const Step& step) { return step.edge == Edge::Press; }))
        for (auto& step : steps) if (step.edge == Edge::Release) step.edge = Edge::Press;
    if (unique()) return true;
    Step all = steps[0];
    all.buttons = 0;
    const auto rest = Canonical(all);
    for (auto step : steps) {
        if (step.need != 1 || Count(step.buttons) != 1) return false;
        all.buttons |= step.buttons; step.buttons = 0;
        if (Canonical(step) != rest) return false;
    }
    if (all.buttons != Punches && all.buttons != Kicks) return false;
    steps.assign(1, all);
    return true;
}
}

// What a combo's notes say to select an ultra: "Ultra 1", "Ultra 2".
inline std::string UltraNote(int ultra) { return ultra ? "Ultra 2" : "Ultra 1"; }

// One typed step as a row of a trial: what the game checks and the text it
// shows. The row is the fighter's own when its trials have this move; else
// the criteria come from the fighter's files and the text from the shared
// ids. vague: that text names only the kind of move. False with a reason
// when no move of the fighter is started by the step.
inline bool StepRow(const Fighter& fighter, const clg::File& stock, const Step& step, int ultra, clg::Step& row, bool& vague, std::string& error) {
    row = clg::Step{}; vague = false;
    const auto moves = StepMoves(fighter.moves, step, ultra);
    if (moves.empty()) { error = "no move of the fighter is started by it"; return false; }
    // The game checks some moves by their script, not their hit: a dash, a
    // teleport, a counter. Where its own trials do so for this move, so does this.
    clg::Step byScript;
    byScript.type = clg::Script;
    for (const auto i : moves) bac::Add(byScript.criteria, fighter.moves.moves[i].script);
    bool exact = false;
    const clg::Step* theirs = detail::StockRow(stock, fighter, byScript, exact);
    if (theirs && exact) { row = *theirs; return true; }
    if (!IsFadc(step)) for (const auto i : moves) bac::Add(row.criteria, fighter.reach[i]);
    if (row.criteria.empty()) row = byScript;
    if (row.criteria.size() > clg::MaxCriteria) { error = "it starts too many moves"; return false; }
    theirs = detail::StockRow(stock, fighter, row, exact);
    if (theirs && exact) row = *theirs;
    else if (theirs) {
        // A wider row of the game's ("Hadoken") gets the strength this step names.
        row.screen = theirs->screen; row.help = theirs->help;
        if (detail::Count(step.buttons) == 1 && row.screen[1].empty() && row.help[1].empty()) row.screen[1] = row.help[1] = detail::Common(detail::StrengthText(step.buttons));
    }
    else vague = !detail::SharedText(fighter, step, moves, row);
    return true;
}

// Makes the game's trial file for one fighter from a pack: `stock`, the
// fighter's own file, with its first levels replaced by the pack's combos for
// `character`, one level each in pack order. The file keeps the 24 levels the
// game's menu lists, so levels without a combo stay the game's own. Returns
// the number of combos written; issues names every combo left out (over 8
// steps, a step no move starts, more than 24 combos) and every row whose
// text is only general. A trial cannot say whether a move is a cancel or a
// link, and it selects one ultra: see UltraNote.
// A combo ReadTrials made from the game's own trial, by the name it gives.
inline bool FromGameTrial(const Combo& combo) {
    return combo.name.compare(0, 6, "Trial ") == 0 && combo.name.size() > 6 &&
        combo.name.find_first_not_of("0123456789", 6) == std::string::npos;
}
// A combo's steps as trial rows. ultra: the one the level selects, named by
// the notes, else started by the combo's first ultra motion. notes: steps
// shown by their kind of move only. False with the failing step in error.
inline bool ComboRows(const Fighter& fighter, const clg::File& stock, const Combo& combo, std::vector<clg::Step>& rows, int& ultra,
    std::vector<std::string>& notes, std::string& error) {
    std::vector<Step> steps(combo.steps.size());
    std::size_t at = 0;
    ultra = combo.notes.find(UltraNote(1)) != std::string::npos ? 1 : combo.notes.find(UltraNote(0)) != std::string::npos ? 0 : -1;
    for (; at < steps.size() && ParseStep(combo.steps[at], steps[at], error); ++at) if (ultra < 0) ultra = detail::StepUltra(fighter.moves, steps[at]);
    rows.assign(steps.size(), clg::Step{});
    for (std::size_t i = 0; at == steps.size() && i < steps.size(); ++i) {
        bool vague = false;
        if (!StepRow(fighter, stock, steps[i], ultra, rows[i], vague, error)) { error = "\"" + combo.steps[i] + "\": " + error; at = i; }
        else if (vague) notes.push_back("\"" + combo.steps[i] + "\" is shown by its kind of move, the game has no name for it here");
    }
    if (at == steps.size()) return true;
    error = "step " + std::to_string(at + 1) + " " + error;
    return false;
}
// The text ids the game's own task list shows for a combo, one row per step;
// empty when a step has no row, so the overlay's list is used instead.
inline std::vector<std::array<std::string, 4>> TrialTexts(const Fighter& fighter, const clg::File& stock, const Combo& combo) {
    std::vector<clg::Step> rows; std::vector<std::string> notes; std::string error; int ultra = -1;
    std::vector<std::array<std::string, 4>> texts;
    if (ComboRows(fighter, stock, combo, rows, ultra, notes, error)) for (const auto& row : rows) texts.push_back(row.screen);
    return texts;
}
inline int BuildTrials(const Pack& pack, const std::string& character, const Fighter& fighter, const clg::File& stock,
    clg::File& trials, std::vector<std::string>& issues) {
    trials = stock;
    if (stock.levels.size() != clg::GameLevels) { issues.push_back("the fighter's trial file does not hold 24 levels"); return 0; }
    // The player's own combos take the first levels; trials read back from
    // the game fill what is left, so a pack holding both still exports the new ones.
    std::vector<const Combo*> order;
    for (const auto& combo : pack.combos) if (!FromGameTrial(combo)) order.push_back(&combo);
    for (const auto& combo : pack.combos) if (FromGameTrial(combo)) order.push_back(&combo);
    std::size_t written = 0;
    for (const Combo* chosen : order) {
        const auto& combo = *chosen;
        if (combo.character != character || combo.steps.empty()) continue;
        const auto label = combo.name.empty() ? JoinSteps(combo.steps) : combo.name;
        if (written == clg::GameLevels) { issues.push_back(label + ": a trial file holds 24 combos"); continue; }
        // Written whole; the game's list shows the first eight rows only.
        if (combo.steps.size() > clg::GameSteps) issues.push_back(label + ": the game shows only the first 8 of its " + std::to_string(combo.steps.size()) + " steps");
        std::vector<clg::Step> rows;
        std::vector<std::string> notes;
        std::string error;
        int ultra = -1;
        if (!ComboRows(fighter, stock, combo, rows, ultra, notes, error)) { issues.push_back(label + ": " + error); continue; }
        for (const auto& note : notes) issues.push_back(label + ": " + note);
        clg::Level& level = trials.levels[written++];
        level.ultra = ultra > 0; level.flags = 1;
        // The dummy the combo asks for, where the file can say it: standing, crouching or jumping.
        level.dummy = combo.setup.action == 1 ? clg::Crouch : combo.setup.action == 2 ? clg::Jump : clg::Stand;
        level.steps = std::move(rows);
    }
    return static_cast<int>(written);
}

// One row of a trial as a typed step. guessed: several moves fit that the
// notation cannot say as one, and the one sharing most with the row was
// taken. False when no move the notation can say fits.
inline bool RowStep(const Fighter& fighter, const clg::Step& row, Step& step, bool& guessed) {
    guessed = false;
    const auto& moves = fighter.moves.moves;
    const auto has = [&](std::int32_t id) { return std::find(row.criteria.begin(), row.criteria.end(), id) != row.criteria.end(); };
    std::vector<Step> steps;
    std::size_t most = 0, best = 0;
    for (std::size_t i = 0; i < moves.size(); ++i) {
        Step typed;
        if (moves[i].script < 0 || !MoveStep(moves[i], typed)) continue;
        std::size_t shared = 0;
        if (row.type == clg::Attack) for (const auto id : fighter.reach[i]) shared += has(id);
        else shared = has(moves[i].script);
        if (!shared) continue;
        if (row.type != clg::Attack && (moves[i].category & (bcm::Focus | bcm::Ex)) == (bcm::Focus | bcm::Ex)) {
            std::string error;
            return ParseStep("FADC", step, error);
        }
        if (shared > most) { most = shared; best = steps.size(); }
        steps.push_back(typed);
    }
    if (steps.empty()) return false;
    step = steps[best];
    if (detail::FoldSteps(steps)) step = steps[0];
    else guessed = true;
    return true;
}

// The game's trials for one fighter as a pack, one combo per level, named
// "Trial 1" onward, its notes the ultra the level selects. A level with a row
// no notation says is left out and named in issues, so no combo is a
// different route than its trial.
inline Pack ReadTrials(const clg::File& trials, const Fighter& fighter, const std::string& character, const std::string& name,
    std::vector<std::string>& issues) {
    Pack pack{name, {}};
    for (std::size_t i = 0; i < trials.levels.size() && pack.combos.size() < MaxCombos; ++i) {
        const auto& rows = trials.levels[i].steps;
        Combo made{"Trial " + std::to_string(i + 1), character, trials.levels[i].ultra ? UltraNote(1) : "", {}};
        std::size_t failed = 0;
        for (std::size_t j = 0; j < rows.size(); ++j) {
            Step step;
            bool guessed = false;
            if (!RowStep(fighter, rows[j], step, guessed)) { if (!failed) failed = j + 1; continue; }
            made.steps.push_back(Canonical(step));
        }
        if (failed) issues.push_back(made.name + ": step " + std::to_string(failed) + " is a move the notation cannot say");
        else if (!made.steps.empty() && made.steps.size() <= MaxSteps) pack.combos.push_back(made);
    }
    return pack;
}

// Reads a whole file of at most `most` bytes.
inline bool ReadFile(const std::filesystem::path& path, std::size_t most, std::vector<std::uint8_t>& bytes, std::string& error) {
    const auto name = path.filename().u8string();
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = in ? static_cast<std::streamoff>(in.tellg()) : -1;
    if (size < 0) { error = "cannot open " + std::string(name.begin(), name.end()); return false; }
    if (static_cast<std::uint64_t>(size) > most) { error = std::string(name.begin(), name.end()) + " is too large"; return false; }
    std::vector<std::uint8_t> read(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size && !in.read(reinterpret_cast<char*>(read.data()), size)) { error = "cannot read " + std::string(name.begin(), name.end()); return false; }
    bytes = std::move(read);
    return true;
}
// A fighter code names folders and files, so it is three capitals or digits.
inline bool IsFighterCode(const std::string& code) {
    return code.size() == 3 && std::all_of(code.begin(), code.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
}
// The folder a fighter's .bcm and .bac are read from. The install is layered
// (SF4, SSF4, AE and USF4 as resource, dlc and patch folders) and a fighter's
// files sit in several layers. From the program: USF4 (edition 14) switches
// per fighter, the 1.11 balance for `later` and 1.10 for the rest, and that
// choice is tried first. The rest is a fallback for an install that lacks it:
// the other layers newest first, down to the SF4 resource folder. Whether the
// game loads the file found is not verified; the runtime's "trial steps that
// never matched" log is the in-game check.
inline std::filesystem::path CommandFolder(const std::filesystem::path& game, const std::string& code) {
    static const char* const later[] = {"RYU", "GUL", "BLR", "VEG", "JHA", "CMY", "DAN", "GUY", "DDL", "RLN", "PSN", "DCP"};
    static const char* const layers[] = {"patch_ae2_tu3/battle/regulation/ae2_111", "patch_ae2_tu2/battle/regulation/ae2_110",
        "patch_ae2_tu1/battle/regulation/ae2_109", "dlc/04_ae2/battle/regulation/ae2", "dlc/03_character_free/battle/regulation/latest",
        "patch/battle/regulation/latest_ae", "resource/battle/chara"};
    const bool newest = std::any_of(std::begin(later), std::end(later), [&](const char* one) { return code == one; });
    std::filesystem::path found;
    std::error_code ignored;
    for (std::size_t i = newest ? 0 : 1; i < sizeof layers / sizeof *layers; ++i) {
        found = game / layers[i] / code;
        if (std::filesystem::exists(found / (code + ".bcm"), ignored)) break;
    }
    return found;
}
// game: the folder of SSFIV.exe. Reads the fighter's command and script files.
inline bool LoadFighter(const std::filesystem::path& game, const std::string& code, Fighter& fighter, std::string& error) {
    if (!IsFighterCode(code)) { error = "\"" + Clean(code) + "\" is not a fighter"; return false; }
    const auto folder = CommandFolder(game, code);
    std::vector<std::uint8_t> bytes;
    bcm::File moves;
    bac::File scripts;
    if (!ReadFile(folder / (code + ".bcm"), bcm::MaxBytes, bytes, error) || !bcm::Read(bytes.data(), bytes.size(), moves, error) ||
        !ReadFile(folder / (code + ".bac"), bac::MaxBytes, bytes, error) || !bac::Read(bytes.data(), bytes.size(), scripts, error)) return false;
    fighter = MakeFighter(std::move(moves), std::move(scripts));
    return true;
}
// The name of the Ultra trial file, the one whose ids belong to the files
// LoadFighter reads. The game's first trials (<CODE>.clg) use older ids.
inline std::string TrialFileName(const std::string& code) { return code + "_swan.clg"; }
// Reads the fighter's Ultra trials from the newest patch that has the file;
// whether that is the one the game loads is not verified. from: the file
// that was read, which a new trial file is laid out like.
inline bool LoadTrials(const std::filesystem::path& game, const std::string& code, clg::File& trials, std::string& error,
    std::filesystem::path* from = nullptr) {
    if (!IsFighterCode(code)) { error = "\"" + Clean(code) + "\" is not a fighter"; return false; }
    std::vector<std::uint8_t> bytes;
    for (const char* patch : {"patch_ae2_tu3", "patch_ae2_tu2"}) {
        const auto path = game / patch / "battle" / "chara" / code / TrialFileName(code);
        if (!ReadFile(path, clg::MaxBytes, bytes, error)) continue;
        if (from) *from = path;
        return clg::Read(bytes.data(), bytes.size(), trials, error);
    }
    return false;
}
// Writes the trial file as folder/<CODE>_swan.clg, making the folder.
inline bool SaveTrials(const std::filesystem::path& folder, const std::string& code, const clg::File& trials, std::string& error) {
    std::vector<unsigned char> bytes;
    if (!IsFighterCode(code)) { error = "\"" + Clean(code) + "\" is not a fighter"; return false; }
    if (!clg::Write(trials, bytes, error)) return false;
    std::error_code failed;
    std::filesystem::create_directories(folder, failed);
    std::ofstream out(folder / TrialFileName(code), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) { error = "cannot write " + TrialFileName(code); return false; }
    return true;
}
} }
