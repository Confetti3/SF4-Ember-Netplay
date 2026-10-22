// The first-run welcome (FirstRun.dc.html): shown once on a fresh install
// (PlayerPreferences::welcomed), speech first, then language, controller and
// look. Every step is rows, so the pad, the keys and the mouse all answer it,
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
constexpr int WelcomeSteps = 4;
// Each step's question and the words under it.
const char* const Titles[WelcomeSteps] = {"welcome.speech_title", "welcome.language_title", "welcome.controller_title", "welcome.look_title"};
const char* const Texts[WelcomeSteps] = {"welcome.speech_text", "welcome.language_text", "welcome.controller_text", "welcome.look_text"};
// Where each step's focus starts.
const char* const FirstRows[WelcomeSteps] = {"welcome-speech_on", "language", "player", "welcome-look_classic"};
// One of a step's answers, marked Chosen while it is the one in effect.
MenuEntry Answer(const char* id, const char* label, const char* detail, bool chosen) {
    auto row = Row(id, loc::T(label), loc::T(detail));
    if (chosen) row.value = loc::T("welcome.chosen");
    return row;
}
}
std::vector<MenuEntry> ApplicationShell::WelcomeRows(const ShellView& v, std::string& title) {
    // V2 draws the brand over the board, so its title is the step's question, the first thing speech says.
    title = loc::T(v2::On() ? Titles[welcomeStep_] : "welcome.title");
    std::vector<MenuEntry> rows;
    switch (welcomeStep_) {
    case 0: rows = {Answer("welcome-speech_on", "welcome.speech_on", "welcome.speech_on_detail", preferences_.speech.enabled),
        Answer("welcome-speech_off", "welcome.speech_off", "welcome.speech_off_detail", !preferences_.speech.enabled)}; break;
    case 1: rows = {LanguageRow()}; break;
    case 2: rows = {Row("player", loc::T("home.choose_controller"), v.controllerReady ? loc::Tf("welcome.controller_ready", v.controller) : std::string(loc::T("home.choose_controller_detail")))}; break;
    default: rows = {Answer("welcome-look_classic", "welcome.look_classic", "welcome.look_classic_detail", !preferences_.lookV2),
        Answer("welcome-look_v2", "welcome.look_v2", "welcome.look_v2_detail", preferences_.lookV2)};
    }
    rows.push_back(Row("welcome-next", loc::T(welcomeStep_ + 1 < WelcomeSteps ? "welcome.next" : "welcome.start"), loc::T("welcome.next_detail")));
    rows.push_back(Row("welcome-skip", loc::T("welcome.skip"), loc::T("welcome.skip_detail")));
    // Side by side on the V2 board: Skip, then Next.
    rows[rows.size() - 2].left = "welcome-skip";
    rows.back().right = "welcome-next";
    return rows;
}
bool ApplicationShell::WelcomeActivate(const MenuAction& a) {
    auto& nav = menu_.navigation;
    if (a.kind == MenuAction::Back) { welcomeStep_ = (std::max)(0, welcomeStep_ - 1); nav.Prefer(FirstRows[welcomeStep_]); return true; }
    if (a.id.compare(0, 8, "welcome-") != 0) return false;
    // Speech and the look are the player's settings from their answer on, saved like any other.
    const bool speech = a.id == "welcome-speech_on" || a.id == "welcome-speech_off", look = a.id == "welcome-look_classic" || a.id == "welcome-look_v2";
    if (speech) preferences_.speech.enabled = a.id == "welcome-speech_on";
    if (look) preferences_.lookV2 = a.id == "welcome-look_v2";
    if (speech || look) { preferencesDirty_ = true; saveAt_ = ImGui::GetTime() + .45; }
    // An answer goes on to the next step, as Next does; Skip and the last step's answer end it.
    if (a.id != "welcome-skip" && ++welcomeStep_ < WelcomeSteps) { nav.Prefer(welcomeStep_ == 3 && preferences_.lookV2 ? "welcome-look_v2" : FirstRows[welcomeStep_]); return true; }
    preferences_.welcomed = true; preferencesDirty_ = true; saveAt_ = ImGui::GetTime() + .45;
    welcomeStep_ = 0; nav.Home();
    return true;
}
void ApplicationShell::DrawWelcomeBoard(const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetCursorScreenPos();
    const auto mouse = ImGui::GetIO().MouseDelta;
    const bool pointerMoved = mouse.x != 0 || mouse.y != 0;
    const std::string& focus = nav.Focus();
    // A row's box: the pointer focuses it, a click chooses it, as the cards do.
    const auto press = [&](const MenuEntry& e, v2::Box b) {
        ImGui::SetCursorScreenPos(b.min);
        const bool pressed = ImGui::InvisibleButton(e.id.c_str(), ImVec2(b.W(), b.H())), hovered = ImGui::IsItemHovered();
        if (pressed && !nav.Asking() && !nav.Editing()) { nav.Focus(e.id, rows); action = nav.Choose(rows); }
        else if (hovered && pointerMoved && !nav.Choosing()) nav.Focus(e.id, rows);
        return hovered;
    };
    float y = origin.y;
    // WELCOME · STEP 1 OF 4, and a dot per step, the current one long.
    d->AddText(HeadingFont(), 14 * s, ImVec2(origin.x, y), c.accent, loc::Tf("welcome.step", welcomeStep_ + 1, WelcomeSteps).c_str());
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
    const std::string text = loc::T(Texts[welcomeStep_]);
    const float textSize = 20 * s;
    float lines = 0;
    for (const char *p = text.c_str(), *end = p + text.size(); p < end; ++lines) {
        const char* cut = ImGui::GetFont()->CalcWordWrapPositionA(textSize / ImGui::GetFont()->FontSize, p, end, wrap);
        d->AddText(ImGui::GetFont(), textSize, ImVec2(origin.x, y + lines * textSize * 1.5f), c.text, p, cut == p ? p + 1 : cut);
        for (p = cut == p ? p + 1 : cut; p < end && *p == ' '; ++p) {}
    }
    y += lines * textSize * 1.5f + 24 * s;
    // The answers, 640 wide: the label over its words, Chosen (or the value) at the right.
    const float card = (std::min)(640 * s, width);
    for (const auto& e : rows) {
        if (e.id == "welcome-next" || e.id == "welcome-skip") continue;
        const v2::Box b{ImVec2(origin.x, y), ImVec2(origin.x + card, y + 68 * s)};
        y += 76 * s;
        const bool focused = e.id == focus, hovered = press(e, b);
        v2::CardGround(d, b, focused, hovered);
        const float valueWidth = e.value.empty() ? 0 : ImGui::CalcTextSize(e.value.c_str()).x + 24 * s;
        d->PushClipRect(b.min, ImVec2(b.max.x - valueWidth - 12 * s, b.max.y), true);
        d->AddText(HeadingFont(), 20 * s, ImVec2(b.min.x + 16 * s, b.min.y + 12 * s), c.text, e.label.c_str());
        d->AddText(ImGui::GetFont(), 16 * s, ImVec2(b.min.x + 16 * s, b.min.y + 38 * s), c.muted, e.detail.substr(0, e.detail.find('\n')).c_str());
        d->PopClipRect();
        if (valueWidth > 0) d->AddText(ImVec2(b.max.x - valueWidth, b.min.y + (b.H() - ImGui::GetFontSize()) * .5f), e.choices.empty() ? c.accent : c.text, e.value.c_str());
        if (focused) {
            v2::Focus(d, b.min, b.max);
            // Language's choices open beside it, in the body.
            if (!e.choices.empty()) menu_.bodyChoices = {b, {origin, ImVec2(origin.x + width, origin.y + height)}};
        }
    }
    // Skip, then Next in Ember, at the bottom right.
    float x = origin.x + width;
    const float bottom = (std::max)(y + 8 * s, origin.y + height - 48 * s);
    for (std::size_t i = rows.size() - 2; i < rows.size(); ++i) {
        const auto& e = rows[i];
        const bool next = e.id == "welcome-next", focused = e.id == focus;
        const float w = HeadingFont()->CalcTextSizeA(17 * s, FLT_MAX, 0, e.label.c_str()).x + 36 * s;
        const v2::Box b{ImVec2(x - w, bottom), ImVec2(x, bottom + 40 * s)};
        x -= w + 12 * s;
        const bool hovered = press(e, b);
        d->AddRectFilled(b.min, b.max, focused ? c.header : next ? (hovered ? c.accentLight : c.accent) : hovered ? c.hover : c.button, 4 * s);
        if (!next) d->AddRect(b.min, b.max, c.border, 4 * s, 0, s);
        if (focused) v2::Focus(d, b.min, b.max, false);
        d->AddText(HeadingFont(), 17 * s, ImVec2(b.min.x + 18 * s, b.min.y + 11.5f * s), next && !focused ? c.ink : c.text, e.label.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom + 40 * s));
    ImGui::Dummy(ImVec2(width, 0));
}
} }
