#include "ApplicationShell.hxx"
#include "PublicRoomsCards.hxx"
#include "RankingsCards.hxx"
#include "ScreenPanel.hxx"
#include "SignInSites.hxx"
#include "../common/Localization.hxx"
#include <imgui.h>

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
        return f.shell.identity_.Rows(f.view, f.screen, title);
    }
    // The panel's own line, when it has one, replaces the shell's.
    void Status(const ScreenFrame& f, std::string& status, Tone& tone) const override {
        std::string own; Tone ownTone = Tone::Neutral;
        if (f.shell.identity_.Status(own, ownTone, ImGui::GetTime())) { status = own; tone = ownTone; }
    }
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        // The retry row a failed save adds to every screen stays the shell's.
        if (a.id == "retry-save") return false;
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
        title = loc::T("screen.public_rooms");
        return shell.publicRooms_.Rows(f.view, shell.identity_.UsableBridge(f.view), shell.identity_.Waiting(), shell.identity_.PublicSetup(), PublicListWidth());
    }
    // Also asked on Create (UpdateStatus), with that screen.
    void Status(const ScreenFrame& f, std::string& status, Tone& tone) const override {
        std::string own = status; Tone ownTone = tone;
        if (f.shell.publicRooms_.Status(f.view, f.screen, own, ownTone, ImGui::GetTime())) { status = own; tone = ownTone; }
    }
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        auto& shell = f.shell; auto& nav = shell.menu_.navigation;
        if (a.id == "pr-quick") { shell.QuickJoin(f.view); return true; }
        // The confirmed setup runs in the Ember ID panel; a locked ID is unlocked on its own screen first.
        if (a.id == "pr-setup") { shell.identity_.SetUpPublicRooms(f.view); shell.error_.clear(); return true; }
        if (a.id == "pr-setup-id") { shell.identity_.ClearPublicSetup(); nav.Push("identity"); return true; }
        return shell.publicRooms_.Activate(a, nav);
    }
    bool Choose(const ScreenFrame& f, const MenuAction& a, const Submit&) const override {
        f.shell.publicRooms_.Choose(a);
        return true;
    }
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
};

// Replays: the list, Inputs and stats, and an export's caption page.
class ApplicationShell::ReplaysScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return ReplaysPanel::Owns(screen); }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool idle, std::string& title) const override {
        return f.shell.replays_.Rows(f.view, f.screen, idle, title);
    }
    // Only an empty status line takes the panel's, so its place among the
    // shell's reports does not change what shows.
    void Status(const ScreenFrame& f, std::string& status, Tone& tone) const override {
        f.shell.replays_.Status(f.view, f.screen, status, tone);
    }
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit& submit) const override {
        return f.shell.replays_.Activate(a, f.view, f.shell.menu_.navigation, submit, f.shell.error_);
    }
    bool Adjust(const ScreenFrame& f, const MenuAction& a) const override {
        f.shell.replays_.Accept(a);
        return true;
    }
    bool Choose(const ScreenFrame& f, const MenuAction& a, const Submit& submit) const override {
        f.shell.replays_.Choose(a, f.view, f.shell.menu_.navigation, submit, f.shell.error_);
        return true;
    }
};

const ScreenPanel* ApplicationShell::ScreenOf(const std::string& screen) {
    // No two panels own a screen, so the order they are asked in decides nothing.
    static const PublicRoomsScreens publicRooms{};
    static const RankingsScreens rankings{};
    static const ReplaysScreens replays{};
    static const IdentityScreens identity{};
    static const ScreenPanel* const owners[] = {&publicRooms, &rankings, &replays, &identity, &SettingsOwner(), &OnlineOwner(),
        &WelcomeOwner(), &NewsOwner(), &AwardsOwner(), &NotificationsOwner(), &RoomOwner()};
    for (const ScreenPanel* owner : owners)
        if (owner->Owns(screen)) return owner;
    return nullptr;
}
} }
