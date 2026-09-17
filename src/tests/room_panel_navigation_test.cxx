#include "../ui/ApplicationShell.hxx"
#include "../ui/GameMenu.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/Theme.hxx"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
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
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = ImVec2(1280, 960);
    ApplyTheme(1); io.Fonts->Build();

    ShellView view;
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
        io.DeltaTime = 1.0f / 60.0f; SetMenuInput({buttons, 0}); ImGui::NewFrame();
        shell.Draw(view, &open, [&](ShellAction action) { actions.push_back(std::move(action)); return true; }, [&] { ++selectionDraws; });
        ImGui::Render();
        if (requireMenuFrame) {
            Check(menuDraws == beforeDraws + 1, "Room navigation skipped its menu body for a frame");
            Check(ImGui::GetDrawData()->TotalVtxCount > 500, "Room navigation emitted an empty menu frame");
            if (screen == "room-table" || screen == "room-rules")
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
    Check(row("benchmark-connection").enabled, "Seated player cannot start benchmark");
    view.probeBenchmark=true;
    view.probeStatus = "checking"; frame();
    Check(!row("benchmark-connection").enabled, "Running check allowed overlapping benchmark");
    Check(row("recommended-delay").detail.find("30 seconds")!=std::string::npos, "Benchmark duration is hidden");
    view.probeBenchmark=false;
    Check(!row("check-connection").enabled && row("check-connection").label == "Checking connection...",
        "A running connection check still accepts another request");
    Check(row("recommended-delay").value == "Checking...", "Connection check progress is not visible");
    Check(!row("apply-recommendation").enabled, "Running check allowed a stale recommendation");
    view.probeStatus = "unavailable"; view.recommendedDelay = -1; frame();
    Check(row("check-connection").label == "Retry connection check" &&
        row("recommended-delay").detail.find("Ready") != std::string::npos,
        "Failed connection check does not explain retry or manual Ready");
    view.probeStatus.clear(); view.recommendedDelay = 4; frame();
    Check(row("leave").enabled, "Leave room is missing from the table screen");
    view.session.coordinated = true; view.session.authorityWritable = false; frame();
    // A checkpoint is the runtime's business: Ready stays pressable and the
    // seated table keeps its seat line instead of room-update chatter.
    Check(row("ready").enabled && !row("check-connection").enabled && row("leave").enabled,
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
    Check(!row("ready").enabled && !row("leave").enabled && menuStatus.find("Leaving room") != std::string::npos,
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
    shell.Navigation().Home(); shell.Navigation().Push("room-table"); frame();
    Check(row("recommended-delay").value == "4 frames" && !row("recommended-delay").adjustable,
        "Recommended delay was not read-only");
    Check(row("selected-delay").enabled && row("selected-delay").value == "2",
        "Selected delay was not adjustable");
    Check(row("check-connection").enabled && row("apply-recommendation").enabled,
        "Delay actions were not available");
    focus("check-connection"); press(MenuInput::Select);
    Check(actions.back().command.kind == netplay::CommandKind::CheckConnection && actions.back().selectedDelay == -1,
        "Check connection did not use its command seam");
    focus("apply-recommendation"); press(MenuInput::Select);
    Check(actions.back().command.kind == netplay::CommandKind::ApplyDelay && actions.back().selectedDelay == -1,
        "Apply recommendation did not request the measured value");
    focus("selected-delay"); press(MenuInput::Right);
    Check(actions.back().command.kind == netplay::CommandKind::ApplyDelay && actions.back().selectedDelay == 3,
        "Manual delay did not submit the bounded value");

    view.session.recovery = netplay::Recovery::Recovering;
    view.session.error = "Room control is recovering. Room actions are paused."; frame();
    Check(!row("ready").enabled && menuStatus.find("recovering") != std::string::npos,
        "Recovery did not freeze room actions with an immediate reason");
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
    focus("table-0"); press(MenuInput::Select);
    Check(shell.Navigation().Screen() == "room-table", "Select did not enter the table");
    focus("queue"); const auto beforeQueue = actions.size(); press(MenuInput::Select);
    Check(actions.size() == beforeQueue + 1 && actions.back().roomAction.kind == room::ActionKind::Queue,
        "Queue entry submitted zero or duplicate actions");
    view.room.tables[0].queue = {1}; ++view.room.revision; frame();
    for (int i = 0; i < 20; ++i) frame();
    focus("room-rules"); press(MenuInput::Select);
    Check(shell.Navigation().Screen() == "room-rules", "Queue submenu did not open");
    press(MenuInput::Back);
    Check(shell.Navigation().Screen() == "room-table", "Queue submenu did not return");

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
    click(targets.at("room-rules")); frame();
    Check(shell.Navigation().Screen() == "room-rules", "Mouse did not open table rules");
    // Locate Back from the shell padding and the fixed non-home header height.
    const auto* window = ImGui::FindWindowByName("SF4 Ember Netplay###EmberShell");
    const ImVec2 back(window->Pos.x + window->WindowPadding.x + 20,
        window->Pos.y + window->WindowPadding.y + 64 * Scale() +
        ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight() * .5f);
    click(back);
    Check(shell.Navigation().Screen() == "room-table", "Mouse Back did not return to the table");
    frame(); requireMenuFrame = false;

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
    frame(); focus("room-rules"); press(MenuInput::Select);
    const auto rulesFocus = shell.Navigation().Focus();
    ++view.session.authorityRevision; ++view.room.revision; frame();
    Check(shell.Navigation().Screen() == "room-rules" && shell.Navigation().Focus() == rulesFocus,
        "Same-room control refresh reset submenu navigation to the room overview");

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

    SetMenuStatusProbe({}); SetMenuCardProbe({}); SetMenuEntriesProbe({}); ImGui::DestroyContext();
    std::cout << "Room controls and uninterrupted controller, keyboard, mouse and queue frames passed.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
