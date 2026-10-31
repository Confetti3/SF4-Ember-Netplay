// The shell's own screens as panels (ScreenPanel), so Draw asks the screen's
// panel for its rows and dress instead of testing the screen in turn: Online
// play with Join and Create, the first-run welcome and a room's screens.
// Settings, News, the awards wall and Notifications have theirs beside their rows.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "NetworkFeedback.hxx"
#include "RoomFeedback.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <imgui.h>

namespace sf4e { namespace ui {

// Online play, and Join and Create under it. Their rows' presses are the
// shell's (HandleActivate): Create and Join are pressed from Home and rooms too.
class ApplicationShell::OnlineScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return screen == "online" || screen == "join" || screen == "create"; }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override {
        auto& s = f.shell;
        const ShellView& v = f.view;
        const bool opening = v.session.room == netplay::RoomState::Opening;
        if (f.screen == "create")
            return s.RoomSetupRows(v, f.screen, opening, v.canEditPreferences ? loc::T("settings.auto_save") : loc::T("settings.leave_room_to_edit"), title);
        if (f.screen == "join") {
            title = loc::T("room.join_title");
            std::vector<MenuEntry> rows = {Row("paste", loc::T("room.paste_invitation"), loc::T("room.paste_invitation_detail"), v.canOpenRoom),
                opening ? ConfirmRow("cancel-open", loc::T("room.stop_joining_action"), loc::T("room.stop_joining"), true) :
                    Row("join-now", loc::T("online.join"), loc::T("room.join_pasted"), v.canOpenRoom && s.invitation_[0]),
                TextRow("invite-text", loc::T("room.edit_invitation"), s.invitation_, sizeof(s.invitation_) - 1, v.canOpenRoom)};
            rows[0].hint = loc::T("menu.hint.paste");
            if (!opening) rows[1].hint = loc::T("online.join");
            return rows;
        }
        // A room needs a controller and networking; a row blocked by one says which.
        const char* blocked = !v.controllerReady ? loc::T("room.controller_required") :
            v.network == netplay::NetworkAvailability::Unavailable ? loc::T("runtime.networking_unavailable") : nullptr;
        const auto open = [&](const char* id, const std::string& label, const char* detail) {
            auto r = Row(id, label, blocked ? blocked : detail, v.canOpenRoom);
            r.quiet = blocked != nullptr;
            return r;
        };
        title = loc::T("online.title");
        std::vector<MenuEntry> rows = {open("create", loc::T("online.create"), loc::T("online.create_detail")), open("join", loc::T("online.join"), loc::T("online.join_detail")),
            open("public-rooms", loc::T("screen.public_rooms"), loc::T("online.public_detail")),
            Row("identity", loc::T("screen.identity"), s.identity_.EntryDetail(v)),
            Row("rankings", loc::T("screen.rankings"), loc::T("rankings.entry_detail"))};
        rows.back().value = RankingsPanel::EntryValue(v, s.identity_.UsableBridge(v));
        // Where this PC connects and how its network treats a direct path, for information only.
        auto relay = Row("relay", loc::T("network.relay"), loc::T("network.relay_detail"));
        relay.info = true;
        relay.value = DescribeRelay(v.netReport);
        auto network = Row("network", loc::T("network.status"), DescribeNatDetail(v.netReport));
        network.info = true;
        network.value = DescribeNat(v.netReport.nat);
        rows.push_back(std::move(relay));
        rows.push_back(std::move(network));
        return rows;
    }
    // Create's line also says how a public room being created stands, which the public rooms panel knows.
    void Status(const ScreenFrame& f, std::string& status, Tone& tone) const override {
        if (f.screen == "create") if (const ScreenPanel* rooms = ScreenOf("public-rooms")) rooms->Status(f, status, tone);
    }
    // The online screens say where they sit, Online play its relay too.
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        const ShellView& v = f.view;
        if (f.screen == "online") {
            menu.eyebrow = loc::T("online.eyebrow");
            menu.headerChips.push_back({std::string(loc::T("network.relay")) + " \xC2\xB7 " + DescribeRelay(v.netReport), v.netReport.relayConnected ? palette::Ready : v2::Colors().muted});
            menu.listWidth = 560;
        } else if (f.screen == "join") {
            menu.eyebrow = loc::T("online.eyebrow_join");
            menu.listWidth = 600;
        } else menu.eyebrow = loc::T("online.eyebrow_host");
    }
    // Online play's detail ends in the focused row's facts.
    GameMenu::Detail Detail(const ScreenFrame& f) const override {
        if (f.screen != "online") return {};
        const ApplicationShell* s = &f.shell; const ShellView* v = &f.view;
        return [s, v](const std::string& id) {
            ImGui::Dummy(ImVec2(0, 8 * Scale()));
            if (id == "create") {
                v2::Fact(loc::T("public.visibility"), loc::T(s->preferences_.roomPublic ? "public.visibility.public" : "public.visibility.private"));
                v2::Fact(loc::T("room.capacity"), std::to_string(s->preferences_.roomCapacity));
            } else if (id == "relay" || id == "network") {
                v2::Fact(loc::T("network.relay"), DescribeRelay(v->netReport));
                v2::Fact(loc::T("network.status"), DescribeNat(v->netReport.nat));
            }
        };
    }
    // Create is the room's settings and its rules side by side, and the button that opens it.
    GameMenu::Body Body(const ScreenFrame& f) const override {
        if (f.screen != "create") return {};
        ApplicationShell* s = &f.shell;
        return [s](const std::vector<MenuEntry>& e, MenuNavigation& n, MenuAction& a, float h, const MenuVisualFeedback& fb) { s->DrawCreateBoard(e, n, a, h, fb); };
    }
};

// The first-run welcome (ApplicationShell__FirstRun.cxx), a step at a time.
// Its Back is its previous step, and nothing on its first.
class ApplicationShell::WelcomeScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return screen == "welcome"; }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override { return f.shell.WelcomeRows(f.view, title); }
    // Its own rows and Back; false for a row another screen owns (Language, Choose controller).
    bool Activate(const ScreenFrame& f, const MenuAction& a, const Submit&) const override { return f.shell.WelcomeActivate(a); }
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        menu.ownsBack = true;
        menu.backInert = !f.shell.welcomeStep_;
        menu.backHint = loc::T("welcome.previous");
    }
    GameMenu::Body Body(const ScreenFrame& f) const override {
        ApplicationShell* s = &f.shell;
        return [s](const std::vector<MenuEntry>& e, MenuNavigation& n, MenuAction& a, float h, const MenuVisualFeedback&) { s->DrawWelcomeBoard(e, n, a, h); };
    }
};

// A room's screens (RoomPanel.cxx builds their rows): the board, a table's
// options, the members, one member, the chat and the room's settings. Their
// presses stay the shell's (RoomAction, RoomShortcut), which take them before
// any panel's.
class ApplicationShell::RoomScreens final : public ScreenPanel {
public:
    bool Owns(const std::string& screen) const override { return screen.compare(0, 4, "room") == 0; }
    std::vector<MenuEntry> Rows(const ScreenFrame& f, bool, std::string& title) const override {
        const ShellView& v = f.view;
        title = v.room.name.empty() ? loc::T("screen.room") : v.room.name;
        NoteUserText(v.room.name);
        return f.shell.RoomEntries(v);
    }
    void Dress(const ScreenFrame& f, GameMenu& menu) const override {
        const ShellView& v = f.view;
        if (!v.room.roomEpoch) return;
        // A room's header says what kind of room it is, who it admits and how full it is.
        if (f.screen == "room") {
            const bool listed = v.room.serverOwned;
            menu.eyebrow = loc::T(listed ? "room.v2.eyebrow_public" : "room.v2.eyebrow_private");
            menu.headerChips = {{loc::T(listed ? "room.v2.listed" : "room.v2.invitation_only"), ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending))},
                {std::string(loc::T("room.members")) + " " + std::to_string(v.room.members.size()) + "/" + std::to_string(v.room.capacity), palette::Ready}};
        }
        // With the message box holding the keyboard, F, T and C type letters, so the keys are not offered there.
        const bool keys = KeyboardPrompts();
        menu.typesText = f.screen == "room-chat" && ChatTypable(v);
        menu.shortcutHints = menu.typesText && keys ? std::vector<LegendHint>{} : RoomShortcutHints();
        // On the board B leaves your seat; the keyboard leaves it with Delete, so Escape keeps its own word.
        const char* placeExit = f.screen == "room" ? f.shell.PlaceExitLabel(v) : "";
        if (!*placeExit) return;
        if (auto leave = ShortcutHint(MenuInput::Leave, placeExit)) menu.shortcutHints.insert(menu.shortcutHints.begin(), std::move(*leave));
        if (!keys) menu.backHint = placeExit;
    }
    // The Members list as player tags.
    GameMenu::Card Card(const ScreenFrame& f, double) const override {
        if (f.screen != "room-members") return {};
        const ApplicationShell* s = &f.shell; const ShellView* v = &f.view;
        return [s, v](const MenuEntry& e, ImVec2 min, ImVec2 max) {
            const auto card = s->RoomMemberCard(*v, e);
            if (card.id.empty()) return false;
            DrawMemberRow(card, min, max);
            return true;
        };
    }
    GameMenu::Body Body(const ScreenFrame& f) const override {
        ApplicationShell* s = &f.shell; const ShellView* v = &f.view;
        if (!v->room.roomEpoch) return {};
        if (f.screen == "room")
            return [s, v](const std::vector<MenuEntry>& e, MenuNavigation& n, MenuAction& a, float h, const MenuVisualFeedback& fb) { s->DrawRoomBoard(*v, e, n, a, h, fb); };
        if (f.screen == "room-chat")
            return [s, v](const std::vector<MenuEntry>& e, MenuNavigation& n, MenuAction& a, float h, const MenuVisualFeedback& fb) { s->DrawChatScreen(*v, e, n, a, h, fb); };
        return {};
    }
};

std::vector<LegendHint> ApplicationShell::RoomShortcutHints() {
    // Only the shortcuts the device has (a stick has none of these).
    return ShortcutHints({{MenuInput::Fighter, loc::T("room.legend_fighter")}, {MenuInput::Options, loc::T("room.legend_options")}, {MenuInput::Chat, loc::T("room.chat")}});
}
const ScreenPanel& ApplicationShell::RoomOwner() {
    static const RoomScreens room{};
    return room;
}
const ScreenPanel& ApplicationShell::OnlineOwner() {
    static const OnlineScreens online{};
    return online;
}
const ScreenPanel& ApplicationShell::WelcomeOwner() {
    static const WelcomeScreens welcome{};
    return welcome;
}
} }
