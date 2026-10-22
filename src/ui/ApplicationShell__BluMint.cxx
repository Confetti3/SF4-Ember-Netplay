// A BluMint match's room (docs/design/BLUMINT_ROOM.md), in the V2 look: BluMint's
// colours and wordmark, the round and the score in the header, a watcher's
// room with nothing to press but watching, and a notice when a game or the
// set ends. The room, its table and its actions are the ordinary tournament
// match room's; only how it looks and what it says change.
#include "ApplicationShell.hxx"
#include "BluMintMoments.hxx"
#include "V2.hxx"
#include "LatencyMark.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <optional>

namespace sf4e { namespace ui {
namespace {
// The assignment of the BluMint match this PC plays or last played, from its list. A
// watcher's list does not hold the match, so a watched match is not known
// as BluMint's until the room's binding says whose it is.
const netplay::tournament::Assignment* BluMintAssignment(const ShellView& v) {
    const auto& t = v.tournament;
    if (t.phase == netplay::tournament::Phase::Idle) return nullptr;
    for (const auto& a : t.list.items) if (a.matchId == t.matchId && a.provider == "blumint") return &a;
    return nullptr;
}
std::string MemberName(const room::Snapshot& s, room::MemberId id) {
    const auto* m = room::FindMember(s, id);
    return m ? m->name : std::string();
}
}

bool ApplicationShell::InBluMintRoom(const ShellView& v, const std::string& screen) {
    return screen.compare(0, 4, "room") == 0 && v.room.roomEpoch && BluMintAssignment(v);
}

void ApplicationShell::UpdateBluMint(const ShellView& v, const std::string& screen, std::vector<MenuEntry>& rows) {
    menu_.eyebrow.clear();
    menu_.headerChips.clear();
    const auto* a = BluMintAssignment(v);
    const bool roomScreen = screen.compare(0, 4, "room") == 0;
    v2::SetBrand(InBluMintRoom(v, screen) || screen == "blumint-matches" ? v2::Brand::BluMint : v2::Brand::Ember);
    // A called match (CallBluMint) is told after this frame's moment, never over it.
    // On BluMint's matches page every match is in sight: none is called over it.
    if (screen == "blumint-matches")
        for (const auto& m : v.tournament.list.items)
            if (m.provider == "blumint" && std::find(bluMintTold_.begin(), bluMintTold_.end(), m.matchId) == bluMintTold_.end()) bluMintTold_.push_back(m.matchId);
    if (!a) { bluMint_ = {}; CallBluMint(v); return; }

    // The fighters and the score at the match's table (table 0 of a bound room).
    const auto& s = v.room;
    const auto& table = s.tables[0];
    BluMintScene scene;
    scene.names[0] = MemberName(s, table.p1);
    scene.names[1] = MemberName(s, table.p2);
    scene.wins[0] = table.score[0];
    scene.wins[1] = table.score[1];
    scene.gamesToWin = s.tournament.gamesToWin ? s.tournament.gamesToWin : a->gamesToWin;
    scene.round = a->roundLabel;
    scene.you = s.localMember == table.p1 ? 0 : s.localMember == table.p2 ? 1 : -1;
    // A set that ended stands at its final score in the list, not at the table.
    if (a->Finished()) { scene.wins[0] = (std::max)(scene.wins[0], a->wins[0]); scene.wins[1] = (std::max)(scene.wins[1], a->wins[1]); }
    const netplay::tournament::Assignment* next = nullptr;
    for (const auto& m : v.tournament.list.items)
        if (m.matchId != a->matchId && m.provider == "blumint" && m.Playable(v.unixNow)) next = &m;
    if (next) scene.next = next->matchId;

    if (v2::On() && roomScreen) {
        std::string eyebrow = loc::T("blumint.eyebrow");
        if (!a->roundLabel.empty()) eyebrow += " \xC2\xB7 " + a->roundLabel;
        menu_.eyebrow = eyebrow;
        const auto& c = v2::Colors();
        if (table.phase == room::TablePhase::Playing) menu_.headerChips.push_back({loc::T("blumint.live"), c.live});
        if (scene.gamesToWin) menu_.headerChips.push_back({loc::Tf("blumint.first_to", scene.gamesToWin), c.muted});
        menu_.headerChips.push_back({std::to_string(scene.wins[0]) + " \xE2\x80\x93 " + std::to_string(scene.wins[1]), c.accent});
        // The board (DrawBluMintBoard): the match at table 0 as the stage, who watches,
        // chat and the actions. A watcher's actions are Chat and Leave: the room's
        // own controls are the fighters' and the host's.
        const std::string fighters[] = {"member-" + std::to_string(table.p1), "member-" + std::to_string(table.p2)};
        static const std::vector<std::string> fighterActions = {"options", "room-controls", "room-chat", "leave", "stop-watching"};
        static const std::vector<std::string> watcherActions = {"room-chat", "leave", "stop-watching"};
        const auto kept = [&](const std::string& id) {
            // The stage is the fighters' Ready; a watcher has nothing to open there.
            if (id == "table-0") return scene.you >= 0;
            if (id.compare(0, 6, "table-") == 0) return false;
            if (id.compare(0, 7, "member-") == 0) return id != fighters[0] && id != fighters[1];
            const auto& list = scene.you < 0 ? watcherActions : fighterActions;
            return std::find(list.begin(), list.end(), id) != list.end();
        };
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return !kept(e.id); }), rows.end());
    }

    // The moment, once each: a game or the set decided, a game cut off, the host's close, the match's end.
    auto& was = bluMint_;
    const bool same = was.match == a->matchId;
    std::optional<BluMintMoment> moment;
    const unsigned played = scene.wins[0] + scene.wins[1], before = was.wins[0] + was.wins[1];
    const bool decided = scene.gamesToWin && (scene.wins[0] >= static_cast<unsigned>(scene.gamesToWin) || scene.wins[1] >= static_cast<unsigned>(scene.gamesToWin));
    const bool failed = v.tournament.phase == netplay::tournament::Phase::Failed;
    if (same && failed && !was.failed) moment = BluMintMoment::Failed;
    else if (same && a->state != was.state && a->state == "cancelled") moment = BluMintMoment::HostTook;
    else if (same && a->state != was.state && a->Expired()) moment = BluMintMoment::Expired;
    else if (same && v.tournament.reason != was.reason && v.tournament.reason == netplay::tournament::ConnectionLostReason) moment = BluMintMoment::Restart;
    else if (same && played > before && decided) moment = BluMintMoment::SetDone;
    else if (same && played > before) moment = BluMintMoment::GameDone;
    was.match = a->matchId; was.state = a->state; was.reason = v.tournament.reason;
    was.wins[0] = scene.wins[0]; was.wins[1] = scene.wins[1]; was.failed = failed;
    if (!moment) { CallBluMint(v); return; }
    scene.moment = *moment;
    scene.failure = IdentityPanel::TournamentFailure(v.tournament.reason);
    // The set's notice offers the next match, so it is not called again on its own.
    if (*moment == BluMintMoment::SetDone && next && scene.you >= 0) { bluMintNext_ = next->matchId; bluMintTold_.push_back(next->matchId); }
    const auto notice = BluMintNotice(scene);
    // A fighter answers it; a watcher only reads it, on the status line.
    if (scene.you >= 0) menu_.ShowNotice(notice);
    else { notice_ = notice.heading; noticeTone_ = Tone::Neutral; noticeUntil_ = ImGui::GetTime() + 6; }
}

void ApplicationShell::CallBluMint(const ShellView& v) {
    const auto& t = v.tournament;
    // Not over a match being played, nor over another notice: it waits its turn.
    if (netplay::tournament::PlayUnderWay(t.phase) || menu_.NoticeOpen()) return;
    for (const auto& m : t.list.items) {
        // The match just played may still read as waiting until the list catches up.
        if (m.provider != "blumint" || !m.Playable(v.unixNow) || m.matchId == t.matchId) continue;
        if (std::find(bluMintTold_.begin(), bluMintTold_.end(), m.matchId) != bluMintTold_.end()) continue;
        bluMintTold_.push_back(m.matchId);
        bluMintNext_ = m.matchId;
        BluMintScene scene;
        scene.moment = BluMintMoment::Called;
        scene.you = 0;
        scene.round = m.roundLabel;
        scene.gamesToWin = m.gamesToWin;
        scene.inRoom = v.session.room != netplay::RoomState::Idle && v.room.tournament.matchId.empty();
        menu_.ShowNotice(BluMintNotice(scene));
        return;
    }
}

bool ApplicationShell::AnswerBluMint(NoticeResult result, const ShellView& v, const Submit& submit) {
    auto& nav = menu_.navigation;
    switch (result) {
    case NoticeResult::BluMintReady: {
        // Ready, never unready: a fighter who readied meanwhile stays so.
        const auto& t = v.room.tables[0];
        const int you = v.room.localMember == t.p1 ? 0 : v.room.localMember == t.p2 ? 1 : -1;
        if (you >= 0 && !t.ready[you]) ToggleReady(v, submit);
        return true;
    }
    case NoticeResult::BluMintFighter: selectionFresh_ = true; selectionOpenOn_ = "roster"; nav.Push("selection"); return true;
    // The match is on the list: BluMint's matches open at its row,
    // after leaving the room the player was in (a match's own room closes by itself).
    case NoticeResult::BluMintNext:
        if (v.session.room != netplay::RoomState::Idle && v.room.tournament.matchId.empty()) Send(netplay::CommandKind::LeaveRoom, v, submit);
        if (!bluMintNext_.empty()) identity_.OpenMatch(v.tournament.list.bridge, bluMintNext_);
        nav.Home(); nav.Push("online"); nav.Push("identity"); nav.Push("blumint-matches");
        return true;
    default: return false;
    }
}

void ApplicationShell::DrawBluMintBoard(const ShellView& v, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height) {
    boardDrawn_ = true;
    const float s = Scale();
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const auto focus = nav.Focus();
    const auto mouse = ImGui::GetIO().MouseDelta;
    const bool pointerMoved = mouse.x != 0 || mouse.y != 0;
    const auto origin = ImGui::GetCursorScreenPos();
    v2::Box stage{origin, ImVec2(origin.x + ImGui::GetContentRegionAvail().x, origin.y + height)};
    v2::Box aside = v2::CutRight(stage, 400 * s, 20 * s);
    // A row's box: a press is Select on it, the moving pointer focuses it.
    const auto press = [&](const MenuEntry& e, const v2::Box& b) {
        ImGui::SetCursorScreenPos(b.min);
        ImGui::PushID(e.id.c_str());
        const bool pressed = ImGui::InvisibleButton("##bm", ImVec2(b.W(), b.H()));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (pressed && !nav.Asking() && !nav.Editing()) { nav.Focus(e.id, rows); action = nav.Choose(rows); }
        else if (hovered && pointerMoved && !nav.Choosing()) nav.Focus(e.id, rows);
        return hovered;
    };
    const auto find = [&](const char* id) { return std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; }); };

    // The stage: the match's table, BluMint's blue light from the top. Select on it is Ready.
    const auto& t = v.room.tables[0];
    const int toWin = v.room.tournament.gamesToWin ? v.room.tournament.gamesToWin : 2;
    const bool decided = t.score[0] >= static_cast<unsigned>(toWin) || t.score[1] >= static_cast<unsigned>(toWin);
    // A watched game fills the stage like a stream, whichever spot the picture
    // has elsewhere: the picture at 16:9 on navy, the fighters, their pips, LIVE
    // and the score on a bar under it. Between games the stage has the cards.
    const bool cinema = v.watchedPicture != ImTextureID{};
    d->AddRectFilled(stage.min, stage.max, c.ink, 4 * s);
    if (!cinema) d->AddRectFilledMultiColor(stage.min, ImVec2(stage.max.x, stage.min.y + stage.H() * .7f), v2::color::Alpha(c.accent, .22f), v2::color::Alpha(c.accent, .22f), v2::color::Alpha(c.accent, 0), v2::color::Alpha(c.accent, 0));
    const auto table = find("table-0");
    if (table != rows.end()) {
        const bool hovered = press(*table, stage);
        if (hovered) d->AddRectFilled(stage.min, stage.max, v2::color::Alpha(c.hover, .35f), 4 * s);
    }
    d->AddRect(stage.min, stage.max, c.border, 4 * s, 0, s);
    const auto centred = [&](ImFont* font, float size, float y, ImU32 color, const std::string& text) {
        const float w = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x;
        d->AddText(font, size, ImVec2(stage.min.x + (stage.W() - w) * .5f, y), color, text.c_str());
    };
    const unsigned played = t.score[0] + t.score[1];
    const room::MemberId seats[] = {t.p1, t.p2};
    const int winner = !decided ? -1 : t.score[0] >= static_cast<unsigned>(toWin) ? 0 : 1;
    const auto fighterOf = [&](const room::Member& m) { return m.id == v.room.localMember ? v.selectedFighter : m.fighter; };
    const auto pips = [&](float x, float y, int side) {
        for (int g = 0; g < toWin; ++g) {
            const ImVec2 at(x + g * 16 * s, y);
            if (static_cast<unsigned>(g) < t.score[side]) d->AddCircleFilled(at, 5 * s, c.accent);
            else d->AddCircle(at, 5 * s, c.accent, 0, 1.5f * s);
        }
    };
    if (cinema) {
        v2::Box screen = stage;
        const v2::Box bar = v2::CutBottom(screen, 56 * s);
        const float w = (std::min)(screen.W(), screen.H() * 16 / 9) - 16 * s, h = w * 9 / 16;
        const ImVec2 at(screen.min.x + (screen.W() - w) * .5f, screen.min.y + (screen.H() - h) * .5f);
        pictureDock_ = v2::Box{at, ImVec2(at.x + w, at.y + h)};
        d->AddRectFilled(bar.min, bar.max, c.frame, 4 * s, ImDrawFlags_RoundCornersBottom);
        d->AddLine(bar.min, ImVec2(bar.max.x, bar.min.y), c.border, s);
        const std::string score = std::to_string(t.score[0]) + " \xE2\x80\x93 " + std::to_string(t.score[1]);
        const float scoreSize = 24 * s, scoreWidth = HeadingFont()->CalcTextSizeA(scoreSize, FLT_MAX, 0, score.c_str()).x;
        const std::string live = loc::T("blumint.live");
        const float liveWidth = t.phase == room::TablePhase::Playing ? v2::ChipWidth(live) + 10 * s : 0;
        const float mid = bar.min.x + bar.W() * .5f, y = bar.min.y + bar.H() * .5f, half = (scoreWidth + liveWidth) * .5f + 20 * s;
        if (liveWidth) v2::Chip(d, ImVec2(mid - half + 20 * s, y - 11 * s), live, c.live);
        d->AddText(HeadingFont(), scoreSize, ImVec2(mid + half - 20 * s - scoreWidth, y - scoreSize * .5f), c.accent, score.c_str());
        const float pipsWidth = toWin * 16 * s;
        for (int side = 0; side < 2; ++side) {
            const auto* m = room::FindMember(v.room, seats[side]);
            if (!m) continue;
            // Pips beside the score; the fighter's portrait at the outer edge, the name beside it.
            const float px = side ? mid + half : mid - half - pipsWidth + 6 * s;
            pips(px, y, side);
            const float face = bar.H() - 12 * s, fx = side ? bar.max.x - 8 * s - face : bar.min.x + 8 * s;
            DrawCharacterPortrait(fighterOf(*m), ImVec2(fx, bar.min.y + 6 * s), ImVec2(fx + face, bar.max.y - 6 * s), c.ink);
            const float tx = side ? px + pipsWidth + 12 * s : fx + face + 10 * s, tw = side ? fx - 10 * s - tx : px - 22 * s - tx;
            v2::TagView tag;
            tag.name = m->name;
            tag.marks = {m->country, m->badge};
            tag.ping = LatencyMarkFor(v.memberLatency, *m, v.room.localMember);
            v2::Tag(d, ImVec2(tx, y - 13 * s), tw, tag, v2::TagSize::S);
        }
    } else {
        centred(HeadingFont(), 13 * s, stage.min.y + 22 * s, c.accent,
            decided ? loc::T("blumint.stage_decided") : t.phase == room::TablePhase::Playing ? loc::Tf("blumint.stage_live", played + 1) : loc::Tf("blumint.stage_next", played + 1));
        const std::string score = std::to_string(t.score[0]) + " \xE2\x80\x93 " + std::to_string(t.score[1]);
        const float scoreSize = 40 * s, scoreWidth = HeadingFont()->CalcTextSizeA(scoreSize, FLT_MAX, 0, score.c_str()).x;
        const float between = scoreWidth + 44 * s, ch = 220 * s;
        // Wide enough for a name beside the ping and its route chip, as far as the stage allows.
        const float cw = (std::max)(200 * s, (std::min)(300 * s, (stage.W() - between - 48 * s) * .5f));
        const float left = stage.min.x + (stage.W() - 2 * cw - between) * .5f, top = stage.min.y + (stage.H() - ch) * .5f - 12 * s;
        for (int side = 0; side < 2; ++side) {
            const v2::Box card{ImVec2(left + side * (cw + between), top), ImVec2(left + side * (cw + between) + cw, top + ch)};
            const auto* m = room::FindMember(v.room, seats[side]);
            const bool you = m && m->id == v.room.localMember;
            d->AddRectFilled(card.min, card.max, c.frame, 4 * s);
            if (m) {
                const float tagY = card.max.y - 12 * s - 22 * s - 8 * s - 26 * s;
                DrawCharacterPortrait(fighterOf(*m), ImVec2(card.min.x + 12 * s, card.min.y + 12 * s), ImVec2(card.max.x - 12 * s, tagY - 8 * s), c.ink);
                v2::TagView tag;
                tag.name = m->name;
                tag.marks = {m->country, m->badge};
                tag.ping = LatencyMarkFor(v.memberLatency, *m, v.room.localMember);
                v2::Tag(d, ImVec2(card.min.x + 12 * s, tagY), cw - 24 * s, tag, v2::TagSize::S);
                // A pip a game won, of first to N.
                const float py = card.max.y - 12 * s - 11 * s;
                pips(card.min.x + 18 * s, py, side);
                // Once the set is decided, the winner's card says so; nobody readies any more.
                const bool ready = t.ready[side];
                const std::string word = winner >= 0 ? (winner == side ? std::string(loc::T("blumint.chip_set_won")) : std::string())
                                                      : loc::T(ready ? "room.status.ready" : "blumint.not_ready");
                if (!word.empty()) v2::Chip(d, ImVec2(card.max.x - 12 * s - v2::ChipWidth(word), py - 11 * s), word, winner == side ? c.accent : ready ? palette::Ready : c.muted);
            }
            // Lit: this PC's fighter during the set, the winner once it is decided.
            if (winner >= 0 ? winner == side : you) {
                d->AddRect(ImVec2(card.min.x - 6 * s, card.min.y - 6 * s), ImVec2(card.max.x + 6 * s, card.max.y + 6 * s), v2::color::Alpha(c.accent, .25f), 8 * s, 0, 6 * s);
                d->AddRect(card.min, card.max, c.accent, 4 * s, 0, 3 * s);
            } else d->AddRect(card.min, card.max, c.border, 4 * s, 0, s);
        }
        d->AddText(HeadingFont(), scoreSize, ImVec2(left + cw + (between - scoreWidth) * .5f, top + (ch - scoreSize) * .5f), c.accent, score.c_str());
        const bool watching = !(room::FindMember(v.room, v.room.localMember) && (t.p1 == v.room.localMember || t.p2 == v.room.localMember));
        const auto* champion = winner >= 0 ? room::FindMember(v.room, seats[winner]) : nullptr;
        centred(ImGui::GetFont(), 16 * s, top + ch + 20 * s, champion ? c.text : c.muted,
            champion ? loc::Tf("blumint.set_winner", champion->name) : watching ? loc::T("blumint.stage_watcher") : loc::Tf("blumint.stage_fighter", toWin));
    }
    if (table != rows.end() && focus == table->id) v2::Focus(d, stage.min, stage.max);

    // Who watches: everyone but the two fighters, as player tags.
    const float headingSize = 14 * s;
    std::vector<const MenuEntry*> watchers, actions;
    for (const auto& e : rows) {
        if (e.id.compare(0, 7, "member-") == 0) watchers.push_back(&e);
        else if (e.id.compare(0, 6, "table-")) actions.push_back(&e);
    }
    d->AddText(HeadingFont(), headingSize, aside.min, c.muted, loc::Tf("blumint.watching", watchers.size()).c_str());
    v2::CutTop(aside, headingSize, 8 * s);
    const float actionsHeight = ((actions.size() + 1) / 2) * 52 * s;
    v2::Box buttons = v2::CutBottom(aside, actionsHeight, 12 * s);
    const float rowHeight = 40 * s, rowGap = 8 * s;
    std::size_t fit = static_cast<std::size_t>((std::max)(1.f, (aside.H() * .4f + rowGap) / (rowHeight + rowGap)));
    // More than fit: the last line counts the rest.
    const bool more = watchers.size() > fit;
    if (more) fit = (std::max)(std::size_t(1), fit - 1);
    for (std::size_t i = 0; i < watchers.size() && i < fit; ++i) {
        const auto& e = *watchers[i];
        const v2::Box r = v2::CutTop(aside, rowHeight, rowGap);
        const bool hovered = press(e, r);
        v2::CardGround(d, r, e.id == focus, hovered);
        const auto* m = room::FindMember(v.room, std::stoull(e.id.substr(7)));
        if (!m) continue;
        const std::string role = loc::T(m->host ? "blumint.role_host" : "blumint.role_watching");
        const float roleWidth = ImGui::GetFont()->CalcTextSizeA(12 * s, FLT_MAX, 0, role.c_str()).x;
        v2::TagView tag;
        tag.name = m->name;
        tag.marks = {m->country, m->badge};
        v2::Tag(d, ImVec2(r.min.x + 12 * s, r.min.y + 11 * s), r.W() - 36 * s - roleWidth, tag, v2::TagSize::XS);
        d->AddText(ImGui::GetFont(), 12 * s, ImVec2(r.max.x - 12 * s - roleWidth, r.min.y + 13 * s), c.muted, role.c_str());
        if (e.id == focus) v2::Focus(d, r.min, r.max);
        if (e.id == focus && e.popout) menu_.bodyChoices = {r, aside};
    }
    if (more) {
        const v2::Box r = v2::CutTop(aside, 20 * s, rowGap);
        d->AddText(ImGui::GetFont(), 14 * s, ImVec2(r.min.x + 12 * s, r.min.y + 2 * s), c.muted, loc::Tf("blumint.more_watching", watchers.size() - fit).c_str());
    }

    // Chat, then the actions two to a line.
    v2::CutTop(aside, 6 * s);
    d->AddText(HeadingFont(), headingSize, aside.min, c.muted, loc::T("room.chat_heading"));
    v2::CutTop(aside, headingSize, 8 * s);
    ImGui::SetCursorScreenPos(aside.min);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("BluMint chat", ImVec2(aside.W(), aside.H()), 0, ImGuiWindowFlags_NoNavInputs);
    DrawChatLog(v, true);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1 || chatShown_ == 0) ImGui::SetScrollHereY(1.f);
    chatShown_ = transcript_.Lines().size();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    const float bw = (buttons.W() - 8 * s) * .5f;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto& e = *actions[i];
        const ImVec2 at(buttons.min.x + (i % 2) * (bw + 8 * s), buttons.min.y + (i / 2) * 52 * s);
        const v2::Box b{at, ImVec2(at.x + bw, at.y + 44 * s)};
        const bool hovered = press(e, b);
        v2::CardGround(d, b, e.id == focus, hovered);
        const bool leave = e.id == "leave" || e.id == "stop-watching";
        const ImU32 color = !e.enabled ? c.muted : leave ? IM_COL32(255, 170, 160, 255) : c.text;
        const float w = ImGui::GetFont()->CalcTextSizeA(16 * s, FLT_MAX, 0, e.label.c_str()).x;
        d->AddText(ImGui::GetFont(), 16 * s, ImVec2(b.min.x + (b.W() - w) * .5f, b.min.y + 14 * s), color, e.label.c_str());
        if (e.id == focus) v2::Focus(d, b.min, b.max, false);
    }
    roomBoardFocus_ = focus;
}
} }
