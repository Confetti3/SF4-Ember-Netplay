#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// Hand-written combos for the custom trials: typed as notation, never
// performed, so anyone can author one. Owns only the data, its share format
// and the route tree; it knows nothing of the game or the overlay.
namespace sf4e { namespace combo {
constexpr int FormatVersion = 1;
constexpr const char* FormatName = "sf4e-combos";
// Imported text comes from other players, so every size is bounded.
constexpr std::size_t MaxBytes = 256 * 1024, MaxPacks = 64, MaxCombos = 256, MaxSteps = 64, MaxText = 96, MaxNotes = 512;

// character: the fighter catalog code, upper case. steps: one move each, in
// order, in the canonical notation ParseStep accepts ("2MK", "xx 236HP").
// How the dummy and the gauges are set for a combo, as the game's Training
// menu numbers them; -1 leaves the game's own setting. action 0 stand, 1
// crouch, 2 jump, 3 cpu; guard 0 none, 1 after the first hit, 2 all, 3
// random; counterHit 0 off, 1 on, 2 random; quickStand 0 quick, 1 normal,
// 2 delayed, 3 random; super and revenge 0 normal, 5 max, 7 infinite, 8 refill.
struct Setup { int action = -1, guard = -1, counterHit = -1, quickStand = -1, super = -1, revenge = -1; };
inline bool operator==(const Setup& a, const Setup& b) {
    return a.action == b.action && a.guard == b.guard && a.counterHit == b.counterHit && a.quickStand == b.quickStand && a.super == b.super && a.revenge == b.revenge;
}
// place: where the fighters stand when the combo starts, Player 1's and
// Player 2's x; placed is false when the combo has none.
struct Combo { std::string name, character, notes; std::vector<std::string> steps; Setup setup; bool placed = false; std::array<float, 2> place{}; };
constexpr float MaxPlace = 10000;
struct Pack { std::string name; std::vector<Combo> combos; };
// One move in a character's routes. combos: "pack / combo" for each combo that ends here.
struct Node { std::string step; std::vector<std::string> combos; std::vector<Node> children; };
using Tree = std::map<std::string, Node>;

// Trims, and folds every run of whitespace or control characters to one space.
inline std::string Clean(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (c > ' ' && c != 0x7f) out += static_cast<char>(c);
        else if (!out.empty() && out.back() != ' ') out += ' ';
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
inline std::string Fold(std::string text, int (*fold)(int)) {
    for (auto& c : text) c = static_cast<char>(fold(static_cast<unsigned char>(c)));
    return text;
}

// The game's own button bits, as the training session records them.
constexpr unsigned LP = 0x10, MP = 0x20, HP = 0x400, LK = 0x40, MK = 0x80, HK = 0x800;
constexpr unsigned Punches = LP | MP | HP, Kicks = LK | MK | HK;
enum class Edge { Press, Hold, Release };
enum class Range { Any, Close, Far };
// One move as inputs a trial can check.
// motion: numpad directions relative to the facing side ("236", "2", "66"),
//   or "360"/"720"; with charge the first digit is held ("[4]6" -> "46").
// buttons/need: `need` of the buttons in the mask together, so "PP" is any
//   two punches and "LP+LK" is both. No buttons is a pure motion (a dash).
// edge: the buttons are pressed, held down, or let go (negative edge).
// range: a standing normal that must come out as its close or far version.
// cancel: this move cancels the one before it; otherwise it links after it.
// follow: "~", a follow-up of the move before it: pressed a few frames in, no cue.
// mash: the buttons are pressed again and again (Hundred Hand Slap).
// offset: frames after this move's cue the press lands on, before it when negative ("cr.MK@+1").
// at: the frame of the replay the press lands on, -1 when the move follows
// the fight's cues instead ("cr.MK#45", as a pattern editor lays it out).
struct Step {
    std::string motion;
    bool charge = false, air = false, cancel = false, mash = false;
    // follow: a follow-up pressed a few frames into the move before it, with
    // no cue to wait for (a run's stop); it counts as a cancel otherwise.
    bool follow = false;
    unsigned buttons = 0; int need = 0;
    Edge edge = Edge::Press; Range range = Range::Any;
    int offset = 0, at = -1;
};
constexpr int MaxAtFrame = 3600;

// Strict notation, any case:
//   [xx|~] [j.|cr.|st.|cl.|far.] [motion] [buttons] [(mash)] [#N] [@N]   or   [xx] FADC[66|44] [#N] [@N]
//   motion:  numpad digits 1-9, "[4]6" for charge, "360", "720"
//   buttons: LP MP HP LK MK HK joined by "+", or P PP PPP K KK KKK;
//            "[HP]" holds them, "]HP[" releases them, "(mash)" mashes them
//   @N:      replay timing, -30..+30 frames ("@-1", "@+3")
//   #N:      replay frame the press lands on, 0..3600 ("#45")
// cr. is 2; st., cl. and far. are 5. FADC is MP+MK then a dash, 66 unless 44.
inline bool ParseStep(const std::string& text, Step& step, std::string& error) {
    step = Step{};
    std::string s;
    for (char c : Fold(text, std::toupper)) if (static_cast<unsigned char>(c) > ' ') s += c;
    const auto fail = [&] { error = "\"" + Clean(text) + "\" is not a move"; return false; };
    std::size_t i = 0;
    const auto starts = [&](const char* prefix) {
        const auto length = std::strlen(prefix);
        if (s.compare(i, length, prefix) != 0) return false;
        i += length; return true;
    };
    // "@N" and "#N" ride at the end, in either order, each at most once.
    for (bool seenAt = false, seenFrame = false;;) {
        const auto at = s.rfind('@'), frame = s.rfind('#');
        if (at == std::string::npos && frame == std::string::npos) break;
        const bool offset = frame == std::string::npos || (at != std::string::npos && at > frame);
        const auto mark = offset ? at : frame;
        const auto digits = s.substr(mark + 1);
        if (offset) {
            const bool signedNumber = digits.size() >= 2 && digits.size() <= 3 && (digits[0] == '-' || digits[0] == '+') && digits.find_first_not_of("0123456789", 1) == std::string::npos;
            if (seenAt || !signedNumber) return fail();
            step.offset = std::atoi(digits.c_str());
            if (step.offset < -30 || step.offset > 30) return fail();
            seenAt = true;
        } else {
            if (seenFrame || digits.empty() || digits.size() > 4 || digits.find_first_not_of("0123456789") != std::string::npos) return fail();
            step.at = std::atoi(digits.c_str());
            if (step.at > MaxAtFrame) return fail();
            seenFrame = true;
        }
        s.resize(mark);
    }
    if (s.size() > 6 && s.compare(s.size() - 6, 6, "(MASH)") == 0) { step.mash = true; s.resize(s.size() - 6); }
    step.follow = starts("~");
    step.cancel = step.follow || starts("XX");
    if (starts("FADC")) {
        const auto dash = s.substr(i);
        if (step.mash || (!dash.empty() && dash != "66" && dash != "44")) return fail();
        step.motion = dash.empty() ? "66" : dash; step.buttons = MP | MK; step.need = 2;
        return true;
    }
    bool stance = false;
    if (starts("J.")) step.air = true;
    else if (starts("CR.")) { step.motion = "2"; stance = true; }
    else if (starts("ST.")) { step.motion = "5"; stance = true; }
    else if (starts("CL.")) { step.motion = "5"; step.range = Range::Close; stance = true; }
    else if (starts("FAR.")) { step.motion = "5"; step.range = Range::Far; stance = true; }
    if (!stance) {
        if (starts("360")) step.motion = "360";
        else if (starts("720")) step.motion = "720";
        else {
            if (i + 2 < s.size() && s[i] == '[' && s[i + 1] >= '1' && s[i + 1] <= '9' && s[i + 2] == ']') {
                step.charge = true; step.motion = s[i + 1]; i += 3;
            }
            while (i < s.size() && s[i] >= '1' && s[i] <= '9') step.motion += s[i++];
        }
        if (step.motion.size() > 10 || (step.charge && step.motion.size() < 2)) return fail();
    }
    auto rest = s.substr(i);
    if (rest.size() > 2 && rest.front() == '[' && rest.back() == ']') step.edge = Edge::Hold;
    else if (rest.size() > 2 && rest.front() == ']' && rest.back() == '[') step.edge = Edge::Release;
    if (step.edge != Edge::Press) rest = rest.substr(1, rest.size() - 2);
    if (!rest.empty() && rest.find_first_not_of('P') == std::string::npos && rest.size() <= 3) {
        step.buttons = Punches; step.need = static_cast<int>(rest.size());
    } else if (!rest.empty() && rest.find_first_not_of('K') == std::string::npos && rest.size() <= 3) {
        step.buttons = Kicks; step.need = static_cast<int>(rest.size());
    } else for (std::size_t at = 0; at < rest.size(); at += 3) {
        // Named buttons are two letters each, joined by "+".
        static const char* const names[] = {"LP", "MP", "HP", "LK", "MK", "HK"};
        static const unsigned masks[] = {LP, MP, HP, LK, MK, HK};
        int found = -1;
        for (int n = 0; n < 6; ++n) if (rest.compare(at, 2, names[n]) == 0) found = n;
        if (found < 0 || (step.buttons & masks[found]) || (at + 2 < rest.size() && rest[at + 2] != '+') || at + 3 == rest.size()) return fail();
        step.buttons |= masks[found]; ++step.need;
    }
    // A stance names a normal, so it needs its button; "j." alone is no input.
    if ((stance || step.motion.empty() || step.mash) && !step.buttons) return fail();
    if (step.motion.empty() && !step.air) step.motion = "5";
    return true;
}
// The one spelling of a step: what is stored, shared and compared.
inline std::string Canonical(const Step& step) {
    std::string out = step.follow ? "~ " : step.cancel ? "xx " : "";
    const std::string timing = (step.at >= 0 ? "#" + std::to_string(step.at) : "") +
        (step.offset ? (step.offset > 0 ? "@+" : "@") + std::to_string(step.offset) : "");
    if (!step.air && !step.charge && (step.motion == "66" || step.motion == "44") && step.buttons == (MP | MK) &&
        step.need == 2 && step.edge == Edge::Press && !step.mash) return out + (step.motion == "66" ? "FADC" : "FADC44") + timing;
    if (step.air) out += "j.";
    if (step.range != Range::Any) out += step.range == Range::Close ? "cl." : "far.";
    else out += step.charge ? "[" + step.motion.substr(0, 1) + "]" + step.motion.substr(1) : step.motion;
    std::string buttons;
    if (step.buttons == Punches || step.buttons == Kicks) buttons.assign(static_cast<std::size_t>(step.need), step.buttons == Punches ? 'P' : 'K');
    else {
        static const char* const names[] = {"LP", "MP", "HP", "LK", "MK", "HK"};
        static const unsigned masks[] = {LP, MP, HP, LK, MK, HK};
        for (int n = 0; n < 6; ++n) if (step.buttons & masks[n]) { buttons += buttons.empty() ? "" : "+"; buttons += names[n]; }
    }
    if (step.edge == Edge::Hold) buttons = "[" + buttons + "]";
    if (step.edge == Edge::Release) buttons = "]" + buttons + "[";
    return out + buttons + (step.mash ? "(mash)" : "") + timing;
}

// Splits a line into its moves: by ">" or ",", and before a lone "xx" or "~",
// which stays on the move it makes a cancel or a follow-up ("2MK xx 236P" ->
// "2MK", "xx 236P"; "236P ~ LP" -> "236P", "~ LP").
inline std::vector<std::string> Tokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::string text;
    const auto flush = [&] { if (!Clean(text).empty()) tokens.push_back(Clean(text)); text.clear(); };
    const auto blank = [&](std::size_t at) { return at >= line.size() || static_cast<unsigned char>(line[at]) <= ' ' || line[at] == '>' || line[at] == ','; };
    for (std::size_t i = 0; i < line.size(); ++i) {
        const bool cancel = (line[i] == 'x' || line[i] == 'X') && i + 1 < line.size() && (line[i + 1] == 'x' || line[i + 1] == 'X') &&
            (i == 0 || blank(i - 1)) && blank(i + 2);
        const bool follow = line[i] == '~' && (i == 0 || blank(i - 1)) && blank(i + 1);
        if (cancel || follow || line[i] == '>' || line[i] == ',') {
            flush();
            if (cancel) { text = "xx "; ++i; }
            if (follow) text = "~ ";
        } else text += line[i];
    }
    flush();
    return tokens;
}
// Turns a named move into notation: "HP Hadoken" for RYU -> "236HP", "EX
// Tatsu" -> "214KK", "xx Sonic Boom" for GUL -> "xx [4]6P". A name may stand
// for several moves: "Rekkaken 2" for FLN -> "236P > 236P". Text that names
// no move comes back unchanged, for ParseStep to judge.
inline std::string ResolveName(const std::string& character, const std::string& text) {
    struct Move { const char* character; const char* name; const char* notation; };
    static const Move moves[] = {
#include "ComboMoves.inc"
    };
    // The fighter's own name for a move wins over the shared ones.
    const auto find = [&](const std::string& words) {
        std::string key;
        for (unsigned char c : words) if (std::isalnum(c)) key += static_cast<char>(std::tolower(c));
        const Move* found = nullptr;
        for (const auto& move : moves) if (key == move.name && (character == move.character || (!*move.character && !found))) found = &move;
        return found;
    };
    auto rest = Clean(text);
    std::string cancel, strength;
    if (rest.size() > 3 && Fold(rest.substr(0, 3), std::tolower) == "xx ") { cancel = "xx "; rest = rest.substr(3); }
    // A name that carries its own tag ("K Crazy Buffalo") is taken whole.
    if (const Move* exact = find(rest)) return cancel + exact->notation;
    const auto space = rest.find(' ');
    if (space == std::string::npos) return text;
    const auto word = Fold(rest.substr(0, space), std::toupper);
    for (const char* known : {"L", "M", "H", "LP", "MP", "HP", "LK", "MK", "HK", "EX"}) if (word == known) strength = word;
    const Move* found = strength.empty() ? nullptr : find(rest.substr(space + 1));
    if (!found) return text;
    // The strength goes on the first move, and only when that has one open
    // button: any punch or any kick.
    std::string notation = found->notation, after;
    const auto end = notation.find(' ');
    if (end != std::string::npos) { after = notation.substr(end); notation.resize(end); }
    const auto size = notation.size();
    const char kind = notation.back();
    if ((kind != 'P' && kind != 'K') || (size > 1 && std::isalpha(static_cast<unsigned char>(notation[size - 2])))) return text;
    // "LK Hadoken" names a kick on a punch move: no move, not a jab.
    if (strength.size() == 2 && strength != "EX" && strength[1] != kind) return text;
    notation.pop_back();
    notation += strength == "EX" ? std::string(2, kind) : std::string(1, strength[0]) + kind;
    return cancel + notation + after;
}
namespace detail {
// One typed move, which a name may expand to several steps.
inline bool Append(const std::string& character, const std::string& token, std::vector<std::string>& steps, std::string& error) {
    for (const auto& piece : Tokens(ResolveName(character, token))) {
        Step step;
        if (!ParseStep(piece, step, error)) return false;
        steps.push_back(Canonical(step));
    }
    return true;
}
inline bool StartsWithCancel(const std::vector<std::string>& steps, std::string& error) {
    if (steps.empty() || steps[0].compare(0, 3, "xx ") != 0) return false;
    error = "the first move cannot be a cancel";
    return true;
}
}
// "cr.MK xx Hadoken > FADC, cl.HP" for RYU -> {"2MK", "xx 236P", "FADC", "cl.HP"}.
// Fails on the first step that is not a move.
inline bool ParseSteps(const std::string& line, const std::string& character, std::vector<std::string>& steps, std::string& error) {
    steps.clear();
    for (const auto& token : Tokens(line)) if (!detail::Append(character, token, steps, error)) { steps.clear(); return false; }
    if (detail::StartsWithCancel(steps, error)) { steps.clear(); return false; }
    return true;
}
// A focus press followed within a few frames by a dash, as a recording sees
// a focus cancel, becomes one FADC step on the focus's frame and cancel.
inline void FoldFadc(std::vector<std::string>& steps) {
    for (std::size_t i = 0; i + 1 < steps.size();) {
        Step focus, dash; std::string error;
        const bool pair = ParseStep(steps[i], focus, error) && ParseStep(steps[i + 1], dash, error) &&
            focus.buttons == (MP | MK) && focus.need == 2 && (focus.motion == "5" || focus.motion.empty()) && focus.edge == Edge::Press && !focus.air &&
            dash.buttons == 0 && (dash.motion == "66" || dash.motion == "44") && !dash.air &&
            (focus.at < 0 || dash.at < 0 || (dash.at >= focus.at && dash.at - focus.at <= 12));
        if (!pair) { ++i; continue; }
        Step fadc; fadc.cancel = focus.cancel; fadc.motion = dash.motion; fadc.buttons = MP | MK; fadc.need = 2; fadc.at = focus.at; fadc.offset = focus.offset;
        steps[i] = Canonical(fadc); steps.erase(steps.begin() + i + 1);
    }
}
// "2MK xx 236P > FADC": a line ParseSteps reads back to the same steps.
inline std::string JoinSteps(const std::vector<std::string>& steps) {
    std::string line;
    for (const auto& step : steps) {
        if (!line.empty()) line += step.compare(0, 3, "xx ") == 0 || step.compare(0, 2, "~ ") == 0 ? " " : " > ";
        line += step;
    }
    return line;
}
// Two combos are the same route when this matches.
inline std::string RouteKey(const Combo& combo) { return combo.character + '\n' + JoinSteps(combo.steps); }

inline nlohmann::json ToJson(const Combo& combo) {
    nlohmann::json value = nlohmann::json::object();
    value["name"] = combo.name; value["character"] = combo.character;
    value["steps"] = combo.steps; value["notes"] = combo.notes;
    if (!(combo.setup == Setup{})) {
        value["setup"] = nlohmann::json::object();
        const int fields[] = {combo.setup.action, combo.setup.guard, combo.setup.counterHit, combo.setup.quickStand, combo.setup.super, combo.setup.revenge};
        const char* names[] = {"action", "guard", "counterHit", "quickStand", "super", "revenge"};
        for (int i = 0; i < 6; ++i) if (fields[i] >= 0) value["setup"][names[i]] = fields[i];
    }
    if (combo.placed) value["place"] = {combo.place[0], combo.place[1]};
    return value;
}
inline nlohmann::json ToJson(const Pack& pack) {
    nlohmann::json value = nlohmann::json::object();
    value["name"] = pack.name; value["combos"] = nlohmann::json::array();
    for (const auto& combo : pack.combos) value["combos"].push_back(ToJson(combo));
    return value;
}
inline std::string Stamp(nlohmann::json value) {
    value["format"] = FormatName; value["version"] = FormatVersion;
    return value.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}
// The three shapes that can be shared. Import reads all of them.
inline std::string Export(const Combo& combo) { return Stamp(ToJson(combo)); }
inline std::string Export(const Pack& pack) { return Stamp(ToJson(pack)); }
inline std::string Export(const std::vector<Pack>& packs) {
    nlohmann::json value = nlohmann::json::object();
    value["packs"] = nlohmann::json::array();
    for (const auto& pack : packs) value["packs"].push_back(ToJson(pack));
    return Stamp(value);
}

namespace detail {
inline bool Text(const nlohmann::json& value, const char* key, std::size_t limit, std::string& out, std::string& error) {
    out.clear();
    const auto found = value.find(key);
    if (found == value.end()) return true;
    if (!found->is_string()) { error = std::string(key) + " must be text"; return false; }
    out = Clean(found->get<std::string>());
    if (out.size() > limit) { error = std::string(key) + " is too long"; return false; }
    return true;
}
inline bool Read(const nlohmann::json& value, Combo& combo, std::string& error) {
    if (!value.is_object()) { error = "a combo must be an object"; return false; }
    if (!Text(value, "name", MaxText, combo.name, error) || !Text(value, "character", MaxText, combo.character, error) ||
        !Text(value, "notes", MaxNotes, combo.notes, error)) return false;
    combo.character = Fold(combo.character, std::toupper);
    if (combo.character.empty()) { error = "a combo needs a character"; return false; }
    combo.placed = false; combo.place[0] = combo.place[1] = 0;
    const auto place = value.find("place");
    if (place != value.end()) {
        if (!place->is_array() || place->size() != 2 || !(*place)[0].is_number() || !(*place)[1].is_number()) { error = "place is not two numbers"; return false; }
        for (int i = 0; i < 2; ++i) { combo.place[i] = (*place)[i].get<float>(); if (!(std::abs(combo.place[i]) <= MaxPlace)) { error = "place is out of the stage"; return false; } }
        combo.placed = true;
    }
    combo.setup = Setup{};
    const auto setup = value.find("setup");
    if (setup != value.end()) {
        if (!setup->is_object()) { error = "setup must be an object"; return false; }
        int* fields[] = {&combo.setup.action, &combo.setup.guard, &combo.setup.counterHit, &combo.setup.quickStand, &combo.setup.super, &combo.setup.revenge};
        const char* names[] = {"action", "guard", "counterHit", "quickStand", "super", "revenge"};
        const int highest[] = {3, 3, 2, 3, 8, 8};
        for (int i = 0; i < 6; ++i) {
            const auto field = setup->find(names[i]);
            if (field == setup->end()) continue;
            if (!field->is_number_integer() || field->get<int>() < 0 || field->get<int>() > highest[i]) { error = std::string(names[i]) + " is not a setting"; return false; }
            *fields[i] = field->get<int>();
        }
    }
    combo.steps.clear();
    const auto steps = value.find("steps");
    // Hand-written files may give the steps as one notation line.
    if (steps != value.end() && steps->is_string()) {
        if (!ParseSteps(steps->get<std::string>(), combo.character, combo.steps, error)) return false;
    } else if (steps != value.end() && steps->is_array()) {
        if (steps->size() > MaxSteps) { error = "a combo has too many steps"; return false; }
        for (const auto& item : *steps) {
            if (!item.is_string()) { error = "a step must be text"; return false; }
            if (!Append(combo.character, item.get<std::string>(), combo.steps, error)) return false;
        }
        if (StartsWithCancel(combo.steps, error)) return false;
    } else { error = "a combo needs steps"; return false; }
    if (combo.steps.empty()) { error = "a combo needs steps"; return false; }
    if (combo.steps.size() > MaxSteps) { error = "a combo has too many steps"; return false; }
    return true;
}
inline bool Read(const nlohmann::json& value, Pack& pack, std::string& error) {
    if (!value.is_object()) { error = "a pack must be an object"; return false; }
    if (!Text(value, "name", MaxText, pack.name, error)) return false;
    const auto combos = value.find("combos");
    if (combos == value.end() || !combos->is_array()) { error = "a pack needs combos"; return false; }
    if (combos->size() > MaxCombos) { error = "a pack has too many combos"; return false; }
    pack.combos.assign(combos->size(), Combo{});
    for (std::size_t i = 0; i < combos->size(); ++i) if (!Read((*combos)[i], pack.combos[i], error)) return false;
    return true;
}
}

// Reads a single combo, a pack or a file of packs. A single combo arrives as
// one unnamed pack. On failure packs is left empty and error says why.
inline bool Import(const std::string& text, std::vector<Pack>& packs, std::string& error) {
    packs.clear(); error.clear();
    if (text.size() > MaxBytes) { error = "the text is too large"; return false; }
    const auto value = nlohmann::json::parse(text, nullptr, false);
    if (!value.is_object()) { error = "this is not combo data"; return false; }
    // Both are optional so a file can be written by hand.
    const auto format = value.find("format"), version = value.find("version");
    if (format != value.end() && *format != FormatName) { error = "this is not combo data"; return false; }
    if (version != value.end() && (!version->is_number_integer() || version->get<long long>() > FormatVersion)) {
        error = "this combo data needs a newer Ember"; return false;
    }
    bool ok = false;
    if (value.contains("packs")) {
        const auto& list = value["packs"];
        if (!list.is_array()) error = "packs must be a list";
        else if (list.size() > MaxPacks) error = "too many packs";
        else {
            packs.assign(list.size(), Pack{});
            ok = true;
            for (std::size_t i = 0; ok && i < list.size(); ++i) ok = detail::Read(list[i], packs[i], error);
        }
    } else if (value.contains("combos")) {
        packs.assign(1, Pack{});
        ok = detail::Read(value, packs[0], error);
    } else {
        packs.assign(1, Pack{});
        packs[0].combos.assign(1, Combo{});
        ok = detail::Read(value, packs[0].combos[0], error);
    }
    if (!ok) packs.clear();
    return ok;
}

// Adds imported packs to a book: packs join by name, and a route the pack
// already holds is not added twice. Returns how many combos were added.
inline int Merge(std::vector<Pack>& book, const std::vector<Pack>& incoming) {
    int added = 0;
    for (const auto& pack : incoming) {
        auto target = std::find_if(book.begin(), book.end(), [&](const Pack& p) { return p.name == pack.name; });
        if (target == book.end()) {
            if (book.size() >= MaxPacks) continue;
            book.push_back(Pack{pack.name, {}});
            target = book.end() - 1;
        }
        for (const auto& combo : pack.combos) {
            const auto key = RouteKey(combo);
            if (target->combos.size() >= MaxCombos || std::any_of(target->combos.begin(), target->combos.end(),
                [&](const Combo& c) { return RouteKey(c) == key; })) continue;
            target->combos.push_back(combo);
            ++added;
        }
    }
    return added;
}

// Every recorded route, per character: combos that start the same way share
// those nodes and branch where they differ.
inline Tree BuildTree(const std::vector<Pack>& packs) {
    Tree tree;
    for (const auto& pack : packs) for (const auto& combo : pack.combos) {
        Node* node = &tree[combo.character];
        for (const auto& step : combo.steps) {
            auto next = std::find_if(node->children.begin(), node->children.end(),
                [&](const Node& child) { return child.step == step; });
            if (next == node->children.end()) {
                node->children.push_back(Node{step, {}, {}});
                next = node->children.end() - 1;
            }
            node = &*next;
        }
        const auto label = pack.name.empty() ? combo.name : pack.name + " / " + combo.name;
        if (std::find(node->combos.begin(), node->combos.end(), label) == node->combos.end()) node->combos.push_back(label);
    }
    return tree;
}
// One line per node, two spaces per depth; a node that ends a combo names it.
inline void RenderNode(const Node& node, int depth, std::string& out) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    out += node.step;
    for (std::size_t i = 0; i < node.combos.size(); ++i) out += (i ? ", " : "  [") + node.combos[i];
    if (!node.combos.empty()) out += ']';
    out += '\n';
    for (const auto& child : node.children) RenderNode(child, depth + 1, out);
}
inline std::string RenderTree(const Tree& tree) {
    std::string out;
    for (const auto& entry : tree) {
        out += entry.first + '\n';
        for (const auto& child : entry.second.children) RenderNode(child, 1, out);
    }
    return out;
}
} }
