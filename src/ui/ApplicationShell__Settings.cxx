// Settings in the V2 look: six tabs, each one page of grouped cards made from
// the classic Settings pages' own rows. Every row keeps its id, so the same
// handlers serve both looks; only where a row is shown changes.
#include "ApplicationShell.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
std::vector<std::pair<std::string, std::string>> ApplicationShell::SettingsTabs() {
    return {{"tab-player", loc::T("settings.tab.player")}, {"tab-matches", loc::T("settings.tab.matches")},
        {"tab-interface", loc::T("settings.tab.interface")}, {"tab-access", loc::T("settings.tab.accessibility")},
        {"tab-online", loc::T("settings.tab.online")}, {"tab-about", loc::T("settings.tab.about")}};
}
bool ApplicationShell::SettingsTab(const std::string& screen) { return screen.compare(0, 4, "tab-") == 0; }

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
    for (const auto& tab : SettingsTabs()) if (tab.first == screen) title = tab.second;
    if (screen == "tab-player") {
        add("player", loc::T("screen.player"));
        add("settings", loc::T("settings.country"), [](const std::string& id) { return id == "country"; });
        add("profile", loc::T("home.profile"));
    } else if (screen == "tab-matches") {
        add("defaults", loc::T("screen.defaults"));
        add("training-replays", loc::T("settings.training_replays"));
    } else if (screen == "tab-interface") {
        add("interface", loc::T("settings.group.look"), [](const std::string& id) { return id == "look"; });
        add("interface", loc::T("settings.match_hud"), [](const std::string& id) { return id.compare(0, 3, "hud") == 0; });
        add("interface", loc::T("settings.group.sounds"), [](const std::string& id) { return id.compare(0, 5, "ready") == 0; });
    } else if (screen == "tab-access") {
        add("interface", loc::T("settings.group.text"), [](const std::string& id) { return id == "scale" || id == "language"; });
        add("interface", loc::T("settings.group.help"), [](const std::string& id) { return id == "shortcut-hint"; });
    } else if (screen == "tab-online") {
        add("discord", loc::T("discord.title"));
        add("interface", loc::T("screen.rankings"), [](const std::string& id) { return id == "class-emblems"; });
    } else {
        add("about", loc::T("home.about"));
        add("problem-reports", loc::T("reports.settings_title"));
    }
    return rows;
}
} }
