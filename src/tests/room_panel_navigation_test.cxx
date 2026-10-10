#include "../ui/ApplicationShell.hxx"
#include "../common/Localization.hxx"
#include "../ui/GameMenu.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/RoomControls.hxx"
#include "../ui/RoomFeedback.hxx"
#include "../ui/Theme.hxx"
#include "imgui_test_support.hxx"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() try {
    using namespace sf4e;
    using namespace sf4e::ui;
    HeadlessImGui imgui; auto& io = imgui.io;

    ShellView view;
    // A chosen delay, until the Auto checks below turn Auto on.
    view.preferences.autoInputDelay = false;
    view.controllerReady = true;
    view.session.room = netplay::RoomState::Joined;
    view.session.control = netplay::Health::Healthy;
    view.session.generation.room = 1;
    view.room.roomEpoch = 4; view.room.localMember = 1; view.room.host = 1; view.room.capacity = 16;
    for (int i = 0; i < 4; ++i) view.room.tables[i].id = i;
    room::Member local; local.id = 1; local.name = "Local"; local.table = 0; local.seat = 0;
    room::Member peer; peer.id = 2; peer.name = "Peer"; peer.table = 0; peer.seat = 1;
    view.room.members = {local, peer}; view.room.tables[0].p1 = 1; view.room.tables[0].p2 = 2;
    view.room.tables[0].phase = room::TablePhase::Waiting;
    view.canReady = true; view.canEditSelection = true;
    view.selectedDelay = 2; view.recommendedDelay = 4; view.canProbe = true; view.canApplyDelay = true;

    ApplicationShell shell;
    bool open = true;
    std::vector<ShellAction> actions;
    std::vector<MenuEntry> rows;
    std::string menuStatus;
    unsigned selectionDraws = 0;
    unsigned menuDraws = 0;
    unsigned cardDraws = 0;
    bool requireMenuFrame = false;
    float step = 1.0f / 60.0f;
    std::map<std::string, ImVec2> targets;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries) { rows = entries; ++menuDraws; });
    SetMenuStatusProbe([&](const char* status, Tone) { menuStatus = status; });
    SetMenuCardProbe([&](const char* id, ImVec2 min, ImVec2 max) {
        ++cardDraws;
        targets[id] = ImVec2((min.x + max.x) * .5f, (min.y + max.y) * .5f);
    });
    const auto frame = [&](unsigned buttons = 0) {
        const auto beforeDraws = menuDraws;
        const auto beforeCards = cardDraws;
        const auto screen = shell.Navigation().Screen();
        io.DeltaTime = step; SetMenuInput({buttons, 0}); ImGui::NewFrame();
        shell.Draw(view, &open, [&](ShellAction action) { actions.push_back(std::move(action)); return true; }, [&] { ++selectionDraws; });
        ImGui::Render();
        if (requireMenuFrame) {
            Check(menuDraws == beforeDraws + 1, "Room navigation skipped its menu body for a frame");
            Check(ImGui::GetDrawData()->TotalVtxCount > 500, "Room navigation emitted an empty menu frame");
            if (screen == "room-table")
                Check(cardDraws > beforeCards, "Room transition drew a header without its menu controls");
            Check(open, "Queue navigation unexpectedly closed the overlay");
        }
    };
    const auto press = [&](unsigned buttons) { frame(); frame(buttons); frame(); };
    const auto row = [&](const char* id) -> const MenuEntry& {
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& entry) { return entry.id == id; });
        if (found == rows.end()) throw std::runtime_error(std::string("missing room row: ") + id);
        return *found;
    };
    const auto focus = [&](const char* id) { shell.Navigation().Focus(id, rows); };

    shell.Navigation().Home(); frame(); shell.Navigation().Push("room-table"); frame();
    Check(std::none_of(rows.begin(), rows.end(), [](const MenuEntry& entry) { return entry.id == "benchmark-connection"; }),
        "The removed 30 second benchmark row is still offered");
    view.probeStatus = "checking"; frame();
    Check(!row("check-connection").enabled && row("check-connection").label == "Checking connection...",
        "A running connection check still accepts another request");
    Check(row("input-delay").detail.find("Checking...") != std::string::npos, "Connection check progress is not visible");
    Check(!row("input-delay").opens, "Running check allowed a stale recommendation");
    view.probeStatus = "unavailable"; view.recommendedDelay = -1; frame();
    Check(row("check-connection").label == "Retry connection check" &&
        row("check-connection").detail.find("Ready") != std::string::npos,
        "Failed connection check does not explain retry or manual Ready");
    Check(!row("input-delay").opens && row("input-delay").detail.find("Check connection") != std::string::npos,
        "A failed check still offers a recommendation");
    view.probeStatus.clear(); view.recommendedDelay = 4; frame();
    // Leave room and Replace room stay on the board; the table page is only the table.
    Check(std::none_of(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "leave" || e.id == "replace-room" ||
        e.id == "room-rules" || e.id == "selected-delay" || e.id == "recommended-delay" || e.id == "apply-recommendation"; }),
        "The table page still carries rows that moved or merged");
    view.session.coordinated = true; view.session.authorityWritable = false; frame();
    // A checkpoint is the runtime's business: Ready stays pressable and the
    // seated table keeps its seat line instead of room-update chatter.
    Check(row("ready").enabled && !row("check-connection").enabled,
        "Room update hid Ready, which the runtime parks and resubmits");
    Check(menuStatus.find("Updating room")==std::string::npos,
        "A one-frame checkpoint delay flashed the room-update message");
    for(int i=0;i<20;++i)frame();
    Check(menuStatus.find("Updating room") == std::string::npos,
        "Checkpoint chatter replaced the seated table's status line");
    Check(row("ready").detail.find("Updating room") == std::string::npos,
        "Ready exposes the room update instead of the lock-in instruction");
    Check(row("check-connection").detail.find("Updating room") != std::string::npos,
        "Room update has no player-facing explanation");
    view.session.authorityWritable = true; view.session.room = netplay::RoomState::Closing; frame();
    Check(!row("ready").enabled && menuStatus.find("Leaving room") != std::string::npos,
        "Closing room still advertises playable actions");
    Check(row("ready").detail.find("Leaving room") != std::string::npos,
        "Disabled Ready does not explain that the room is closing");
    view.session.room = netplay::RoomState::Idle; frame();
    Check(shell.Navigation().Screen() == "home", "Completed room exit stranded the player on the old table");
    view.session.room = netplay::RoomState::Joined; view.session.coordinated = false;
    shell.Navigation().Home(); shell.Navigation().Push("room");
    for(unsigned i=1;i<=40;++i)view.room.chat.push_back({i,1,"Earlier chat message for reading."});
    frame();frame();frame();
    ImGuiWindow* chatWindow=nullptr;
    for(auto* window:GImGui->Windows)if(window->Active && std::strstr(window->Name,"Recent chat"))chatWindow=window;
    Check(chatWindow && chatWindow->ScrollMax.y>0,"Chat scroll fixture is not scrollable");
    ImGui::SetScrollY(chatWindow,0);frame();frame();
    Check(chatWindow->Scroll.y<1,"Could not scroll back to earlier chat");
    view.room.chat.push_back({41,2,"A new message arrives while reading."});
    frame();frame();
    Check(chatWindow->Scroll.y<1,"New chat pulled the reader away from earlier messages");
    ImGui::SetScrollY(chatWindow,chatWindow->ScrollMax.y);frame();frame();
    view.room.chat.push_back({42,2,"Another message while following live chat."});
    frame();frame();
    Check(std::abs(chatWindow->Scroll.y-chatWindow->ScrollMax.y)<1,"Live chat stopped following at the bottom");
    view.room.chat.clear();
    {
        // Player text asks the atlas for glyphs where the renderer draws it, not
        // from the snapshot. Old messages stay in the snapshot but scroll out of
        // view, so once the retention passes they hold nothing and a message that
        // becomes visible later still gets its characters. A muted sender's text
        // is never drawn and never holds a glyph.
        const auto encode = [](unsigned codepoint) {
            std::string text;
            text += static_cast<char>(0xE0 | (codepoint >> 12));
            text += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (codepoint & 0x3F));
            return text;
        };
        const auto drawable = [&](ImWchar glyph) { return io.FontDefault->FindGlyphNoFallback(glyph) != nullptr; };
        const auto settle = [&] { ApplyTheme(1.f); io.Fonts->Build(); };
        settle();
        SetUserGlyphRebuildInterval(std::chrono::milliseconds(0));
        SetUserGlyphRetention(std::chrono::milliseconds(60));
        room::Member muted; muted.id = 3; muted.name = "Muted";
        view.room.members.push_back(muted);
        shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
        const auto toggleMute = [&] {
            shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
            focus("member-3"); press(MenuInput::Select);
            Check(shell.Navigation().Screen() == "room-member", "The member row did not open its screen");
            focus("mute"); press(MenuInput::Select);
            shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
        };
        toggleMute();
        // 600 distinct characters in old messages, more than the glyph budget holds.
        for (unsigned i = 0; i < 40; ++i) {
            std::string text = "Old message ";
            for (unsigned k = 0; k < 15; ++k) text += encode(0x4E00 + i * 15 + k);
            view.room.chat.push_back({101 + i, 2, text});
        }
        view.room.chat.push_back({141, 3, "Muted sender " + encode(0x6000) + encode(0x6001)});
        view.room.chat.push_back({142, 2, "Recent " + encode(0x6708) + encode(0x65E5)});
        for (int i = 0; i < 6; ++i) frame();
        ImGuiWindow* recent = nullptr;
        for (auto* window : GImGui->Windows) if (window->Active && std::strstr(window->Name, "Recent chat")) recent = window;
        Check(recent && recent->ScrollMax.y > 0 && recent->Scroll.y > recent->ScrollMax.y - 1,
            "The chat did not scroll to its newest messages");
        // Everything noted while the first frames still showed the top has aged out.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (int i = 0; i < 3; ++i) frame();
        settle();
        Check(drawable(0x6708) && drawable(0x65E5), "A visible chat message did not get its glyphs");
        Check(!drawable(0x4E00) && !drawable(0x4E00 + 20),
            "Chat scrolled out of view kept holding its glyphs while it stayed in the snapshot");
        Check(!drawable(0x6000) && !drawable(0x6001), "A muted sender's message held glyphs");
        // The retained old messages no longer starve a message that becomes visible.
        view.room.chat.push_back({143, 2, "Later " + encode(0x6C34) + encode(0x706B)});
        for (int i = 0; i < 3; ++i) frame();
        Check(ApplyTheme(1.f), "A newly visible chat message did not rebuild the atlas");
        io.Fonts->Build();
        Check(drawable(0x6C34) && drawable(0x706B), "A newly visible chat message did not get its glyphs");
        Check(!drawable(0x4E00), "The rebuild brought back glyphs of hidden chat");
        view.room.chat.clear();
        toggleMute();
        view.room.members.pop_back();
        SetUserGlyphRetention(std::chrono::seconds(10));
        SetUserGlyphRebuildInterval(std::chrono::milliseconds(250));
        shell.Navigation().Home(); frame();
    }
    shell.Navigation().Home(); shell.Navigation().Push("room-table"); frame();
    // One delay row: its value is yours, its detail names the recommendation
    // and independent local behavior, Left and Right choose, Select applies the recommendation.
    Check(row("input-delay").enabled && row("input-delay").adjustable && row("input-delay").value == "2 frames",
        "Input delay was not adjustable");
    Check(row("input-delay").detail.find("Recommended: 4 frames") != std::string::npos &&
        row("input-delay").detail.find("Delay applies only to your own inputs.") != std::string::npos,
        "Input delay did not name the recommendation, or waited on the opponent's choice wrongly");
    view.opponentDelay = 5; frame();
    Check(row("input-delay").detail.find("Opponent: 5 frames") != std::string::npos && row("input-delay").value == "2 frames",
        "Opponent delay overwrote the local choice or was not displayed");
    view.opponentDelay = -1; frame();
    view.selectedDelay = 0; frame();
    Check(row("input-delay").detail.find(loc::T("settings.input_delay.zero_warning")) == 0,
        "Room zero delay did not put its warning first");
    view.selectedDelay = 1; frame();
    Check(row("input-delay").detail.find(loc::T("settings.input_delay.zero_warning")) == std::string::npos,
        "Room one-frame delay retained the zero warning");
    view.selectedDelay = 2; frame();
    Check(row("check-connection").enabled && row("input-delay").opens && row("input-delay").hint == "Apply recommendation",
        "Delay actions were not available");
    focus("check-connection"); press(MenuInput::Select);
    Check(actions.back().command.kind == netplay::CommandKind::CheckConnection && actions.back().selectedDelay == -1,
        "Check connection did not use its command seam");
    focus("input-delay"); press(MenuInput::Select);
    Check(actions.back().command.kind == netplay::CommandKind::ApplyDelay && actions.back().selectedDelay == -1,
        "Select on Input delay did not request the measured value");
    press(MenuInput::Right);
    Check(actions.back().command.kind == netplay::CommandKind::ApplyDelay && actions.back().selectedDelay == 3,
        "Manual delay did not submit the bounded value");
    // Zero is selectable and displayed without raising it to one.
    view.selectedDelay = 0; frame();
    Check(row("input-delay").value == "0 frames", "Input delay failed to show 0 frames");
    press(MenuInput::Right);
    Check(actions.back().command.kind == netplay::CommandKind::ApplyDelay && actions.back().selectedDelay == 1,
        "Zero did not step to one frame");
    press(MenuInput::Left);
    Check(actions.back().command.kind == netplay::CommandKind::ApplyDelay && actions.back().selectedDelay == AutoInputDelayChoice,
        "Left from zero did not choose Auto");
    // Auto sits before zero frames. With it on, the row shows its bounds and,
    // once the opponent is measured, the delay it resolves to; Select no longer
    // takes the recommendation, and Right returns to zero frames.
    view.selectedDelay = 0; frame(); press(MenuInput::Left);
    Check(actions.back().command.kind == netplay::CommandKind::ApplyDelay && actions.back().selectedDelay == AutoInputDelayChoice,
        "Left from zero did not choose Auto");
    view.preferences.autoInputDelay = true; view.selectedDelay = 2; frame();
    Check(row("input-delay").value == "Auto", "Auto showed a delay before measuring the opponent");
    view.autoDelayMeasured = true; view.selectedDelay = 3; frame();
    Check(row("input-delay").value == "Auto (3 frames)" && !row("input-delay").opens &&
        row("input-delay").detail.find(loc::Tf("room.input_delay.auto.detail", AutoInputDelayMinimum, AutoInputDelayMaximum)) != std::string::npos &&
        row("input-delay").detail.find("Delay applies only to your own inputs.") != std::string::npos,
        "Auto did not show its delay and bounds, or still offered the recommendation");
    press(MenuInput::Right);
    Check(actions.back().command.kind == netplay::CommandKind::ApplyDelay && actions.back().selectedDelay == 0,
        "Right from Auto did not choose zero frames");
    view.preferences.autoInputDelay = false; view.autoDelayMeasured = false; view.selectedDelay = 2; frame();
    // Fighter, Ultra and Appearance sit under Ready. Left and Right step the
    // Ultra and the color without leaving the page; Select opens their cards
    // in fighter select.
    view.fighterName = "Ryu"; view.ultraName = "Ultra I: Metsu Hadoken"; view.ultraSteps = true;
    view.appearanceName = "Original / Color 1"; view.colorSteps = true; frame();
    Check(row("selection").value == "Ryu" && row("ultra").value == "Ultra I: Metsu Hadoken" && row("ultra").adjustable,
        "The table page does not show the fighter and a steppable Ultra");
    Check(row("appearance").value == "Original / Color 1" && row("appearance").adjustable && row("appearance").enabled,
        "The table page does not show a steppable appearance");
    // Under Ready: the player's own pick, then the match (stage and the table's
    // rules), then the connection, then leaving.
    const auto at = [&](const char* id) {
        for (std::size_t i = 0; i < rows.size(); ++i) if (rows[i].id == id) return static_cast<int>(i);
        return -1;
    };
    const int rules = at("edition") >= 0 ? at("edition") : at("rules");
    Check(rows[1].id == "selection" && rows[2].id == "ultra" && rows[3].id == "appearance" &&
        rows[4].id == "fighter-options" && rows[5].id == "stage" && rules == 6 &&
        at("input-delay") > rules && at("check-connection") == at("input-delay") + 1 && at("unqueue") > at("check-connection"),
        "The table page is not ordered pick, match, connection, leave");
    // Personal action, win quote and the other options are reachable from a seat
    // and shown on the row; the stage only for P1, who sends it.
    view.fighterOptionsName = "Action 1, Quote Random"; frame();
    Check(row("fighter-options").enabled && row("fighter-options").value == "Action 1, Quote Random" && !row("fighter-options").adjustable,
        "The table page does not show the fighter options, or advertises stepping them in place");
    view.localSlot = 1; view.stageName = "Random"; frame();
    Check(!row("stage").enabled && row("stage").detail == loc::T("selection.only_p1_stage"), "P2 could change the stage");
    view.localSlot = 0; frame();
    Check(row("stage").enabled && row("stage").value == "Random" && !row("stage").adjustable,
        "P1 cannot change the stage from the table page, or it advertises stepping in place");
    focus("ultra"); press(MenuInput::Right);
    using Field = ShellAction::SelectionStep::Field;
    Check(actions.back().selectionStep.field == Field::Ultra && actions.back().selectionStep.delta == 1, "Right on Ultra did not step it");
    focus("appearance"); press(MenuInput::Left);
    Check(actions.back().selectionStep.field == Field::Color && actions.back().selectionStep.delta == -1, "Left on Appearance did not step the color");
    view.ultraSteps = false; view.colorSteps = false; frame();
    Check(!row("ultra").adjustable, "A fighter with one Ultra still offers to step it");
    Check(!row("appearance").adjustable, "A costume with one color still offers to step it");
    view.ultraSteps = true; view.colorSteps = true; frame();
    {
        const auto seatedView = view;
        view.room.members[0].seat = -1; view.localSlot = -1;
        view.room.tables[0].p1 = 2; view.room.tables[0].p2 = 3;
        // What the runtime shows while a pick cannot change, and not the generic wait text.
        const std::string lockReason = loc::T("runtime.lock.return_to_menu_fighter");
        Check(!lockReason.empty() && lockReason != loc::T("room.change_fighter.waiting"), "The runtime lock text is not distinct");
        // The local member's place built as the authority derives each status:
        // queued is the queue, watching next the watching-next list, and watching
        // the spectator list of a live game.
        const auto becomes = [&](room::MemberStatus status) {
            const std::vector<room::MemberId> self{1}, nobody;
            auto& table = view.room.tables[0];
            const bool live = status == room::MemberStatus::Watching;
            view.room.members[0].status = status;
            view.room.members[0].table = status == room::MemberStatus::Idle ? -1 : 0;
            table.queue = status == room::MemberStatus::Queued ? self : nobody;
            table.watchingNext = status == room::MemberStatus::WatchingNext ? self : nobody;
            table.spectators = live ? self : nobody;
            table.phase = live ? room::TablePhase::Playing : room::TablePhase::Waiting;
            view.session.match = live ? netplay::MatchState::Playing : netplay::MatchState::None;
        };
        const auto walk = [&](unsigned key, const std::vector<std::string>& ids) {
            for (const auto& id : ids) {
                press(key);
                Check(shell.Navigation().Focus() == id, "Up/Down did not walk the unseated pick rows into Queue/Watch");
            }
        };
        for (const auto status : {room::MemberStatus::Idle, room::MemberStatus::Queued,
            room::MemberStatus::WatchingNext, room::MemberStatus::Watching}) {
            const bool queued = status == room::MemberStatus::Queued;
            becomes(status);
            shell.Navigation().Home(); shell.Navigation().Push("room-table"); frame();
            Check(rows[0].id == "selection" && rows[1].id == "ultra" && rows[2].id == "appearance" &&
                rows[3].id == "fighter-options" && rows[4].id == (queued ? "unqueue" : "queue"),
                "Unseated pick rows are missing or not above Queue/Watch");
            focus("selection");
            walk(MenuInput::Down, {"ultra", "appearance", "fighter-options", rows[4].id});
            walk(MenuInput::Up, {"fighter-options", "appearance", "ultra", "selection"});
            Check(at("stage") == -1, "An unseated member was offered Stage");
            Check(row("selection").value == view.fighterName && row("ultra").value == view.ultraName &&
                row("appearance").value == view.appearanceName && row("fighter-options").value == view.fighterOptionsName,
                "Unseated pick rows did not show the current pick");
            for (const auto* id : {"selection", "ultra", "appearance", "fighter-options"}) {
                Check(row(id).enabled, "An editable unseated pick row is disabled");
                focus(id); press(MenuInput::Select);
                Check(shell.Navigation().Screen() == "selection", "An unseated pick row did not open selection");
                Check(EmbeddedReturnContext().openOn == (std::strcmp(id, "selection") == 0 ? "roster" :
                    std::strcmp(id, "appearance") == 0 ? "costumes" : std::strcmp(id, "ultra") == 0 ? "ultra" : "options"),
                    "An unseated pick row opened the wrong selection page");
                shell.Navigation().Home(); shell.Navigation().Push("room-table"); frame();
            }
            focus("ultra"); const auto beforeSteps = actions.size(); press(MenuInput::Right);
            Check(actions.size() == beforeSteps + 1 && actions.back().selectionStep.field == Field::Ultra &&
                actions.back().selectionStep.delta == 1 && shell.Navigation().Focus() == "ultra",
                "Unseated Right did not step Ultra in place");
            focus("appearance"); press(MenuInput::Left);
            Check(actions.size() == beforeSteps + 2 && actions.back().selectionStep.field == Field::Color &&
                actions.back().selectionStep.delta == -1 && shell.Navigation().Focus() == "appearance",
                "Unseated Left did not step color in place");
            shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
            const auto beforeShortcut = selectionDraws; press(MenuInput::Fighter);
            Check(shell.Navigation().Screen() == "selection" && selectionDraws > beforeShortcut,
                "X/F did not open selection for an unseated member");
            shell.Navigation().Home(); frame(); focus("selection"); press(MenuInput::Select);
            Check(shell.Navigation().Screen() == "selection", "Home did not open unseated selection");
            shell.Navigation().Home(); shell.Navigation().Push("room-table");
            view.canEditSelection = false; view.selectionLockReason = lockReason; frame();
            for (const auto* id : {"selection", "ultra", "appearance", "fighter-options"}) {
                Check(!row(id).enabled && !row(id).adjustable && row(id).detail == lockReason,
                    "A blocked unseated pick row is enabled or missing the runtime's reason");
                focus(id); const auto beforeBlocked = selectionDraws; const auto beforeActions = actions.size();
                press(MenuInput::Select); press(MenuInput::Left); press(MenuInput::Right);
                Check(shell.Navigation().Screen() == "room-table" && selectionDraws == beforeBlocked && actions.size() == beforeActions,
                    "A blocked unseated pick row opened or stepped selection");
            }
            press(MenuInput::Fighter);
            Check(shell.Navigation().Screen() == "room-table", "Blocked unseated X/F opened selection");
            // Home still lets a blocked member inspect the pick, a live watcher included.
            shell.Navigation().Home(); frame(); focus("selection"); press(MenuInput::Select);
            Check(shell.Navigation().Screen() == "selection", "A blocked unseated member could not inspect selection from Home");
            view.canEditSelection = true; view.selectionLockReason.clear();
        }
        view = seatedView;
        shell.Navigation().Home(); shell.Navigation().Push("room-table"); frame();
    }
    // The host changes the rules in place; nothing is sent until Apply rules,
    // which only appears once something changed.
    Check(row("rounds").enabled && row("time").enabled && row("edition").enabled &&
        std::none_of(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "apply-rules"; }),
        "The host's rules are not on the table page, or Apply shows with nothing to apply");
    {
        const auto beforeRules = actions.size();
        // Stepping a rule and back leaves nothing to apply, since applying clears Ready.
        focus("rounds"); press(MenuInput::Right); press(MenuInput::Left);
        Check(std::none_of(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "apply-rules"; }),
            "Rules stepped back to the table's still offer Apply");
        focus("rounds"); press(MenuInput::Right); focus("time"); press(MenuInput::Left);
        Check(actions.size() == beforeRules, "Changing a rule sent it before Apply");
        focus("apply-rules"); press(MenuInput::Select);
        Check(actions.size() == beforeRules + 1 && actions.back().roomAction.kind == room::ActionKind::SetRules &&
            actions.back().roomAction.rules.roundCount == 5 && actions.back().roomAction.rules.roundTime == 60,
            "Apply rules did not send the edited rounds and time");
        view.room.tables[0].rules = actions.back().roomAction.rules; ++view.room.tables[0].revision; frame();
        Check(std::none_of(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "apply-rules"; }),
            "Apply rules stayed after the rules were applied");
        // Everyone else reads the rules on one line.
        view.room.host = 2; frame();
        Check(row("rules").value == "5 rounds, 60 s, Edition Select On" && !row("rules").enabled &&
            std::none_of(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "rounds"; }),
            "A guest does not see the table's rules as one line");
        view.room.host = 1; frame();
    }

    view.session.recovery = netplay::Recovery::Recovering;
    view.session.error = "Room control is recovering. Room actions are paused."; frame();
    Check(!row("ready").enabled && menuStatus.find("recovering") != std::string::npos,
        "Recovery did not freeze room actions with an immediate reason");
    // Replace room is offered on the board, not on the table page.
    shell.Navigation().Home(); shell.Navigation().Push("room");
    view.session.recovery = netplay::Recovery::ReplacementOffered; frame();
    Check(!row("replace-room").enabled, "Replacement ignored the owner's default retirement fence");
    view.canReplaceRoom = true; frame();
    Check(row("replace-room").enabled, "Replacement was not offered after recovery");
    view.room.tables[0].phase = room::TablePhase::Playing;
    view.session.match = netplay::MatchState::Preparing;
    view.canReplaceRoom = false;
    frame();
    Check(!row("replace-room").enabled &&
        row("replace-room").detail.find("still closing") != std::string::npos,
        "Replacement ignored the owner's active native socket fence");
    // Preparation without a native GGPO owner can be explicitly abandoned;
    // final room creation must still await helper retirement in the owner.
    view.canReplaceRoom = true; frame();
    Check(row("replace-room").enabled, "Incomplete preparation could not request explicit retirement");
    focus("replace-room"); const auto beforePreparationReplace = actions.size(); press(MenuInput::Select);
    Check(actions.size() == beforePreparationReplace,
        "Preparation replacement confirmation did not default to Cancel");
    press(MenuInput::Back);
    view.canReplaceRoom = false; view.session.match = netplay::MatchState::Playing; frame();
    Check(!row("replace-room").enabled, "Replacement ignored active local gameplay");
    view.session.match = netplay::MatchState::PostMatch; frame();
    Check(!row("replace-room").enabled, "Post-match status bypassed an active native socket fence");
    focus("replace-room");
    // A lost quorum cannot update this stale Playing projection. Only the
    // owner can establish that local GGPO released its socket and that
    // explicit replacement can retire any remaining helper mappings.
    view.canReplaceRoom = true;
    frame();
    Check(row("replace-room").enabled && shell.Navigation().Focus() == "replace-room",
        "Stale replicated Playing blocked locally retired replacement or moved focus");
    focus("replace-room"); const auto beforeReplace = actions.size(); press(MenuInput::Select);
    Check(actions.size() == beforeReplace, "Replacement confirmation did not default to Cancel");
    press(MenuInput::Right); press(MenuInput::Select);
    Check(actions.size() == beforeReplace + 1 && actions.back().command.kind == netplay::CommandKind::ReplaceRoom,
        "Confirmed replacement did not submit ReplaceRoom");

    shell.Navigation().Home(); shell.Navigation().Push("room");
    view.session.match = netplay::MatchState::None; frame();
    Check(row("replace-room").enabled,
        "An idle local member could not replace while the selected table remained Playing");

    view.canReplaceRoom = false; view.room.tables[0].phase = room::TablePhase::Waiting;
    view.session.recovery = netplay::Recovery::None; view.session.error.clear();
    shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
    focus("member-2"); press(MenuInput::Select); frame();
    Check(row("transfer-host").enabled, "Host member menu omitted Transfer host");
    focus("transfer-host"); const auto beforeTransfer = actions.size(); press(MenuInput::Select);
    Check(actions.size() == beforeTransfer, "Transfer host confirmation did not default to Cancel");
    press(MenuInput::Right); press(MenuInput::Select);
    Check(actions.size() == beforeTransfer + 1 && actions.back().roomAction.kind == room::ActionKind::TransferHost &&
        actions.back().roomAction.target == 2, "Transfer host action was not submitted");

    // A committed terminal receipt is a room-wide eligibility fence.  The
    // snapshot carries it before any failed action, so Ready/Queue/Watch are
    // visibly disabled with a durable teardown reason rather than relying on
    // the last rejected command to populate the explanation.
    view.session.recovery = netplay::Recovery::None; view.session.error.clear();
    view.room.localTerminalPending = true; view.room.terminalPending[0] = true;
    view.room.members[0].table = -1; view.room.members[0].seat = -1;
    view.room.tables[0].p1 = 2; view.room.tables[0].p2 = 0; view.room.tables[0].phase = room::TablePhase::Waiting;
    shell.Navigation().Home(); shell.Navigation().Push("room-table"); frame();
    Check(!row("queue").enabled && !row("watch").enabled &&
        row("queue").detail.find("finish returning") != std::string::npos,
        "Terminal receipt did not disable Queue/Watch with the committed teardown reason");

    // The fence gates admission to a table, never departure from it. A
    // spectator whose own teardown is what the room is waiting for must still
    // be able to leave, or it waits on itself.
    view.room.members[0].table = 0; view.room.tables[0].spectators = {1};
    frame();
    Check(row("unwatch").enabled && row("unwatch").detail.find("finish returning") == std::string::npos,
        "Terminal receipt disabled Stop watching, stranding the spectator in the finished match");
    const auto beforeUnwatch = actions.size();
    focus("unwatch"); press(MenuInput::Select);
    Check(actions.size() == beforeUnwatch + 1 && actions.back().roomAction.kind == room::ActionKind::Unwatch,
        "Stop watching did not emit one unwatch action during the terminal wait");
    // Lock-in is not admission either: the spectator still returning is who
    // it is for. It sends the wanted value, not a toggle.
    Check(row("lock-spectating").enabled && row("lock-spectating").label == "Lock in to watch",
        "Terminal receipt disabled a spectator's lock-in");
    focus("lock-spectating"); press(MenuInput::Select);
    Check(actions.size() == beforeUnwatch + 2 && actions.back().roomAction.kind == room::ActionKind::LockSpectating &&
        actions.back().roomAction.locked, "Lock in did not emit one locking action");
    view.room.members[0].spectatorLocked = true; ++view.room.revision; frame();
    Check(row("lock-spectating").label == "Release lock-in", "A locked-in spectator was offered Lock in again");
    press(MenuInput::Select);
    Check(actions.size() == beforeUnwatch + 3 && !actions.back().roomAction.locked, "Release lock-in did not unlock");
    view.room.members[0].spectatorLocked = false;
    // A queued member is listed as a spectator of the game it waits out, but
    // the authority refuses it a lock-in, so the panel must not offer one.
    view.room.tables[0].queue = {1}; ++view.room.revision; frame();
    Check(std::none_of(rows.begin(), rows.end(), [](const MenuEntry& entry) { return entry.id == "lock-spectating"; }),
        "A queued member was offered a spectator lock-in");
    view.room.tables[0].queue.clear();
    view.room.tables[0].spectators.clear(); view.room.tables[0].queue = {1};
    frame();
    Check(row("unqueue").enabled && row("unqueue").detail.find("finish returning") == std::string::npos,
        "Terminal receipt disabled Leave queue");
    // A queued member is offered no watch choice at all: the authority refuses
    // it, and the game it waits out is not its choice.
    Check(std::none_of(rows.begin(), rows.end(), [](const MenuEntry& entry) { return entry.id == "watch" || entry.id == "unwatch"; }),
        "A queued member was offered Watch");
    view.room.tables[0].queue.clear(); view.room.members[0].table = -1;
    frame();

    // A watcher at another table may still queue or watch here, since either
    // ends the old watch, as the board's chooser offers. A queue place
    // elsewhere still blocks both.
    view.room.localTerminalPending = false; view.room.terminalPending[0] = false;
    view.room.members[0].table = 1; view.room.members[0].seat = -1; view.room.members[0].status = room::MemberStatus::Watching;
    view.room.tables[1].spectators = {1}; ++view.room.revision; frame();
    Check(row("queue").enabled && row("watch").enabled && row("queue").detail.find("Leave your current table") == std::string::npos,
        "A watcher at another table was told to leave it before queueing here");
    const auto beforeWatcherQueue = actions.size();
    focus("queue"); press(MenuInput::Select);
    Check(actions.size() == beforeWatcherQueue + 1 && actions.back().roomAction.kind == room::ActionKind::Queue &&
        actions.back().roomAction.table == 0, "A watcher's Queue at another table was not sent");
    view.room.tables[1].spectators.clear(); view.room.tables[1].queue = {1};
    view.room.members[0].status = room::MemberStatus::Queued; ++view.room.revision; frame();
    Check(!row("queue").enabled && !row("watch").enabled && row("queue").detail.find("Leave your current table") != std::string::npos,
        "A queue place at another table no longer blocks queueing here");
    view.room.tables[1].queue.clear();

    // Queued at this table, the member sees Leave queue and no watch row at any
    // point of the game it waits out, whatever the spectator list holds.
    const auto offersWatch = [&] {
        return std::any_of(rows.begin(), rows.end(), [](const MenuEntry& entry) {
            return entry.id == "watch" || entry.id == "unwatch" || entry.id == "lock-spectating"; });
    };
    view.room.tables[0].p1 = 2; view.room.tables[0].p2 = 3; view.room.tables[0].phase = room::TablePhase::Waiting;
    view.room.tables[0].queue = {1}; view.room.tables[0].spectators.clear();
    view.room.members[0].table = 0; view.room.members[0].status = room::MemberStatus::Queued;
    ++view.room.revision; frame();
    Check(row("unqueue").enabled && !offersWatch(), "A queued member was offered a watch row while waiting");
    view.room.tables[0].phase = room::TablePhase::Playing; view.room.tables[0].matchGeneration = 3;
    view.room.tables[0].spectators = {1}; ++view.room.revision; frame();
    Check(row("unqueue").enabled && !offersWatch(), "A queued member was offered Stop watching during the game it waits out");
    // Host, not a fighter: a live game is not stuck until it has run this long.
    Check(!row("cancel-result").enabled && row("cancel-result").detail.find("in progress") != std::string::npos,
        "A live game was offered to the host as stuck");
    step = float(room::StaleGameSeconds + 60); frame(); step = 1.0f / 60.0f;
    Check(row("cancel-result").enabled && row("cancel-result").label == "Cancel stuck game",
        "A game with no result for a long time was not offered as stuck");
    ++view.room.tables[0].matchGeneration; frame();
    Check(!row("cancel-result").enabled, "A new game inherited the last one's stale clock");
    view.room.tables[0].phase = room::TablePhase::Waiting; frame();
    Check(!offersWatch(), "A queued member was offered a watch row after the game");
    view.room.tables[0].queue.clear(); view.room.members[0].status = room::MemberStatus::Watching; ++view.room.revision; frame();
    Check(row("unwatch").enabled, "A member who left the queue could not stop the watch it now chose");
    view.room.tables[0].spectators.clear(); view.room.tables[0].matchGeneration = 0;
    view.room.members[0].table = -1; view.room.members[0].status = room::MemberStatus::Idle;
    ++view.room.revision; frame();

    view.room.members[0].table = 0; view.room.members[0].seat = 0;
    view.room.tables[0].p1 = 1; view.room.tables[0].p2 = 2;
    view.room.localTerminalPending = false; view.room.terminalPending[0] = false;
    frame(); focus("ready"); frame();
    view.room.localTerminalPending = true; view.room.terminalPending[0] = true;
    ++view.room.revision; frame();
    Check(shell.Navigation().Focus() == "ready", "Terminal wait changed the focused Ready control");
    const auto beforeBlockedReady = actions.size();
    // The receipt fence is invisible to Ready (the runtime parks the press);
    // fighter changes still wait, with the committed reason.
    Check(row("ready").enabled && !row("selection").enabled &&
        row("ready").detail.find("finish returning") == std::string::npos &&
        row("selection").detail.find("finish returning") != std::string::npos,
        "Seated terminal receipt blocked Ready or hid the selection fence");
    press(MenuInput::Select);
    Check(actions.size() == beforeBlockedReady + 1 && actions.back().command.kind == netplay::CommandKind::Ready &&
        shell.Navigation().Focus() == "ready", "Ready during the receipt wait was dropped or lost focus");
    focus("selection"); const auto beforeBlockedSelection = selectionDraws;
    press(MenuInput::Select);
    Check(shell.Navigation().Screen() == "room-table" && selectionDraws == beforeBlockedSelection,
        "Disabled selection opened during terminal wait");
    focus("ready");
    view.room.localTerminalPending = false; view.room.terminalPending[0] = false;
    ++view.room.revision; frame();
    Check(row("ready").enabled && row("selection").enabled && shell.Navigation().Focus() == "ready",
        "Committed terminal completion failed to unlock controls with focus intact");
    press(MenuInput::Select);
    Check(actions.size() == beforeBlockedReady + 2 && actions.back().command.kind == netplay::CommandKind::Ready,
        "Ready did not dispatch after terminal completion");
    focus("selection"); press(MenuInput::Select);
    Check(shell.Navigation().Screen() == "selection" && selectionDraws > beforeBlockedSelection,
        "Selection failed to open after terminal completion");

    // The opponent changed fighter between games: a line on the table card and
    // the Ready row say so, and nothing takes the menu away from the player.
    {
        shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
        view.room.tables[0].ready[0] = view.room.tables[0].ready[1] = false;
        view.opponentChangedFighter = 5; ++view.opponentChangeSequence; ++view.room.revision;
        targets.clear(); frame(); frame();
        const auto changed = room_controls::OpponentChangedText(5);
        const auto banner = room_controls::DescribeTableBanner(view, view.room.tables[0]);
        Check(banner.text == changed && banner.seat == 1, "The table card did not name the opponent's new fighter on their seat");
        Check(!room_controls::DescribeTableBanner(view, view.room.tables[1]).text.size(), "Another table's card named the opponent's new fighter");
        Check(targets.count("table-0/banner") && !targets.count("table-1/banner"), "The opponent's new fighter was not drawn on the table card");
        Check(!shell.NoticeOpen(), "The opponent's new fighter opened a modal notice");
        const auto beforeChangedPress = actions.size();
        focus("table-0"); press(MenuInput::Select);
        Check(actions.size() == beforeChangedPress + 1, "The board did not take input while the opponent's new fighter was shown");
        shell.Navigation().Push("room-table"); frame();
        Check(row("ready").detail.compare(0, changed.size(), changed) == 0, "The Ready row did not start with the opponent's new fighter");
        // It ends when the player readies or the matchup changes (OpponentFighterWatch).
        view.opponentChangedFighter = -1; ++view.room.revision; shell.Navigation().Home(); shell.Navigation().Push("room");
        targets.clear(); frame(); frame();
        Check(!targets.count("table-0/banner") && row("table-0").id == "table-0", "The opponent's new fighter stayed on the card");
    }
    // A notice with no heading is not an error; ShowError is.
    {
        GameMenu menu;
        menu.ShowNotice("Advice");
        Check(menu.NoticeOpen() && !menu.NoticeError(), "A notice with no heading read as an error");
        menu.ShowError("Ready failed");
        Check(menu.NoticeError(), "An error notice did not read as one");
    }
    // Both fighters are ready and a locked-in spectator holds the start.
    shell.Navigation().Home(); shell.Navigation().Push("room-table");
    view.room.tables[0].phase = room::TablePhase::Ready; view.room.tables[0].spectatorHold = true;
    view.room.tables[0].ready[0] = view.room.tables[0].ready[1] = true;
    ++view.room.revision; frame();
    Check(row("ready").detail.find("locked-in spectators") != std::string::npos,
        "A held start did not say it waits for a locked-in spectator");
    // The hold can last ten seconds, so the fighter can still take Ready back.
    Check(row("ready").enabled, "A held start could not be cancelled");
    const auto beforeHeldUnready = actions.size();
    focus("ready"); press(MenuInput::Select);
    Check(actions.size() == beforeHeldUnready + 1 && actions.back().roomAction.kind == room::ActionKind::Unready,
        "Cancelling a held start did not send Unready");
    // The held start is a Ready to take back, not a game: every row says so.
    // The runtime locks the fighter for a readied seat, as it does here.
    view.canEditSelection = false; frame();
    Check(!row("selection").enabled && row("selection").detail == sf4e::loc::T("room.change_fighter.unready"),
        "A held start told the fighter a match was in progress instead of to unready");
    Check(!row("unqueue").enabled && row("unqueue").detail == sf4e::loc::T("room.leave_seat.unready_first"),
        "A held start told the fighter to finish a game instead of to unready");
    // On the board A keeps the Unready and B does not repeat it: it names the
    // seat, dimmed, and says what to do first.
    std::set<std::string> strip, legend;
    SetMenuTextProbe([&](const char* id, float, float, float, float) {
        if (!std::strncmp(id, "board-strip-", 12)) strip.insert(id + 12);
        else if (!std::strncmp(id, "legend/", 7)) legend.insert(id + 7);
    });
    SetMenuGlyphs(3, 0x40000, 0x20000);
    shell.Navigation().Home(); shell.Navigation().Push("room"); frame(); focus("table-0"); frame(); frame();
    Check(strip == std::set<std::string>({std::string(sf4e::loc::T("room.unready.short")), std::string(sf4e::loc::T("room.cancel_start"))}),
        "During the held start the seat's two controls did not read differently");
    Check(legend.count(sf4e::loc::T("room.unready")) && legend.count(sf4e::loc::T("room.cancel_start")),
        "The legend did not name Unready for A and Cancel the start for B during the held start");
    SetMenuTextProbe({});
    const auto beforeHeldBack = actions.size();
    press(MenuInput::Back);
    Check(actions.size() == beforeHeldBack + 1 && actions.back().roomAction.kind == room::ActionKind::Unready &&
        shell.Navigation().Screen() == "room", "B during the held start did not take Ready back");
    view.canEditSelection = true;
    view.room.tables[0].phase = room::TablePhase::Waiting; view.room.tables[0].spectatorHold = false;
    view.room.tables[0].ready[0] = view.room.tables[0].ready[1] = false;

    // Ready timeout: the last 30 seconds on the card and the Ready row.
    {
        using sf4e::loc::Tf;
        auto& table = view.room.tables[0];
        table.ready[1] = true; table.readyRemainingMs = 30001;
        Check(room_controls::DescribeTableBanner(view, table).text.empty(), "Ready timeout appeared above 30 seconds");
        table.readyRemainingMs = 30000;
        const auto own = Tf("room.ready_timeout.you", 30);
        Check(room_controls::DescribeTableBanner(view, table).text == own, "The unready fighter did not get their countdown");
        shell.Navigation().Home(); shell.Navigation().Push("room"); targets.clear(); frame();
        Check(targets.count("table-0/banner"), "The Ready timeout was not drawn on the table card");
        shell.Navigation().Push("room-table"); frame();
        Check(row("ready").detail.find(own) == 0, "The Ready row did not show the countdown");
        view.room.localMember = 2; table.readyRemainingMs = 29001;
        Check(room_controls::DescribeTableBanner(view, table).text == Tf("room.ready_timeout.other", 30, "Local"),
            "The opponent did not see who must ready");
        view.room.localMember = 99; table.readyRemainingMs = 1;
        Check(room_controls::DescribeTableBanner(view, table).text == Tf("room.ready_timeout.other", 1, "Local"),
            "A bystander did not see the final second");
        view.room.localMember = 1; table.ready[1] = false; table.readyRemainingMs = 0;
        shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
    }
    // A held start the host timed: the card and the Ready row name who it
    // waits for and count down, and say that B, Cancel the start, calls it off.
    {
        using sf4e::loc::T; using sf4e::loc::Tf;
        const auto members = view.room.members;
        room::Member alex; alex.id = 3; alex.name = "Alex"; alex.spectatorLocked = true;
        room::Member other; other.id = 4; other.name = "Other";
        view.room.members.push_back(alex); view.room.members.push_back(other);
        auto& held = view.room.tables[0];
        held.spectators = {3}; held.phase = room::TablePhase::Ready; held.spectatorHold = true;
        held.ready[0] = held.ready[1] = true; held.holdRemainingMs = 6200;
        view.canEditSelection = false; ++view.room.revision;
        shell.Navigation().Home(); shell.Navigation().Push("room"); targets.clear(); frame(); frame();
        const std::string waiting = Tf("room.hold.waiting_for", "Alex", 7);
        Check(room_controls::DescribeTableBanner(view, held).text == waiting, "The held start did not name Alex and count down");
        Check(targets.count("table-0/banner"), "The held start's line was not drawn on the card");
        Check(row("table-0").detail == waiting + " " + T("room.hold.cancel_detail"), "The seat card did not say what Cancel the start does");
        shell.Navigation().Push("room-table"); frame();
        Check(row("ready").detail == waiting + " " + T("room.hold.cancel_detail"), "The Ready row did not count down the held start");
        Check(menuStatus == Tf("room.hold.status", 7), "The status line did not count down the held start in one short line");
        held.holdRemainingMs = 1; ++view.room.revision; frame();
        Check(row("ready").detail.find(Tf("room.hold.waiting_for", "Alex", 1)) == 0, "A hold that has run out did not read one second");
        // The locked-in spectator the start waits for is told so.
        view.room.members[0].seat = -1; view.room.members[0].spectatorLocked = true; view.room.localTerminalPending = true;
        held.p1 = 4; held.spectators = {1, 3}; held.holdRemainingMs = 4000; ++view.room.revision;
        shell.Navigation().Home(); shell.Navigation().Push("room"); frame(); frame();
        Check(room_controls::DescribeTableBanner(view, held).text == Tf("room.hold.waiting_you", 4), "The spectator holding the start was not told");
        Check(row("table-0").detail.find(T("room.phase.holding")) != std::string::npos, "The held table read as preparing a game");
        shell.Navigation().Push("room-table"); frame();
        Check(menuStatus == Tf("room.hold.waiting_you", 4), "The status line did not say the start waits for this spectator");
        Check(row("lock-spectating").detail.find(Tf("room.hold.waiting_you", 4)) == std::string::npos, "The lock-in row repeated the status line");
        // Released by the player: no notice. Cleared by the room: a notice says why.
        // Cleared by the room, the lock-in gets a notice saying why; released
        // by the player, none.
        view.room.members[0].spectatorLocked = false; ++view.room.revision; frame();
        Check(menuStatus == T("room.lock_in_ended.dropped"), "A lock-in the stream failure ended was not explained");
        view.room.members[0].spectatorLocked = true; ++view.room.revision; frame();
        view.room.members[0].spectatorLocked = false; held.spectators = {3}; ++view.room.revision; frame();
        Check(menuStatus == T("room.lock_in_ended.stopped"), "A lock-in that ended with watching was not explained");
        Check(!shell.NoticeOpen(), "A lock-in that ended opened a modal");
        view.room.members[0].spectatorLocked = true; held.spectators = {1, 3}; ++view.room.revision; frame(); frame();
        focus("lock-spectating"); press(MenuInput::Select);
        Check(actions.back().roomAction.kind == room::ActionKind::LockSpectating && !actions.back().roomAction.locked, "Release lock-in was not sent");
        view.room.members[0].spectatorLocked = false; ++view.room.revision; frame(); frame();
        Check(menuStatus != T("room.lock_in_ended.dropped"), "The player's own release was announced as the room's");
        view.room.members = members; view.room.localTerminalPending = false;
        held.p1 = 1; held.spectators.clear(); held.phase = room::TablePhase::Waiting; held.spectatorHold = false;
        held.ready[0] = held.ready[1] = false; held.holdRemainingMs = 0; view.canEditSelection = true; ++view.room.revision;
        shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
    }
    // The Members list names every member's connection in words, unknown too.
    shell.Navigation().Home(); shell.Navigation().Push("room-members");
    view.room.members[1].link = NetworkLink::Wired; ++view.room.revision; frame();
    Check(row("member-2").value == "Wired connection" && row("member-1").value == "Connection unknown",
        "The Members list did not name each connection");
    view.room.members[1].link = NetworkLink::Unknown;

    // Recovery snapshots may reorder members but must keep the focused
    // identity, so Select still opens the intended member's actions.
    shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
    focus("member-2"); frame();
    std::reverse(view.room.members.begin(), view.room.members.end());
    ++view.room.revision; frame();
    Check(shell.Navigation().Focus() == "member-2", "Member snapshot reorder changed focused identity");
    press(MenuInput::Select); frame();
    Check(row("transfer-host").detail.find("Peer") != std::string::npos,
        "Focused member action opened a different identity after reorder");

    // Check the transition frame itself, not only the settled parent screen.
    // Previously Back changed navigation and returned before drawing the menu.
    shell.Navigation().Home(); shell.Navigation().Push("room");
    shell.Navigation().Push("room-table"); frame(); frame();
    const auto beforeBackDraws = menuDraws;
    frame(MenuInput::Back);
    Check(shell.Navigation().Screen() == "room", "Back did not return to the room");
    Check(menuDraws == beforeBackDraws + 1, "Back left a blank room-menu transition frame");

    // Every frame of queue changes, promotion, submenus and Back must render.
    requireMenuFrame = true;
    view.room.members = {local, peer};
    view.room.members[0].table = -1; view.room.members[0].seat = -1;
    view.room.tables[0].p1 = 2; view.room.tables[0].p2 = 3;
    shell.Navigation().Home(); shell.Navigation().Push("room"); frame(); frame();
    // While a receipt is outstanding the card's queue and watch options are
    // shown dimmed with the reason, as the options list shows them, and cannot
    // be picked; the table's options stay open to look at.
    view.room.localTerminalPending = true; ++view.room.revision; frame();
    {
        const auto& choices = row("table-0").choices;
        Check(choices.size() == 3 && !choices[0].enabled && !choices[1].enabled && choices[2].enabled &&
            choices[0].detail.find("finish returning") != std::string::npos,
            "The seat chooser offered choices the terminal fence refuses");
    }
    focus("table-0"); press(MenuInput::Select);
    Check(shell.Navigation().Choosing(), "The chooser did not open during a receipt wait");
    const auto beforeFencedChoice = actions.size();
    press(MenuInput::Select); press(MenuInput::Right); press(MenuInput::Select);
    Check(actions.size() == beforeFencedChoice && shell.Navigation().Choosing(), "A dimmed chooser option was picked");
    press(MenuInput::Back);
    Check(!shell.Navigation().Choosing(), "Back did not close the chooser");
    view.room.localTerminalPending = false; ++view.room.revision; frame();
    // A full table's card offers the queue or watching in place; Y opens its
    // options list.
    focus("table-0"); press(MenuInput::Select);
    Check(shell.Navigation().Screen() == "room" && shell.Navigation().Choosing(), "Select did not open the seat chooser");
    press(MenuInput::Back);
    press(MenuInput::Options);
    Check(shell.Navigation().Screen() == "room-table", "Y did not enter the table");
    focus("queue"); const auto beforeQueue = actions.size(); press(MenuInput::Select);
    Check(actions.size() == beforeQueue + 1 && actions.back().roomAction.kind == room::ActionKind::Queue,
        "Queue entry submitted zero or duplicate actions");
    view.room.tables[0].queue = {1}; ++view.room.revision; frame();
    for (int i = 0; i < 20; ++i) frame();
    Check(row("rounds").enabled, "The host lost the table's rules when not seated");

    // Keyboard Escape goes through the same input adapter as the real shell.
    io.AddKeyEvent(ImGuiKey_Escape, true); frame();
    io.AddKeyEvent(ImGuiKey_Escape, false); frame();
    Check(shell.Navigation().Screen() == "room", "Keyboard Back did not return to the room");
    focus("table-0"); press(MenuInput::Select);
    const auto click = [&](ImVec2 point) {
        io.AddMousePosEvent(point.x, point.y); frame();
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
    };
    const auto beforeLeave = actions.size(); click(targets.at("unqueue"));
    Check(actions.size() == beforeLeave + 1 && actions.back().roomAction.kind == room::ActionKind::Unqueue,
        "Mouse queue exit submitted zero or duplicate actions");
    view.room.tables[0].queue.clear(); ++view.room.revision; frame();
    const auto beforeMouseQueue = actions.size(); click(targets.at("queue"));
    Check(actions.size() == beforeMouseQueue + 1 && actions.back().roomAction.kind == room::ActionKind::Queue,
        "Mouse queue entry submitted zero or duplicate actions");
    view.room.tables[0].queue = {1}; ++view.room.revision; frame();
    view.room.tables[0].queue.clear(); view.room.tables[0].p2 = 1;
    view.room.members[0].table = 0; view.room.members[0].seat = 1;
    ++view.room.revision; frame();
    Check(row("ready").enabled, "Promotion to a seat lost the Ready control");
    // Locate Back from the shell padding and the fixed non-home header height.
    const auto* window = ImGui::FindWindowByName("SF4 Ember Netplay###EmberShell");
    const ImVec2 back(window->Pos.x + window->WindowPadding.x + 20,
        window->Pos.y + window->WindowPadding.y + 64 * Scale() +
        ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight() * .5f);
    click(back);
    Check(shell.Navigation().Screen() == "room", "Mouse Back did not return to the board");
    shell.Navigation().Push("room-table"); frame(); requireMenuFrame = false;

    // Room-control refreshes must not move controls under a held pointer or
    // controller focus. Same room, same table, different control health.
    view.room.members[0].table = -1; view.room.members[0].seat = -1;
    view.room.tables[0].p2 = 3;
    view.session.recovery = netplay::Recovery::None;
    view.session.control = netplay::Health::Healthy; view.session.error.clear();
    frame(); frame(); focus("queue"); frame(); frame();
    const auto queuePosition = targets.at("queue");
    const auto beforeRefreshActions = actions.size();
    view.session.control = netplay::Health::Lost;
    view.session.recovery = netplay::Recovery::Recovering;
    view.session.error = "Room control is recovering. Active matches may finish; room actions are paused.";
    ++view.room.revision; frame();
    Check(shell.Navigation().Screen() == "room-table" && shell.Navigation().Focus() == "queue",
        "Room-control refresh changed the active page or selected control");
    Check(!row("queue").enabled, "Room-control refresh failed to pause unsafe queue actions");
    const auto refreshedPosition = targets.at("queue");
    Check(std::abs(queuePosition.x - refreshedPosition.x) < 1 &&
        std::abs(queuePosition.y - refreshedPosition.y) < 1,
        "Room-control refresh moved the queue control and flickered the menu layout");
    Check(actions.size() == beforeRefreshActions, "Room-control refresh dispatched an action");

    view.session.control = netplay::Health::Healthy;
    view.session.recovery = netplay::Recovery::None; view.session.error.clear();
    frame(); focus("rounds"); frame();
    const auto rulesFocus = shell.Navigation().Focus();
    ++view.session.authorityRevision; ++view.room.revision; frame();
    Check(shell.Navigation().Screen() == "room-table" && shell.Navigation().Focus() == rulesFocus,
        "Same-room control refresh reset table navigation to the room overview");

    // A cached explanation belongs to its table: table 0's fenced queue
    // reason must not show on table 1's queue row during a checkpoint.
    {
        view.session.control = netplay::Health::Healthy; view.session.recovery = netplay::Recovery::None;
        view.session.error.clear(); view.session.coordinated = false; view.session.authorityWritable = true;
        view.room.members[0].table = -1; view.room.members[0].seat = -1; view.room.localTerminalPending = false;
        view.room.terminalPending[0] = true; ++view.room.revision;
        shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
        focus("table-0"); press(MenuInput::Options);
        Check(shell.Navigation().Screen() == "room-table" && row("queue").detail.find("finish returning") != std::string::npos,
            "Table 0's fenced queue row lost its reason");
        shell.Navigation().Home(); shell.Navigation().Push("room"); frame();
        view.session.coordinated = true; view.session.authorityWritable = false; frame();
        focus("table-1"); press(MenuInput::Options);
        Check(shell.Navigation().Screen() == "room-table" && row("queue").detail.find("finish returning") == std::string::npos,
            "Table 0's queue explanation showed on table 1 during a checkpoint");
        view.room.terminalPending[0] = false; view.session.coordinated = false; view.session.authorityWritable = true;
        ++view.room.revision; frame();
    }

    for (const float scale : {1.f, 1.5f}) {
      ApplyTheme(scale); io.Fonts->Build();
      for (const auto size : {ImVec2(1280, 960), ImVec2(640, 720)}) {
        io.DisplaySize = size;
        for (const auto* screen : {"room", "room-table"}) {
            shell.Navigation().Home(); shell.Navigation().Push("room");
            if (std::strcmp(screen, "room-table") == 0) shell.Navigation().Push(screen);
            view.session.control = netplay::Health::Healthy;
            view.session.recovery = netplay::Recovery::None; view.session.error.clear();
            frame(); frame();
            const bool table = shell.Navigation().Screen() == "room-table";
            const char* selected = table ? "queue" : "table-0";
            focus(selected); frame(); frame();
            const auto panel = [&]() -> ImVec4 {
                const char* name = table ? "Menu list" : size.x - 40 * Scale() >= 820 * Scale() ? "Battle slots" : "Room stacked";
                for (const auto* window : ImGui::GetCurrentContext()->Windows)
                    if (window->Active && std::strstr(window->Name, name))
                        return ImVec4(window->Pos.x, window->Pos.y, window->Size.x, window->Size.y);
                throw std::runtime_error("Room refresh regression could not find the visible controls pane");
            };
            const auto baseline = panel();
            const auto baselineQueue = targets.at("queue");
            const auto beforeCycles = actions.size();
            requireMenuFrame = true;
            for (int cycle = 0; cycle < 60; ++cycle) {
                const bool recovering = cycle % 2 == 0;
                view.session.control = recovering ? netplay::Health::Lost : netplay::Health::Healthy;
                view.session.recovery = recovering ? netplay::Recovery::Recovering : netplay::Recovery::None;
                view.session.error = recovering ?
                    "Room control is recovering. Active matches may finish; room actions are paused." : "";
                ++view.session.authorityRevision; ++view.room.revision;
                frame();
                const auto current = panel();
                Check(std::abs(current.x - baseline.x) < 1 && std::abs(current.y - baseline.y) < 1 &&
                    std::abs(current.z - baseline.z) < 1 && std::abs(current.w - baseline.w) < 1,
                    "Repeated room-control refresh resized or moved the controls pane");
                Check(shell.Navigation().Screen() == screen && shell.Navigation().Focus() == selected,
                    "Repeated room-control refresh changed page or focus");
                if (table) {
                    Check(row("queue").enabled != recovering, "Refresh lost the queue eligibility fence");
                    Check(std::abs(targets.at("queue").y - baselineQueue.y) < 1,
                        "Repeated room-control refresh moved Queue under the pointer");
                }
                if (recovering) Check(menuStatus.find("recovering") != std::string::npos,
                    "Stable layout hid the room recovery reason");
            }
    Check(actions.size() == beforeCycles, "Background room refresh dispatched an action");
            requireMenuFrame = false;
        }
      }
    }

    // The runtime increments the match generation as preparation starts. This
    // must cancel stale dialogs without replacing the player's table screen.
    view.session.control = netplay::Health::Healthy;
    view.session.recovery = netplay::Recovery::None; view.session.error.clear();
    shell.Navigation().Home(); shell.Navigation().Push("room-table"); frame();
    ++view.session.generation.match; frame();
    Check(shell.Navigation().Screen() == "room-table",
        "Match preparation replaced the table page with the room overview");
    view.session.match=netplay::MatchState::PostMatch;
    shell.ShowPlay(); frame();
    Check(shell.Navigation().Screen()=="room-table",
        "Reopening Ember after a match discarded the room's table page");

    // Ordinary checkpoint bursts must not make update text appear/disappear
    // every other frame on the room overview, which has no match-status row.
    shell.Navigation().Home(); shell.Navigation().Push("room");
    view.session.coordinated = true;
    view.session.authorityWritable = true; frame();
    const auto healthyStatus=menuStatus;
    for(int cycle=0;cycle<60;++cycle) {
        view.session.authorityWritable=cycle%2!=0; frame();
        Check(menuStatus==healthyStatus,"Brief room updates replaced stable room status");
    }
    view.session.authorityWritable=false;
    for(int i=0;i<20;++i)frame();
    const auto updateStatus = menuStatus;
    Check(updateStatus.find("Updating room")!=std::string::npos,"Sustained checkpoint wait was hidden");
    for (int cycle = 0; cycle < 60; ++cycle) {
        view.session.authorityWritable = cycle % 2 == 0;
        ++view.session.authorityRevision; frame();
        Check(menuStatus == updateStatus, "Room update text flashed between healthy checkpoint revisions");
    }
    view.session.authorityWritable = true;
    for (int i = 0; i < 40; ++i) frame();
    Check(menuStatus != updateStatus, "Room update feedback did not clear after updates settled");
    view.room.members = {local, peer};
    view.room.tables[0].p1=1; view.room.tables[0].p2=2;
    view.room.tables[0].phase=room::TablePhase::Waiting;
    view.room.tables[0].ready[0]=true;
    view.canReady=false; view.canEditSelection=false;
    shell.Navigation().Push("room-table");
    view.session.authorityWritable=true; frame(); focus("ready"); frame();
    const auto pendingReadyDetail=row("ready").detail;
    const auto beforeReadyRefresh=actions.size();
    for(int cycle=0;cycle<60;++cycle) {
        view.session.authorityWritable=cycle%2==0;
        frame(view.session.authorityWritable?0:MenuInput::Select);
        Check(row("ready").detail==pendingReadyDetail,
            "Ready explanation flashed during checkpoint bursts");
        Check(row("ready").enabled==view.session.authorityWritable,
            "Readable feedback changed immediate Unready eligibility");
    }
    Check(actions.size()==beforeReadyRefresh,"Pending checkpoint accepted Unready");
    view.session.authorityWritable=true;
    for(int i=0;i<40;++i)frame();
    Check(row("ready").detail.find("You are READY")!=std::string::npos,
        "Readable update feedback hid settled readiness");
    ++view.session.generation.room; ++view.room.roomEpoch; frame();
    Check(shell.Navigation().Screen()=="room","A new room retained the previous room's table page");

    // Both fighters have readied. Healthy control revisions can arrive before
    // their checkpoint is applied (authorityWritable=false); the committed
    // match status must not flash between preparation and room-update text.
    view = ShellView{};
    view.controllerReady = true;
    view.session.room = netplay::RoomState::Joined;
    view.session.control = netplay::Health::Healthy;
    view.session.coordinated = true;
    view.session.generation.room = 1;
    view.room.roomEpoch = 4; view.room.localMember = 1; view.room.host = 1;
    view.room.members = {local, peer};
    view.room.tables[0].p1 = 1; view.room.tables[0].p2 = 2;
    view.room.tables[0].ready[0] = view.room.tables[0].ready[1] = true;
    view.delayLocked = true;
    for (const float scale : {1.f, 1.5f}) {
      ApplyTheme(scale); io.Fonts->Build();
      for (const auto size : {ImVec2(1280, 960), ImVec2(640, 720)}) {
        io.DisplaySize = size;
        for (const auto phase : {room::TablePhase::Ready, room::TablePhase::Playing, room::TablePhase::Paused}) {
            view.room.tables[0].phase = phase;
            view.session.authorityWritable = true;
            shell.Navigation().Home(); frame(); shell.Navigation().Push("room-table");
            frame(); focus("ready"); frame();
            const auto readyStatus = menuStatus;
            Check(readyStatus.find(phase == room::TablePhase::Ready ? "Preparing match" :
                phase == room::TablePhase::Playing ? "Match in progress" : "Result unresolved") != std::string::npos,
                "Post-Ready fixture did not render the committed match phase");
            const auto readyDetail = row("ready").detail;
            const auto selectionDetail = row("selection").detail;
            // One reason for each hold on the table, worded once.
            Check(selectionDetail == sf4e::loc::T(phase == room::TablePhase::Paused ? "room.result_unresolved.detail" : "room.match_active"),
                "A fighter locked out by a live table was given the wrong reason");
            const auto beforeUpdates = actions.size();
            requireMenuFrame = true;
            for (int cycle = 0; cycle < 60; ++cycle) {
                view.session.authorityWritable = cycle % 2 != 0;
                ++view.session.authorityRevision;
                frame(cycle % 2 == 0 ? MenuInput::Select : 0);
                Check(menuStatus == readyStatus,
                    "Healthy room updates replaced the post-Ready status text");
                Check(row("ready").detail == readyDetail && row("selection").detail == selectionDetail,
                    "Healthy room updates replaced the locked fighter explanation");
                Check(!row("ready").enabled && !row("selection").enabled && !row("unqueue").enabled,
                    "Post-Ready refresh enabled a locked match action");
            }
            Check(actions.size() == beforeUpdates, "Post-Ready refresh dispatched a locked action");
            view.session.authorityWritable = false;
            view.session.control = netplay::Health::Lost;
            view.session.recovery = netplay::Recovery::Recovering; frame();
            Check(menuStatus.find("recovering") != std::string::npos &&
                row("ready").detail.find("recovering") != std::string::npos,
                "Committed match feedback hid a real control disconnect");
            view.session.control = netplay::Health::Healthy;
            view.session.recovery = netplay::Recovery::None;
            view.error = "Match setup failed."; frame();
            Check(menuStatus == view.error, "Committed match feedback hid a runtime error");
            view.error.clear(); view.room.closed = true; frame();
            Check(menuStatus.find("closed") != std::string::npos &&
                row("ready").detail.find("closed") != std::string::npos,
                "Committed match feedback hid room closure");
            view.room.closed = false; view.session.room = netplay::RoomState::Closing; frame();
            Check(menuStatus.find("Leaving room") != std::string::npos &&
                row("ready").detail.find("Leaving room") != std::string::npos,
                "Committed match feedback hid the room-exit transition");
            view.session.room = netplay::RoomState::Joined;
            requireMenuFrame = false;
        }
      }
    }

    // A Paused table has no result coming. Either seated fighter may abandon
    // it with a generation-scoped AbortMatch; the row never exists for a live
    // game or for a spectator.
    ApplyTheme(1.f); io.Fonts->Build(); io.DisplaySize = ImVec2(1280, 960);
    const auto hasRow = [&](const char* id) {
        return std::any_of(rows.begin(), rows.end(), [&](const MenuEntry& entry) { return entry.id == id; });
    };
    view.room.tables[0].phase = room::TablePhase::Playing;
    view.room.tables[0].matchGeneration = 9;
    view.session.authorityWritable = true;
    shell.Navigation().Home(); frame(); shell.Navigation().Push("room-table"); frame();
    Check(!hasRow("abandon-result"), "A live game offered Abandon unresolved game");
    view.room.tables[0].resultPending = true; ++view.room.revision; frame();
    Check(row("selection").detail == sf4e::loc::T("room.awaiting_result") &&
        row("unqueue").detail == sf4e::loc::T("room.awaiting_result"),
        "Fighter change and Leave seat did not agree while a result is awaited");
    view.room.tables[0].resultPending = false; ++view.room.revision; frame();
    // The table back in Waiting while this client's own game is still under
    // way holds the seat with the same words B gives.
    view.room.tables[0].phase = room::TablePhase::Waiting; view.session.match = netplay::MatchState::Playing;
    view.room.tables[0].ready[0] = view.room.tables[0].ready[1] = false;
    ++view.room.revision; frame();
    Check(!row("unqueue").enabled && row("unqueue").detail == sf4e::loc::T("room.leave_seat.finish_first"),
        "Leave seat did not say to finish this client's own game");
    view.session.match = netplay::MatchState::None; view.room.tables[0].phase = room::TablePhase::Playing;
    view.room.tables[0].ready[0] = view.room.tables[0].ready[1] = true;
    ++view.room.revision; frame();
    view.room.tables[0].phase = room::TablePhase::Paused; ++view.room.tables[0].revision; ++view.room.revision;
    for (int i = 0; i < 5; ++i) frame();
    Check(hasRow("abandon-result") && row("abandon-result").enabled, "A seated fighter cannot abandon an unresolved game");
    Check(!row("unqueue").enabled && row("unqueue").detail.find("Abandon") != std::string::npos,
        "Leave seat does not point a fighter at Abandon unresolved game");
    focus("abandon-result"); const auto beforeAbandon = actions.size(); press(MenuInput::Select);
    Check(actions.size() == beforeAbandon, "Abandon confirmation did not default to Cancel");
    press(MenuInput::Right); press(MenuInput::Select);
    Check(actions.size() == beforeAbandon + 1 && actions.back().roomAction.kind == room::ActionKind::AbortMatch &&
        actions.back().roomAction.matchGeneration == 9, "Confirmed abandon did not submit a generation-scoped AbortMatch");
    view.room.tables[0].p1 = 3; view.room.members[0].seat = -1; view.room.tables[0].spectators = {1};
    ++view.room.tables[0].revision; ++view.room.revision;
    for (int i = 0; i < 5; ++i) frame();
    Check(!hasRow("abandon-result"), "A spectator was offered Abandon unresolved game");

    // Catalog changes rebuild rows, but their stable IDs preserve all local
    // interaction and room state. A draft remains owned by the same editor.
    view.session.room = netplay::RoomState::Joined;
    view.session.control = netplay::Health::Healthy;
    view.session.coordinated = view.session.authorityWritable = true;
    view.room.closed = false;
    // A new game at a table closes a confirmation about the last one on the
    // table's own screen, but never a message or a name being typed elsewhere.
    shell.Navigation().Home(); shell.Navigation().Push("room-table"); frame();
    focus("leave"); press(MenuInput::Select);
    Check(shell.Navigation().Confirming(), "Leave room did not ask for confirmation");
    for (auto& table : view.room.tables) ++table.matchGeneration;
    frame();
    Check(!shell.Navigation().Confirming(), "A new game left a confirmation about the last one open");
    const auto draft = [&] { return std::find_if(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "compose"; })->value; };
    shell.Navigation().Home(); shell.Navigation().Push("room-chat");
    for (int i = 0; i < 4; ++i) frame();
    io.AddInputCharactersUTF8("draft survives a new game"); frame(); frame();
    Check(draft() == "draft survives a new game", "Chat draft fixture did not take typed text");
    for (auto& table : view.room.tables) ++table.matchGeneration;
    frame(); frame();
    Check(shell.Navigation().Screen() == "room-chat" && draft() == "draft survives a new game",
        "A new game at the selected table discarded the chat being typed");
    shell.Navigation().Home(); shell.Navigation().Push("room-admin"); frame();
    focus("rename"); shell.Navigation().Choose(rows);
    Check(shell.Navigation().Editing(), "Room name fixture did not enter the editor");
    for (auto& table : view.room.tables) ++table.matchGeneration;
    frame(); frame();
    Check(shell.Navigation().Editing(), "A new game discarded the room name being typed");
    shell.Navigation().Cancel();
    shell.Navigation().Home(); shell.Navigation().Push("room-chat");
    for (int i = 0; i < 4; ++i) frame();
    io.AddInputCharactersUTF8(" and a locale switch"); frame(); frame();
    const auto localeScreen = shell.Navigation().Screen();
    const auto localeFocus = shell.Navigation().Focus();
    const auto localeDraft = draft();
    Check(localeDraft == "draft survives a new game and a locale switch", "Chat draft fixture did not keep adding to the draft");
    const auto localeActions = actions.size();
    const auto localeEpoch = view.room.roomEpoch;
    const auto localeMembers = view.room.members.size();
    sf4e::loc::SetActive(sf4e::loc::Locale::PtBR); frame();
    Check(shell.Navigation().Screen() == localeScreen && shell.Navigation().Focus() == localeFocus,
        "Locale switch changed room screen or focus");
    Check(draft() == localeDraft, "Locale switch discarded the chat draft");
    Check(actions.size() == localeActions && view.room.roomEpoch == localeEpoch && view.room.members.size() == localeMembers,
        "Locale switch mutated room state or submitted a command");
    // Scripts Inter lacks rebuild the atlas mid-screen; the room keeps its place.
    for (const auto locale : {sf4e::loc::Locale::Ja, sf4e::loc::Locale::Ru, sf4e::loc::Locale::ZhHans}) {
        sf4e::loc::SetActive(locale);
        Check(ApplyTheme(1.f), "A language needing other glyphs kept the old atlas");
        io.Fonts->Build(); frame();
        Check(shell.Navigation().Screen() == localeScreen && shell.Navigation().Focus() == localeFocus &&
            draft() == localeDraft,
            "Atlas rebuild for a language switch lost the screen, focus or draft");
        Check(!ApplyTheme(1.f), "An unchanged language rebuilt the atlas again");
    }
    // The atlas was rebuilt between frames, so ImGui::GetFont() still names the
    // freed font until the next NewFrame; the rebuilt default font is current.
    // Japanese text is drawable while Japanese is active, and every native
    // language name stays drawable in any language for the picker.
    sf4e::loc::SetActive(sf4e::loc::Locale::Ja); ApplyTheme(1.f); io.Fonts->Build();
    Check(io.FontDefault->FindGlyphNoFallback(0x8A2D) != nullptr, "Japanese catalog glyph missing from the atlas");
    sf4e::loc::SetActive(sf4e::loc::Locale::En); ApplyTheme(1.f); io.Fonts->Build();
    // Inter draws Latin and Cyrillic alike, so moving between them keeps the atlas.
    sf4e::loc::SetActive(sf4e::loc::Locale::Ru);
    Check(!ApplyTheme(1.f), "A language Inter already draws rebuilt the atlas");
    sf4e::loc::SetActive(sf4e::loc::Locale::En);
    for (const ImWchar glyph : {ImWchar(0x8A9E), ImWchar(0xD55C), ImWchar(0x7B80), ImWchar(0x0420)})
        Check(io.FontDefault->FindGlyphNoFallback(glyph) != nullptr, "A language's native name is not drawable in English");
    sf4e::loc::SetActive(sf4e::loc::Locale::En);

    // Names, room names and chat use characters no catalog does. Noting them
    // queues one rebuild; afterwards the atlas draws Han, kana and Hangul
    // whatever the interface language, and a second batch waits for the interval.
    ApplyTheme(1.f); io.Fonts->Build();
    const auto drawable = [&](ImWchar glyph) { return io.FontDefault->FindGlyphNoFallback(glyph) != nullptr; };
    Check(!drawable(0x5F20) && !drawable(0xAE40), "A character outside every catalog was already in the atlas");
    Check(!ApplyTheme(1.f), "The atlas rebuilt with no player text to draw");
    SetUserGlyphRebuildInterval(std::chrono::milliseconds(0));
    NoteUserText("Ünïcödé Привет ASCII");
    Check(!ApplyTheme(1.f), "Text Inter already draws rebuilt the atlas");
    NoteUserText("张伟 田中 김민수 ひらがな");
    Check(ApplyTheme(1.f), "Player text needing glyphs did not rebuild the atlas");
    io.Fonts->Build();
    for (const ImWchar glyph : {ImWchar(0x5F20), ImWchar(0x4F1F), ImWchar(0x7530), ImWchar(0x4E2D), ImWchar(0xAE40), ImWchar(0xBBFC), ImWchar(0xC218), ImWchar(0x3072)})
        Check(drawable(glyph), "A character of player text is not drawable after the rebuild");
    Check(!ApplyTheme(1.f), "Noted text rebuilt the atlas twice");
    SetUserGlyphRebuildInterval(std::chrono::hours(1));
    NoteUserText("雪");
    Check(!ApplyTheme(1.f), "A second batch of player text rebuilt inside the interval");
    SetUserGlyphRebuildInterval(std::chrono::milliseconds(0));
    Check(ApplyTheme(1.f), "A waiting batch of player text never rebuilt the atlas");
    io.Fonts->Build();
    Check(drawable(0x96EA), "The waiting batch is not drawable after its rebuild");
    // A language change keeps what players have written.
    sf4e::loc::SetActive(sf4e::loc::Locale::Ja); ApplyTheme(1.f); io.Fonts->Build();
    Check(drawable(0xAE40) && drawable(0x5F20), "A language change dropped the player text glyphs");
    sf4e::loc::SetActive(sf4e::loc::Locale::En); ApplyTheme(1.f); io.Fonts->Build();

    // The glyph budget follows the text on screen. A chat of more distinct
    // characters than the cap fills it, names still outrank that chat, and once
    // the room's text stops being drawn a later name gets its glyph, where a
    // budget that only filled up would refuse it for good.
    const auto encode = [](unsigned codepoint) {
        std::string text;
        text += static_cast<char>(0xE0 | (codepoint >> 12));
        text += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (codepoint & 0x3F));
        return text;
    };
    std::string chat;
    for (unsigned codepoint = 0x4E00; codepoint < 0x4E00 + 600; ++codepoint) chat += encode(codepoint);
    chat += encode(0x9F9F); // the 601st distinct character, past the cap
    Check(!drawable(0x9F99) && !drawable(0x971C) && !drawable(0x9F9F), "A later name's character was already in the atlas");
    NoteUserText(chat, UserTextRole::Chat);
    NoteUserText(encode(0x9F99), UserTextRole::Name);
    Check(ApplyTheme(1.f), "A full chat and a name did not rebuild the atlas");
    io.Fonts->Build();
    Check(drawable(0x9F99), "A name lost its glyph to a chat that filled the budget");
    Check(drawable(0x4E00 + 20), "The chat's first characters are not drawable");
    Check(!drawable(0x9F9F), "The glyph budget did not cap the chat's distinct characters");
    // Everything that was on screen retires; the next name is drawable.
    SetUserGlyphRetention(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    NoteUserText(encode(0x971C) + encode(0x9F9F), UserTextRole::Name);
    Check(ApplyTheme(1.f), "A name after retired room text did not rebuild the atlas");
    io.Fonts->Build();
    Check(drawable(0x971C) && drawable(0x9F9F), "A name shown after the budget filled up is not drawable");
    Check(!drawable(0x9F99) && !drawable(0x4E00 + 20), "Text no longer on screen kept its glyphs at the next rebuild");
    Check(drawable(0x8A9E) && drawable(0xD55C), "Retiring player text dropped the native language names");
    SetUserGlyphRetention(std::chrono::seconds(10));
    SetUserGlyphRebuildInterval(std::chrono::milliseconds(250));

    // The room link row belongs to a public room this shell joined from the list: a private room
    // and a server-owned room reached any other way offer none (the journeys cover the row itself).
    const auto offers = [&](const char* id) {
        return std::any_of(rows.begin(), rows.end(), [&](const MenuEntry& entry) { return entry.id == id; });
    };
    view.session.room = netplay::RoomState::Joined; view.canEditSelection = true;
    shell.Navigation().Home(); frame(); shell.Navigation().Push("room"); frame(); frame();
    Check(offers("copy") && !offers("copy-room-link"), "A private room offers a room link or lost its invitation row");
    view.room.serverOwned = true; frame(); frame();
    Check(!offers("copy") && !offers("copy-short") && !offers("copy-room-link") && offers("leave"),
        "A server-owned room not joined from the list offers an invitation or a room link");
    view.room.serverOwned = false;

    SetMenuStatusProbe({}); SetMenuCardProbe({}); SetMenuEntriesProbe({});
    std::cout << "Room controls and uninterrupted controller, keyboard, mouse and queue frames passed.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
