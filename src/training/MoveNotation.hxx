#pragma once
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Moves typed in numpad notation ("2MK", "xx 236HP", "[4]6P"): one move as
// the inputs it stands for, and a typed line as its moves. Owns only the
// text and its meaning; it knows nothing of the game or the overlay.
// The dummy's typed reply is read with it (PracticeSettings.hxx).
namespace sf4e { namespace combo {
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
inline const char* ButtonName(unsigned bit) { return bit == LP ? "LP" : bit == MP ? "MP" : bit == HP ? "HP" : bit == LK ? "LK" : bit == MK ? "MK" : bit == HK ? "HK" : ""; }
enum class Edge { Press, Hold, Release };
enum class Range { Any, Close, Far };
// One move as the inputs it stands for.
// motion: numpad directions relative to the facing side ("236", "2", "66"),
//   or "360"/"720"; with charge the first digit is held ("[4]6" -> "46").
// buttons/need: `need` of the buttons in the mask together, so "PP" is any
//   two punches and "LP+LK" is both. No buttons is a pure motion (a dash).
// edge: the buttons are pressed, held down, or let go (negative edge).
// range: a standing normal that must come out as its close or far version.
// cancel: this move cancels the one before it; otherwise it links after it.
// follow: "~", a follow-up of the move before it: pressed a few frames in, no cue.
// mash: five presses; P, K or several buttons are cycled one per frame, one
// button goes on and off (Hundred Hand Slap, lightning legs).
// offset: frames after this move's cue the press lands on, before it when negative ("cr.MK@+1").
// at: the frame of the playback the press lands on, -1 when the move follows
// the fight's cues instead ("cr.MK#45").
struct Step {
    std::string motion;
    bool charge = false, air = false, cancel = false, mash = false;
    // follow: a follow-up pressed a few frames into the move before it, with
    // no cue to wait for (a run's stop); it counts as a cancel otherwise.
    bool follow = false;
    // mashOrder: the presses of a mash in order, one per frame, each a
    // button with its own direction digit ('0' for the step's); empty for
    // the default five presses cycling the step's buttons.
    struct MashPress { char direction = '0'; unsigned button = 0; };
    std::vector<MashPress> mashOrder;
    unsigned buttons = 0; int need = 0;
    Edge edge = Edge::Press; Range range = Range::Any;
    int offset = 0, at = -1;
};
constexpr int MaxAtFrame = 7200;

// Strict notation, any case:
//   [xx|~] [j.|cr.|st.|cl.|far.] [motion] [buttons] [(mash)] [#N] [@N]   or   [xx] FADC[66|44] [#N] [@N]   or   [xx] RFADC[66|44] [#N] [@N]
//   motion:  numpad digits 1-9, "[4]6" for charge, "360", "720"
//   buttons: LP MP HP LK MK HK joined by "+", or P PP PPP K KK KKK;
//            "[HP]" holds them, "]HP[" releases them, "(mash)" mashes them,
//            "(mash HP-MP-LP)" in that order, one press per frame, up to 10;
//            "(mash 1MK-1MK-1MK)" with a direction for each press
//   @N:      playback timing, -120..+120 frames ("@-1", "@+3")
//   #N:      playback frame the press lands on, 0..7200 ("#45")
// cr. is 2; st., cl. and far. are 5. FADC is MP+MK then a dash, 66 unless 44;
// RFADC is the red focus, LP+MP+MK tapped so the attack comes out and lands, then the dash on its hit.
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
            const bool signedNumber = digits.size() >= 2 && digits.size() <= 4 && (digits[0] == '-' || digits[0] == '+') && digits.find_first_not_of("0123456789", 1) == std::string::npos;
            if (seenAt || !signedNumber) return fail();
            step.offset = std::atoi(digits.c_str());
            if (step.offset < -120 || step.offset > 120) return fail();
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
    else if (s.rfind("(MASH") != std::string::npos && s.back() == ')') {
        // "(mash HP-MP-LP)" or "(mash 1MK-1MK-1MK)": the presses in order,
        // each a button with its own direction; the dashes are optional.
        const auto open = s.rfind("(MASH");
        std::string list;
        for (char c : s.substr(open + 5, s.size() - open - 6)) if (c != '-') list += c;
        if (list.empty()) return fail();
        for (std::size_t at = 0; at < list.size();) {
            Step::MashPress press;
            if (list[at] >= '1' && list[at] <= '9') press.direction = list[at++];
            const auto name = list.substr(at, 2);
            press.button = name == "LP" ? LP : name == "MP" ? MP : name == "HP" ? HP : name == "LK" ? LK : name == "MK" ? MK : name == "HK" ? HK : 0;
            if (!press.button || step.mashOrder.size() >= 10) return fail();
            step.mashOrder.push_back(press); at += 2;
        }
        step.mash = true; s.resize(open);
    }
    step.follow = starts("~");
    step.cancel = step.follow || starts("XX");
    const bool red = starts("RFADC");
    if (red || starts("FADC")) {
        const auto dash = s.substr(i);
        if (step.mash || (!dash.empty() && dash != "66" && dash != "44")) return fail();
        step.motion = dash.empty() ? "66" : dash; step.buttons = red ? LP | MP | MK : MP | MK; step.need = red ? 3 : 2;
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
    const bool focus = step.buttons == (MP | MK) && step.need == 2, red = step.buttons == (LP | MP | MK) && step.need == 3;
    if (!step.air && !step.charge && (step.motion == "66" || step.motion == "44") && (focus || red) && step.edge == Edge::Press && !step.mash)
        return out + (red ? "RFADC" : "FADC") + (step.motion == "66" ? "" : "44") + timing;
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
    std::string mash;
    if (step.mash) {
        mash = "(mash";
        for (std::size_t i = 0; i < step.mashOrder.size(); ++i)
            mash += (i ? "-" : " ") + (step.mashOrder[i].direction != '0' ? std::string(1, step.mashOrder[i].direction) : std::string()) + ButtonName(step.mashOrder[i].button);
        mash += ")";
    }
    return out + buttons + mash + timing;
}

// Splits a line into its moves: by ">" or ",", and before a lone "xx" or "~",
// which stays on the move it makes a cancel or a follow-up ("2MK xx 236P" ->
// "2MK", "xx 236P"; "236P ~ LP" -> "236P", "~ LP").
inline std::vector<std::string> Tokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::string text;
    const auto flush = [&] { if (!Clean(text).empty()) tokens.push_back(Clean(text)); text.clear(); };
    // A new line separates moves as ">" does, so a line may be typed a move a line.
    const auto separates = [](char c) { return c == '>' || c == ',' || c == '\n' || c == '\r'; };
    const auto blank = [&](std::size_t at) { return at >= line.size() || static_cast<unsigned char>(line[at]) <= ' ' || line[at] == '>' || line[at] == ','; };
    for (std::size_t i = 0; i < line.size(); ++i) {
        const bool cancel = (line[i] == 'x' || line[i] == 'X') && i + 1 < line.size() && (line[i + 1] == 'x' || line[i + 1] == 'X') &&
            (i == 0 || blank(i - 1)) && blank(i + 2);
        const bool follow = line[i] == '~' && (i == 0 || blank(i - 1)) && blank(i + 1);
        if (cancel || follow || separates(line[i])) {
            flush();
            if (cancel) { text = "xx "; ++i; }
            if (follow) text = "~ ";
        } else text += line[i];
    }
    flush();
    return tokens;
}
// "cr.MK xx 236P > FADC, cl.HP" -> {"2MK", "xx 236P", "FADC", "cl.HP"}.
// Fails on the first step that is not a move.
inline bool ParseSteps(const std::string& line, std::vector<std::string>& steps, std::string& error) {
    steps.clear();
    for (const auto& token : Tokens(line)) {
        Step step;
        if (!ParseStep(token, step, error)) { steps.clear(); return false; }
        steps.push_back(Canonical(step));
    }
    if (!steps.empty() && steps[0].compare(0, 3, "xx ") == 0) { error = "the first move cannot be a cancel"; steps.clear(); return false; }
    return true;
}
} }
