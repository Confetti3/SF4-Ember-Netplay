#pragma once
#include "MenuNavigation.hxx"
#include "../common/Localization.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include <algorithm>
#include <string>
#include <vector>

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
    // Every row on the screen, for speech to prepare when the screen opens.
    const std::vector<MenuEntry>* entries = nullptr;
};
// One utterance in parts: a row's name, its value, its state. A voice that
// keeps what it has said (Piper on Linux) keeps each part on its own, so a
// row it has named before is ready at once whatever its value. render lists
// what a screen that just opened may soon say, to prepare while the player
// listens to its title.
struct Utterance {
    std::vector<std::string> parts, render;
    bool interrupt = true;
    // The parts as one line, for a voice that says it whole. A part that ends
    // its own sentence is not given a comma.
    std::string Text() const {
        std::string text;
        for (const auto& part : parts) text += (text.empty() ? "" : text.back() == '.' ? " " : ", ") + part;
        return text;
    }
};
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
            if (button_.empty() || frame.question != question_) Add(said.parts, frame.question);
            Add(said.parts, frame.button);
        } else if (!dialog && (titled || id != id_ || !button_.empty())) {
            if (titled) {
                Add(said.parts, frame.title);
                if (frame.count) said.parts.push_back(loc::Tf("speech.items", frame.count));
                if (frame.entries) for (const auto& entry : *frame.entries) { Add(said.render, entry.label); Add(said.render, entry.value); }
            }
            if (frame.focus) {
                Add(said.parts, frame.focus->label);
                Add(said.parts, frame.focus->value);
                if (!frame.focus->enabled) said.parts.push_back(loc::T("common.unavailable"));
                if (speech.position) said.parts.push_back(loc::Tf("speech.position", frame.index + 1, frame.count));
            }
        } else if (!dialog && value != value_) {
            Add(said.parts, value);
        }
        if (titled || id != id_ || frame.button != button_) { since_ = now; rest_ = true; }
        title_ = frame.title; id_ = id; value_ = value; question_ = frame.question; button_ = frame.button;
        if (said.parts.empty() && rest_ && now - since_ >= DetailDelay) {
            rest_ = false;
            said.interrupt = false;
            if (speech.details && frame.focus && !dialog) Add(said.parts, frame.focus->detail);
            if (speech.hints) Add(said.parts, frame.hint);
        }
        return said;
    }
private:
    static void Add(std::vector<std::string>& parts, const std::string& part) {
        if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end()) parts.push_back(part);
    }
    std::string title_, id_, value_, question_, button_;
    double since_ = 0;
    bool rest_ = false;
};
} }
