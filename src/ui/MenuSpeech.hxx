#pragma once
#include "MenuNavigation.hxx"
#include "../common/Localization.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../platform/Speech.hxx"
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
    // The dialog is a notice that opened by itself, not one the player asked
    // for; buttons are all of its buttons.
    bool notice = false;
    std::vector<std::string> buttons;
    // The accessibility button's name on the player's device ("RT", "F2").
    std::string accessKey;
};
// One utterance in parts: a row's name, its value, its state. A voice that
// keeps what it has said (Piper on Linux) keeps each part on its own, so a
// row it has named before is ready at once whatever its value. render lists
// what a screen that just opened may soon say, to prepare while the player
// listens to its title.
struct Utterance {
    std::vector<std::string> parts, render;
    bool interrupt = true;
    // The accessibility button pressed again on what it is reading: be quiet.
    bool stop = false;
    // A notice opened by itself: its chime comes first.
    bool chime = false;
    std::string Text() const { return speech::Line(parts); }
};
// The voice speech uses, as the Accessibility page shows it.
inline std::string VoiceText(speech::VoiceStatus status) {
    switch (status.voice) {
    case speech::Voice::Downloading: return loc::Tf("speech.voice_downloading", status.percent);
    case speech::Voice::Natural: return loc::T("speech.voice_natural");
    case speech::Voice::Failed: return loc::T("speech.voice_download_failed");
    default: return loc::T("speech.voice_system");
    }
}
// Turns what a menu shows into speech, the way Tokon reads its menus: a new
// screen says its title, a move says the row's name, and its value only where
// the value is the point (a setting the player changes or chooses). Moving on
// interrupts. A dialog the player opens says its question, then its button; a
// notice that opens by itself chimes and says only that the accessibility
// button reads it, which it does, text and buttons. The
// row's description (a notice's text) is read when the player presses the
// accessibility button (describe), and a second press on the same row stops
// it; or DetailDelay seconds after a stop when the player wants that. Its
// button hint and place in the list are read only if the player wants them.
class MenuNarrator {
public:
    static constexpr double DetailDelay = 1.5;
    Utterance Update(const SpokenFrame& frame, double now, const netplay::SpeechPreferences& speech, bool describe = false) {
        const std::string id = frame.focus ? frame.focus->id : std::string();
        const std::string value = frame.focus && Changes(*frame.focus) ? frame.focus->value : std::string();
        const bool titled = frame.title != title_, dialog = !frame.button.empty();
        Utterance said;
        if (dialog && frame.notice && button_.empty()) {
            // A notice that opened by itself chimes and says how to hear it.
            said.chime = true;
            said.parts.push_back(loc::Tf("speech.notice", frame.accessKey));
        } else if (dialog && (titled || frame.button != button_)) {
            // A dialog that just opened says its question first.
            if (button_.empty() || frame.question != question_) Add(said.parts, frame.question);
            Add(said.parts, frame.button);
        } else if (!dialog && (titled || id != id_ || !button_.empty())) {
            if (titled) {
                Add(said.parts, frame.title);
                if (frame.entries) for (const auto& entry : *frame.entries) { Add(said.render, entry.label); if (Changes(entry)) Add(said.render, entry.value); }
            }
            if (frame.focus) {
                Add(said.parts, frame.focus->label);
                Add(said.parts, value);
                if (speech.position) said.parts.push_back(loc::Tf("speech.position", frame.index + 1, frame.count));
            }
        } else if (!dialog && value != value_) {
            Add(said.parts, value);
        }
        if (titled || id != id_ || frame.button != button_) { since_ = now; rest_ = true; described_ = false; }
        title_ = frame.title; id_ = id; value_ = value; question_ = frame.question; button_ = frame.button;
        const bool waited = speech.details && said.parts.empty() && rest_ && now - since_ >= DetailDelay;
        if (describe && described_) {
            described_ = false;
            said = {};
            said.stop = true;
        } else if (describe || waited) {
            rest_ = false;
            described_ = describe;
            said.interrupt = describe;
            Add(said.parts, dialog ? frame.question : frame.focus ? frame.focus->detail : std::string());
            // A notice is read whole: its text, then its buttons.
            if (dialog && frame.notice) for (const auto& button : frame.buttons) Add(said.parts, button);
            if (speech.hints) Add(said.parts, frame.hint);
        }
        return said;
    }
private:
    // A setting the player changes in place or picks from a list.
    static bool Changes(const MenuEntry& entry) { return entry.adjustable || !entry.choices.empty(); }
    static void Add(std::vector<std::string>& parts, const std::string& part) {
        if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end()) parts.push_back(part);
    }
    std::string title_, id_, value_, question_, button_;
    double since_ = 0;
    bool rest_ = false, described_ = false;
};
} }
