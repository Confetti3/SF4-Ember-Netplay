#pragma once
#include "MenuNavigation.hxx"
#include "../common/Localization.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include <string>

namespace sf4e { namespace ui {
// What a menu shows this frame, in the words speech reads. A dialog (a
// choice, a confirmation, a notice) is its question and its highlighted
// button; with none open both are empty.
struct SpokenFrame {
    std::string title;
    const MenuEntry* focus = nullptr;
    std::size_t index = 0, count = 0;
    std::string question, button;
    // What the focused row's buttons do, in the player's own button names.
    std::string hint;
};
struct Utterance { std::string text; bool interrupt = true; };
// Turns what a menu shows into speech, one utterance at a time: the screen's
// title and how many rows it has when it changes, then the focused row's name
// and value, as Tokon reads its menus; its place in the list only if the
// player wants it. Moving on interrupts. A row the player stays on for
// DetailDelay seconds after moving to it has its detail and button hint read
// once, if the player wants them; the pause keeps them clear of the name.
class MenuNarrator {
public:
    static constexpr double DetailDelay = 1.5;
    Utterance Update(const SpokenFrame& frame, double now, const netplay::SpeechPreferences& speech) {
        const std::string id = frame.focus ? frame.focus->id : std::string();
        const std::string value = frame.focus ? frame.focus->value : std::string();
        const bool titled = frame.title != title_, dialog = !frame.button.empty();
        Utterance said;
        if (dialog && (titled || frame.button != button_)) {
            // A dialog that just opened says its question first.
            said.text = button_.empty() || frame.question != question_ ? Join(frame.question, frame.button, ". ") : frame.button;
        } else if (!dialog && (titled || id != id_ || !button_.empty())) {
            const std::string screen = titled && frame.count ? Join(frame.title, loc::Tf("speech.items", frame.count), ", ") : titled ? frame.title : std::string();
            said.text = Join(screen, Row(frame, speech.position), ". ");
        } else if (!dialog && value != value_) {
            said.text = value;
        }
        if (titled || id != id_ || frame.button != button_) { since_ = now; rest_ = true; }
        title_ = frame.title; id_ = id; value_ = value; question_ = frame.question; button_ = frame.button;
        if (said.text.empty() && rest_ && now - since_ >= DetailDelay) {
            rest_ = false;
            const std::string detail = speech.details && frame.focus && !dialog ? frame.focus->detail : std::string();
            said = {Join(detail, speech.hints ? frame.hint : std::string(), ". "), false};
        }
        return said;
    }
private:
    // A sentence that ends in its own full stop is not given a second one.
    static std::string Join(const std::string& a, const std::string& b, const char* between) {
        if (a.empty() || b.empty()) return a + b;
        return a + (between[0] == '.' && a.back() == '.' ? " " : between) + b;
    }
    // "Lower input lag, On", with ", 3 of 12" when position is wanted; an
    // empty screen says only its title.
    static std::string Row(const SpokenFrame& frame, bool position) {
        if (!frame.focus) return {};
        std::string text = Join(frame.focus->label, frame.focus->value, ", ");
        if (!frame.focus->enabled) text = Join(text, loc::T("common.unavailable"), ", ");
        return position ? Join(text, loc::Tf("speech.position", frame.index + 1, frame.count), ", ") : text;
    }
    std::string title_, id_, value_, question_, button_;
    double since_ = 0;
    bool rest_ = false;
};
} }
