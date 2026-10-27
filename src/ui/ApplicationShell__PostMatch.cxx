#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "RoomControls.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cmath>

// The post-match prompt: over the WIN screen of a room game this PC fought,
// Rematch (Next game inside an unfinished set), Character select or Back to
// room, for PostMatchSeconds. A Rematch is the room's own Ready, so when both
// fighters choose it the next game starts with no stop on the room board.
// Back to room and no answer leave the seat after a set or with no set
// length, so whoever waits gets it; inside a set the seat is kept, not ready.
namespace sf4e { namespace ui {
namespace {
constexpr double PostMatchSeconds = 10;
// Rematch chosen: the prompt waits this long past its timer for the opponent's
// answer, and, both having chosen, for the next game to load.
constexpr double StartGraceSeconds = 15;
using room_controls::ExitFromPlace;

int SeatOf(const room::Table& t, room::MemberId member) {
    return member && t.p1 == member ? 0 : member && t.p2 == member ? 1 : -1;
}
// The game ended a set only once the room has its result: until then the
// game is taken to be inside one.
bool MidSet(const room::Table& t, std::uint64_t generation) {
    return t.rules.format != room::SetFormat::Unlimited && t.lastSet.generation != generation;
}
}

void ApplicationShell::UpdatePostMatch(const ShellView& v, const room::Snapshot& snapshot, double now, const Submit& submit, bool* open) {
    using namespace netplay; auto& nav = menu_.navigation;
    if (open) {
        const auto place = room::PlaceOf(snapshot, snapshot.localMember);
        if (v.matchResult && !matchResultSeen_ && place.table >= 0 && place.table < static_cast<int>(snapshot.tables.size())) {
            const auto& t = snapshot.tables[place.table];
            PostMatchPrompt prompt;
            prompt.table = place.table; prompt.generation = t.matchGeneration; prompt.until = now + PostMatchSeconds;
            // A set the result has already ended rotated its seats: the opponent is the set's.
            const bool ended = t.lastSet.generation == t.matchGeneration;
            const auto p1 = ended ? t.lastSet.p1 : t.p1, p2 = ended ? t.lastSet.p2 : t.p2;
            prompt.opponent = p1 == snapshot.localMember ? p2 : p2 == snapshot.localMember ? p1 : 0;
            if (prompt.opponent) {
                postMatch_ = prompt; afterPostMatch_ = AfterPostMatch::None;
                if (nav.Screen() != "post-match") nav.Push("post-match");
                nav.Prefer("again");
            }
        }
        matchResultSeen_ = v.matchResult;
    }
    if (postMatch_) {
        auto& p = *postMatch_;
        const bool here = v.session.room == RoomState::Joined && p.table < static_cast<int>(snapshot.tables.size());
        const auto* t = here ? &snapshot.tables[p.table] : nullptr;
        const int mine = t ? SeatOf(*t, snapshot.localMember) : -1, theirs = t ? SeatOf(*t, p.opponent) : -1;
        // The press counts until the room shows it: the runtime holds it while
        // the WIN plays out and resends it after. A Ready the room takes back
        // later can be pressed again.
        if (p.chose && mine >= 0 && t->ready[mine]) p.chose = false;
        const bool ready = mine >= 0 && (p.chose || t->ready[mine] || v.readyRequested);
        // Both chose Rematch: the room starts the next game at once, and the
        // prompt covers the room until the game loads. The snapshot can bring
        // the other Ready and the new game together.
        if (t && !p.starting && ready && theirs >= 0 && (t->ready[theirs] || t->matchGeneration != p.generation)) {
            p.starting = true; p.until = now + StartGraceSeconds;
        }
        if (!t || (p.starting ? (v.session.match == MatchState::Preparing && t->matchGeneration != p.generation) || now >= p.until :
            t->matchGeneration != p.generation)) ClosePostMatch(v, AfterPostMatch::None, open);
        // Having chosen, wait for the opponent's answer: their own timer, and the
        // WIN they still play out, end later than this one.
        else if (!p.starting && ready && theirs >= 0) { if (now >= p.until + StartGraceSeconds) ClosePostMatch(v, AfterPostMatch::None, open); }
        // A WIN cut short ends the game before the timer: no answer by then is no answer.
        else if (!p.starting && (now >= p.until || ready || (!Fighting(v.session.match) && v.atMainMenu)))
            ClosePostMatch(v, ready || MidSet(*t, p.generation) ? AfterPostMatch::None : AfterPostMatch::LeaveSeat, open);
    }
    // What the prompt left to do waits for the game's end, in the room.
    if (!open || postMatch_ || afterPostMatch_ == AfterPostMatch::None || Fighting(v.session.match) || !v.atMainMenu) return;
    if (afterPostMatch_ == AfterPostMatch::FighterSelect) {
        nav.Home(); nav.Push("room"); nav.Push("selection"); selectionFresh_ = true;
        afterPostMatch_ = AfterPostMatch::None;
        return;
    }
    const auto exit = ExitFromPlace(v);
    // Not seated any more (the set's rotation took the seat): nothing to leave.
    if (exit.table < 0 || !exit.seat) { afterPostMatch_ = AfterPostMatch::None; return; }
    // The room is still taking the game's result.
    if (!exit.allowed || exit.unready || !room_controls::RoomTakesNow(v, room::ActionKind::Unqueue, exit.table)) return;
    LeavePlace(v, submit);
    afterPostMatch_ = AfterPostMatch::None;
}

void ApplicationShell::ClosePostMatch(const ShellView& v, AfterPostMatch after, bool* open) {
    postMatch_.reset(); afterPostMatch_ = after;
    if (menu_.navigation.Screen() == "post-match") menu_.navigation.Return();
    // Over the WIN screen Ember steps aside; it comes back with the room once the game is over.
    if (open && netplay::Fighting(v.session.match)) *open = false;
}

void ApplicationShell::DrawPostMatch(const ShellView& v, double now, const Submit& submit, bool* open) {
    if (!postMatch_) { menu_.navigation.Return(); return; }
    const auto p = *postMatch_;
    const auto& t = v.room.tables[p.table];
    const int mine = SeatOf(t, v.room.localMember), theirs = SeatOf(t, p.opponent);
    const bool midSet = MidSet(t, p.generation);
    const bool chosen = p.starting || (mine >= 0 && (p.chose || t.ready[mine] || v.readyRequested));
    const bool theyChose = p.starting || (theirs >= 0 && t.ready[theirs]);
    const auto* opponent = room::FindMember(v.room, p.opponent);
    const auto* local = room::FindMember(v.room, v.room.localMember);
    const std::string name = opponent ? opponent->name : loc::T("room.player");
    // Rematch needs both fighters still seated, and after a set nobody waiting for the seat.
    const char* refusal = p.starting ? nullptr : mine < 0 ? "postmatch.not_seated" : theirs < 0 ? nullptr : !midSet && !t.queue.empty() ? "postmatch.queue_waiting" : nullptr;
    const bool again = p.starting || (mine >= 0 && theirs >= 0 && !refusal);
    std::string title = chosen && theyChose ? loc::T("postmatch.starting") : theirs < 0 ? loc::Tf("postmatch.opponent_left", name) :
        chosen ? loc::Tf("postmatch.waiting_for", name) : theyChose ? loc::Tf(midSet ? "postmatch.wants_next" : "postmatch.wants_rematch", name) :
        std::string(loc::T("postmatch.choose"));
    const int seconds = static_cast<int>(std::ceil((std::max)(0.0, p.until - now)));
    const std::string status = chosen ? std::string() : loc::Tf("postmatch.seconds_left", seconds);
    const auto state = [](bool ready, bool seated) {
        return std::string(loc::T(!seated ? "postmatch.state.left" : ready ? "postmatch.state.ready" : "postmatch.state.choosing"));
    };
    std::vector<MenuEntry> rows;
    rows.push_back(Row("again", loc::T(midSet ? "postmatch.next_game" : "postmatch.rematch"),
        again ? std::string(loc::T("postmatch.again.detail")) : refusal ? std::string(loc::T(refusal)) : loc::Tf("postmatch.opponent_left", name),
        again && !chosen));
    if (chosen) rows.back().value = loc::T("postmatch.state.ready");
    rows.push_back(Row("fighter", loc::T("postmatch.character_select"), loc::T("postmatch.character_select.detail")));
    rows.push_back(Row("back", loc::T("postmatch.back"), loc::T(midSet ? "postmatch.back.keep_seat" : "postmatch.back.leave_seat")));
    rows.push_back(InfoRow("pm-you", local ? local->name : loc::T("room.player"), state(chosen, mine >= 0), ""));
    rows.push_back(InfoRow("pm-opponent", name, state(theyChose, theirs >= 0), ""));
    const bool keys = KeyboardPrompts();
    menu_.shortcutHints = {{keys ? "F" : "X", loc::T("postmatch.character_select")}};
    menu_.backHint = loc::T("postmatch.back"); menu_.exitName.clear(); menu_.ownsBack = true;
    const auto a = menu_.Draw(title.c_str(), rows, status.c_str(), {}, 1, {}, {}, 0, 70, true, Tone::Pending);
    menu_.ownsBack = false;
    AnswerDialog(submit);
    const auto back = [&] { ClosePostMatch(v, midSet || chosen ? AfterPostMatch::None : AfterPostMatch::LeaveSeat, open); };
    if (a.kind == MenuAction::Back) back();
    else if (a.kind == MenuAction::Shortcut && a.delta == MenuInput::Fighter) ClosePostMatch(v, AfterPostMatch::FighterSelect, open);
    else if (a.kind == MenuAction::Activate) {
        if (a.id == "again" && again && !chosen) { postMatch_->chose = true; selectedTable_ = p.table; Send(netplay::CommandKind::Rematch, v, submit); }
        else if (a.id == "fighter") ClosePostMatch(v, AfterPostMatch::FighterSelect, open);
        else if (a.id == "back") back();
    }
}
} }
