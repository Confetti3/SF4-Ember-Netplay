// Ember opened over a room game this PC only watches lands on the board at
// Stop watching, which asks first and submits one leave naming the watched
// game (commands::LeaveWatchedGame); the runtime decides what leaving it
// takes when it runs. A watcher by choice is offered Stop watching, a queued
// member Leave this game. Once the room has finished the game while its stream
// still plays, the row stays and says so. The row and dispatch read the same
// rule (netplay::LeaveOfWatchedGame), so a press never outlives the game it
// was about, and a room still catching up on its checkpoint takes the leave
// all the same. The table's own Unwatch of that game is the same leave.
#include "../ui/ApplicationShell.hxx"
#include "../common/Localization.hxx"
#include "../ui/GameMenu.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/Theme.hxx"
#include "imgui_test_support.hxx"
#include "shell_action_support.hxx"
#include <imgui.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace sf4e;
using namespace sf4e::ui;

void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// A room whose table 0 plays a live game (generation 9) between members 2 and
// 3, which this PC (member 1) watches with Ember hidden.
struct WatchedBoard {
    ShellView view;
    ApplicationShell shell;
    std::vector<ShellAction> actions;
    std::vector<MenuEntry> rows;
    std::string status;
    bool open = true;
    WatchedBoard() {
        view.controllerReady = true;
        view.session.room = netplay::RoomState::Joined;
        view.session.control = netplay::Health::Healthy;
        view.session.generation.room = 1;
        view.session.match = netplay::MatchState::Playing;
        view.room.roomEpoch = 4; view.room.localMember = 1; view.room.host = 2; view.room.capacity = 16;
        for (int i = 0; i < 4; ++i) view.room.tables[i].id = i;
        room::Member local; local.id = 1; local.name = "Local"; local.table = 0; local.status = room::MemberStatus::Watching;
        room::Member p1; p1.id = 2; p1.name = "Peer"; p1.table = 0; p1.seat = 0; p1.status = room::MemberStatus::Playing;
        room::Member p2; p2.id = 3; p2.name = "Third"; p2.table = 0; p2.seat = 1; p2.status = room::MemberStatus::Playing;
        view.room.members = {local, p1, p2};
        auto& table = Table();
        table.p1 = 2; table.p2 = 3; table.phase = room::TablePhase::Playing; table.matchGeneration = 9; table.revision = 5;
        table.spectators = {1};
        view.watchedGame = {0, 9};
        SetMenuEntriesProbe([this](const std::vector<MenuEntry>& entries) { rows = entries; });
        SetMenuStatusProbe([this](const char* text, Tone) { status = text; });
    }
    ~WatchedBoard() { SetMenuEntriesProbe({}); SetMenuStatusProbe({}); }
    room::Table& Table() { return view.room.tables[0]; }
    room::Member& Local() { return view.room.members[0]; }
    ApplicationShell::Submit Submit() { return [this](ShellAction action) { actions.push_back(std::move(action)); return true; }; }
    void Frame(unsigned buttons = 0, float seconds = 1.f / 60) {
        ImGui::GetIO().DeltaTime = seconds; SetMenuInput({buttons, 0}); ImGui::NewFrame();
        shell.Draw(view, &open, Submit(), [] {});
        ImGui::Render();
    }
    void Press(unsigned buttons) { Frame(); Frame(buttons); Frame(); }
    // Start or F10 over the game: the frame after a hidden one opens Ember.
    void OpenOverGame() {
        shell.Background(view, view.room, Submit());
        shell.Navigation().Home();
        for (int i = 0; i < 3; ++i) Frame();
    }
    // Asks with Select, then answers the question's confirm button.
    void Confirm() { Press(MenuInput::Select); Press(MenuInput::Right); Press(MenuInput::Select); }
    bool Has(const char* id) const {
        return std::any_of(rows.begin(), rows.end(), [&](const MenuEntry& entry) { return entry.id == id; });
    }
    const MenuEntry& Row(const char* id) const {
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& entry) { return entry.id == id; });
        if (found == rows.end()) throw std::runtime_error(std::string("missing room row: ") + id);
        return *found;
    }
    // Other submissions (a preference save) may ride along; only the leaves
    // count. No press may send an Unwatch of its own for the watched game.
    std::vector<netplay::WatchedGame> Leaves() const {
        std::vector<netplay::WatchedGame> sent;
        for (const auto& action : actions) {
            Check(!test::RoomActionIs(action, room::ActionKind::Unwatch),
                "A press sent an Unwatch of its own instead of the watched game's leave");
            if (const auto* leave = test::As<commands::LeaveWatchedGame>(action)) sent.push_back(leave->leaveWatchedGame);
        }
        return sent;
    }
    bool LeftOnce() const {
        const auto sent = Leaves();
        return sent.size() == 1 && sent[0] == netplay::WatchedGame{0, 9};
    }
    // The room has taken the member off the table: no longer listed, and its lock-in gone.
    void LeftTable() {
        Table().spectators.clear(); Local().table = -1; Local().spectatorLocked = false;
        Local().status = room::MemberStatus::Idle;
        ++Table().revision; ++view.room.revision;
    }
};

// A watcher by choice lands on Stop watching, is asked first, and leaves the
// table with one Unwatch scoped to the live game.
void ByChoiceWatcherStopsWatching() {
    WatchedBoard board;
    board.OpenOverGame();
    Check(board.shell.Navigation().Screen() == "room" && board.shell.Navigation().Focus() == "stop-watching",
        "Ember opened over a watched game did not land on Stop watching");
    const auto& row = board.Row("stop-watching");
    Check(row.enabled && row.confirm && row.label == loc::T("room.stop_watching") && row.detail == loc::T("room.stop_watching.detail"),
        "Stop watching was not an enabled row that asks first");
    board.Press(MenuInput::Select);
    Check(board.Leaves().empty(), "Stop watching did not ask first");
    board.Press(MenuInput::Right); board.Press(MenuInput::Select);
    Check(board.LeftOnce(), "Stop watching did not submit one leave of the watched game");
    // Leaving the table and the room both say what they end over the game.
    Check(board.Row("leave").detail.find(loc::T("room.leave.watching")) != std::string::npos,
        "Leave room did not say it ends the spectator view");
    const auto& card = board.Row("table-1");
    Check(!card.choices.empty() && std::all_of(card.choices.begin(), card.choices.end(), [](const MenuChoice& choice) {
        return choice.id == "options" || choice.detail.find(loc::T("room.watching_ends")) != std::string::npos;
    }), "A seat choice elsewhere did not say it ends the spectator view");
    // Once Ember is shown, the board stays where the player goes.
    board.shell.Navigation().Home(); board.Frame();
    Check(board.shell.Navigation().Screen() != "room", "A drawn frame moved the player back to Stop watching");
    std::cout << "ByChoiceWatcherStopsWatching passed\n";
}

// A queued member watches the game it waits out: Leave this game leaves that
// game alone and keeps the queue place (the runtime's Unwatch keeps watching).
void QueuedWatcherLeavesTheGame() {
    WatchedBoard board;
    board.Table().queue = {1}; board.Local().status = room::MemberStatus::Queued;
    board.OpenOverGame();
    Check(board.shell.Navigation().Focus() == "stop-watching" && board.Row("stop-watching").label == loc::T("room.leave_game") &&
        board.Row("stop-watching").detail == loc::T("room.leave_game.detail"), "A queued watcher was not offered Leave this game");
    board.Confirm();
    Check(board.LeftOnce(), "Leave this game did not submit one leave of the watched game");
    std::cout << "QueuedWatcherLeavesTheGame passed\n";
}

// Once the game is no longer watched the row and its question go, and
// nothing is sent.
void DispatchReadsTheRuleWhenAnswered() {
    WatchedBoard board;
    board.OpenOverGame();
    board.Press(MenuInput::Select);
    board.view.watchedGame = {}; board.Frame();
    Check(!board.Has("stop-watching") && !board.shell.Navigation().Confirming(),
        "Stop watching, or the question about it, outlived the watched game");
    board.Press(MenuInput::Right); board.Press(MenuInput::Select);
    Check(board.Leaves().empty(), "A press after the stream ended still sent a leave");
    std::cout << "DispatchReadsTheRuleWhenAnswered passed\n";
}

// The table page's own Stop watching of the game the room still plays is the
// same leave, never an Unwatch scoped to whatever the table plays when it lands.
void TableUnwatchIsTheSameLeave() {
    WatchedBoard board;
    board.OpenOverGame();
    board.shell.Navigation().Push("room-table"); board.Frame();
    board.shell.Navigation().Focus("unwatch", board.rows);
    board.Press(MenuInput::Select);
    Check(board.LeftOnce(), "The table's own Stop watching did not submit the watched game's leave");
    std::cout << "TableUnwatchIsTheSameLeave passed\n";
}

// A fighter has no spectator's exit, whatever the context says.
void FighterHasNoWatchedExit() {
    WatchedBoard board;
    board.Table().p1 = 1; board.Local().seat = 0; board.Local().status = room::MemberStatus::Playing;
    board.Table().spectators.clear();
    board.view.room.members[1].table = -1; board.view.room.members[1].seat = -1;
    board.OpenOverGame();
    Check(!board.Has("stop-watching") && board.Leaves().empty(), "A fighter was offered Stop watching");
    std::cout << "FighterHasNoWatchedExit passed\n";
}

// A locked-in watcher of the live game, Ember open over it. How its lock-in
// ended is told from the room state that follows, whoever caused it: it left
// the table (stopped), watches another (moved), or took a place (playing).
// Still watching the table it held, the lock-in went by the player's own
// Release and nothing is told, however late the room shows it, unless the
// runtime counted a failed view (lockInDrops), which is told once, when counted.
struct LockedBoard : WatchedBoard {
    LockedBoard() { Local().spectatorLocked = true; OpenOverGame(); }
    void Release() {
        shell.Navigation().Push("room-table"); Frame();
        shell.Navigation().Focus("lock-spectating", rows); Press(MenuInput::Select);
        Check(!actions.empty() && test::Is<commands::RoomAction>(actions.back(), [](const commands::RoomAction& sent) {
            return sent.roomAction.kind == room::ActionKind::LockSpectating && !sent.roomAction.locked; }), "Release lock-in was not sent");
    }
    // The table page's Stop watching, which is the watched game's leave.
    void StopWatching() {
        shell.Navigation().Focus("unwatch", rows); Press(MenuInput::Select);
        Check(LeftOnce(), "Stop watching did not submit the watched game's leave");
    }
    // The room's projection after a Release it accepted: unlocked, still watching.
    void Unlock() { Local().spectatorLocked = false; ++Table().revision; ++view.room.revision; }
    // The lock-in notice the status line shows a few frames after a change; empty for none.
    std::string Told() {
        status.clear();
        for (int i = 0; i < 3; ++i) Frame();
        for (const char* id : {"room.lock_in_ended.stopped", "room.lock_in_ended.moved", "room.lock_in_ended.playing", "room.lock_in_ended.dropped"})
            if (status == loc::T(id)) return id;
        return {};
    }
    // Lets a notice shown run out.
    void Settle() { Frame(0, 9.f); Frame(); }
};

void OwnReleaseIsNeverTold() {
    for (const bool late : {false, true}) {
        LockedBoard board;
        board.Release();
        if (late) {
            // Accepted only after more than 30 s, once the table has begun its next game.
            for (int i = 0; i < 40; ++i) board.Frame(0, 1.f);
            board.Table().matchGeneration = 10; board.view.watchedGame = {};
            ++board.Table().revision; ++board.view.room.revision;
            board.Settle();
        }
        board.Unlock();
        Check(board.Told().empty(), "The player's own Release was told as a lost lock-in");
    }
    std::cout << "OwnReleaseIsNeverTold passed\n";
}

// A Release the room refused leaves the lock-in, and a failed view then loses
// it: told once, when the runtime counts it, and not again when the room's
// release shows.
void FailedViewIsToldOnce() {
    LockedBoard board;
    board.Release();
    board.Settle();
    board.view.lockInDrops = 1;
    Check(board.Told() == "room.lock_in_ended.dropped", "A lock-in lost to a failed view was not told");
    board.Settle();
    Check(board.Told().empty(), "A lock-in lost to a failed view was told twice");
    board.Unlock();
    Check(board.Told().empty(), "The room's release after a failed view was told again");
    std::cout << "FailedViewIsToldOnce passed\n";
}

// Release and Stop watching both in flight, answered one at a time, either
// refused. With no failed view counted nothing says dropped, and the notice is
// the final state's: a lock-in still held when Stop watching lands is told
// stopped; one the Release already ended has nothing left to tell.
void ReleaseAndStopWatchingInFlight() {
    struct Case { bool unwatchFirst, releaseTaken, unwatchTaken; const char* told; };
    const Case cases[] = {
        {false, true, true, ""}, {false, false, true, "room.lock_in_ended.stopped"}, {false, true, false, ""},
        {true, false, true, "room.lock_in_ended.stopped"}, {false, false, false, ""},
    };
    for (const auto& test : cases) {
        LockedBoard board;
        board.Release(); board.StopWatching();
        std::string told;
        const auto answer = [&](bool unwatch) {
            if (unwatch && test.unwatchTaken) { board.LeftTable(); board.view.watchedGame = {}; }
            else if (!unwatch && test.releaseTaken) board.Unlock();
            const auto shown = board.Told();
            Check(shown != "room.lock_in_ended.dropped", "An own Release or Stop watching was told as a failed view");
            if (!shown.empty()) told = shown;
            board.Settle();
        };
        answer(test.unwatchFirst); answer(!test.unwatchFirst);
        Check(told == test.told, "Release and Stop watching in flight did not tell the final state's notice");
    }
    std::cout << "ReleaseAndStopWatchingInFlight passed\n";
}

// Stop watching from Ember over the fight is told like any other end that
// leaves the table: true, and the same rule whoever caused it. Moving to
// another table or taking a queue place says so.
void LeavingTheTableIsTold() {
    {
        LockedBoard board;
        board.Confirm();
        Check(board.LeftOnce(), "A locked-in watcher's Stop watching submitted no leave");
        board.LeftTable(); board.view.watchedGame = {};
        Check(board.Told() == "room.lock_in_ended.stopped", "Stop watching did not say the lock-in went with it");
    }
    {
        LockedBoard board;
        board.Table().spectators.clear(); board.view.room.tables[1].spectators = {1};
        board.Local().table = 1; board.Unlock();
        Check(board.Told() == "room.lock_in_ended.moved", "Moving to another table did not say the lock-in stayed behind");
    }
    {
        LockedBoard board;
        board.Table().queue = {1}; board.Local().status = room::MemberStatus::Queued; board.Unlock();
        Check(board.Told() == "room.lock_in_ended.playing", "Taking a queue place did not say it ended the lock-in");
    }
    std::cout << "LeavingTheTableIsTold passed\n";
}

// A room still catching up on its checkpoint takes the leave; a room whose
// control is recovering does not.
void CatchingUpRoomTakesTheLeave() {
    {
        WatchedBoard board;
        board.view.session.coordinated = true; board.view.session.authorityWritable = false;
        board.OpenOverGame();
        Check(board.Row("stop-watching").enabled, "Stop watching waited for the room to catch up");
        const auto& choices = board.Row("table-0").choices;
        Check(std::any_of(choices.begin(), choices.end(), [](const MenuChoice& choice) { return choice.id == "unwatch" && choice.enabled; }),
            "The watched table's own Unwatch choice waited for the room to catch up");
        board.Confirm();
        Check(board.LeftOnce(), "A room catching up did not take Stop watching");
    }
    {
        WatchedBoard board;
        board.view.session.control = netplay::Health::Lost;
        board.OpenOverGame();
        Check(board.Has("stop-watching") && !board.Row("stop-watching").enabled, "Stop watching was offered over a lost room control");
        board.Confirm();
        Check(board.Leaves().empty(), "Stop watching was sent over a lost room control");
    }
    std::cout << "CatchingUpRoomTakesTheLeave passed\n";
}

// The room has finished the watched game while its delayed stream still plays:
// the table is back in Waiting, a queued watcher was rotated into a seat, or
// the table already plays its next game. Stop watching stays, says the game
// is over, and needs no room, so a lost control does not take it away. Its
// leave names the watched game, never the table's newer one, and the runtime
// only ends the rest of the stream (LeaveWatchedGame).
void FinishedGameEndsOnlyTheView() {
    enum class After { Waiting, RotatedIntoSeat, NextGame };
    for (const auto after : {After::Waiting, After::RotatedIntoSeat, After::NextGame}) {
        WatchedBoard board;
        if (after == After::RotatedIntoSeat) board.Table().queue = {1};
        board.OpenOverGame();
        auto& table = board.Table();
        table.phase = room::TablePhase::Waiting; ++table.revision; ++board.view.room.revision;
        if (after == After::RotatedIntoSeat) {
            // The loser went to the queue and the queued watcher took its seat.
            table.queue = {3}; table.spectators = {3}; table.p2 = 1;
            board.Local().seat = 1; board.Local().status = room::MemberStatus::Idle;
            board.view.room.members[2].seat = -1; board.view.room.members[2].status = room::MemberStatus::Queued;
        }
        if (after == After::NextGame) { table.phase = room::TablePhase::Playing; table.matchGeneration = 10; }
        board.view.session.control = netplay::Health::Lost;
        board.Frame();
        const auto& row = board.Row("stop-watching");
        Check(row.enabled && row.label == loc::T("room.stop_watching") && row.detail == loc::T("room.stop_watching.finished"),
            "Stop watching did not stay for the rest of a finished game's stream");
        board.Confirm();
        Check(board.LeftOnce(), "The rest of a finished game's stream was not left with one leave naming it");
    }
    std::cout << "FinishedGameEndsOnlyTheView passed\n";
}
}

int main() try {
    HeadlessImGui imgui;
    ByChoiceWatcherStopsWatching();
    QueuedWatcherLeavesTheGame();
    DispatchReadsTheRuleWhenAnswered();
    TableUnwatchIsTheSameLeave();
    FighterHasNoWatchedExit();
    OwnReleaseIsNeverTold();
    FailedViewIsToldOnce();
    ReleaseAndStopWatchingInFlight();
    LeavingTheTableIsTold();
    CatchingUpRoomTakesTheLeave();
    FinishedGameEndsOnlyTheView();
    std::cout << "Watched match exit tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
