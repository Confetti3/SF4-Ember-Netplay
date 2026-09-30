#pragma once
// What the room panel's controls offer and refuse, worded for the player:
// seat choices, Leave, Ready and fighter changes. Each reads the shell view
// only, so the board, the table options and dispatch agree on one answer.
#include "ApplicationShell.hxx"
#include "MenuNavigation.hxx"
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace room_controls {
// This client's own game, as the seat rules ask about it.
bool GameLive(const ShellView& v);
bool PostMatch(const ShellView& v);
// Each rule the model states once (room::SeatHold) is worded for the control
// that asks: leaving the seat, or changing fighter.
const char* LeaveBlockerKey(room::SeatHold hold);
// The durable receipt fence gates admission to a table, never departure from
// it: the member the room is waiting on can be exactly the one that needs to
// leave. RoomAuthority accepts Unqueue and Unwatch while a receipt is
// outstanding, so neither is fenced here.
bool TerminalFenced(const room::Snapshot& snapshot, room::ActionKind kind, std::size_t table);
const char* TerminalPendingReason();
// One description of Leave room for both screens, stating what it costs.
std::string LeaveRoomDetail(const ShellView& v);
// The seat chooser's options. A choice carries the option's id, so dispatch
// never re-derives what the player picked from a newer snapshot.
// Each has a button label short enough for three to a card, and its full
// wording.
struct SeatOption { const char* id; room::ActionKind kind; int seat; const char* label; const char* detail; };
const SeatOption* FindSeatOption(const std::string& id);
// Left to right: both seats when the table is empty, otherwise play (the
// open seat, or the queue) and watch; then the table's options, so looking
// at a table never means joining it.
// A sit option is offered only where the authority would grant that seat now:
// open, between games, with nobody queued first. An option the room would
// refuse anyway (updating, or a receipt still outstanding) is shown dimmed with
// its reason, as the table options list shows it.
std::vector<MenuChoice> SeatChoices(const ShellView& v, const room::Table& t);
// What B does on the board for a member with a place at a table: leave it when
// the table allows, take Ready back while a start is held for a locked-in
// spectator, otherwise say why not. table < 0: no place, so B is the ordinary
// Back.
struct PlaceExit {
    int table = -1;
    bool seat = false;
    bool allowed = false;
    // During a held start, B on the seat takes Ready back instead of leaving.
    bool unready = false;
    int seatIndex = -1;
    const char* label = "";
    std::string blocker;
    // What leaving a seat gives up; a queue place gives up nothing.
    room::LeavingCost cost;
};
PlaceExit ExitFromPlace(const ShellView& v);
// The board's focus is on the card of the table where the member has a place.
bool OnOwnPlace(const PlaceExit& place, const std::string& focus);
bool LeaveBlocked(const ShellView& v);
// The sentences a confirmed departure states, one per thing it gives up.
std::string LeavingCostText(const room::LeavingCost& cost, const room::Table& t);
// The Ready control, described once for the board (card strip and legend),
// the table options row and dispatch, so none of them offers a press another
// refuses. kind None: nothing to send, and refusal says why. detail is the
// row's explanation, which a brief room update must not flicker.
struct ReadyControl {
    enum Kind { None, Ready, Rematch, Unready } kind = None;
    const char* label = "";
    std::string detail, refusal;
};
ReadyControl DescribeReady(const ShellView& v, const room::Table& t, int seat);
// Why a fighter cannot change fighter or appearance now, or empty when they can.
// The board's X, the table options row and the fighter screen all read it.
std::string SelectionBlocker(const ShellView& v);
} } }
