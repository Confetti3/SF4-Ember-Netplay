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
bool TerminalFenced(const ShellView& v, room::ActionKind kind, std::size_t table);
const char* TerminalPendingReason();
// The room's Wired players only rule keeps this PC on Wi-Fi from sitting
// down: a seat, the queue or a cabinet (room::LinkMaySit). One already
// seated is not fenced, so moving between Arcade cabinets still works.
bool WiredFenced(const ShellView& v, room::ActionKind kind);
const char* WiredOnlyReason();
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
    // The board row that is the place: its table, or an Arcade room's cabinet.
    std::string row;
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
// The game at the member's own table that its fighter may end with no result
// (AbortMatch): an unresolved one (Abandon unresolved game), or a Practice
// match this PC fights in (End practice), which otherwise ends only when its
// round time runs out. A live game of any other kind is never cut short by
// its own fighter. The rows that offer it and dispatch read this one answer.
enum class AbandonableGame { None, Unresolved, Practice };
AbandonableGame AbandonableAt(const ShellView& v, const room::Table& t);
// Whether an Unwatch at `table` leaves the game this PC watches while the room
// still plays it (netplay::LeaveOfWatchedGame). Every such press, from Stop
// watching or the table's own Unwatch, goes as that game's leave
// (commands::LeaveWatchedGame), which the runtime scopes to the watched
// game when it runs, never as an Unwatch of whatever the table plays by then.
bool LeavesWatchedGame(const ShellView& v, std::size_t table);
// Whether the room takes a press of `kind` at `table` now. Everything waits
// for a writable authority but the watched game's leave: the runtime sends
// its Unwatch straight to the live link, and the authority scopes it by the
// game's generation rather than the table's revision, so a room still
// catching up on its checkpoint takes it too. SendRoom, the board's Stop
// watching row and the seat chooser read it; the table page keeps its own
// wait wording for the whole page.
bool RoomTakesNow(const ShellView& v, room::ActionKind kind, std::size_t table);
// What a press of `kind` means for the game this PC watches, as a sentence
// added to its row: Queue and Watch take the game back to the room first,
// which ends the view, and leaving the queue keeps the view until the game
// ends. Empty away from a watched game.
std::string WatchingNote(const ShellView& v, room::ActionKind kind);
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
// The first line of Ready's detail: the pick it locks in, the Ultra by its own
// name, and the opponent's fighter once they picked one, so the Ultra is
// chosen with the matchup in view: "Ready with Ryu, Metsu Hadoken, against Ken."
std::string ReadyWithText(const ShellView& v, const room::Table& t, int seat);
// Why a fighter cannot change fighter or appearance now, or empty when they can.
// The board's X, the table options row and the fighter screen all read it.
std::string SelectionBlocker(const ShellView& v);
// "Opponent changed to <fighter>. Check your Ultra before Ready."
std::string OpponentChangedText(int fighter);
// Whether this client is the locked-in spectator a held start at t waits for:
// it watches t locked in and has not finished retiring the last game.
bool HoldingStart(const ShellView& v, const room::Table& t);
// What a held start at t waits for and how long is left: "Waiting for Alex to
// return. Starts in 7 s.", or for the spectator it waits for, "Game waits for
// you: 7 s". A host that sends no time left gets the fixed wording. Empty when
// t is not held.
std::string HoldText(const ShellView& v, const room::Table& t);
// The same in one short line for the status line, without names, which can be
// any length: "Waiting for spectators: 7 s", or the spectator's own line.
std::string HoldStatus(const ShellView& v, const room::Table& t);
// The deadline a seated fighter who has not readied is under: the room's
// Ready timeout in its last 30 seconds and, for the local player, the window
// of a Training call. Either one takes the seat, so with both running the
// one that ends first counts. seconds is 0 when there is none; text is the
// line to show for it.
struct ReadyDeadline {
    std::string text;
    int seat = -1;
    unsigned seconds = 0;
    bool training = false;
};
ReadyDeadline DescribeReadyDeadline(const ShellView& v, const room::Table& t);
// A highlighted line across a table card, for something the player at it
// should act on or wait for: a held start, a seated fighter being called back
// from Training, or the opponent's new fighter until the player readies or
// the matchup changes. seat is the side it is about, or -1. Empty text: none.
struct TableBanner {
    std::string text;
    int seat = -1;
};
TableBanner DescribeTableBanner(const ShellView& v, const room::Table& t);
// The table the options open without a card to say otherwise: your own
// place's, else `selected`, the last table card the player was on.
int OptionsTable(const ShellView& v, int selected);
// A draft with something in it besides spaces.
bool HasChatText(const char* text);
// A table's state in a word or two: open, waiting, in game and so on.
const char* PhaseName(const room::Table& table);
// What an Arcade room's cabinet row says of its player: waiting, or the
// opponent with the score or the Rematch choice, and the streak.
std::string CabinetState(const room::Snapshot& s, std::size_t cabinet);
// An Arcade pair choosing after a set (Table::deciding): `seat`'s answer so
// far (Rematch?, Rematch chosen, New opponent chosen), as every view says it.
const char* ArcadeAnswerKey(const room::Table& table, int seat);
// Both answers for `seat` at the deciding Arcade pair at `table`, as the
// cabinet, the room's bar and the post-match prompt offer them: each open until
// it is given and while the room takes actions now; its detail says what it
// does (New opponent: room::ArcadeNewOpponentFor, to part now, swap or wait),
// or why the room cannot take it yet. The bar's Ready is the same Rematch
// (DescribeReady).
struct ArcadeChoice {
    bool rematchOpen = false, newOpponentOpen = false;
    std::string rematchDetail, newOpponentDetail;
};
ArcadeChoice DescribeArcadeChoice(const ShellView& v, std::size_t table, int seat);
// "3 sets in a row" for the member who has won two or more sets in a row at
// this table (Table::streakHolder), else empty.
std::string StreakText(const room::Table& t, room::MemberId member);
// "Idle 4 min" for a member who has done nothing in the room for a minute
// or more while waiting (not in a game, not ready, not watching one), else
// empty. A waiting member who said they are in Training is shown as that
// instead: it is why they are quiet. The room stamped the idle time
// `elapsedMs` ago (ShellView::roomElapsedMs).
std::string IdleText(const room::Member& member, std::uint64_t elapsedMs);
} } }
