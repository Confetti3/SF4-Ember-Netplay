// The first-run welcome (FirstRun.dc.html): shown once on a fresh install
// (PlayerPreferences::welcomed), speech first, then language and controller.
// Every step is rows, so the pad, the keys and the mouse all answer it,
// and Skip leaves it at any step.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>

namespace sf4e { namespace ui {
namespace {
constexpr int WelcomeSteps = 3;
// Each step's question and the words under it.
const char* const Titles[WelcomeSteps] = {"welcome.speech_title", "welcome.language_title", "welcome.controller_title"};
const char* const Texts[WelcomeSteps] = {"welcome.speech_text", "welcome.language_text", "welcome.controller_text"};
// Where each step's focus starts.
const char* const FirstRows[WelcomeSteps] = {"welcome-speech_on", "language", "player"};
// One of a step's answers, marked Chosen while it is the one in effect.
MenuEntry Answer(const char* id, const char* label, const char* detail, bool chosen) {
    auto row = Row(id, loc::T(label), loc::T(detail));
    if (chosen) row.value = loc::T("welcome.chosen");
    return row;
}
}
std::vector<MenuEntry> ApplicationShell::WelcomeRows(const ShellView& v, std::string& title) {
    // The brand is drawn over the board, so the title is the step's question, the first thing speech says.
    title = loc::T(Titles[welcomeStep_]);
    std::vector<MenuEntry> rows;
    switch (welcomeStep_) {
    case 0: rows = {Answer("welcome-speech_on", "welcome.speech_on", "welcome.speech_on_detail", preferences_.speech.enabled),
        Answer("welcome-speech_off", "welcome.speech_off", "welcome.speech_off_detail", !preferences_.speech.enabled)}; break;
    case 1: rows = {LanguageRow()}; break;
    default: rows = {Row("player", loc::T("home.choose_controller"), v.controllerReady ? loc::Tf("welcome.controller_ready", v.controller) : std::string(loc::T("home.choose_controller_detail")))};
    }
    rows.push_back(Row("welcome-next", loc::T(welcomeStep_ + 1 < WelcomeSteps ? "welcome.next" : "welcome.start"), loc::T("welcome.next_detail")));
    rows.push_back(Row("welcome-skip", loc::T("welcome.skip"), loc::T("welcome.skip_detail")));
    // Side by side on the board: Skip, then Next.
    rows[rows.size() - 2].left = "welcome-skip";
    rows.back().right = "welcome-next";
    return rows;
}
bool ApplicationShell::WelcomeActivate(const MenuAction& a) {
    auto& nav = menu_.navigation;
    if (a.kind == MenuAction::Back) { welcomeStep_ = (std::max)(0, welcomeStep_ - 1); nav.Prefer(FirstRows[welcomeStep_]); return true; }
    if (a.id.compare(0, 8, "welcome-") != 0) return false;
    // Speech is the player's setting from their answer on, saved like any other.
    if (a.id == "welcome-speech_on" || a.id == "welcome-speech_off") {
        preferences_.speech.enabled = a.id == "welcome-speech_on";
        preferencesDirty_ = true; saveAt_ = ImGui::GetTime() + .45;
    }
    // An answer goes on to the next step, as Next does; Skip and the last step's answer end it.
    if (a.id != "welcome-skip" && ++welcomeStep_ < WelcomeSteps) { nav.Prefer(FirstRows[welcomeStep_]); return true; }
    preferences_.welcomed = true; preferencesDirty_ = true; saveAt_ = ImGui::GetTime() + .45;
    welcomeStep_ = 0; nav.Home();
    return true;
}
void ApplicationShell::DrawWelcomeBoard(const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetCursorScreenPos();
    const std::string& focus = nav.Focus();
    const auto press = [&](const MenuEntry& e, v2::Box b) { return PressEntry(nav, rows, e, b, action).hovered; };
    float y = origin.y;
    // WELCOME · STEP 1 OF 3, and a dot per step, the current one long.
    v2::SectionHeading(d, ImVec2(origin.x, y), loc::Tf("welcome.step", welcomeStep_ + 1, WelcomeSteps), c.accent);
    y += 30 * s;
    for (int i = 0, x = 0; i < WelcomeSteps; ++i) {
        const float w = (i == welcomeStep_ ? 40 : 12) * s;
        d->AddRectFilled(ImVec2(origin.x + x, y), ImVec2(origin.x + x + w, y + 6 * s), i <= welcomeStep_ ? c.accent : c.border, 3 * s);
        x += static_cast<int>(w + 8 * s);
    }
    y += 30 * s;
    const float wrap = (std::min)(760 * s, width), heading = (v2::Narrow(width, s) ? 24 : 32) * s;
    const char* title = loc::T(Titles[welcomeStep_]);
    d->AddText(HeadingFont(), heading, ImVec2(origin.x, y), c.text, title, nullptr, wrap);
    y += HeadingFont()->CalcTextSizeA(heading, FLT_MAX, wrap, title).y + 16 * s;
    y += v2::Paragraph(d, ImVec2(origin.x, y), wrap, loc::T(Texts[welcomeStep_]), 20 * s, c.text) + 24 * s;
    // The answers, 640 wide: the label over its words, Chosen (or the value) at the right.
    const float card = (std::min)(640 * s, width);
    for (const auto& e : rows) {
        if (e.id == "welcome-next" || e.id == "welcome-skip") continue;
        // The words wrap, and the card grows to hold them.
        const float valueWidth = e.value.empty() ? 0 : ImGui::CalcTextSize(e.value.c_str()).x + 24 * s, room = card - valueWidth - 28 * s;
        const std::string words = e.detail.substr(0, e.detail.find('\n'));
        const float lines = v2::Paragraph(nullptr, ImVec2(), room, words, 16 * s, 0);
        const v2::Box b{ImVec2(origin.x, y), ImVec2(origin.x + card, y + (std::max)(68 * s, 46 * s + lines))};
        y = b.max.y + 8 * s;
        const bool focused = e.id == focus, hovered = press(e, b);
        v2::CardGround(d, b, focused, hovered);
        v2::Text(d, ImVec2(b.min.x + 16 * s, b.min.y + 12 * s), room, e.label, 20 * s, c.text, HeadingFont());
        v2::Paragraph(d, ImVec2(b.min.x + 16 * s, b.min.y + 36 * s), room, words, 16 * s, c.muted);
        if (valueWidth > 0) d->AddText(ImVec2(b.max.x - valueWidth, b.min.y + (b.H() - ImGui::GetFontSize()) * .5f), e.choices.empty() ? c.accent : c.text, e.value.c_str());
        if (focused) {
            v2::Focus(d, b.min, b.max);
            // Language's choices open beside it, in the body.
            if (!e.choices.empty()) menu_.bodyChoices = {b, {origin, ImVec2(origin.x + width, origin.y + height)}};
        }
    }
    // Skip, then Next in Ember, at the bottom right.
    float x = origin.x + width;
    const float bottom = (std::max)(y + 8 * s, origin.y + height - 56 * s);
    for (std::size_t i = rows.size() - 2; i < rows.size(); ++i) {
        const auto& e = rows[i];
        const float w = HeadingFont()->CalcTextSizeA(17 * s, FLT_MAX, 0, e.label.c_str()).x + 36 * s;
        const v2::Box b{ImVec2(x - w, bottom), ImVec2(x, bottom + 48 * s)};
        x -= w + 12 * s;
        v2::Button(d, b, e.label, e.id == "welcome-next" ? v2::ButtonKind::Primary : v2::ButtonKind::Secondary, e.id == focus, press(e, b));
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom + 48 * s));
    ImGui::Dummy(ImVec2(width, 0));
}
} }
