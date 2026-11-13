#include "ApplicationShell.hxx"
#include "MenuPresentation.hxx"
#include "PlayerCard.hxx"
#include "NetworkFeedback.hxx"
#include "FriendsPanel.hxx"
#include "PublicRoomsCards.hxx"
#include "RowId.hxx"
#include "RankingsCards.hxx"
#include "ScreenPanel.hxx"
#include "SignInSites.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <imgui.h>
#include <cstring>

// The panels that own screens, as the shell routes a drawn frame's parts to
// them (ScreenPanel). Each holds nothing of its own: it reaches the panel, and
// the shell's state that the panel's rows need, through the frame's shell.
namespace sf4e { namespace ui {

// The Ember ID screens: the identity, its backup, linked accounts, tournament
// matches, Connect Discord and the sign-in sites' Connect screens.
class ApplicationShell::IdentityScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return IdentityPanel::Owns(screen); }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override {
        auto rows = f.shell.identity_.Rows(f.view, f.screen, title);
        // A match link's row is focused once the matches screen lists it.
        if (f.screen == "tournament-matches") {
            const auto focus = f.shell.identity_.TakeFocus(rows);
            if (!focus.empty()) f.shell.menu_.navigation.Focus(focus, rows);
        }
        return rows;
    }
    // The panel's own line, when it has one, replaces the shell's.
    void Status(const ScreenFrame& f, std::string& status, Tone& tone) const override {
        std::string own; Tone ownTone = Tone::Neutral;
        if (f.shell.identity_.Status(own, ownTone, ImGui::GetTime())) { status = own; tone = ownTone; }
    }
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        f.shell.identity_.Activate(a, f.view, f.shell.menu_.navigation);
        return true;
    }
    bool Adjust(const ScreenFrame& f, const MenuAction& a) const override {
        f.shell.identity_.Accept(a, f.view);
        return true;
    }
    bool Choose(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        f.shell.identity_.Accept(a, f.view);
        return true;
    }
};

// Public rooms: the list, its toolbar and cards. Quick join and the one-press
// setup are the shell's and the Ember ID panel's, pressed from here.
class ApplicationShell::PublicRoomsScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return PublicRoomsPanel::Owns(screen); }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override {
        auto& shell = f.shell;
        if (f.screen == PublicRoomsPanel::FiltersScreen) {
            title = loc::T("public.filters");
            return PublicRoomsPanel::FilterRows(shell.preferences_.roomFilter, PublicRoomsPanel::KnownRegion(f.view), f.view.canEditPreferences);
        }
        title = loc::T("screen.public_rooms");
        return shell.publicRooms_.Rows(f.view, shell.identity_.UsableBridge(f.view), shell.identity_.Waiting(), shell.identity_.PublicSetup(), PublicListWidth(),
            shell.preferences_.roomFilter);
    }
    // Also asked on Create (UpdateStatus), with that screen.
    void Status(const ScreenFrame& f, std::string& status, Tone& tone) const override {
        std::string own = status; Tone ownTone = tone;
        if (f.shell.publicRooms_.Status(f.view, f.screen, own, ownTone, ImGui::GetTime(), f.shell.preferences_.roomFilter)) { status = own; tone = ownTone; }
    }
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        auto& shell = f.shell; auto& nav = shell.menu_.navigation;
        if (a.id == "pr-quick") { shell.QuickJoin(f.view); return true; }
        // The confirmed setup runs in the Ember ID panel; a locked ID is unlocked on its own screen first.
        if (a.id == "pr-setup") { shell.identity_.SetUpPublicRooms(f.view); shell.error_.clear(); return true; }
        if (a.id == "pr-setup-id") { shell.identity_.ClearPublicSetup(); nav.Push("identity"); return true; }
        return ChangeFilter(f, a) || shell.publicRooms_.Activate(a, nav);
    }
    // Left and Right on a filter row, as the preferences' own rows save.
    bool Adjust(const ScreenFrame& f, const MenuAction& a) const override { return ChangeFilter(f, a); }
    void Shortcut(const ScreenFrame& f, const MenuAction& a) const override { f.shell.publicRooms_.Shortcut(a, f.view); }
    // The toolbar, room cards and state cards are drawn by PublicRoomsCards. The
    // cards get the setup and each card's phase from the panels, not from the rows.
    GameMenu::Card Card(const ScreenFrame& f, double now) const override {
        ApplicationShell* shell = &f.shell; const ShellView* view = &f.view;
        const PublicSetupView setup = shell->identity_.PublicSetup();
        return [shell, view, setup, now](const MenuEntry& e, ImVec2 min, ImVec2 max) {
            return DrawPublicCard(*view, shell->publicRooms_, setup, shell->publicRooms_.PhaseOf(e.id), e, min, max,
                e.id == shell->menu_.navigation.Focus(), now);
        };
    }
    GameMenu::Detail Detail(const ScreenFrame& f) const override {
        ApplicationShell* shell = &f.shell; const ShellView* view = &f.view;
        return [shell, view](const std::string& id) { DrawPublicPreview(*view, shell->publicRooms_, id); };
    }
    // The filter is a saved preference, so the filters page says how its save stands.
    bool ReportsSave(const std::string& screen) const override { return screen == PublicRoomsPanel::FiltersScreen; }
    // The list leads with a toolbar of three cells, in one row on a wide list and two on a narrow one.
    ScreenGrid Grid(const ScreenFrame& f) const override {
        if (!f.rows || f.rows->empty() || f.rows->front().id != "pr-quick") return {};
        return {PublicListWidth() >= 600 * Scale() ? 3 : 2, 46};
    }
private:
    // The filter is a saved preference: a change takes the usual save, and only
    // while the preferences can be edited (not mid-room), as any setting does.
    static bool ChangeFilter(const ScreenFrame& f, const MenuAction& a) {
        auto& shell = f.shell;
        auto filter = shell.preferences_.roomFilter;
        if (!PublicRoomsPanel::ChangeFilter(a, PublicRoomsPanel::KnownRegion(f.view), filter)) return false;
        if (f.view.canEditPreferences && filter != shell.preferences_.roomFilter) {
            shell.preferences_.roomFilter = filter;
            shell.preferencesDirty_ = true; shell.saveAt_ = ImGui::GetTime() + .45; shell.error_.clear();
        }
        return true;
    }
public:
    // Public rooms say where they sit and the player's region, and Options
    // refreshes the list while it can. An opening room's card is information,
    // so the cursor waits on its Stop row.
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        const ShellView& v = f.view;
        menu.eyebrow = loc::T("online.eyebrow_browse");
        if (const char* region = RelayRegionName(PublicRoomsPanel::KnownRegion(v))) menu.headerChips.push_back({loc::Tf("public.your_region", region), palette::Ready});
        menu.paneWidth = 380;
        if (f.shell.publicRooms_.Refreshable(v)) menu.shortcutHints = ShortcutHints({{MenuInput::Options, loc::T("legend.refresh")}});
        if (v.session.room == netplay::RoomState::Opening && f.rows && menu.navigation.Focus() == "pr-opening") menu.navigation.Focus("cancel-open", *f.rows);
    }
};

// Rankings: the player's standing and the board.
class ApplicationShell::RankingsScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return RankingsPanel::Owns(screen); }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override {
        return f.shell.rankings_.Rows(f.view, f.screen, f.shell.identity_.Waiting(), title);
    }
    void Status(const ScreenFrame& f, std::string& status, Tone& tone) const override {
        std::string own = status; Tone ownTone = tone;
        if (f.shell.rankings_.Status(f.view, f.screen, own, ownTone)) { status = own; tone = ownTone; }
    }
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        auto& shell = f.shell; auto& nav = shell.menu_.navigation;
        // Connect Discord is the Ember ID panel's.
        if (a.id == DiscordConnectScreen) { shell.identity_.Activate(a, f.view, nav); return true; }
        return shell.rankings_.Activate(a, f.view, nav);
    }
    void Shortcut(const ScreenFrame& f, const MenuAction& a) const override { f.shell.rankings_.Shortcut(a); }
    GameMenu::Card Card(const ScreenFrame& f, double) const override {
        const ShellView* view = &f.view;
        return [view](const MenuEntry& e, ImVec2 min, ImVec2 max) { return DrawRankingsRow(*view, e, min, max); };
    }
    // The two pages, your own and the board, are tabs; Options refreshes them while it can.
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        menu.tabs = {{RankingsPanel::Screen, MenuScreenName(RankingsPanel::Screen)}, {RankingsPanel::BoardScreen, MenuScreenName(RankingsPanel::BoardScreen)}};
        menu.tab = f.screen == RankingsPanel::Screen ? 0 : 1;
        if (f.shell.rankings_.Refreshable()) menu.shortcutHints = ShortcutHints({{MenuInput::Options, loc::T("legend.refresh")}});
    }
};

// Profile: the player's Card and Fighters pages, under the strip Friends and
// the awards wall share.
class ApplicationShell::ProfileScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return ProfilePanel::Owns(screen); }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override {
        // The levels part of the card, as of this frame (the published card is last frame's).
        PlayerCardView levels;
        player_card::Fill(levels, f.view.levels, f.shell.preferences_.mainFighter);
        return f.shell.profile_.Rows(f.view, f.screen, levels, f.view.canEditPreferences, title);
    }
    // A strip tile opens the Fighters page on that fighter, Edit card its page; Connect Discord is the Ember ID panel's.
    // A fighter's row on the Fighters page makes it the main, which the shell's own rows do.
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        auto& nav = f.shell.menu_.navigation;
        if (a.id == DiscordConnectScreen) { f.shell.identity_.Activate(a, f.view, nav); return true; }
        if (a.id == CardEditRow) { nav.Push(CardEditRow); return true; }
        if (!HasPrefix(a.id, "card-fighter:")) return false;
        nav.Replace(ProfilePanel::FightersScreen);
        nav.Prefer("main-" + a.id.substr(std::strlen("card-fighter:")));
        return true;
    }
    // Y on the Card tab is Edit card.
    void Shortcut(const ScreenFrame& f, const MenuAction& a) const override {
        if (a.delta == MenuInput::Options && f.screen == ProfilePanel::CardScreen) f.shell.menu_.navigation.Push(CardEditRow);
    }
    GameMenu::Body Body(const ScreenFrame& f) const override {
        ApplicationShell* shell = &f.shell; const ShellView* view = &f.view; const std::string* screen = &f.screen;
        return [shell, view, screen](const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height, const MenuVisualFeedback&) {
            shell->profile_.Draw(*view, shell->preferences_.mainFighter, *screen, rows, nav, action, height);
        };
    }
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        ProfilePanel::Dress(menu, f.screen, f.shell.preferences_.displayName);
        // In a room X is the room's own shortcut on every screen (RoomShortcut).
        if (f.screen == ProfilePanel::CardScreen && f.view.session.room == netplay::RoomState::Idle) menu.shortcutHints = ShortcutHints({{MenuInput::Options, loc::T("card.edit")}});
    }
};

// Friends: the tab under Profile's strip, the pages over it (Add a friend, Sent, Blocked) and the
// question a Join asks. The tab draws a board of its own; the pages are lists.
class ApplicationShell::FriendsScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return FriendsPanel::Owns(screen); }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override {
        return f.shell.friends_.Rows(f.view, f.screen, f.shell.menu_.navigation.Choosing(), title);
    }
    // Connect Discord is the Ember ID panel's.
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit& submit) const override {
        auto& nav = f.shell.menu_.navigation;
        if (a.id == DiscordConnectScreen) { f.shell.identity_.Activate(a, f.view, nav); return true; }
        return f.shell.friends_.Activate(f.view, a, nav, submit);
    }
    bool Adjust(const ScreenFrame& f, const MenuAction& a) const override { return f.shell.friends_.Adjust(a); }
    bool Choose(const ScreenFrame& f, const MenuAction& a, const Submit& submit) const override { return f.shell.friends_.Choose(f.view, a, submit); }
    void Shortcut(const ScreenFrame& f, const MenuAction& a) const override { f.shell.friends_.Shortcut(f.view, a, f.shell.menu_.navigation); }
    // The tab and its pages take X, Y and Delete before the room's own, so More is reachable on a pad in a room.
    bool TakesShortcut(const ScreenFrame& f, const MenuAction& a) const override { return FriendsPanel::OnTab(f.screen) && FriendsPanel::Takes(a.delta); }
    GameMenu::Body Body(const ScreenFrame& f) const override {
        if (f.screen != FriendsPanel::Screen) return {};
        ApplicationShell* shell = &f.shell; const ShellView* view = &f.view;
        return [shell, view](const std::vector<MenuEntry>& rows, MenuNavigation&, MenuAction& action, float height, const MenuVisualFeedback&) {
            shell->friends_.Draw(*view, shell->menu_, rows, action, height);
        };
    }
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        if (f.screen == FriendsPanel::Screen) ProfilePanel::Dress(menu, f.screen, f.shell.preferences_.displayName);
        else if (FriendsPanel::Shown(f.screen)) menu.eyebrow = loc::Tf("profile.eyebrow", f.shell.preferences_.displayName);
        if (f.rows) f.shell.friends_.Dress(f.view, f.screen, *f.rows, menu);
    }
};

// Replays: the list, Inputs and stats, and an export's caption page.
class ApplicationShell::ReplaysScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return ReplaysPanel::Owns(screen); }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool idle, std::string& title) const override {
        return f.shell.replays_.Rows(f.view, f.screen, f.shell.Navigation().Parent(), idle, title);
    }
    // Only an empty status line takes the panel's, so its place among the
    // shell's reports does not change what shows.
    void Status(const ScreenFrame& f, std::string& status, Tone& tone) const override {
        f.shell.replays_.Status(f.view, f.screen, status, tone);
    }
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit& submit) const override {
        return f.shell.replays_.Activate(a, f.view, f.shell.preferences_, f.shell.menu_.navigation, submit, f.shell.error_);
    }
    // An export's on/off row is also kept as the player's choice for the next
    // export, when preferences can be saved here; otherwise it is this export's only.
    bool Adjust(const ScreenFrame& f, const MenuAction& a) const override {
        const auto saved = f.shell.replays_.Accept(a);
        if (saved && f.view.canEditPreferences && f.shell.preferences_.*saved != (a.delta > 0)) {
            f.shell.preferences_.*saved = a.delta > 0;
            f.shell.preferencesDirty_ = true; f.shell.saveAt_ = ImGui::GetTime() + .45; f.shell.error_.clear();
        }
        return true;
    }
    bool Choose(const ScreenFrame& f, const MenuAction& a, const Submit& submit) const override {
        f.shell.replays_.Choose(a, f.view, f.shell.preferences_, f.shell.menu_.navigation, submit, f.shell.error_);
        return true;
    }
    // The match and set cards and the focused one's pane, and Export video's
    // preview, on the tabbed screens (Inputs and a set's page are plain lists).
    GameMenu::Card Card(const ScreenFrame& f, double) const override {
        // The archive's fighter page is the same grid, its one chosen fighter lit and marked.
        if (f.screen == ArchiveTab::FighterScreen)
            return [shell = &f.shell](const MenuEntry& e, ImVec2 min, ImVec2 max) {
                const int fighter = ArchiveTab::FighterOf(e.id);
                if (fighter < 0) return false;
                const bool on = shell->replays_.Archive().Ticked(fighter);
                DrawMainPortrait(fighter, on, min, max);
                if (on) ImGui::GetWindowDrawList()->AddText(ImVec2(max.x - 18 * Scale(), min.y + 3 * Scale()), palette::Ember, "\xE2\x9C\x93");
                return true;
            };
        // The filter's fighters are fighter select's grid, a ticked one lit and marked.
        if (f.screen == "replay-filter")
            return [shell = &f.shell](const MenuEntry& e, ImVec2 min, ImVec2 max) {
                const auto fighter = ReplaysPanel::FilterFighter(e.id);
                if (!fighter) return false;
                const bool on = shell->replays_.Ticked(*fighter);
                DrawMainPortrait(*fighter, on, min, max);
                if (on) ImGui::GetWindowDrawList()->AddText(ImVec2(max.x - 18 * Scale(), min.y + 3 * Scale()), palette::Ember, "\xE2\x9C\x93");
                return true;
            };
        if (!Tabbed(f.screen)) return {};
        const ApplicationShell* shell = &f.shell; const ShellView* view = &f.view;
        return [shell, view](const MenuEntry& e, ImVec2 min, ImVec2 max) { return shell->replays_.DrawCard(*view, e, min, max); };
    }
    GameMenu::Detail Detail(const ScreenFrame& f) const override {
        if (!Tabbed(f.screen) || !f.rows) return {};
        ApplicationShell* shell = &f.shell; const ShellView* view = &f.view; const std::string* screen = &f.screen; const std::vector<MenuEntry>* rows = f.rows;
        return [shell, view, screen, rows](const std::string& id) { shell->replays_.DrawDetail(*view, *screen, id, shell->menu_.navigation, *rows); };
    }
    ScreenGrid Grid(const ScreenFrame& f) const override {
        if (f.screen != "replay-filter" && f.screen != ArchiveTab::FighterScreen) return {};
        // Smaller cards than fighter select's, so Sort, Show and Side stay on screen under them.
        const RosterGrid roster = LayOutRosterGrid(ImGui::GetContentRegionAvail().x);
        return {roster.columns + 3, roster.cardHeight * .66f};
    }
    // The tabs, the pane beside a list (its own over a replay, a set and an
    // export), and whether matches watched are saved too.
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        if (!Tabbed(f.screen)) return;
        const std::string& focus = menu.navigation.Focus();
        menu.customDetail = f.screen == "replay-export" || focus.compare(0, 7, "replay:") == 0 || focus.compare(0, 4, "set:") == 0 || ArchiveTab::IsRow(focus);
        if (f.screen != "replay-export") menu.paneWidth = 380;
        menu.tabs = f.shell.replays_.Tabs(f.view);
        for (int i = 0; i < static_cast<int>(menu.tabs.size()); ++i)
            if (menu.tabs[i].first == f.screen) { menu.tab = i; menu.eyebrow = std::string(loc::T("replays.title")) + " \xC2\xB7 " + menu.tabs[i].second; }
        const bool watched = f.shell.preferences_.recordWatched;
        menu.headerChips.push_back({std::string(loc::T("replays.save_watched")) + " \xC2\xB7 " + loc::T(watched ? "common.on" : "common.off"), watched ? palette::Ready : v2::Colors().muted});
        // The archive's filter is summarised in the header, and its shortcuts are the hint bar's.
        if (f.screen == ArchiveTab::Screen) {
            for (const auto& chip : f.shell.replays_.Archive().Chips(f.view)) menu.headerChips.push_back({chip, v2::Colors().muted});
            menu.shortcutHints = f.shell.replays_.Archive().Hints(f.view, f.screen);
        }
    }
    // The Archive tab's Filters and Retry or Refresh.
    void Shortcut(const ScreenFrame& f, const MenuAction& a) const override {
        f.shell.replays_.Shortcut(a, f.view, f.screen, f.shell.menu_.navigation);
    }
private:
    static bool Tabbed(const std::string& screen) { return screen == "replays" || screen == "replay-sets" || screen == "replay-export" || screen == ArchiveTab::Screen; }
};

const ScreenPanel* ApplicationShell::ScreenOf(const std::string& screen) {
    // No two panels own a screen, so the order they are asked in decides nothing, except that a member's page is asked
    // before the room's panel, which owns every screen whose name begins with "room".
    static const PublicRoomsScreens publicRooms{};
    static const RankingsScreens rankings{};
    static const ProfileScreens profile{};
    static const FriendsScreens friends{};
    static const ReplaysScreens replays{};
    static const IdentityScreens identity{};
    static const ScreenPanel* const owners[] = {&publicRooms, &rankings, &profile, &friends, &replays, &identity, &SettingsOwner(), &OnlineOwner(),
        &WelcomeOwner(), &NewsOwner(), &AwardsOwner(), &NotificationsOwner(), &RoomMemberOwner(), &RoomOwner()};
    for (const ScreenPanel* owner : owners)
        if (owner->Owns(screen)) return owner;
    return nullptr;
}
} }
