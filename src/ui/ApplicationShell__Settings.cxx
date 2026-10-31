// Settings: six tabs, each a rail of panels. A panel (SettingsPanel) is the
// model: a heading, the one builder of its rows and, for a few, a picture
// beside them. A tab's screen ("tab-matches") has one row per panel; Select
// opens the panel as its own screen ("tab-matches/1"), so Back closes it. A
// few pages stand on their own (Page): Player from Home and a room's Controls
// row, Controllers from Pad setup, Profile from Home, Sent reports from the
// Problem reports panel, and About for a screen nothing else draws. Each is
// one builder a panel may share, so every row keeps its id and the shell's
// handlers serve both.
#include "ApplicationShell.hxx"
#include "ControllersScreen.hxx"
#include "MenuRows.hxx"
#include "RowId.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "VersionLine.hxx"
#include "WatchedPicture.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include "../common/MatchFrameMeter.hxx"
#include "../platform/LocaleWindows.hxx"
#include "../session/TrainingCall.hxx"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace sf4e { namespace ui {
namespace {
// The room board's ready-made layouts, by their column (PlayerPreferences::roomColumn),
// and the one a column is nearest: Board first, Balanced, Chat first.
constexpr int RoomPresets[] = {28, 36, 56};
const char* const RoomPresetKeys[] = {"settings.room_preset.board", "settings.room_preset.balanced", "settings.room_preset.chat"};
int RoomPreset(int column) { return column <= 30 ? 0 : column >= 50 ? 2 : 1; }
// "Automatic" first, then every locale by its own name.
std::vector<MenuChoice> LanguageChoices() {
    std::vector<MenuChoice> choices;
    std::string preference = "auto";
    do {
        choices.push_back({preference, preference == "auto" ? loc::Tf("settings.language.system_with", loc::NativeName(platform::ResolveUiLocale("auto"))) :
            std::string(loc::NativeName(loc::ResolveLocale(preference, {}, {})))});
        preference = std::string(loc::NextPreference(preference, 1));
    } while (preference != "auto" && choices.size() < 64);
    return choices;
}
// When a report was sent, in this PC's time, as 2026-10-09 14:03.
std::string ReportTime(std::int64_t time) {
    const std::time_t at = static_cast<std::time_t>(time);
    std::tm local{};
    char text[32] = {};
    if (localtime_s(&local, &at) != 0 || !std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local)) return "-";
    return text;
}
// What a row whose setting cannot change now says instead.
const char* Reason(const ShellView& v) { return v.canEditPreferences ? loc::T("settings.auto_save") : loc::T("settings.leave_room_to_edit"); }
}

// A Settings panel: its heading (a locale key), the one builder of its rows
// and, for a panel with something to show beside them, its picture. Each is a
// named constant in SettingsScreens::Tabs; its screen is its place on its tab
// ("tab-access/1"), the id the menu walk and the journeys open it by.
struct ApplicationShell::SettingsPanel {
    using Build = std::vector<MenuEntry> (*)(ApplicationShell&, const ShellView&);
    using Picture = void (*)(ApplicationShell&, const ShellView&);
    const char* heading;
    Build rows;
    Picture picture = nullptr;
};

class ApplicationShell::SettingsScreens final : public ScreenPanel {
    using Shell = ApplicationShell;
    using Entries = std::vector<MenuEntry>;
public:
    struct Tab { const char* screen; const char* label; std::vector<const SettingsPanel*> panels; };
    // The tabs in order, each with its panels in order: a panel's screen is its place there.
    static const std::vector<Tab>& Tabs() {
        static const SettingsPanel controls{"screen.player", &Controls}, country{"settings.country", &Country},
            profile{"home.profile", &Profile, &ProfileCard}, defaults{"screen.defaults", &Defaults},
            trainingReplays{"settings.training_replays", &TrainingReplays, &PictureSpot}, look{"settings.group.look", &Look},
            matchHud{"settings.match_hud", &MatchHud, &MatchHudPicture}, roomLayout{"settings.group.room_layout", &RoomLayout, &RoomLayoutPicture},
            sounds{"settings.group.sounds", &Sounds}, speech{"speech.setting", &Speech}, text{"settings.group.text", &Text},
            help{"settings.group.help", &Help}, discord{"screen.discord", &Discord}, rankings{"screen.rankings", &Rankings},
            about{"home.about", &About}, reports{"reports.settings_title", &Reports};
        static const std::vector<Tab> tabs = {
            {"tab-player", "settings.tab.player", {&controls, &country, &profile}},
            {"tab-matches", "settings.tab.matches", {&defaults, &trainingReplays}},
            {"tab-interface", "settings.tab.interface", {&look, &matchHud, &roomLayout, &sounds}},
            {"tab-access", "settings.tab.accessibility", {&speech, &text, &help}},
            {"tab-online", "settings.tab.online", {&discord, &rankings}},
            {"tab-about", "settings.tab.about", {&about, &reports}}};
        return tabs;
    }
    static const Tab* TabOf(const std::string& screen) {
        const std::string tab = SettingsTabOf(screen);
        for (const auto& t : Tabs()) if (tab == t.screen) return &t;
        return nullptr;
    }
    // The open panel's place on its tab ("tab-matches/1"), or -1 on the tab itself.
    static int OpenPanel(const std::string& screen) {
        const auto slash = screen.find('/');
        return slash == std::string::npos ? -1 : std::atoi(screen.c_str() + slash + 1);
    }

    // The pages outside the tabs, each one builder's rows.
    struct Page { const char* screen; const char* title; SettingsPanel::Build rows; };
    static const Page* PageOf(const std::string& screen) {
        static const Page pages[] = {
            {"player", "player.title", &Controls},
            {"controllers", "controllers.title", &ControllerList},
            {"profile", "profile.title", &Profile},
            {"main-character", "profile.choose_main_title", &MainCharacters},
            {"sent-reports", "reports.sent_list", &SentReports},
            {"about", "about.title", &About}};
        for (const auto& p : pages) if (screen == p.screen) return &p;
        return nullptr;
    }

    // The builders. Each says what its rows are; a tab's panel and a page share them.
    static Entries Controls(Shell& s, const ShellView& v) {
        return PlayerControllerRows(s.preferences_, v.controller, v.inputDevice.type, v.gameplayBrand, v.pads,
            gamepads::LayerPending(s.preferences_.controllerLayer, v.runningControllerLayer), v.canChangeController, v.canEditPreferences,
            v.session.room == netplay::RoomState::Idle, v.padLayer);
    }
    static Entries ControllerList(Shell&, const ShellView& v) { return ControllerRows(v.padLayer, v.pads, v.canChangeController, v.canEditPreferences); }
    // The flag beside the player's name; Select opens the country picker.
    static Entries Country(Shell& s, const ShellView& v) {
        auto row = Row("country", loc::T("settings.country"), v.canEditPreferences ? loc::T("settings.country_detail") : Reason(v), v.canEditPreferences);
        row.value = CountryPicker::ChoiceText(s.preferences_.country, v.automaticCountry);
        return {row};
    }
    static Entries Profile(Shell& s, const ShellView& v) {
        const auto& p = s.preferences_;
        const auto& record = v.preferences.record;
        Entries rows = {TextRow("name", loc::T("profile.player_name"), p.displayName, 31, v.canEditPreferences),
            Row("main-character", loc::T("profile.main_character"), loc::Tf("profile.main_character_detail", selection::FindFighter(p.mainFighter)->name)),
            Row("record", loc::T("profile.record"), record.available ? loc::Tf("profile.record_detail", record.wins, record.losses) : loc::T("profile.record_unavailable"))};
        rows[2].info = true;
        rows[1].value = selection::FindFighter(p.mainFighter)->name;
        rows[2].value = record.available ? loc::Tf("profile.record_value", record.wins, record.losses) : loc::T("common.unavailable");
        rows.push_back(OnOffValue(p, "share-steam", loc::T("profile.share_steam"), loc::T("profile.share_steam_detail"), v.canEditPreferences));
        rows.push_back(Row("awards", loc::T("screen.awards"), loc::T("awards.entry_detail")));
        rows.back().value = EarnedCount(v);
        return rows;
    }
    static Entries MainCharacters(Shell&, const ShellView& v) {
        Entries rows;
        for (const int id : selection::RosterDisplayOrder) {
            rows.push_back(Row("main-" + std::to_string(id), selection::FindFighter(id)->name,
                v.canEditPreferences ? loc::T("profile.choose_main_detail") : loc::T("profile.leave_room_to_edit"), v.canEditPreferences));
            rows.back().hint = loc::T("menu.hint.save_main");
            rows.back().key = static_cast<std::uint64_t>(id);
        }
        return rows;
    }
    // Create and Defaults edit the same room settings (RoomSetupRows); the title is the page's.
    static Entries Defaults(Shell& s, const ShellView& v) {
        std::string title;
        return s.RoomSetupRows(v, "defaults", v.session.room == netplay::RoomState::Opening, Reason(v), title);
    }
    static Entries TrainingReplays(Shell& s, const ShellView& v) {
        const auto& p = s.preferences_;
        Entries rows = {OnOffValue(p, "match-frame-meter", loc::T("settings.match_frame_meter"),
                loc::T(match_frame_meter::Allowed(v.room.serverOwned) ? "settings.match_frame_meter_detail" : "settings.match_frame_meter_private"), v.canEditPreferences),
            OnOffValue(p, "training-auto-ready", loc::T("settings.training_auto_ready"), loc::Tf("settings.training_auto_ready_detail", room::TrainingCall::ReadyWindowMs / 1000), v.canEditPreferences),
            OnOffValue(p, "replay-save-watched", loc::T("replays.save_watched"), loc::T("replays.save_watched_detail"), v.canEditPreferences),
            OnOffValue(p, "replay-share", loc::T("replays.share"), loc::T("replays.share_detail"), v.canEditPreferences),
            PictureMode(s, v)};
        // Where the picture sits and how big (the Matches tab's map also drags it there).
        const int corners[] = {4, 3, 2, 1};
        const char* sizes[] = {loc::T("size.small"), loc::T("size.standard"), loc::T("size.large")};
        const bool shown = v.canEditPreferences && p.pictureMode != netplay::PictureOff;
        rows.push_back(Value("picture-corner", loc::T("settings.picture_corner"), MatchStripAnchorName(corners[p.pictureSpot % 4]), loc::T("settings.picture_corner_detail"), shown));
        rows.push_back(Value("picture-size", loc::T("settings.picture_size"), sizes[p.pictureSpot / 4], loc::T("settings.picture_size_detail"), shown));
        return rows;
    }
    // The watched game's picture: Small, Big or Off (PlayerPreferences::pictureMode).
    static MenuEntry PictureMode(Shell& s, const ShellView& v) {
        const char* const names[] = {"size.small", "size.large", "room.v2.picture_none"};
        return Value("watched-picture", loc::T("settings.watched_picture"), loc::T(names[s.preferences_.pictureMode]), loc::T("settings.watched_picture_detail"), v.canEditPreferences);
    }
    static MenuEntry Tv(Shell& s, const ShellView& v) {
        return Value("tv", loc::T("settings.tv_mode"), s.preferences_.tvMode ? loc::T("common.on") : loc::T("common.off"), loc::T("settings.tv_mode_detail"), v.canEditPreferences);
    }
    static Entries Look(Shell& s, const ShellView& v) { return {Tv(s, v)}; }
    static Entries MatchHud(Shell& s, const ShellView& v) {
        const auto& p = s.preferences_;
        const char* sizes[] = {loc::T("size.small"), loc::T("size.standard"), loc::T("size.large")};
        return {OnOffValue(p, "hud", loc::T("settings.match_hud"), Reason(v), v.canEditPreferences),
            Value("hud-layout", loc::T("settings.match_hud_layout"), MatchStripLayoutName(p.matchHudLayout), loc::T("settings.match_hud_layout_detail"), v.canEditPreferences),
            Value("hud-name-offset", loc::T("settings.name_offset"), MatchHudNameOffsetText(p.matchHudNameOffset), loc::T("settings.name_offset_detail"), v.canEditPreferences && p.matchHudLayout == 1),
            Value("hud-size", loc::T("settings.match_hud_size"), sizes[(std::max)(0, (std::min)(2, p.matchHudSize))], loc::T("settings.match_hud_size_detail"), v.canEditPreferences),
            Value("hud-position", loc::T("settings.match_hud_position"), MatchStripAnchorName(p.matchHudAnchor), loc::T("settings.match_hud_position_detail"), v.canEditPreferences),
            Value("hud-spacing", loc::T("settings.edge_spacing"), p.matchHudRaised ? loc::T("spacing.raised") : loc::T("spacing.normal"), loc::T("settings.edge_spacing_detail"), v.canEditPreferences)};
    }
    static Entries Sounds(Shell& s, const ShellView& v) {
        const auto& p = s.preferences_;
        return {OnOffValue(p, "ready-sound", loc::T("settings.ready_sound"), loc::T("settings.ready_sound_detail"), v.canEditPreferences),
            Value("ready-volume", loc::T("settings.ready_sound_volume"), std::to_string(p.readySoundVolume) + "%", loc::T("settings.ready_sound_volume_detail"), v.canEditPreferences && p.readySound),
            Row("ready-test", loc::T("settings.ready_sound_test"), loc::T("settings.ready_sound_test_detail"), v.canEditPreferences && p.readySound),
            OnOffValue(p, "lobby-music", loc::T("settings.lobby_music"), loc::T("settings.lobby_music_detail"), v.canEditPreferences && v.lobbyMusicAvailable)};
    }
    static Entries Help(Shell& s, const ShellView& v) {
        return {OnOffValue(s.preferences_, "shortcut-hint", loc::T("settings.shortcut_hint"), loc::T("settings.shortcut_hint_detail"), v.canEditPreferences)};
    }
    static Entries Rankings(Shell& s, const ShellView& v) {
        return {OnOffValue(s.preferences_, "class-emblems", loc::T("settings.class_emblems"), loc::T("settings.class_emblems_detail"), v.canEditPreferences)};
    }
    static Entries Text(Shell& s, const ShellView& v) {
        char size[32];
        std::snprintf(size, sizeof(size), "%.2fx", s.preferences_.interfaceScale);
        return {Value("scale", loc::T("settings.interface_size"), size, Reason(v), v.canEditPreferences), Tv(s, v), s.LanguageRow()};
    }
    // The room board's layout, which its grips also drag (RoomGrips). Its parts:
    // hiding one gives its place to the other, or both to the tables.
    static Entries RoomLayout(Shell& s, const ShellView& v) {
        const auto& p = s.preferences_;
        const int column = p.roomColumn, split = p.roomSplit;
        Entries rows = {Value("arrange-column", loc::T("settings.room_column"), std::to_string(column) + "%", loc::T("settings.room_column_detail"), true),
            Value("arrange-split", loc::T("settings.room_split"), std::to_string(split) + " / " + std::to_string(100 - split), loc::T("settings.room_split_detail"), true),
            Value("arrange-preset", loc::T("settings.room_preset"), loc::T(RoomPresetKeys[RoomPreset(column)]), loc::T("settings.room_preset_detail"), true),
            Value("arrange-members", loc::T("room.members"), loc::T(p.roomShowMembers ? "common.on" : "common.off"), loc::T("settings.room_show_members_detail"), true),
            Value("arrange-chat", loc::T("room.chat"), loc::T(p.roomShowChat ? "common.on" : "common.off"), loc::T("settings.room_show_chat_detail"), true),
            PictureMode(s, v),
            Row("arrange-reset", loc::T("settings.room_layout_reset"), loc::T("settings.room_layout_reset_detail"), true)};
        rows[5].id = "arrange-picture";
        return rows;
    }
    static Entries Speech(Shell& s, const ShellView& v) { return s.AccessibilityRows(v); }
    static Entries Discord(Shell& s, const ShellView& v) {
        const auto& p = s.preferences_;
        return {OnOffValue(p, "presence", loc::T("discord.show_activity"), v.discordStatus, v.canEditPreferences),
            OnOffValue(p, "invites", loc::T("discord.allow_invitations"), p.discordPresence ? loc::T("discord.invitation_detail") : loc::T("discord.enable_first"), v.canEditPreferences && p.discordPresence),
            // The account tournaments use, which may not be the one in the Discord app.
            s.identity_.TournamentAccountRow(v)};
    }
    // The setting's pane says what a report holds; Select reads it in full.
    static Entries Reports(Shell& s, const ShellView& v) {
        auto send = OnOffValue(s.preferences_, "send-reports", loc::T("reports.setting"),
            loc::Tf("reports.setting_detail", reports::AutomaticPerDay) + "\n\n" + loc::T("reports.payload") + "\n\n" + loc::T("reports.what_is_sent"), v.canEditPreferences);
        send.reading = true;
        Entries rows = {send, Row("sent-reports", loc::T("reports.sent_list"), loc::T("reports.sent_list_detail"))};
        rows.back().value = std::to_string(v.services.sentReports.size());
        return rows;
    }
    // Newest first: when, how it went, and the ID the report service gave it.
    static Entries SentReports(Shell&, const ShellView& v) {
        Entries rows;
        const auto& sent = v.services.sentReports;
        // An unreadable record says so first: it is why nothing goes without asking.
        if (!v.services.sentReadable) rows.push_back(InfoRow("sent-unreadable", loc::T("reports.history_unreadable"), "", loc::T("reports.history_unreadable_detail")));
        for (std::size_t i = sent.size(); i-- > 0;)
            rows.push_back(InfoRow("sent-" + std::to_string(i), ReportTime(sent[i].time), ReportStatusLabel(sent[i].status),
                std::string(loc::T(sent[i].kind == "problem" ? "reports.row" : "reports.crash_report")) + "\n\n" + ReportRecordText(sent[i])));
        if (rows.empty()) rows.push_back(InfoRow("sent-none", loc::T("reports.none"), "", loc::T("reports.none_detail")));
        return rows;
    }
    static Entries About(Shell& s, const ShellView& v) {
        using platform::ServiceAction;
        // Each row shows the service message only for the actions it requests.
        const auto outcome = [&](std::initializer_list<ServiceAction> own, std::string idle) {
            for (const auto action : own) if (v.services.lastAction == action && !v.services.message.empty()) return v.services.message;
            return idle;
        };
        // The controls, credits and licence are longer than the detail pane, so
        // Select opens them in a reader a pad or keyboard can scroll.
        const auto reading = [](MenuEntry e) { e.reading = true; return e; };
        s.channelPick_.Settle(v.services.channel, v.services.pending);
        Entries rows = {reading(Row("help", loc::T("about.controls"), std::string(loc::T("about.controls_detail")) + "\n\n" + loc::T("about.controls_keyboard"))),
            reading(Row("credits", loc::T("about.ember"), loc::Tf("about.ember_detail", v.build))),
            reading(Row("font", loc::T("about.font_license"), FontLicense())),
            Row("diagnostics", loc::T("about.export_diagnostics"), outcome({ServiceAction::ExportDiagnostics}, loc::T("about.export_diagnostics_detail")), !v.services.pending),
            Row("problem-report", loc::T("reports.problem"), loc::T("reports.problem_detail"), !v.services.pending),
            ConfirmRow("community", loc::T("about.discord"), outcome({ServiceAction::OpenCommunity}, loc::Tf("about.discord_detail", platform::CommunityInvite)), !v.services.pending),
            // The version on this PC and the channel its update checks use, as Home's
            // corner shows them, above the check itself; the build in its pane.
            InfoRow("version", loc::T("updates.installed_version"), VersionLine(InstalledVersion(v), v.services.channel),
                v.build.empty() ? std::string(loc::T("updates.installed_version_detail")) : loc::Tf("about.version_detail", v.build)),
            ChannelRow(v.services.channel, &s.channelPick_, v.services.pending),
            Row("updates", loc::T("updates.check"), outcome({ServiceAction::CheckUpdates, ServiceAction::InstallUpdate, ServiceAction::OpenRecovery}, loc::T("about.updates_detail")), !v.services.pending),
            // Saved on its own, as the language is: it is no room or fighter preference.
            Value("launch-update-check", loc::T("settings.update_check"), s.launchCheck_ ? loc::T("common.on") : loc::T("common.off"),
                s.launchCheckSaveError_.empty() ? std::string(loc::T("settings.update_check_detail")) : s.launchCheckSaveError_)};
        // The found release, as the launch question offers it: the game closes and the Updates window installs it.
        if (const auto& offer = v.services.offer)
            rows.push_back(ConfirmRow("update-now", loc::T("updates.launch_now"),
                v.canEditPreferences ? loc::Tf("updates.launch_text", offer->tag, InstalledVersion(v)) : std::string(loc::T("about.leave_room_first")), v.canEditPreferences && !v.services.pending));
        if (v.network == netplay::NetworkAvailability::Unavailable)
            rows.push_back(ConfirmRow("recovery", loc::T("about.open_recovery"),
                v.canEditPreferences ? loc::T("about.open_recovery_detail") : loc::T("about.leave_room_first"), v.canEditPreferences && !v.services.pending));
        return rows;
    }

    // The pictures beside a panel's rows. The Player tab shows the profile card
    // (the L tag) as others will see it.
    static void ProfileCard(Shell&, const ShellView&) {
        ImGui::Dummy(ImVec2(0, 8 * Scale()));
        const auto p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(0, v2::Tag(ImGui::GetWindowDrawList(), p, ImGui::GetContentRegionAvail().x, MenuPlayerCard().profile, v2::TagSize::L)));
    }
    static void HudStrip(const netplay::PlayerPreferences& p) {
        MatchStripView preview;
        preview.names[0] = loc::T("settings.player_one");
        preview.names[1] = loc::T("settings.player_two");
        preview.pingMs = 68; preview.rollbackFrames = 2; preview.appliedDelay = 3;
        preview.size = p.matchHudSize; preview.raised = p.matchHudRaised; preview.anchor = p.matchHudAnchor; preview.layout = p.matchHudLayout; preview.nameOffset = p.matchHudNameOffset;
        preview.spectators = 2;
        DrawMatchStripPreview(preview);
    }
    // The match HUD, and where it sits: drag it to another place, or click one.
    static void MatchHudPicture(Shell& s, const ShellView& v) {
        auto& p = s.preferences_;
        HudStrip(p);
        if (!v.canEditPreferences) return;
        ImGui::Dummy(ImVec2(0, 8 * Scale()));
        const int picked = v2::SpotMap("##hud-spot", {{.5f, .88f}, {.16f, .88f}, {.84f, .88f}, {.16f, .12f}, {.84f, .12f}, {.5f, .12f}}, p.matchHudAnchor, {.26f, .1f});
        if (picked >= 0 && picked != p.matchHudAnchor) { p.matchHudAnchor = picked; s.preferencesDirty_ = true; s.saveAt_ = ImGui::GetTime() + .45; }
    }
    // Where the watched game's picture sits and how big, on a small screen. The
    // corners in pictureSpot's order: top right, top left, bottom right, bottom left.
    static void PictureSpot(Shell& s, const ShellView& v) {
        auto& p = s.preferences_;
        const float share = WatchedPictureShareFor(p.pictureSpot), c = share * .5f + .03f;
        const int corner = p.pictureSpot % 4;
        const int picked = v2::SpotMap("##picture-spot", {{1 - c, c}, {c, c}, {1 - c, 1 - c}, {c, 1 - c}}, corner, {share, share});
        if (picked >= 0 && picked != corner && v.canEditPreferences) { p.pictureSpot = p.pictureSpot / 4 * 4 + picked; s.preferencesDirty_ = true; s.saveAt_ = ImGui::GetTime() + .45; }
    }
    // The room board as the layout arranges it, with the grips that drag it.
    static void RoomLayoutPicture(Shell& s, const ShellView&) { s.DrawRoomLayoutPreview(); }

    // A tab's screen: its panels built once for the frame, then a row per
    // panel, or the open panel's own rows.
    static Entries TabRows(Shell& s, const ShellView& v, const Tab& tab, const std::string& screen) {
        auto& built = s.settingsPanels_;
        built.clear();
        for (const SettingsPanel* panel : tab.panels) {
            built.push_back({panel, panel->rows(s, v)});
            for (auto& row : built.back().rows) row.group = loc::T(panel->heading);
        }
        const int open = OpenPanel(screen);
        if (open >= 0) return open < static_cast<int>(built.size()) ? built[open].rows : Entries{};
        Entries heads;
        for (std::size_t i = 0; i < built.size(); ++i) {
            // The head says what its panel holds: its first values, and every row's name in its detail.
            std::string summary, names;
            int values = 0;
            for (const auto& row : built[i].rows) {
                names += (names.empty() ? "" : " \xC2\xB7 ") + row.label;
                if (!row.info && !row.value.empty() && values++ < 3) summary += (summary.empty() ? "" : " \xC2\xB7 ") + row.value;
            }
            heads.push_back(Row(screen + "/" + std::to_string(i), loc::T(built[i].panel->heading), names));
            heads.back().value = summary;
            heads.back().hint = loc::T("settings.panel.open_action");
        }
        return heads;
    }
    static void DrawBoard(Shell& shell, const ShellView& v, const Entries& entries, MenuNavigation& nav, MenuAction& action, float height, const MenuVisualFeedback& feedback);

    bool Owns(const std::string& screen) const override {
        return TabOf(screen) || PageOf(screen) || screen == CountryPicker::Screen;
    }
    Entries Rows(const ScreenFrame& f, bool, std::string& title) const override {
        auto& s = f.shell;
        if (const Tab* tab = TabOf(f.screen)) {
            title = loc::T("home.settings");
            return TabRows(s, f.view, *tab, f.screen);
        }
        if (f.screen == CountryPicker::Screen) return CountryRows(s, f.view, title);
        const Page& page = *PageOf(f.screen);
        title = loc::T(page.title);
        return page.rows(s, f.view);
    }
    // The letters board, where the language's initials fit a grid. A typed
    // letter goes to its first country, and the board shows that letter's.
    static Entries CountryRows(Shell& s, const ShellView& v, std::string& title);
    bool ReportsSave(const std::string& screen) const override {
        // About and the sent reports have no Saved line.
        return Owns(screen) && screen != "about" && screen != "sent-reports";
    }
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        const auto& s = f.shell;
        if (f.screen == CountryPicker::Screen) {
            // The header names the choice in use; a typed letter goes to a country, so letters are no shortcuts here.
            menu.headerChips.push_back({CountryPicker::ChoiceText(s.preferences_.country, f.view.automaticCountry), v2::Colors().accentLight});
            menu.typesText = true;
        }
        const Tab* tab = TabOf(f.screen);
        if (!tab) return;
        // Settings · Tab, and · Panel while one is open.
        menu.eyebrow = std::string(loc::T("home.settings")) + " \xC2\xB7 " + loc::T(tab->label);
        const int open = OpenPanel(f.screen);
        if (open >= 0 && open < static_cast<int>(s.settingsPanels_.size())) menu.eyebrow += std::string(" \xC2\xB7 ") + loc::T(s.settingsPanels_[open].panel->heading);
        if (open >= 0) menu.backHint = loc::T("settings.panel.close_action");
        for (const auto& t : Tabs()) {
            if (t.screen == tab->screen) menu.tab = static_cast<int>(menu.tabs.size());
            menu.tabs.push_back({t.screen, loc::T(t.label)});
        }
    }
    GameMenu::Body Body(const ScreenFrame& f) const override {
        Shell* s = &f.shell; const ShellView* v = &f.view;
        if (SettingsTab(f.screen))
            return [s, v](const std::vector<MenuEntry>& e, MenuNavigation& n, MenuAction& a, float h, const MenuVisualFeedback& fb) { DrawBoard(*s, *v, e, n, a, h, fb); };
        if (f.screen == CountryPicker::Screen && s->countryPicker_.Lettered())
            return [s, v](const std::vector<MenuEntry>& e, MenuNavigation& n, MenuAction& a, float, const MenuVisualFeedback&) { s->countryPicker_.DrawBoard(e, n, a, v->automaticCountry); };
        return {};
    }
    GameMenu::Card Card(const ScreenFrame& f, double) const override {
        Shell* s = &f.shell; const ShellView* v = &f.view;
        if (f.screen == CountryPicker::Screen)
            return [s, v](const MenuEntry& e, ImVec2 min, ImVec2 max) { return s->countryPicker_.DrawRow(e, v->automaticCountry, min, max); };
        // The main-character grid is fighter select's.
        if (f.screen == "main-character")
            return [v](const MenuEntry& e, ImVec2 min, ImVec2 max) {
                if (e.id.compare(0, 5, "main-") != 0) return false;
                const int id = static_cast<int>(e.key);
                DrawMainPortrait(id, id == v->preferences.mainFighter, min, max);
                return true;
            };
        return {};
    }
    ScreenGrid Grid(const ScreenFrame& f) const override {
        if (f.screen != "main-character") return {};
        const RosterGrid roster = LayOutRosterGrid(ImGui::GetContentRegionAvail().x);
        return {roster.columns, roster.cardHeight};
    }
    GameMenu::Detail Detail(const ScreenFrame& f) const override {
        Shell* s = &f.shell; const ShellView* v = &f.view;
        const std::string screen = f.screen;
        // A tab's panel shows its picture whichever of its rows a reader reads.
        if (SettingsTab(screen))
            return [s, v](const std::string& id) {
                for (const auto& b : s->settingsPanels_)
                    if (b.panel->picture && std::any_of(b.rows.begin(), b.rows.end(), [&](const MenuEntry& e) { return e.id == id; })) { b.panel->picture(*s, *v); return; }
            };
        if (screen == "profile" || screen == "main-character")
            return [v, screen](const std::string& id) {
                const auto chosen = screen == "main-character" ? RowKey<int>(id, "main-") : std::nullopt;
                const int fighter = chosen.value_or(v->preferences.mainFighter);
                const auto space = ImGui::GetContentRegionAvail();
                const float size = (std::min)(220 * Scale(), (std::min)(space.x, space.y - 12 * Scale()));
                if (size >= 48 * Scale()) {
                    const auto p = ImGui::GetCursorScreenPos();
                    DrawCharacterPortrait(fighter, p, ImVec2(p.x + size, p.y + size));
                    ImGui::Dummy(ImVec2(size, size));
                }
            };
        if (screen == "controllers")
            return [s, v](const std::string& id) {
                const auto pad = std::find_if(v->pads.begin(), v->pads.end(), [&](const gamepads::PadView& p) { return id == "pad-" + std::to_string(p.id); });
                if (pad != v->pads.end()) DrawControllerDetail(*pad, s->preferences_.buttonPrompts);
            };
        return {};
    }
};

std::vector<MenuEntry> ApplicationShell::SettingsScreens::CountryRows(Shell& s, const ShellView& v, std::string& title) {
    auto& picker = s.countryPicker_;
    auto& nav = s.menu_.navigation;
    const auto build = [&](const std::string& letter) {
        return picker.Lettered() ? picker.BoardRows(s.preferences_.country, v.automaticCountry, v.canEditPreferences, letter) :
            picker.Rows(s.preferences_.country, v.automaticCountry, v.canEditPreferences);
    };
    title = loc::T("settings.country");
    auto rows = build(nav.Focus());
    if (!nav.Editing())
        for (const ImWchar typed : ImGui::GetIO().InputQueueCharacters) {
            const auto row = picker.Jump(typed, nav.Focus());
            if (row.empty()) continue;
            if (picker.Lettered()) rows = build(row);
            nav.Focus(row, rows);
        }
    return rows;
}

const ScreenPanel& ApplicationShell::SettingsOwner() {
    static const SettingsScreens settings{};
    return settings;
}
bool ApplicationShell::SettingsTab(const std::string& screen) { return screen.compare(0, 4, "tab-") == 0; }
std::string ApplicationShell::SettingsTabOf(const std::string& screen) { return screen.substr(0, screen.find('/')); }

MenuEntry ApplicationShell::LanguageRow() const {
    const auto value = languagePreference_ == "auto" ? loc::Tf("settings.language.system_with", loc::NativeName(loc::Active())) :
        std::string(loc::NativeName(loc::ResolveLocale(languagePreference_, {}, {})));
    auto row = Value("language", loc::T("settings.language"), value, languageSaveError_.empty() ? std::string(loc::T("settings.language.detail")) : languageSaveError_, true);
    // Select lists the languages by their own names; browsing them changes nothing.
    row.choices = LanguageChoices();
    row.chosen = languagePreference_;
    return row;
}

bool ApplicationShell::AdjustSetting(netplay::PlayerPreferences& p, const MenuAction& a) {
    using namespace netplay;
    const auto clamp = [](int value, int least, int most) { return (std::max)(least, (std::min)(most, value)); };
    const std::string& id = a.id;
    if (id == "hud-layout") p.matchHudLayout = clamp(p.matchHudLayout + a.delta, 0, 1);
    else if (id == "hud-name-offset") p.matchHudNameOffset = clamp(p.matchHudNameOffset + 2 * a.delta, -MaxMatchHudNameOffset, MaxMatchHudNameOffset);
    else if (id == "hud-size") p.matchHudSize = clamp(p.matchHudSize + a.delta, 0, 2);
    else if (id == "hud-position") p.matchHudAnchor = clamp(p.matchHudAnchor + a.delta, 0, MaxMatchHudAnchor);
    else if (id == "hud-spacing") p.matchHudRaised = a.delta > 0;
    else if (id == "arrange-column") p.roomColumn = clamp(p.roomColumn + RoomColumnStep * a.delta, MinRoomColumn, MaxRoomColumn);
    else if (id == "arrange-split") p.roomSplit = clamp(p.roomSplit + RoomSplitStep * a.delta, MinRoomSplit, MaxRoomSplit);
    else if (id == "arrange-preset") p.roomColumn = RoomPresets[clamp(RoomPreset(p.roomColumn) + a.delta, 0, 2)];
    else if (id == "arrange-members") p.roomShowMembers = a.delta > 0;
    else if (id == "arrange-chat") p.roomShowChat = a.delta > 0;
    else if (id == "tv") p.tvMode = a.delta > 0;
    else if (id == "picture-corner") p.pictureSpot = p.pictureSpot / 4 * 4 + (p.pictureSpot % 4 + 4 + a.delta) % 4;
    else if (id == "picture-size") p.pictureSpot = p.pictureSpot % 4 + 4 * clamp(p.pictureSpot / 4 + a.delta, 0, 2);
    else if (id == "watched-picture" || id == "arrange-picture") p.pictureMode = clamp(p.pictureMode + a.delta, PictureSmall, PictureOff);
    else if (id == "ready-volume") p.readySoundVolume = clamp(p.readySoundVolume + 10 * a.delta, 10, 100);
    else if (id == "scale") p.interfaceScale = (std::max)(1.f, (std::min)(1.5f, p.interfaceScale + .05f * a.delta));
    else return false;
    return true;
}

// A Settings tab, as the design's board: the rail of panels on the left
// (360 px wide with all closed, 220 while one is open), the panel's rows in a
// section beside it with its picture, and the focused row's words under them.
// Narrow, the rail and the open panel each take the whole width in turn.
void ApplicationShell::SettingsScreens::DrawBoard(Shell& shell, const ShellView& v, const std::vector<MenuEntry>& entries, MenuNavigation& nav, MenuAction& action,
    float height, const MenuVisualFeedback& feedback) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, font = ImGui::GetFontSize();
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const auto& panels = shell.settingsPanels_;
    const std::string& screen = nav.Screen(), tab = SettingsTabOf(screen), focus = nav.Focus();
    const bool open = tab != screen, narrow = v2::Narrow(ImGui::GetWindowWidth(), s), changed = focus != shell.settingsFocus_;
    shell.settingsFocus_ = focus;
    // The panel shown is the open one, else the focused head's.
    const std::string& shownId = open ? screen : focus;
    const int current = shownId.size() > tab.size() ? std::atoi(shownId.c_str() + tab.size() + 1) : 0;
    if (panels.empty() || current >= static_cast<int>(panels.size())) return;
    const BuiltPanel& shown = panels[current];

    v2::Box body{ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + width, ImGui::GetCursorScreenPos().y + height)};
    v2::CutTop(body, 0, 8 * s);
    // The rail: a head per panel with its arrow, name, and while all are closed
    // its summary and state word. The heading gives way last: the state word
    // drops first (from every head, so the column stays even), then a heading
    // too wide for its room wraps and its head grows (v2::Heading).
    if (!narrow || !open) {
        const v2::Box rail = v2::CutLeft(body, narrow ? width : (open ? 220 : 360) * s, 16 * s);
        const float x = rail.min.x + 48 * s, size = (open ? 16 : 20) * s;
        const char* state = loc::T("settings.panel.closed");
        float sw = open ? 0 : FLT_MAX;
        for (const auto& p : panels) sw = (std::min)(sw, v2::HeadWord(loc::T(p.panel->heading), size, state, rail.max.x - 10 * s - x, 10 * s));
        const float room = rail.max.x - 10 * s - x - (sw > 0 ? sw + 10 * s : 0);
        float y = rail.min.y;
        for (int i = 0; i < static_cast<int>(panels.size()); ++i) {
            const char* heading = loc::T(panels[i].panel->heading);
            MenuEntry head;
            head.id = tab + "/" + std::to_string(i);
            const std::string& id = head.id;
            const bool isOpen = open && i == current, focused = !open && id == focus;
            const float grows = v2::HeadingHeight(heading, room, size) - size;
            const v2::Box h{ImVec2(rail.min.x, y), ImVec2(rail.max.x, y + (open ? 52 : 64) * s + grows)};
            y = h.max.y + 8 * s;
            const auto [pressed, hovered] = PressEntry(nav, entries, head, h, action, false, {}, false);
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
            if (open) v2::Heading(d, ImVec2(x, h.min.y + (h.H() - size - grows) * .5f), room, heading, size, c.text);
            else {
                if (sw > 0) v2::Text(d, ImVec2(h.max.x - 10 * s - sw, h.min.y + (h.H() - font) * .5f), sw, state, font, c.muted);
                v2::Heading(d, ImVec2(x, h.min.y + 10 * s), room, heading, size, c.text);
                // The summary is a preview of the panel's values (the player's name among them), cut where it must be.
                const auto row = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == id; });
                if (row != entries.end()) v2::Text(d, ImVec2(x, h.min.y + 36 * s + grows), room, row->value, 14 * s, c.muted, nullptr, false, false);
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
    v2::Box section = v2::Panel(d, column, loc::T(shown.panel->heading), loc::T(open ? "settings.panel.open" : "settings.preview"), open);

    // The picture, beside the rows, for a panel with something to show.
    if (shown.panel->picture && !narrow && section.W() >= 760 * s) {
        const v2::Box pv = v2::CutRight(section, 380 * s);
        v2::SectionHeading(d, ImVec2(pv.min.x, pv.min.y + 12 * s), loc::T("settings.preview"));
        ImGui::SetCursorScreenPos(ImVec2(pv.min.x, pv.min.y + 32 * s));
        ImGui::BeginChild("settings preview", ImVec2(pv.W() - 12 * s, pv.H() - 44 * s), 0, ImGuiWindowFlags_NoNavInputs);
        shown.panel->picture(shell, v);
        ImGui::EndChild();
    }
    // The rows: on the Ember rule while the panel is open, dimmed while it is only shown.
    if (open) d->AddRectFilled(section.min, ImVec2(section.min.x + 3 * s, section.max.y), c.accent);
    const v2::Box list = v2::Inset(section, 12 * s, 8 * s);
    ImGui::SetCursorScreenPos(list.min);
    ImGui::BeginChild("settings rows", ImVec2(list.W(), list.H()), 0, ImGuiWindowFlags_NoNavInputs);
    // The open panel's rows are the screen's entries: its values offered, and
    // the shell's own (Retry saving). A panel only shown draws its rows as built.
    for (const MenuEntry& e : open ? entries : shown.rows) {
        const auto p = ImGui::GetCursorScreenPos();
        // While closed a row opens its panel; while open its value's ends step it, anything else on it is Select.
        const auto row = shell.menu_.DrawBoardRow(entries, e, p, list.W(), action, {list, open && feedback.Enabled(e), open && e.id == focus && changed, open});
        if (row.hit.pressed && !open) action = {MenuAction::Shortcut, tab + "/" + std::to_string(current)};
        ImGui::SetCursorScreenPos(ImVec2(p.x, row.box.max.y + 4 * s));
    }
    ImGui::EndChild();
}
} }
