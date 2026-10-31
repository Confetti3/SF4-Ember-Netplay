// Settings: six tabs, each a rail of panels made from the Settings pages' own
// rows (BuildRows: "player", "interface", ...). A tab's screen ("tab-matches")
// has one row per panel; Select opens the panel as its own screen
// ("tab-matches/1"), so Back closes it. Every row keeps its id, so the pages'
// handlers serve the tabs.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

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
    // A page's rows under `group`, those `keep` takes.
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
        add("interface", loc::T("settings.group.look"), [](const std::string& id) { return id == "tv"; });
        add("interface", loc::T("settings.match_hud"), [](const std::string& id) { return id.compare(0, 3, "hud") == 0; });
        add("interface", loc::T("settings.group.room_layout"), [](const std::string& id) { return id.compare(0, 8, "arrange-") == 0; });
        add("interface", loc::T("settings.group.sounds"), [](const std::string& id) { return id.compare(0, 5, "ready") == 0; });
    } else if (tab == "tab-access") {
        add("accessibility", loc::T("speech.setting"));
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

// A Settings tab, as the design's board: the rail of panels on the left
// (360 px wide with all closed, 220 while one is open), the panel's rows in a
// section beside it with its preview, and the focused row's words under them.
// Narrow, the rail and the open panel each take the whole width in turn.
void ApplicationShell::DrawSettingsBoard(const std::vector<MenuEntry>& entries, MenuNavigation& nav, MenuAction& action, float height,
    const MenuVisualFeedback& feedback, const GameMenu::Detail& preview) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, font = ImGui::GetFontSize();
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const std::string& screen = nav.Screen(), tab = SettingsTabOf(screen), focus = nav.Focus();
    const bool open = tab != screen, narrow = v2::Narrow(ImGui::GetWindowWidth(), s), changed = focus != settingsFocus_;
    settingsFocus_ = focus;
    // The panels as [first, end) of settingsRows_; the one shown is the open one, else the focused head's.
    std::vector<std::pair<std::size_t, std::size_t>> panels;
    for (std::size_t i = 0; i < settingsRows_.size(); ++i)
        if (!i || settingsRows_[i].group != settingsRows_[i - 1].group) panels.push_back({i, i + 1}); else panels.back().second = i + 1;
    const std::string& shownId = open ? screen : focus;
    const int current = shownId.size() > tab.size() ? std::atoi(shownId.c_str() + tab.size() + 1) : 0;
    if (panels.empty() || current >= static_cast<int>(panels.size())) return;

    v2::Box body{ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + width, ImGui::GetCursorScreenPos().y + height)};
    v2::CutTop(body, 0, 8 * s);
    // The rail: a head per panel with its arrow, name, and while all are closed its summary and state.
    if (!narrow || !open) {
        const v2::Box rail = v2::CutLeft(body, narrow ? width : (open ? 220 : 360) * s, 16 * s);
        float y = rail.min.y;
        for (int i = 0; i < static_cast<int>(panels.size()); ++i) {
            const MenuEntry& first = settingsRows_[panels[i].first];
            MenuEntry head;
            head.id = tab + "/" + std::to_string(i);
            const std::string& id = head.id;
            const bool isOpen = open && i == current, focused = !open && id == focus;
            const v2::Box h{ImVec2(rail.min.x, y), ImVec2(rail.max.x, y + (open ? 52 : 64) * s)};
            y = h.max.y + 8 * s;
            const auto [pressed, hovered] = PressEntry(nav, entries, head, h, action, false, 0, false);
            if (pressed) {
                if (!open) { nav.Focus(id, entries); action = nav.Choose(entries); }
                else action = isOpen ? MenuAction{MenuAction::Back} : MenuAction{MenuAction::Shortcut, id};
            }
            d->AddRectFilled(h.min, h.max, focused || isOpen ? c.header : hovered ? c.hover : v2::PanelGround(), 4 * s);
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
            if (open) v2::Text(d, ImVec2(x, h.min.y + (h.H() - 16 * s) * .5f), h.max.x - 10 * s - x, first.group, 16 * s, c.text, HeadingFont());
            else {
                const char* state = loc::T("settings.panel.closed");
                const float sw = ImGui::CalcTextSize(state).x;
                d->AddText(ImVec2(h.max.x - 10 * s - sw, h.min.y + (h.H() - font) * .5f), c.muted, state);
                const auto row = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == id; });
                v2::Text(d, ImVec2(x, h.min.y + 10 * s), h.max.x - 20 * s - sw - x, first.group, 20 * s, c.text, HeadingFont());
                if (row != entries.end()) v2::Text(d, ImVec2(x, h.min.y + 36 * s), h.max.x - 20 * s - sw - x, row->value, 14 * s, c.muted);
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
    const float wrapped = words ? v2::WordsHeight(column.W(), words->detail) : 0;
    const v2::Box bar = v2::CutBottom(column, (std::max)((ImGui::GetIO().DisplaySize.y / s <= 720 ? 52 : 74) * s, wrapped), 8 * s);
    v2::Words(d, bar, words ? words->label : std::string(), words ? words->detail : std::string(), true);
    v2::Box section = v2::Panel(d, column, settingsRows_[panels[current].first].group.c_str(), loc::T(open ? "settings.panel.open" : "settings.preview"), open);

    // The preview, beside the rows, for a panel with something to show.
    std::string pictured;
    for (std::size_t i = panels[current].first; i < panels[current].second && pictured.empty(); ++i)
        if (SettingsPreview(settingsRows_[i].id)) pictured = settingsRows_[i].id;
    if (open && SettingsPreview(focus)) pictured = focus;
    if (!pictured.empty() && preview && !narrow && section.W() >= 760 * s) {
        const v2::Box pv = v2::CutRight(section, 380 * s);
        v2::SectionHeading(d, ImVec2(pv.min.x, pv.min.y + 12 * s), loc::T("settings.preview"));
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
    const float rowHeight = 48 * s;
    for (std::size_t i = panels[current].first; i < panels[current].second; ++i) {
        // The open panel's rows are the screen's entries, with their values offered.
        const auto live = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& x) { return x.id == settingsRows_[i].id; });
        const MenuEntry& e = live != entries.end() ? *live : settingsRows_[i];
        const auto p = ImGui::GetCursorScreenPos();
        // The row as its label and value share it (FitMenuRow), a line taller with the value under the label.
        const v2::Share value = FitMenuRow(e, list.W() - 28 * s, 8 * s);
        const v2::Box r{p, ImVec2(p.x + list.W(), p.y + rowHeight + (value.under ? v2::UnderLine * s : 0))};
        const bool focused = open && e.id == focus;
        // While closed a row opens its panel; while open its value's ends step it, anything else on it is Select.
        const auto [pressed, hovered] = PressEntry(nav, entries, e, r, action, focused && changed, value.value, open);
        ImGui::SetCursorScreenPos(ImVec2(p.x, r.max.y + 4 * s));
        if (pressed && !open) action = {MenuAction::Shortcut, tab + "/" + std::to_string(current)};
        v2::RowGround(ld, r, focused, open && hovered);
        DrawMenuRow(ld, e, r, open && feedback.Enabled(e), value, 14 * s);
        if (focused) {
            v2::Focus(ld, r.min, r.max);
            if (!e.choices.empty()) menu_.bodyChoices = {r, list};
        }
    }
    ImGui::EndChild();
}
} }
