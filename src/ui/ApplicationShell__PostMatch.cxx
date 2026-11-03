#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "RoomControls.hxx"
#include "LatencyMark.hxx"
#include "Theme.hxx"
#include "MenuGlyphs.hxx"
#include "V2.hxx"
#include "MenuProbes.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cmath>

// The post-match prompt: over the WIN screen of a room game this PC fought,
// Rematch (Next game inside an unfinished set), Character select or Back to
// room, for PostMatchSeconds. A Rematch is the room's own Ready, so when both
// fighters choose it the next game starts with no stop on the room board.
// An Arcade pair that decides after a set (Table::deciding) answers the room's
// own question instead: Rematch or New opponent.
// Back to room never gives up the seat, so a Back mashed through the WIN costs
// nothing; leaving it is the board's Leave seat. No answer gives it up only
// where a set hands seats over by the queue (KeepsSeat).
namespace sf4e { namespace ui {
namespace {
// The prompt's time. The game's WIN waits as long for a fighter still asked,
// and a second more (rematch::AskingMs), so an answer in time always counts.
constexpr double PostMatchSeconds = 10;
// Rematch chosen: the prompt waits this long past its timer for the opponent's
// answer, and, both having chosen, for the next game to load.
constexpr double StartGraceSeconds = 15;
using room_controls::ExitFromPlace;

int SeatOf(const room::Table& t, room::MemberId member) {
    return member && t.p1 == member ? 0 : member && t.p2 == member ? 1 : -1;
}
// The prompt's game left a set going on: one the room has recorded as ended,
// or that its result ends, is over (room::StandingAfter).
bool MidSet(const room::Snapshot& room, const room::Table& t, std::uint64_t generation, room::MatchResult result) {
    return t.rules.format != room::SetFormat::Unlimited && !room::StandingAfter(room, t, generation, result).over;
}
// The room has taken this game's result: the game is no longer live at its
// generation (the table left Playing and Paused, or moved on to another game).
bool ResultIn(const room::Table& t, std::uint64_t generation) {
    return !(t.matchGeneration == generation && (t.phase == room::TablePhase::Playing || t.phase == room::TablePhase::Paused));
}
// Table::keeper describes this game once the room has its result, or before
// it when the game cannot have ended the set (neither seat was a win from the
// goal); until then a keeper may be the last set's, not this one's. A rematch
// played on in the same battle stays live, so for its few seconds of Starting
// the score already counts this game (a short, harmless lag).
bool KeeperKnown(const room::Table& t, std::uint64_t generation) {
    if (ResultIn(t, generation)) return true;
    return t.rules.format != room::SetFormat::Unlimited &&
        (std::max)(t.score[0], t.score[1]) + 1 < static_cast<std::uint32_t>(t.rules.format);
}
// No answer gives the seat to whoever waits only in a Classic room after a
// set. An Arcade or King of the hill room keeps everyone at their cabinet and
// rotates them itself (ArcadeSetEnded); a set's game keeps both seats.
bool KeepsSeat(const room::Snapshot& room, const room::Table& t, std::uint64_t generation, room::MatchResult result) {
    return room.arcade != room::ClassicRoom || MidSet(room, t, generation, result);
}
// The glyph of the press each button answers to, drawn on it; none where the
// device has no such button (MenuShortcutGlyph).
const char* ButtonGlyph(const std::string& id) {
    if (id == "fighter") return MenuShortcutGlyph(MenuInput::Fighter);
    if (id == "new-opponent") return MenuShortcutGlyph(MenuInput::Options);
    return id == "back" ? MenuBackGlyph() : MenuSelectGlyph();
}
// A seat's Ready for the next game. The room clears the Readies that started
// this one only when its match ends, after the WIN of a game that may go on in place.
bool Readied(const room::Table& t, int seat, std::uint64_t generation) {
    return seat >= 0 && ResultIn(t, generation) && t.ready[seat];
}
}

void ApplicationShell::UpdatePostMatch(const ShellView& v, const room::Snapshot& snapshot, double now, const Submit& submit, bool* open) {
    using namespace netplay; auto& nav = menu_.navigation;
    // The battle went on to the rematch both chose: that WIN's prompt is done,
    // and the next game's WIN opens its own. The count starts again from none
    // when the battle closes, which ends no prompt: one both chose stays over
    // the main menu until the next game loads.
    if (v.gamesInPlace != gamesInPlaceSeen_) {
        if (v.gamesInPlace > gamesInPlaceSeen_ && postMatch_) ClosePostMatch(v, AfterPostMatch::None, open);
        gamesInPlaceSeen_ = v.gamesInPlace;
    }
    // Each WIN opens one prompt. Its end counts whether Ember shows or not,
    // or a prompt closed over one WIN would keep the next one's shut.
    if (!v.matchResult) matchResultSeen_ = false;
    const auto& fought = v.foughtGame;
    if (open) {
        // Only over the game's WIN: the game over, the room is shown, not a new
        // prompt. It is the game's own table, which an Arcade set's loser left.
        if (v.matchResult && !matchResultSeen_ && Fighting(v.session.match) && fought.Active() && fought.table < static_cast<int>(snapshot.tables.size())) {
            const auto& t = snapshot.tables[fought.table];
            PostMatchPrompt prompt;
            prompt.table = fought.table; prompt.generation = fought.generation; prompt.result = fought.result;
            prompt.until = now + PostMatchSeconds;
            prompt.failures = v.readyFailureSequence;
            // A set the result has already ended rotated its seats: the opponent is the set's.
            const bool ended = t.lastSet.generation == fought.generation;
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
        // The room gone (closed, a kick, an invite switch): nothing to answer, no seat to leave.
        if (!here) { ClosePostMatch(v, AfterPostMatch::None, open); return; }
        const auto& t = snapshot.tables[p.table];
        const int mine = SeatOf(t, snapshot.localMember), theirs = SeatOf(t, p.opponent);
        if (p.result == room::MatchResult::Abort && fought.generation == p.generation) p.result = fought.result;
        // The pair's question answered Rematch by both: the room asks no more,
        // and the player's Rematch is their Ready for the next set.
        if (p.pairRematch && !t.deciding && mine >= 0 && theirs >= 0) {
            p.pairRematch = false;
            if (!Readied(t, mine, p.generation)) p.answered |= p.chose = Send(netplay::CommandKind::Rematch, v, submit);
        }
        // The press counts until the room shows it: the runtime holds it while
        // the WIN plays out and resends it after. A Ready the room takes back
        // later can be pressed again.
        if (p.chose && Readied(t, mine, p.generation)) p.chose = false;
        // A Rematch the runtime refused (it says why) is offered again while
        // the next game has not begun, with the start it promised taken back.
        if (v.readyFailureSequence != p.failures) {
            p.failures = v.readyFailureSequence;
            if (t.matchGeneration == p.generation) p.chose = p.starting = false;
        }
        if (p.left) {
            if (!Fighting(v.session.match) || v.atMainMenu) ClosePostMatch(v, p.after, open);
            return;
        }
        const bool ready = mine >= 0 && (p.chose || p.pairRematch || Readied(t, mine, p.generation) || v.readyPress != netplay::ReadyPress::None);
        // Both chose Rematch: the room starts the next game at once, and the
        // prompt covers the room and the game's main menu until the game
        // loads. The snapshot can bring the other Ready and the new game
        // together. Over the WIN the other's choice arrives with their inputs,
        // and the battle restarts in place.
        if (!p.starting && ready && theirs >= 0 &&
            (Readied(t, theirs, p.generation) || t.matchGeneration != p.generation || (v.rematchChoices >> theirs & 1))) {
            p.starting = true; p.until = now + StartGraceSeconds;
        }
        if (p.starting ? (v.session.match == MatchState::Playing && t.matchGeneration != p.generation) || now >= p.until :
            t.matchGeneration != p.generation) ClosePostMatch(v, AfterPostMatch::None, open);
        // Having chosen, wait for the opponent's answer: their own timer, and the
        // WIN they still play out, end later than this one.
        else if (!p.starting && ready && theirs >= 0) { if (now >= p.until + StartGraceSeconds) ClosePostMatch(v, AfterPostMatch::None, open); }
        // A WIN cut short ends the game before the timer: no answer by then is no answer.
        else if (!p.starting && (now >= p.until || ready || (!Fighting(v.session.match) && v.atMainMenu)))
            ClosePostMatch(v, ready || p.answered || KeepsSeat(snapshot, t, p.generation, p.result) ? AfterPostMatch::None : AfterPostMatch::LeaveSeat, open);
    }
    // What the prompt left to do waits for the game's end, in the room.
    if (!open || postMatch_ || afterPostMatch_ == AfterPostMatch::None || Fighting(v.session.match) || !v.atMainMenu) return;
    if (afterPostMatch_ == AfterPostMatch::FighterSelect) {
        nav.Home(); nav.Push("room"); nav.Push("selection"); selectionFresh_ = true; selectionOpenOn_ = RoomSelectionStart(v.room);
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
    const auto standing = room::StandingAfter(v.room, t, p.generation, p.result);
    const bool midSet = t.rules.format != room::SetFormat::Unlimited && !standing.over;
    // An Arcade or King of the hill room after a set: the pair decides when
    // nobody waited alone but others sit at the cabinets (Arcade only). Once
    // one of them has left the table for their cabinet the room has paired
    // someone new, so there is nothing to choose (an opponent gone from the
    // cabinets has left, as in a Classic room). A pair still seated together (nobody else to play,
    // or the result still on its way, as in an Unlimited game that may go on
    // in place) has the ordinary Rematch, as a Classic pair does.
    const bool cabinets = v.room.arcade != room::ClassicRoom && !midSet;
    const bool deciding = cabinets && t.deciding && mine >= 0 && theirs >= 0;
    const bool opponentGone = room::CabinetOf(v.room, p.opponent) < 0;
    const bool rotates = cabinets && !deciding && (mine < 0 || (theirs < 0 && !opponentGone));
    const bool chosen = p.starting || p.left || (mine >= 0 && (p.chose || Readied(t, mine, p.generation) || v.readyPress != netplay::ReadyPress::None ||
        (deciding && t.rematch[mine])));
    const bool theyChose = p.starting || Readied(t, theirs, p.generation) || (deciding && t.rematch[theirs]);
    const auto* opponent = room::FindMember(v.room, p.opponent);
    const auto* local = room::FindMember(v.room, v.room.localMember);
    const std::string name = opponent ? opponent->name : loc::T("room.player");
    // Rematch needs both fighters still seated, and after a set nobody waiting for the seat.
    const char* refusal = p.starting ? nullptr : mine < 0 ? "postmatch.not_seated" : theirs < 0 ? nullptr : !midSet && !t.queue.empty() ? "postmatch.queue_waiting" : nullptr;
    const bool again = p.starting || (mine >= 0 && theirs >= 0 && !refusal);
    const char* leftAs = p.after == AfterPostMatch::FighterSelect ? "postmatch.character_select" : "postmatch.back";
    std::string title = p.left ? std::string(loc::T(leftAs)) : chosen && theyChose ? loc::T("postmatch.starting") : theirs < 0 ? loc::Tf("postmatch.opponent_left", name) :
        chosen ? loc::Tf("postmatch.waiting_for", name) : theyChose ? loc::Tf(midSet ? "postmatch.wants_next" : "postmatch.wants_rematch", name) :
        std::string(loc::T("postmatch.choose"));
    if (rotates && !p.left) {
        const auto place = room::PlaceOf(v.room, v.room.localMember);
        const auto* next = place.table >= 0 ? room::SeatOpponent(v.room, static_cast<std::uint8_t>(place.table), v.room.localMember) : nullptr;
        if (next) NoteUserText(next->name);
        title = next ? loc::Tf("postmatch.king.next", next->name) : std::string(loc::T("postmatch.king.waiting"));
    }
    const int seconds = static_cast<int>(std::ceil((std::max)(0.0, p.until - now)));
    const auto state = [&](bool ready, bool seated) {
        return std::string(loc::T(!seated ? "postmatch.state.left" : !ready ? "postmatch.state.choosing" : deciding ? "room.rematch_chosen" : "postmatch.state.ready"));
    };
    // This PC's row: its choice, Back to room or Character select once made.
    std::string mineWord = p.left ? std::string(loc::T(leftAs)) : state(chosen, mine >= 0);
    std::string theirWord = state(theyChose, theirs >= 0);
    // The set's winner stays on and its loser waits, once the room has the
    // result; an opponent gone from the cabinets has left all the same.
    if (rotates && t.lastSet.generation == p.generation) {
        const auto word = [&](room::MemberId m) { return std::string(loc::T(m == t.lastSet.Winner() ? "postmatch.king.stays" : "postmatch.king.waits")); };
        if (!p.left) mineWord = word(v.room.localMember);
        if (!opponentGone) theirWord = word(p.opponent);
    }
    // The buttons are the prompt's only rows: the answers there are now. A
    // deciding pair's Rematch and New opponent are the room's own choice (the
    // cabinet's). Character select and Back to room are always rows, so a
    // device without the X or Y shortcut reaches every answer with the D-pad.
    std::vector<MenuEntry> rows;
    if (deciding) {
        rows.push_back(Row("rematch", loc::T("room.rematch"), loc::T("room.rematch_detail"), !chosen));
        rows.push_back(Row("new-opponent", loc::T("room.new_opponent"), loc::T("room.new_opponent_detail"), !p.left));
    } else if (!rotates)
        rows.push_back(Row("again", loc::T(midSet ? "postmatch.next_game" : "postmatch.rematch"),
            again ? std::string(loc::T("postmatch.again.detail")) : refusal ? std::string(loc::T(refusal)) : loc::Tf("postmatch.opponent_left", name),
            again && !chosen));
    // After a rotation nothing is left to choose, so each button first says
    // what the room did: the winner plays whoever sits down next, the loser
    // the next player free.
    std::string rotation;
    if (rotates && !p.left && t.lastSet.generation == p.generation)
        rotation = std::string(t.lastSet.Winner() == v.room.localMember ? loc::T("postmatch.rotate.winner") : loc::Tf("postmatch.rotate.loser", name)) + "\n";
    // A winner who keeps the fighter (Rules::winnerKeepsFighter) is told so here,
    // not only once the fighter select refuses another fighter.
    const bool keeps = KeeperKnown(t, p.generation) && room::KeepsFighter(v.room, v.room.localMember);
    rows.push_back(Row("fighter", loc::T("postmatch.character_select"),
        rotation + loc::T(keeps ? "selection.winner_keeps_fighter" : "postmatch.character_select.detail"), !p.left));
    rows.push_back(Row("back", loc::T("postmatch.back"), rotation + loc::T(KeepsSeat(v.room, t, p.generation, p.result) ? "postmatch.back.keep_seat" : "room.leave_seat.keep"), !p.left));
    std::vector<std::pair<unsigned, std::string>> shortcuts{{MenuInput::Fighter, loc::T("postmatch.character_select")}};
    if (deciding) shortcuts.push_back({MenuInput::Options, loc::T("room.new_opponent")});
    menu_.shortcutHints = ShortcutHints(shortcuts);
    menu_.backHint = loc::T("postmatch.back"); menu_.exitName.clear(); menu_.ownsBack = true;
    // The two fighters with their answers beside the countdown ring, the
    // focused button's words, and the buttons in a row, over the dimmed
    // WIN screen. B is the Back to room button, so the header has no Back.
    // The set's score counts the game just played, whether the room has it yet or not.
    GameMenu::Body board;
    menu_.backInert = true; menu_.headerChips.clear();
    menu_.eyebrow = t.rules.format == room::SetFormat::Unlimited ? std::string() : SetLengthText(t.rules.format) + " \xC2\xB7 " + SetScoreText(standing.score);
    const auto* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(20, 19, 18, 140));
    board = [&](const std::vector<MenuEntry>& entries, MenuNavigation& nav, MenuAction& action, float, const MenuVisualFeedback& feedback) {
        const float s = Scale(), rowHeight = 52 * s;
        const auto& c = v2::Colors(); auto* d = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos(); const float width = ImGui::GetContentRegionAvail().x;
        // The countdown beside the two rows: a ring that empties over the ten
        // seconds, red for the last three.
        const float ring = 64 * s;
        const ImVec2 centre(at.x + width - ring * .5f, at.y + rowHeight + 4 * s);
        d->AddCircle(centre, 26 * s, c.button, 48, 5 * s);
        if (!p.starting && !p.left && seconds > 0) {
            const float left = static_cast<float>((std::min)(1.0, (std::max)(0.0, (p.until - now) / PostMatchSeconds)));
            d->PathArcTo(centre, 26 * s, -1.5707963f, -1.5707963f + 6.2831853f * left, 48);
            d->PathStroke(seconds <= 3 ? c.danger : c.accent, 0, 5 * s);
            const std::string number = std::to_string(seconds);
            const ImVec2 size = HeadingFont()->CalcTextSizeA(24 * s, FLT_MAX, 0, number.c_str());
            d->AddText(HeadingFont(), 24 * s, ImVec2(centre.x - size.x * .5f, centre.y - size.y * .5f), c.text, number.c_str());
        }
        // The two fighters' answers are pictures of the state, not rows: the title says them too.
        const struct { const char* id; const room::Member* m; bool ready, seated; std::string word; } players[] = {
            {"pm-you", local, chosen, mine >= 0, mineWord}, {"pm-opponent", opponent, theyChose, theirs >= 0, theirWord}};
        // A tag's line, as v2::Tag sets it, to centre the tag in its row.
        const float line = (std::max)(ImGui::GetFontSize(), v2::ChipFont() + 8 * s);
        float y = at.y;
        for (const auto& player : players) {
            const v2::Box r{ImVec2(at.x, y), ImVec2(at.x + width - ring - 16 * s, y + rowHeight)};
            d->AddRectFilled(r.min, r.max, c.ink, 4 * s); d->AddRect(r.min, r.max, c.border, 4 * s, 0, s);
            const std::string& word = player.word;
            const ImU32 tone = !player.seated ? c.danger : &player == players && p.left ? c.muted :
                player.ready ? c.ready : ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending));
            const float chip = v2::ChipWidth(word);
            if (player.m) {
                v2::TagView tag; tag.name = player.m->name; tag.marks = {player.m->country, player.m->badge};
                tag.ping = LatencyMarkFor(v.memberLatency, *player.m, v.room.localMember);
                v2::Tag(d, ImVec2(r.min.x + 12 * s, r.min.y + (rowHeight - line) * .5f), r.W() - chip - 36 * s, tag, v2::TagSize::S);
            }
            v2::Chip(d, ImVec2(r.max.x - 12 * s - chip, r.min.y + (rowHeight - line) * .5f), word, tone);
            ReportMenuCardText(player.id, word);
            y += rowHeight + 8 * s;
        }
        // What the focused button does, in the room the longest one takes, so
        // the buttons stay put while the focus moves.
        float words = 0;
        for (const auto& e : entries) words = (std::max)(words, v2::Paragraph(nullptr, ImVec2(), width, e.detail, 17 * s, c.muted));
        const auto focused = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == nav.Focus(); });
        if (focused != entries.end()) v2::Paragraph(d, ImVec2(at.x, y + 8 * s), width, focused->detail, 17 * s, c.muted);
        y += words + 24 * s;
        // The buttons in a bar, each with the glyph of its own press, set apart from
        // the players' rows: on one line where every label fits beside its glyph, else
        // on as few as let them (v2::BarLines), as the room's action bar does.
        const float glyph = 26 * s, font = 17 * s, gap = 12 * s;
        std::vector<float> needs;
        for (const auto& e : entries)
            needs.push_back((ButtonGlyph(e.id) ? glyph + 10 * s : 0) + ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0, e.label.c_str()).x + 24 * s);
        const int lines = v2::BarLines(width, needs, gap);
        const float barHeight = lines * (rowHeight + gap) - gap;
        const auto places = v2::BarPlaces({ImVec2(at.x, y), ImVec2(at.x + width, y + barHeight)}, needs, gap, lines, rowHeight);
        // A wrapped bar can reach below the panel, so the focused button scrolls
        // into view when the focus moves, the button moves in the panel's content
        // (the bar or the words over it took another line) or the panel changes
        // size; a scroll of the player's own stays put between.
        PostMatchBarView shown;
        shown.focus = nav.Focus();
        const ImVec2 panel = ImGui::GetWindowSize();
        shown.panelWidth = std::round(panel.x); shown.panelHeight = std::round(panel.y);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id != shown.focus) continue;
            // In whole pixels, so the player's own scroll never reads as a move.
            const float top = ImGui::GetWindowPos().y - ImGui::GetScrollY();
            shown.top = std::round(places[i].min.y - top); shown.bottom = std::round(places[i].max.y - top);
        }
        const bool reveal = !(shown == postMatchBarShown_);
        postMatchBarShown_ = shown;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            const v2::Box& b = places[i];
            ReportMenuCard(e.id.c_str(), b.min, b.max);
            const bool hovered = PressEntry(nav, entries, e, b, action, reveal).hovered, enabled = feedback.Enabled(e);
            v2::Button(d, b, "", v2::ButtonKind::Secondary, e.id == nav.Focus(), hovered, enabled);
            const char* press = ButtonGlyph(e.id);
            const float mark = press ? glyph + 10 * s : 0, room = b.W() - mark - 24 * s;
            const float text = (std::min)(room, ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0, e.label.c_str()).x);
            const float x = b.min.x + (b.W() - mark - text) * .5f;
            if (press) DrawPromptGlyph(d, press, ImVec2(x, b.min.y + (rowHeight - glyph) * .5f), glyph, MenuArt(), s);
            v2::Text(d, ImVec2(x + mark, b.min.y + (rowHeight - font) * .5f), room, e.label, font, enabled ? c.text : c.muted);
        }
        // The focus frame reaches 4 px past a button: the scroll keeps room for it under the last line.
        ImGui::SetCursorScreenPos(ImVec2(at.x, y + barHeight)); ImGui::Dummy(ImVec2(width, 6 * s));
    };
    const auto a = menu_.Draw(title.c_str(), rows, "", {}, static_cast<int>(rows.size()), {}, board, false, 70, Tone::Pending);
    menu_.ownsBack = false; menu_.eyebrow.clear();
    AnswerDialog(submit);
    // Over the WIN the choice stays shown until the game is over (UpdatePostMatch).
    const auto leave = [&](AfterPostMatch after) {
        if (p.left) return;
        if (!netplay::Fighting(v.session.match)) { ClosePostMatch(v, after, open); return; }
        postMatch_->left = true; postMatch_->after = after;
    };
    if (a.kind == MenuAction::Back) leave(AfterPostMatch::None);
    else if (a.kind == MenuAction::Shortcut && a.delta == MenuInput::Fighter) leave(AfterPostMatch::FighterSelect);
    // The room's Rematch or New opponent: the same answer as the cabinet's (RoomAction).
    else if (a.kind == MenuAction::Shortcut && a.delta == MenuInput::Options && deciding) {
        postMatch_->pairRematch = false; RoomAction({MenuAction::Activate, "new-opponent"}, v, submit);
    }
    else if (a.kind == MenuAction::Activate) {
        if (a.id == "again" && again && !chosen) { selectedTable_ = p.table; postMatch_->answered |= postMatch_->chose = Send(netplay::CommandKind::Rematch, v, submit); }
        else if (a.id == "rematch" || a.id == "new-opponent") { postMatch_->pairRematch = a.id == "rematch"; RoomAction(a, v, submit); }
        else if (a.id == "fighter") leave(AfterPostMatch::FighterSelect);
        else if (a.id == "back") leave(AfterPostMatch::None);
    }
}
} }
