#include "MusicPanel.hxx"
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
namespace {
const music::Choices& Choices(const ShellView& view) {
    static const music::Choices empty;
    return view.musicChoices ? *view.musicChoices : empty;
}
std::string Name(int choice, const char* defaultLabel) {
    if (choice == music::Random) return loc::T("music.random");
    const auto* track = music::Find(choice);
    return track ? loc::Tf(track->character ? "music.character_theme" : "music.stage_theme", track->name) : defaultLabel;
}
MenuEntry TrackRow(const char* id, const char* label, const char* defaultLabel, const char* detail,
    int preference, const ShellView& view) {
    const auto& choices = Choices(view);
    const int shown = choices.ids[choices.Index(preference)];
    auto row = Value(id, label, Name(shown, defaultLabel), detail, view.canEditMusic);
    row.chosen = std::to_string(shown);
    for (const int choice : choices.ids) row.choices.push_back({std::to_string(choice), Name(choice, defaultLabel)});
    if (!view.musicError.empty()) row.detail = view.musicError + "\n\n" + row.detail;
    return row;
}
}
std::vector<MenuEntry> MusicPanel::Rows(const netplay::PlayerPreferences& preferences, const ShellView& view) {
    auto reset = Row("music-reset", loc::T("music.reset"), loc::T("music.reset_detail"), view.canEditMusic);
    if (!view.musicError.empty()) reset.detail = view.musicError + "\n\n" + reset.detail;
    return {
        TrackRow("music-menu", loc::T("music.lobby"), loc::T("music.lobby_default"),
            loc::T("music.lobby_detail"), preferences.lobbyMusic, view),
        TrackRow("music-online", loc::T("music.match"), loc::T("music.match_default"),
            loc::T("music.match_detail"), preferences.matchMusic, view),
        std::move(reset)
    };
}
bool MusicPanel::Accept(const MenuAction& action, const ShellView& view, netplay::PlayerPreferences& preferences) {
    if (!view.canEditMusic) return false;
    if (action.kind == MenuAction::Activate && action.id == "music-reset") {
        preferences.lobbyMusic = preferences.matchMusic = music::Default;
        return true;
    }
    if (action.kind != MenuAction::Chosen && action.kind != MenuAction::Adjust) return false;
    int* choice = action.id == "music-menu" ? &preferences.lobbyMusic :
        action.id == "music-online" ? &preferences.matchMusic : nullptr;
    if (!choice) return false;
    const auto& choices = Choices(view);
    *choice = action.kind == MenuAction::Chosen ? choices.FromKey(action.text) : choices.Step(*choice, action.delta);
    return true;
}
} }
