// Settings in the V2 look: six tabs, each a rail of panels made from the
// classic Settings pages' own rows. A tab's screen ("tab-matches") has one row
// per panel; Select opens the panel as its own screen ("tab-matches/1"), so
// Back closes it. Every row keeps its id, so the same handlers serve both
// looks; only where a row is shown changes.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>

namespace sf4e { namespace ui {
std::vector<std::pair<std::string, std::string>> ApplicationShell::SettingsTabs() {
    return {{"tab-player", loc::T("settings.tab.player")}, {"tab-matches", loc::T("settings.tab.matches")},
        {"tab-interface", loc::T("settings.tab.interface")}, {"tab-access", loc::T("settings.tab.accessibility")},
        {"tab-online", loc::T("settings.tab.online")}, {"tab-about", loc::T("settings.tab.about")}};
}
bool ApplicationShell::SettingsTab(const std::string& screen) { return screen.compare(0, 4, "tab-") == 0; }
std::string ApplicationShell::SettingsTabOf(const std::string& screen) { return screen.substr(0, screen.find('/')); }
bool ApplicationShell::SettingsPreview(const std::string& id) {
    return id == "name" || id == "main-character" || id == "awards" || id == "record" || id == "share-steam" || id.compare(0, 3, "hud") == 0 ||
        id == "watched-picture" || id == "picture-corner" || id == "picture-size";
}

std::vector<MenuEntry> ApplicationShell::SettingsTabRows(const ShellView& v, const std::string& screen, bool idle, bool opening,
    const DrawSelection& selection, const DrawSelection& developer, std::string& title) {
    std::vector<MenuEntry> rows;
    std::string ignored;
    // A classic page's rows under `group`, those `keep` takes.
    const auto add = [&](const char* page, const char* group, bool (*keep)(const std::string&) = nullptr) {
        for (auto& row : BuildRows(v, page, idle, opening, selection, developer, ignored)) {
            if (keep && !keep(row.id)) continue;
            row.group = group;
            rows.push_back(std::move(row));
        }
    };
    const std::string tab = SettingsTabOf(screen);
    title = loc::T("home.settings");
    for (const auto& t : SettingsTabs()) if (t.first == tab) settingsEyebrow_ = std::string(loc::T("home.settings")) + " \xC2\xB7 " + t.second;
    if (tab == "tab-player") {
        add("player", loc::T("screen.player"));
        add("settings", loc::T("settings.country"), [](const std::string& id) { return id == "country"; });
        add("profile", loc::T("home.profile"));
    } else if (tab == "tab-matches") {
        add("defaults", loc::T("screen.defaults"));
        add("training-replays", loc::T("settings.training_replays"));
    } else if (tab == "tab-interface") {
        add("interface", loc::T("settings.group.look"), [](const std::string& id) { return id == "look"; });
        add("interface", loc::T("settings.match_hud"), [](const std::string& id) { return id.compare(0, 3, "hud") == 0; });
        add("interface", loc::T("settings.group.room_layout"), [](const std::string& id) { return id.compare(0, 8, "arrange-") == 0; });
        add("interface", loc::T("settings.group.sounds"), [](const std::string& id) { return id.compare(0, 5, "ready") == 0; });
    } else if (tab == "tab-access") {
        add("interface", loc::T("settings.group.text"), [](const std::string& id) { return id == "scale" || id == "tv" || id == "language"; });
        add("interface", loc::T("settings.group.help"), [](const std::string& id) { return id == "shortcut-hint"; });
    } else if (tab == "tab-online") {
        add("discord", loc::T("screen.discord"));
        add("interface", loc::T("screen.rankings"), [](const std::string& id) { return id == "class-emblems"; });
    } else {
        add("about", loc::T("home.about"));
        add("problem-reports", loc::T("reports.settings_title"));
    }
    settingsRows_ = rows;
    // An open panel's screen has its rows; the tab's own, a row per panel.
    const int open = tab == screen ? -1 : std::atoi(screen.c_str() + tab.size() + 1);
    std::vector<MenuEntry> shown;
    int panel = -1;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!i || rows[i].group != rows[i - 1].group) {
            ++panel;
            if (open == panel) settingsEyebrow_ += " \xC2\xB7 " + rows[i].group;
            if (open < 0) {
                // The head says what its panel holds: its first values, and every row's name in its detail.
                std::string summary, names;
                for (std::size_t j = i, values = 0; j < rows.size() && rows[j].group == rows[i].group; ++j) {
                    names += (names.empty() ? "" : " \xC2\xB7 ") + rows[j].label;
                    if (!rows[j].info && !rows[j].value.empty() && values++ < 3) summary += (summary.empty() ? "" : " \xC2\xB7 ") + rows[j].value;
                }
                shown.push_back(Row(tab + "/" + std::to_string(panel), rows[i].group, names));
                shown.back().value = summary;
                shown.back().hint = loc::T("settings.panel.open_action");
            }
        }
        if (open == panel) shown.push_back(rows[i]);
    }
    return shown;
}

// V2's Settings tab, as the design's board: the rail of panels on the left
// (360 px wide with all closed, 220 while one is open), the panel's rows in a
// section beside it with its preview, and the focused row's words under them.
// Narrow, the rail and the open panel each take the whole width in turn.
void ApplicationShell::DrawSettingsBoard(const std::vector<MenuEntry>& entries, MenuNavigation& nav, MenuAction& action, float height,
    const MenuVisualFeedback& feedback, const GameMenu::Detail& preview) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, font = ImGui::GetFontSize();
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const std::string& screen = nav.Screen(), tab = SettingsTabOf(screen), focus = nav.Focus();
    const bool open = tab != screen, narrow = v2::Narrow(ImGui::GetIO().DisplaySize.x, s), changed = focus != settingsFocus_;
    settingsFocus_ = focus;
    const auto mouse = ImGui::GetIO().MouseDelta;
    const bool pointerMoved = mouse.x != 0 || mouse.y != 0, idle = !nav.Asking() && !nav.Editing() && !nav.Choosing();
    // The panels as [first, end) of settingsRows_; the one shown is the open one, else the focused head's.
    std::vector<std::pair<std::size_t, std::size_t>> panels;
    for (std::size_t i = 0; i < settingsRows_.size(); ++i)
        if (!i || settingsRows_[i].group != settingsRows_[i - 1].group) panels.push_back({i, i + 1}); else panels.back().second = i + 1;
    const std::string& shownId = open ? screen : focus;
    const int current = shownId.size() > tab.size() ? std::atoi(shownId.c_str() + tab.size() + 1) : 0;
    if (panels.empty() || current >= static_cast<int>(panels.size())) return;
    // A box that is a button: pressed, and the moving pointer focuses an entry.
    const auto press = [&](const std::string& id, const v2::Box& b) {
        ImGui::SetCursorScreenPos(b.min);
        ImGui::PushID(id.c_str());
        const bool pressed = ImGui::InvisibleButton("##settings", ImVec2(b.W(), b.H())) && idle;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered && pointerMoved && !nav.Choosing()) nav.Focus(id, entries);
        return std::make_pair(pressed, hovered);
    };

    v2::Box body{ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + width, ImGui::GetCursorScreenPos().y + height)};
    v2::CutTop(body, 0, 8 * s);
    // The rail: a head per panel with its arrow, name, and while all are closed its summary and state.
    if (!narrow || !open) {
        const v2::Box rail = v2::CutLeft(body, narrow ? width : (open ? 220 : 360) * s, 16 * s);
        float y = rail.min.y;
        for (int i = 0; i < static_cast<int>(panels.size()); ++i) {
            const MenuEntry& first = settingsRows_[panels[i].first];
            const std::string id = tab + "/" + std::to_string(i);
            const bool isOpen = open && i == current, focused = !open && id == focus;
            const v2::Box h{ImVec2(rail.min.x, y), ImVec2(rail.max.x, y + (open ? 52 : 64) * s)};
            y = h.max.y + 8 * s;
            const auto [pressed, hovered] = press(id, h);
            if (pressed) {
                if (!open) { nav.Focus(id, entries); action = nav.Choose(entries); }
                else action = isOpen ? MenuAction{MenuAction::Back} : MenuAction{MenuAction::Shortcut, id};
            }
            d->AddRectFilled(h.min, h.max, focused || isOpen ? c.header : hovered ? c.hover : v2::color::Alpha(c.panel, .94f), 4 * s);
            d->AddRect(h.min, h.max, isOpen ? c.accentLight : c.border, 4 * s, 0, s);
            if (isOpen) d->AddRectFilled(h.min, ImVec2(h.min.x + 4 * s, h.max.y), c.accent, 4 * s, ImDrawFlags_RoundCornersLeft);
            // The arrow: right while closed, down and filled while open.
            const ImVec2 a(h.min.x + 10 * s, h.min.y + (h.H() - 28 * s) * .5f), m(a.x + 14 * s, a.y + 14 * s);
            if (isOpen) {
                d->AddRectFilled(a, ImVec2(a.x + 28 * s, a.y + 28 * s), c.accent, 4 * s);
                d->AddTriangleFilled(ImVec2(m.x - 4 * s, m.y - 2 * s), ImVec2(m.x + 4 * s, m.y - 2 * s), ImVec2(m.x, m.y + 3 * s), c.ink);
            } else {
                d->AddRect(a, ImVec2(a.x + 28 * s, a.y + 28 * s), c.muted, 4 * s, 0, s);
                d->AddTriangleFilled(ImVec2(m.x - 2 * s, m.y - 4 * s), ImVec2(m.x + 3 * s, m.y), ImVec2(m.x - 2 * s, m.y + 4 * s), c.text);
            }
            const float x = a.x + 38 * s;
            if (open) {
                d->PushClipRect(ImVec2(x, h.min.y), ImVec2(h.max.x - 10 * s, h.max.y), true);
                d->AddText(HeadingFont(), 18 * s, ImVec2(x, h.min.y + (h.H() - 18 * s) * .5f), c.text, first.group.c_str());
                d->PopClipRect();
            } else {
                const char* state = loc::T("settings.panel.closed");
                const float sw = ImGui::CalcTextSize(state).x;
                d->AddText(ImVec2(h.max.x - 10 * s - sw, h.min.y + (h.H() - font) * .5f), c.muted, state);
                const auto head = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == id; });
                d->PushClipRect(ImVec2(x, h.min.y), ImVec2(h.max.x - 20 * s - sw, h.max.y), true);
                d->AddText(HeadingFont(), 20 * s, ImVec2(x, h.min.y + 10 * s), c.text, first.group.c_str());
                if (head != entries.end()) d->AddText(ImGui::GetFont(), 14 * s, ImVec2(x, h.min.y + 36 * s), c.muted, head->value.c_str());
                d->PopClipRect();
            }
            if (focused) v2::Focus(d, h.min, h.max);
        }
    }
    if (narrow && !open) return;

    // The section and, under it, the focused row's words (the panel's while closed).
    const MenuEntry* words = nullptr;
    for (const auto& e : entries) if (e.id == focus) words = &e;
    v2::Box column = body;
    if (const float cap = (width / s > 1700 ? 1400 : 1100) * s; column.W() > cap) column.max.x = column.min.x + cap;
    // The words' bar: the design's height, taller when they wrap past it.
    const float wrapped = words ? ImGui::CalcTextSize(words->detail.c_str(), nullptr, false, column.W() - 32 * s).y : 0;
    const v2::Box bar = v2::CutBottom(column, (std::max)((ImGui::GetIO().DisplaySize.y / s <= 720 ? 52 : 74) * s, 36 * s + wrapped), 8 * s);
    d->AddRectFilled(bar.min, bar.max, c.frame, 4 * s);
    d->AddRect(bar.min, bar.max, c.border, 4 * s, 0, s);
    if (words) {
        d->PushClipRect(bar.min, bar.max, true);
        d->AddText(HeadingFont(), 16 * s, ImVec2(bar.min.x + 16 * s, bar.min.y + 6 * s), c.text, words->label.c_str());
        d->AddText(ImGui::GetFont(), 15 * s, ImVec2(bar.min.x + 16 * s, bar.min.y + 28 * s), c.muted, words->detail.c_str(), nullptr, bar.W() - 32 * s);
        d->PopClipRect();
    }
    v2::Box section = column;
    d->AddRectFilled(section.min, section.max, v2::color::Alpha(c.panel, .94f), 4 * s);
    const v2::Box head = v2::CutTop(section, 40 * s);
    d->AddRectFilled(head.min, head.max, open ? c.header : c.panel, 4 * s, ImDrawFlags_RoundCornersTop);
    d->AddLine(ImVec2(head.min.x, head.max.y), ImVec2(head.max.x, head.max.y), c.border, s);
    d->AddRect(column.min, column.max, open ? c.accentLight : c.border, 4 * s, 0, s);
    const MenuEntry& first = settingsRows_[panels[current].first];
    const char* state = loc::T(open ? "settings.panel.open" : "settings.preview");
    d->AddText(HeadingFont(), 20 * s, ImVec2(head.min.x + 16 * s, head.min.y + 10 * s), c.text, first.group.c_str());
    d->AddText(ImVec2(head.max.x - 16 * s - ImGui::CalcTextSize(state).x, head.min.y + (head.H() - font) * .5f), c.muted, state);

    // The preview, beside the rows, for a panel with something to show.
    std::string pictured;
    for (std::size_t i = panels[current].first; i < panels[current].second && pictured.empty(); ++i)
        if (SettingsPreview(settingsRows_[i].id)) pictured = settingsRows_[i].id;
    if (open && SettingsPreview(focus)) pictured = focus;
    if (!pictured.empty() && preview && !narrow && section.W() >= 760 * s) {
        const v2::Box pv = v2::CutRight(section, 380 * s);
        d->AddText(HeadingFont(), 14 * s, ImVec2(pv.min.x, pv.min.y + 12 * s), c.muted, loc::T("settings.preview"));
        ImGui::SetCursorScreenPos(ImVec2(pv.min.x, pv.min.y + 32 * s));
        ImGui::BeginChild("settings preview", ImVec2(pv.W() - 12 * s, pv.H() - 44 * s), 0, ImGuiWindowFlags_NoNavInputs);
        preview(pictured);
        ImGui::EndChild();
    }
    // The rows: on the Ember rule while the panel is open, dimmed while it is only shown.
    if (open) d->AddRectFilled(section.min, ImVec2(section.min.x + 3 * s, section.max.y), c.accent);
    const v2::Box list = v2::Inset(section, 12 * s, 8 * s);
    ImGui::SetCursorScreenPos(list.min);
    ImGui::BeginChild("settings rows", ImVec2(list.W(), list.H()), 0, ImGuiWindowFlags_NoNavInputs);
    auto* ld = ImGui::GetWindowDrawList();
    const float rowHeight = 48 * s, valueWidth = (std::min)(196 * s, list.W() * .45f);
    for (std::size_t i = panels[current].first; i < panels[current].second; ++i) {
        // The open panel's rows are the screen's entries, with their values offered.
        const auto live = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& x) { return x.id == settingsRows_[i].id; });
        const MenuEntry& e = live != entries.end() ? *live : settingsRows_[i];
        const auto p = ImGui::GetCursorScreenPos();
        const v2::Box r{p, ImVec2(p.x + list.W(), p.y + rowHeight)};
        const bool focused = open && e.id == focus;
        const auto [pressed, hovered] = press(e.id, r);
        ImGui::SetCursorScreenPos(ImVec2(p.x, r.max.y + 4 * s));
        if (focused && changed) ImGui::SetScrollHereY(.5f);
        // While closed a row opens its panel; while open its value's ends step it, anything else on it is Select.
        if (pressed && !open) action = {MenuAction::Shortcut, tab + "/" + std::to_string(current)};
        else if (pressed) {
            nav.Focus(e.id, entries);
            const float mx = ImGui::GetMousePos().x, vx = r.max.x - 6 * s - valueWidth;
            const int step = MenuValueIsNumber(e) && e.enabled && mx >= vx ? (mx < vx + valueWidth * .5f ? -1 : 1) : 0;
            action = step ? MenuAction{MenuAction::Adjust, e.id, {}, step} : nav.Choose(entries);
        }
        if (focused || (open && hovered)) ld->AddRectFilled(r.min, r.max, focused ? c.header : c.hover, 4 * s);
        const bool enabled = feedback.Enabled(e);
        const ImU32 text = open && enabled ? c.text : c.muted, arrow = open && enabled && e.adjustable ? c.accent : c.muted;
        const float ty = r.min.y + (rowHeight - font) * .5f, vx = r.max.x - 6 * s - valueWidth;
        ld->PushClipRect(r.min, ImVec2(e.adjustable || !e.value.empty() ? vx - 8 * s : r.max.x, r.max.y), true);
        ld->AddText(ImVec2(r.min.x + 14 * s, ty), text, e.label.c_str());
        ld->PopClipRect();
        // The value between the arrows that step it, or on its own with a chevron when Select opens something.
        if (e.userText || e.text) NoteUserText(e.value);
        if (e.adjustable) DrawMenuValue(ld, e, {ImVec2(vx, r.min.y), ImVec2(r.max.x - 14 * s, r.max.y)}, open && enabled, text);
        else {
            const float tw = (std::min)(ImGui::CalcTextSize(e.value.c_str()).x, valueWidth - 40 * s);
            ld->PushClipRect(ImVec2(vx + 20 * s, r.min.y), ImVec2(vx + valueWidth - 20 * s, r.max.y), true);
            ld->AddText(ImVec2(vx + valueWidth - 20 * s - tw, ty), e.valueHighlighted ? c.accent : e.info ? c.muted : text, e.value.c_str());
            ld->PopClipRect();
            if (!e.info) ld->AddText(ImVec2(r.max.x - 14 * s - ImGui::CalcTextSize(">").x, ty), arrow, ">");
        }
        if (focused) {
            v2::Focus(ld, r.min, r.max);
            if (!e.choices.empty()) menu_.bodyChoices = {r, list};
        }
    }
    ImGui::EndChild();
}
} }
