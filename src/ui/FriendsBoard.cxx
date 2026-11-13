// The Friends tab's board (docs/design/PROFILE.md, "Friends tab"): the list on the left, a row each placed through
// PressEntry so the pad moves over them by where they are drawn, and the focused row's panel on the right (under the
// list on a narrow window, as words). What each row means is FriendsPanel.cxx's.
#include "CardParts.hxx"
#include "FriendsPanel.hxx"
#include "MenuGlyphs.hxx"
#include "PlayerCard.hxx"
#include "RowId.hxx"
#include "ShellView.hxx"
#include "SignInSites.hxx"
#include "Theme.hxx"
#include "../common/LocalTime.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>

namespace sf4e { namespace ui {
namespace {
using namespace friends_panel;

// What a row's board knows of the frame: the window, the sizes, and where a popout opens.
struct BoardFrame {
    ImDrawList* d;
    GameMenu& menu;
    // Opens More on a row (FriendsPanel::Ask).
    std::function<void(const std::string&)> more;
    const std::vector<MenuEntry>& rows;
    MenuAction& action;
    const nf::View& view;
    const nf::State& state;
    Gate gate;
    FighterArt art;
    std::uint64_t now;
    bool changed;
    // The list's column, whose width a popout's choices lay out in.
    v2::Box column;
    float s;
    // The cards the runtime looked up (ShellView::memberCards): a friend's, by their friend id, gives their Panel its chips,
    // record and families (FriendCardWanted). Null while there are none.
    const netplay::levels::CardMap* cards;
    MenuNavigation& nav() const { return menu.navigation; }
};

const nf::Incoming* RequestOf(const nf::State& state, const std::string& row) {
    const std::string id = IdOf(row, RequestPrefix);
    const auto at = std::find_if(state.incoming.begin(), state.incoming.end(), [&](const nf::Incoming& r) { return r.id == id; });
    return at == state.incoming.end() ? nullptr : &*at;
}

float TextWidth(const std::string& text, float size, ImFont* font) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; }

// A friend's presence as a mark that differs in shape: a filled disc in a room, a ring online, a dashed ring offline.
void DrawPresenceMark(ImDrawList* d, ImVec2 at, float radius, nf::Presence::Kind kind) {
    using player_card::PresenceMark;
    card_parts::DrawPresence(d, at, radius, kind == nf::Presence::Kind::InRoom ? PresenceMark::InRoom : kind == nf::Presence::Kind::Online ? PresenceMark::Online : PresenceMark::Offline);
}
// A request's mark: a diamond in the light accent.
void DrawRequestMark(ImDrawList* d, ImVec2 at, float radius) {
    const float r = radius * .9f;
    d->AddQuadFilled(ImVec2(at.x, at.y - r), ImVec2(at.x + r, at.y), ImVec2(at.x, at.y + r), ImVec2(at.x - r, at.y), v2::Colors().accentLight);
}

// A friend's main on its tier edge in `tile`; no main is a bare frame, and an offline friend's is dimmed.
void DrawMainPicture(const BoardFrame& f, v2::Box tile, const nf::Friend& entry) {
    const auto& c = v2::Colors();
    if (entry.hasMain) DrawCharacterPortrait(FighterPicture{entry.main.fighter, f.art, 0, 0}, tile.min, tile.max, c.frame);
    else f.d->AddRectFilled(tile.min, tile.max, c.frame);
    if (entry.presence.kind == nf::Presence::Kind::Offline) f.d->AddRectFilled(tile.min, tile.max, v2::color::Alpha(c.ink, .45f));
    DrawFighterEdge(f.d, tile, entry.main.showsPoints ? entry.main.points : 0, entry.main.tier);
}

// The room the row's value takes at its right end: its word and the margin.
float ValueRoom(const BoardFrame& f, const MenuEntry& e) {
    return e.value.empty() ? 0 : TextWidth(e.value, 16 * f.s, HeadingFont()) + 1 + 18 * f.s;
}

// The row's value at the row's right end.
void DrawValue(const BoardFrame& f, v2::Box box, const MenuEntry& e, bool dim) {
    if (e.value.empty()) return;
    const auto& c = v2::Colors();
    const float s = f.s, size = 16 * s, w = ValueRoom(f, e) - 18 * s;
    v2::Text(f.d, ImVec2(box.max.x - 18 * s - w, box.min.y + (box.H() - size) * .5f), w, e.value, size, dim ? c.muted : e.valueHighlighted ? c.accent : c.text, HeadingFont());
}

// The focus marks over a row, and its popout's place when it has choices.
void Finish(const BoardFrame& f, const MenuEntry& e, v2::Box box) {
    if (e.id != f.nav().Focus()) return;
    v2::Focus(f.d, box.min, box.max);
    if (!e.choices.empty()) f.menu.bodyChoices = {box, f.column};
}

// A request or a friend: the mark, the friend's picture, flag, name and class, and the status under them. The status
// wraps under the name, the row growing by its lines (the Played you words and the friend's table are long in some languages).
float DrawPerson(const BoardFrame& f, const MenuEntry& e, ImVec2 at, float width) {
    const float s = f.s;
    const nf::Friend* entry = HasPrefix(e.id, FriendPrefix) ? f.state.FindFriend(IdOf(e.id, FriendPrefix)) : nullptr;
    const nf::Incoming* request = entry ? nullptr : RequestOf(f.state, e.id);
    const float x = at.x + 48 * s + (entry ? 60 * s : 0), room = at.x + width - x - ValueRoom(f, e) - 12 * s;
    const float lines = v2::Paragraph(nullptr, ImVec2(), room, e.detail, 14 * s, 0);
    const v2::Box box{at, ImVec2(at.x + width, at.y + (std::max)(64 * s, 41 * s + lines))};
    const auto hit = PressEntry(f.nav(), f.rows, e, box, f.action, f.changed);
    v2::CardGround(f.d, box, e.id == f.nav().Focus(), hit.hovered);
    const ImVec2 centre(at.x + 20 * s, box.min.y + 32 * s);
    if (entry) DrawPresenceMark(f.d, centre, 6 * s, entry->presence.kind);
    else DrawRequestMark(f.d, centre, 6 * s);
    if (entry) DrawMainPicture(f, v2::Box{ImVec2(at.x + 48 * s, box.min.y + 8 * s), ImVec2(at.x + 96 * s, box.min.y + 56 * s)}, *entry);
    // The word dims while its action cannot be taken: Join waits on the list being current, the rest on no action in flight.
    const bool dim = entry && PressOf(*entry, f.view.canInvite) == Press::Join ? !f.gate.go : !f.gate.act;
    DrawValue(f, box, e, dim);
    const CountryCode country = entry ? entry->country : request ? request->country : CountryCode();
    DrawNameLine(f.d, x, box.min.y + 8 * s, room, 22 * s, 16 * s, e.label, country, entry ? entry->badge : SkillBadge{});
    NoteUserText(e.detail);
    v2::Paragraph(f.d, ImVec2(x, box.min.y + 31 * s), room, e.detail, 14 * s, v2::Colors().muted);
    Finish(f, e, box);
    return box.max.y;
}

// Add a friend: the word and, at the right, "Your code" with the code in the heading font.
float DrawAdd(const BoardFrame& f, const MenuEntry& e, ImVec2 at, float width, bool compact) {
    const auto& c = v2::Colors();
    const float s = f.s;
    const v2::Box box{at, ImVec2(at.x + width, at.y + (compact ? 50 : 56) * s)};
    const auto hit = PressEntry(f.nav(), f.rows, e, box, f.action, f.changed);
    v2::CardGround(f.d, box, e.id == f.nav().Focus(), hit.hovered);
    const float x = box.min.x + 18 * s, mid = box.min.y + box.H() * .5f;
    const float label = TextWidth(e.label, 17 * s, HeadingFont());
    v2::Text(f.d, ImVec2(x, mid - 9 * s), width, e.label, 17 * s, e.enabled ? c.text : c.muted, HeadingFont());
    const std::string& code = f.state.code;
    if (e.enabled && !e.value.empty()) {
        const bool coded = !code.empty() && e.value.size() >= code.size() && e.value.compare(e.value.size() - code.size(), code.size(), code) == 0;
        const std::string words = coded ? e.value.substr(0, e.value.size() - code.size()) : e.value, shown = coded ? code : std::string();
        const float wordsWide = TextWidth(words, 15 * s, ImGui::GetFont()), codeWide = TextWidth(shown, 17 * s, HeadingFont());
        const float right = box.max.x - 18 * s, left = x + label + 16 * s, room = right - left;
        // The code matters more than the words before it: both when they fit, else the code alone, else the code shrunk.
        // A value with no code of its own is words alone, shrunk before it is cut.
        if (room > 0) {
            if (shown.empty()) {
                const float w = (std::min)(wordsWide + 1, room);
                v2::TextShrunk(f.d, ImVec2(right - w, mid - 8 * s), w, words, 15 * s, player_card::TextFloor * s, c.muted);
            } else if (wordsWide + codeWide <= room) {
                if (!words.empty()) v2::Text(f.d, ImVec2(right - codeWide - wordsWide, mid - 8 * s), wordsWide + 1, words, 15 * s, c.muted);
                v2::Text(f.d, ImVec2(right - codeWide, mid - 9 * s), codeWide + 1, shown, 17 * s, c.text, HeadingFont());
            } else if (codeWide <= room) {
                v2::Text(f.d, ImVec2(right - codeWide, mid - 9 * s), codeWide + 1, shown, 17 * s, c.text, HeadingFont());
            } else {
                v2::TextShrunk(f.d, ImVec2(left, mid - 9 * s), room, shown, 17 * s, player_card::TextFloor * s, c.text, HeadingFont());
            }
        }
    }
    Finish(f, e, box);
    return box.max.y;
}

// Sent and Blocked: the word and a chevron; Appear offline: the word and its switch.
float DrawLink(const BoardFrame& f, const MenuEntry& e, ImVec2 at, float width) {
    const auto& c = v2::Colors();
    const float s = f.s;
    const v2::Box box{at, ImVec2(at.x + width, at.y + 48 * s)};
    const auto hit = PressEntry(f.nav(), f.rows, e, box, f.action, f.changed);
    v2::CardGround(f.d, box, e.id == f.nav().Focus(), hit.hovered);
    const float mid = box.min.y + box.H() * .5f;
    const bool toggle = e.id == AppearRow;
    float used = 18 * s;
    if (toggle) {
        // Drawn as the Settings board draws an On or Off value, which only an adjustable one is.
        MenuEntry shown = e;
        shown.adjustable = true;
        const float w = MenuValueWidth(shown);
        DrawMenuValue(f.d, shown, v2::Box{ImVec2(box.max.x - 18 * s - w, box.min.y), ImVec2(box.max.x - 18 * s, box.max.y)}, e.enabled, c.text);
        used += w;
    } else {
        v2::Chevron(f.d, ImVec2(box.max.x - 24 * s, mid), 7 * s, c.muted);
        used += 12 * s;
    }
    v2::Text(f.d, ImVec2(box.min.x + 18 * s, mid - 8 * s), box.W() - 36 * s - used, e.label, 16 * s, e.enabled ? c.text : c.muted, HeadingFont());
    Finish(f, e, box);
    return box.max.y;
}

// One line of words on a card: a state with nothing to list, or the empty list's hint.
float DrawNote(const BoardFrame& f, const MenuEntry& e, ImVec2 at, float width) {
    const auto& c = v2::Colors();
    const float s = f.s, pad = 18 * s;
    ImDrawListSplitter split;
    split.Split(f.d, 2);
    split.SetCurrentChannel(f.d, 1);
    const float h = v2::Paragraph(f.d, ImVec2(at.x + pad, at.y + pad), (std::min)(width, 720 * s) - 2 * pad, e.label, 16 * s, c.muted);
    const v2::Box box{at, ImVec2(at.x + (std::min)(width, 720 * s), at.y + h + 2 * pad)};
    split.SetCurrentChannel(f.d, 0);
    const auto hit = PressEntry(f.nav(), f.rows, e, box, f.action, f.changed);
    v2::CardGround(f.d, box, e.id == f.nav().Focus(), hit.hovered);
    split.Merge(f.d);
    Finish(f, e, box);
    return box.max.y;
}

// A panel button: the word, after the pad button's glyph when given, pressed with the pointer (the pad uses A and Y on the row).
// The Primary button carries no glyph, which would clash with its ground; the hint bar names A.
bool PanelButton(const BoardFrame& f, const char* id, v2::Box box, const std::string& label, const char* glyph, v2::ButtonKind kind, bool enabled) {
    const auto& c = v2::Colors();
    const float s = f.s;
    ImGui::SetCursorScreenPos(box.min);
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##panel", ImVec2(box.W(), box.H())) && f.nav().Choosing() == false && !f.nav().Asking();
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    v2::Button(f.d, box, std::string(), kind, false, hovered, enabled);
    const float side = glyph ? 26 * s : 0, gap = glyph ? 10 * s : 0, font = 17 * s, whole = side + gap + TextWidth(label, font, HeadingFont()), left = box.min.x + (box.W() - whole) * .5f;
    if (glyph) DrawPromptGlyph(f.d, glyph, ImVec2(left, box.min.y + (box.H() - side) * .5f), side, MenuArt(), s);
    f.d->AddText(HeadingFont(), font, ImVec2(left + side + gap, box.min.y + (box.H() - font) * .5f), enabled ? c.text : c.muted, label.c_str());
    return pressed && enabled;
}

// "14 - 9" from this player's side, or a dash before they have played.
std::string RecordOf(const nf::Friend& entry) {
    return entry.hasRecord ? std::to_string(entry.record.won) + " \xE2\x80\x93 " + std::to_string(entry.record.lost) : std::string("-");
}

// The focused friend's or request's panel: for a friend their card (main, flag, name, class, the main's points), where
// they are, the record, and the row's action beside More; for a request the name and the played line with Accept and More.
void DrawPersonPanel(const BoardFrame& f, v2::Box box, const MenuEntry& e, bool compact) {
    const auto& c = v2::Colors();
    const float s = f.s, pad = 16 * s;
    const nf::Friend* entry = HasPrefix(e.id, FriendPrefix) ? f.state.FindFriend(IdOf(e.id, FriendPrefix)) : nullptr;
    const nf::Incoming* request = entry ? nullptr : RequestOf(f.state, e.id);
    if (!entry && !request) return;
    ImDrawListSplitter split;
    split.Split(f.d, 2);
    split.SetCurrentChannel(f.d, 1);
    const std::string& name = entry ? entry->name : request->name;
    const float x = box.min.x + pad, room = box.W() - 2 * pad, head = 20 * s + v2::HeadingHeight(name, room, 20 * s);
    float y = box.min.y + head + pad;
    if (entry) {
        OtherCardView card = CardOf(*entry, f.art);
        // Their card, once the runtime has read it: the list's own words stand until then.
        if (f.cards)
            if (const auto read = f.cards->find(entry->id); read != f.cards->end())
                card = PanelOf(player_card::CardSubject(read->second, player_opening::OpeningOf(*entry).known, -1, false, true), f.art);
        // The Panel stands on this panel's own ground; its height is its content's.
        const float cardHeight = DrawOtherCard(f.d, v2::Box{ImVec2(x, y), ImVec2(x + room, y)}, card, false);
        // The pointer on the card opens their profile; the pad does it with A, or from More.
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID("panel-card");
        const bool opened = ImGui::InvisibleButton("##card", ImVec2(room, cardHeight)) && !f.nav().Choosing() && !f.nav().Asking();
        ImGui::PopID();
        if (opened) f.action = MenuAction{MenuAction::Activate, CardPrefix + e.id};
        y += cardHeight + 14 * s;
        // Where they are, with the room's count.
        DrawPresenceMark(f.d, ImVec2(x + 6 * s, y + 10 * s), 6 * s, entry->presence.kind);
        std::string where = e.detail;
        if (entry->presence.kind == nf::Presence::Kind::InRoom && entry->presence.hasRoom && entry->presence.room.capacity)
            where += " \xC2\xB7 " + loc::Tf("card.of", static_cast<int>(entry->presence.room.members), static_cast<int>(entry->presence.room.capacity));
        NoteUserText(where);
        y += v2::Paragraph(f.d, ImVec2(x + 22 * s, y), room - 22 * s, where, 15 * s, c.text) + 14 * s;
        const auto ago = Ago(f.now, entry->record.lastAt);
        y += v2::FactCells(f.d, ImVec2(x, y), room, {{loc::Tf("friends.record", name), RecordOf(*entry)}, {loc::T("friends.last_played"), ago.empty() ? std::string("-") : ago},
            {loc::T("friends.since"), entry->since ? LocalTime(entry->since, "%Y-%m-%d") : std::string("-")}}, 3) + 8 * s;
    } else {
        y += DrawNameLine(f.d, x, y, room, 30 * s, 22 * s, name, request->country, SkillBadge{}) + 6 * s;
        y += v2::Paragraph(f.d, ImVec2(x, y), room, e.detail, 15 * s, c.muted) + 12 * s;
    }
    // The row's action and More beside it; where More is the action it stands alone.
    const Press press = entry ? PressOf(*entry, f.view.canInvite) : Press::Join;
    const bool card = entry && press == Press::Card;
    const std::string word = request ? loc::T("friends.accept") : card ? loc::T("room.member_card") : loc::T(press == Press::Join ? "friends.join_room" : "friends.invite_room");
    const bool gated = card ? false : request || press != Press::Join ? !f.gate.act : !f.gate.go;
    const float high = 48 * s;
    v2::Box row{ImVec2(x, y + 4 * s), ImVec2(x + room, y + 4 * s + high)};
    const v2::Box primary = v2::CutLeft(row, room - 150 * s - 10 * s, 10 * s);
    if (PanelButton(f, "panel-primary", primary, word, nullptr, v2::ButtonKind::Primary, !gated)) f.action = MenuAction{MenuAction::Activate, e.id};
    if (PanelButton(f, "panel-more", row, loc::T("friends.more"), "Y", v2::ButtonKind::Secondary, true)) f.more(e.id);
    y += high + 4 * s;
    box.max.y = y + pad;
    split.SetCurrentChannel(f.d, 0);
    v2::Panel(f.d, box, name.c_str(), entry ? loc::T("friends.are_friends") : nullptr);
    split.Merge(f.d);
}

// The other rows' detail, as words in a panel's place.
void DrawWordsPanel(const BoardFrame& f, v2::Box box, const MenuEntry& e) {
    if (e.detail.empty()) return;
    box.max.y = box.min.y + v2::WordsHeight(box.W(), e.detail);
    NoteUserText(e.detail);
    v2::Words(f.d, box, e.label, e.detail, true);
}

// Under the list on a narrow window: the focused friend's presence, record and points as words.
float DrawNarrowWords(const BoardFrame& f, ImVec2 at, float width, const MenuEntry& e) {
    const nf::Friend* entry = HasPrefix(e.id, FriendPrefix) ? f.state.FindFriend(IdOf(e.id, FriendPrefix)) : nullptr;
    std::string detail = e.detail;
    if (entry) {
        detail += "\n" + loc::Tf("friends.record", entry->name) + ": " + RecordOf(*entry);
        if (entry->hasMain && entry->main.showsPoints) detail += "\n" + loc::Tf("level.value", entry->main.points);
    }
    if (detail.empty()) return at.y;
    const v2::Box box{at, ImVec2(at.x + width, at.y + v2::WordsHeight(width, detail))};
    NoteUserText(e.label);
    NoteUserText(detail);
    v2::Words(f.d, box, e.label, detail, true);
    return box.max.y;
}
}

void FriendsPanel::Draw(const ShellView& v, GameMenu& menu, const std::vector<MenuEntry>& rows, MenuAction& action, float height) {
    const auto& c = v2::Colors();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 12 * s;
    auto* d = ImGui::GetWindowDrawList();
    const bool narrow = v2::Narrow(ImGui::GetIO().DisplaySize.x, s), compact = !narrow && height < 620 * s;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    auto& nav = menu.navigation;
    const bool changed = nav.Focus() != focus_;
    focus_ = nav.Focus();
    const Mode mode = ModeOf(v.friends, service_);
    v2::Box list{origin, ImVec2(origin.x + width, FLT_MAX)};
    const v2::Box side = narrow || mode != Mode::Listed ? v2::Box{} : v2::CutRight(list, (compact ? 400 : 520) * s, 24 * s);
    const BoardFrame f{d, menu, [this](const std::string& row) { Ask(row, {}); }, rows, action, v.friends, StateOf(v.friends), GateOf(v.friends, service_, pending_.active),
        v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait, v.unixNow, changed,
        v2::Box{origin, ImVec2(list.max.x, origin.y + height)}, s, v.memberCards.get()};
    float y = origin.y;
    // Without an account the words, and the way on.
    if (mode == Mode::NoAccount) {
        v2::Paragraph(d, ImVec2(list.min.x, y), (std::min)(list.W(), 720 * s), loc::T("friends.no_account"), 16 * s, c.muted);
        y += 56 * s;
        y = DrawRowButton(DiscordConnectScreen, ImVec2(list.min.x, y), (std::min)(list.W(), 360 * s), rows, nav, action, changed);
    }
    std::string group;
    for (const auto& e : rows) {
        if (e.id == DiscordConnectScreen) continue;
        if (e.group != group) {
            group = e.group;
            y += 10 * s;
            if (!group.empty()) { v2::SectionHeading(d, ImVec2(list.min.x, y), group); y += 26 * s; }
        }
        const ImVec2 at(list.min.x, y);
        if (HasPrefix(e.id, RequestPrefix) || HasPrefix(e.id, FriendPrefix)) y = DrawPerson(f, e, at, list.W());
        else if (e.id == AddRow) y = DrawAdd(f, e, at, list.W(), compact);
        else if (e.id == NoteRow) y = DrawNote(f, e, at, list.W());
        else y = DrawLink(f, e, at, list.W());
        y += 6 * s;
    }
    // The focused row's panel beside the list stays where the list's scroll has left the top of the body.
    const auto focused = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == nav.Focus(); });
    if (focused != rows.end() && mode == Mode::Listed) {
        const float pinned = (std::max)(origin.y, ImGui::GetWindowPos().y);
        if (narrow) y = DrawNarrowWords(f, ImVec2(list.min.x, y + gap), width, *focused);
        else if (HasPrefix(focused->id, RequestPrefix) || HasPrefix(focused->id, FriendPrefix)) DrawPersonPanel(f, v2::Box{ImVec2(side.min.x, pinned), ImVec2(side.max.x, 0)}, *focused, compact);
        else DrawWordsPanel(f, v2::Box{ImVec2(side.min.x, pinned), ImVec2(side.max.x, 0)}, *focused);
    }
    const float total = (std::max)(y, origin.y + 1) + gap - origin.y;
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, total));
}
} }
