#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "RoomControls.hxx"
#include "LatencyMark.hxx"
#include "Theme.hxx"
#include "MenuGlyphs.hxx"
#include "V2.hxx"
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
// King of the hill keeps everyone at their cabinet after a set: the winner
// stays on, the loser waits for the next opponent (ArcadeSetEnded).
bool KeepsSeat(const room::Snapshot& room, const room::Table& t, std::uint64_t generation) {
    return room.arcade == room::KingRoom || MidSet(t, generation);
}
// A seat's Ready for the next game. The room clears the Readies that started
// this one only when its match ends, after the WIN of a game that may go on in place.
bool Readied(const room::Table& t, int seat, std::uint64_t generation) {
    const bool live = t.matchGeneration == generation && (t.phase == room::TablePhase::Playing || t.phase == room::TablePhase::Paused);
    return seat >= 0 && !live && t.ready[seat];
}
}

void ApplicationShell::UpdatePostMatch(const ShellView& v, const room::Snapshot& snapshot, double now, const Submit& submit, bool* open) {
    using namespace netplay; auto& nav = menu_.navigation;
    // The battle went on to the rematch both chose: that WIN's prompt is done,
    // and the next game's WIN opens its own.
    if (v.gamesInPlace != gamesInPlaceSeen_) {
        gamesInPlaceSeen_ = v.gamesInPlace;
        if (postMatch_) ClosePostMatch(v, AfterPostMatch::None, open);
        matchResultSeen_ = false;
    }
    if (open) {
        const auto place = room::PlaceOf(snapshot, snapshot.localMember);
        // Only over the game's WIN: the game over, the room is shown, not a new prompt.
        if (v.matchResult && !matchResultSeen_ && Fighting(v.session.match) && place.table >= 0 && place.table < static_cast<int>(snapshot.tables.size())) {
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
        // The room gone (closed, a kick, an invite switch): nothing to answer, no seat to leave.
        if (!here) { ClosePostMatch(v, AfterPostMatch::None, open); return; }
        const auto& t = snapshot.tables[p.table];
        const int mine = SeatOf(t, snapshot.localMember), theirs = SeatOf(t, p.opponent);
        // The press counts until the room shows it: the runtime holds it while
        // the WIN plays out and resends it after. A Ready the room takes back
        // later can be pressed again.
        if (p.chose && Readied(t, mine, p.generation)) p.chose = false;
        if (p.left) {
            if (!Fighting(v.session.match) || v.atMainMenu) ClosePostMatch(v, p.after, open);
            return;
        }
        const bool ready = mine >= 0 && (p.chose || Readied(t, mine, p.generation) || v.readyPress != netplay::ReadyPress::None);
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
            ClosePostMatch(v, ready || KeepsSeat(snapshot, t, p.generation) ? AfterPostMatch::None : AfterPostMatch::LeaveSeat, open);
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
    // King of the hill after a set: nothing to choose, the room pairs the next game.
    const bool rotates = v.room.arcade == room::KingRoom && !midSet;
    const bool chosen = p.starting || p.left || (mine >= 0 && (p.chose || Readied(t, mine, p.generation) || v.readyPress != netplay::ReadyPress::None));
    const bool theyChose = p.starting || Readied(t, theirs, p.generation);
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
    const auto state = [](bool ready, bool seated) {
        return std::string(loc::T(!seated ? "postmatch.state.left" : ready ? "postmatch.state.ready" : "postmatch.state.choosing"));
    };
    // This PC's row: its choice, Back to room or Character select once made.
    std::string mineWord = p.left ? std::string(loc::T(leftAs)) : state(chosen, mine >= 0);
    std::string theirWord = state(theyChose, theirs >= 0);
    // The set's winner stays on and its loser waits, once the room has the result.
    if (rotates && t.lastSet.generation == p.generation) {
        const auto word = [&](room::MemberId m) { return std::string(loc::T(m == t.lastSet.Winner() ? "postmatch.king.stays" : "postmatch.king.waits")); };
        if (!p.left) mineWord = word(v.room.localMember);
        theirWord = word(p.opponent);
    }
    std::vector<MenuEntry> rows;
    if (!rotates) {
        rows.push_back(Row("again", loc::T(midSet ? "postmatch.next_game" : "postmatch.rematch"),
            again ? std::string(loc::T("postmatch.again.detail")) : refusal ? std::string(loc::T(refusal)) : loc::Tf("postmatch.opponent_left", name),
            again && !chosen));
        if (chosen && !p.left) rows.back().value = loc::T("postmatch.state.ready");
    }
    rows.push_back(Row("fighter", loc::T("postmatch.character_select"), loc::T("postmatch.character_select.detail"), !p.left));
    rows.push_back(Row("back", loc::T("postmatch.back"), loc::T(KeepsSeat(v.room, t, p.generation) ? "postmatch.back.keep_seat" : "postmatch.back.leave_seat"), !p.left));
    rows.push_back(InfoRow("pm-you", local ? local->name : loc::T("room.player"), mineWord, ""));
    rows.push_back(InfoRow("pm-opponent", name, theirWord, ""));
    const bool keys = KeyboardPrompts();
    menu_.shortcutHints = {{keys ? "F" : "X", loc::T("postmatch.character_select")}};
    menu_.backHint = loc::T("postmatch.back"); menu_.exitName.clear(); menu_.ownsBack = true;
    // The two fighters with their answers beside the countdown ring, the
    // focused button's words, and the three buttons in a row, over the dimmed
    // WIN screen. B is the Back to room button, so the header has no Back.
    GameMenu::Body board;
    menu_.backInert = true; menu_.headerChips.clear();
    menu_.eyebrow = t.rules.format == room::SetFormat::Unlimited ? std::string() : SetLengthText(t.rules.format) + " \xC2\xB7 " + SetScoreText(t.score);
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
        const struct { const room::Member* m; bool ready, seated; std::string word; } players[] = {
            {local, chosen, mine >= 0, mineWord}, {opponent, theyChose, theirs >= 0, theirWord}};
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
            y += rowHeight + 8 * s;
        }
        // What the focused button does, in the room the longest one takes, so
        // the buttons stay put while the focus moves.
        float words = 0;
        for (int i = 0; i < 3; ++i) words = (std::max)(words, v2::Paragraph(nullptr, ImVec2(), width, entries[i].detail, 17 * s, c.muted));
        const auto focused = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == nav.Focus(); });
        if (focused != entries.end()) v2::Paragraph(d, ImVec2(at.x, y + 8 * s), width, focused->detail, 17 * s, c.muted);
        y += words + 24 * s;
        // The buttons, each with its pad or key glyph, set apart from the players' rows.
        const char* glyphs[] = {MenuSelectGlyph(), keys ? "F" : "X", MenuBackGlyph()};
        const float w = (width - 24 * s) / 3, glyph = 26 * s, font = 17 * s;
        for (int i = 0; i < 3; ++i) {
            const auto& e = entries[i];
            const v2::Box b{ImVec2(at.x + i * (w + 12 * s), y), ImVec2(at.x + i * (w + 12 * s) + w, y + rowHeight)};
            const bool hovered = PressEntry(nav, entries, e, b, action).hovered, enabled = feedback.Enabled(e);
            v2::Button(d, b, "", v2::ButtonKind::Secondary, e.id == nav.Focus(), hovered, enabled);
            const float text = ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0, e.label.c_str()).x;
            const float x = b.min.x + (b.W() - glyph - 10 * s - text) * .5f;
            DrawPromptGlyph(d, glyphs[i], ImVec2(x, b.min.y + (rowHeight - glyph) * .5f), glyph, MenuArt(), s);
            d->AddText(ImGui::GetFont(), font, ImVec2(x + glyph + 10 * s, b.min.y + (rowHeight - font) * .5f), enabled ? c.text : c.muted, e.label.c_str());
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x, y + rowHeight)); ImGui::Dummy(ImVec2(width, 0));
    };
    const auto a = menu_.Draw(title.c_str(), rows, "", {}, 3, {}, board, false, 70, Tone::Pending);
    menu_.ownsBack = false; menu_.eyebrow.clear();
    AnswerDialog(submit);
    // Over the WIN the choice stays shown until the game is over (UpdatePostMatch).
    const auto leave = [&](AfterPostMatch after) {
        if (p.left) return;
        if (!netplay::Fighting(v.session.match)) { ClosePostMatch(v, after, open); return; }
        postMatch_->left = true; postMatch_->after = after;
    };
    const auto back = [&] { leave(KeepsSeat(v.room, t, p.generation) || chosen ? AfterPostMatch::None : AfterPostMatch::LeaveSeat); };
    if (a.kind == MenuAction::Back) back();
    else if (a.kind == MenuAction::Shortcut && a.delta == MenuInput::Fighter) leave(AfterPostMatch::FighterSelect);
    else if (a.kind == MenuAction::Activate) {
        if (a.id == "again" && again && !chosen) { postMatch_->chose = true; selectedTable_ = p.table; Send(netplay::CommandKind::Rematch, v, submit); }
        else if (a.id == "fighter") leave(AfterPostMatch::FighterSelect);
        else if (a.id == "back") back();
    }
}
} }
