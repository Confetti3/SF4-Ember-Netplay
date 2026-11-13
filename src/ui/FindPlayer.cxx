#include "FindPlayer.hxx"
#include "CardModel.hxx"
#include "CardParts.hxx"
#include "FriendsPanel.hxx"
#include "GameMenu.hxx"
#include "MenuRows.hxx"
#include "RowId.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cctype>

namespace sf4e { namespace ui { namespace find_player {
namespace {
// What the search was asked for, as it is asked from the text typed.
bool Asked(const Page& page) {
    if (!page.search || page.search->text.empty()) return false;
    const std::string text = Trimmed(page.typed);
    return page.search->text == text && page.search->byCode == WholeCode(text);
}

std::string SinceWords(const profiles::Hit& hit) {
    return hit.since ? loc::Tf("card.since", player_card::MonthWords(hit.since)) : std::string();
}

// "Played 2 h ago · Player 5's room": the game's end and the room, whichever of the two is known.
std::string RecentWords(const profiles::Recent& recent, std::uint64_t now) {
    const std::string ago = friends_panel::Ago(now, recent.at);
    if (ago.empty()) return recent.room;
    return recent.room.empty() ? loc::Tf("find.recent_when", ago) : loc::Tf("find.recent_line", ago, recent.room);
}

MenuEntry PlayerRow(const std::string& id, const std::string& name, const std::string& detail, const std::string& group, std::size_t at) {
    auto row = Row(id, name, detail);
    row.userText = true;
    row.detailText = DetailText::Name;
    row.height = 64;
    row.key = at;
    row.group = group;
    row.hint = loc::T("room.member_card");
    return row;
}
}

std::string Trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

bool WholeCode(const std::string& typed) {
    std::size_t symbols = 0;
    for (const char c : typed) {
        if (std::isalnum(static_cast<unsigned char>(c))) ++symbols;
        else if (c != ' ' && c != '-') return false;
    }
    return symbols == friends_panel::CodeLength;
}

Ask AskOf(const std::string& typed, profiles::Service offering) {
    Ask ask;
    ask.text = Trimmed(typed);
    if (WholeCode(ask.text)) { ask.ok = ask.code = true; return ask; }
    ask.why = offering == profiles::Service::Missing ? "profile.unsupported" : ask.text.size() < LeastName ? "find.text_detail" : nullptr;
    ask.ok = !ask.why;
    return ask;
}

const profiles::SearchAnswer* AnswerOf(const Page& page) {
    return Asked(page) && page.search->answer.loaded ? &page.search->answer.value : nullptr;
}

MenuEntry SearchEntry(const Page& page) {
    const Ask ask = AskOf(page.typed, page.offering);
    auto row = Row(SearchRow, loc::T("find.search"), ask.ok ? std::string() : std::string(loc::T(ask.why)), ask.ok);
    row.hint = loc::T("find.search");
    return row;
}

std::vector<MenuEntry> ResultRows(const Page& page) {
    std::vector<MenuEntry> rows;
    const auto note = [&](const char* words) {
        auto row = InfoRow(FindNoteRow, loc::T(words), {}, {});
        row.quiet = true;
        rows.push_back(std::move(row));
    };
    const auto* answer = AnswerOf(page);
    if (Asked(page) && page.search->answer.loading && !answer) note("find.searching");
    if (answer) {
        const std::string group = loc::Tf("find.players", answer->players.size());
        for (std::size_t i = 0; i < answer->players.size(); ++i) {
            const auto& hit = answer->players[i];
            rows.push_back(PlayerRow(HitPrefix + std::to_string(i), hit.name, SinceWords(hit), group, i));
        }
        if (answer->players.empty()) note("find.none");
        else if (answer->more) note("find.more");
    }
    const std::vector<profiles::Recent> none;
    const auto& recent = page.recent ? *page.recent : none;
    const std::string group = loc::T("find.recent");
    for (std::size_t i = 0; i < recent.size(); ++i) rows.push_back(PlayerRow(RecentPrefix + std::to_string(i), recent[i].name, RecentWords(recent[i], page.now), group, i));
    // The words are long, so they are the row's detail under the group's name, which wraps.
    if (recent.empty() && rows.empty()) {
        auto row = InfoRow(FindNoteRow, loc::T("find.recent"), {}, loc::T("find.recent_empty"));
        row.quiet = true;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::optional<player_opening::Opening> OpeningOfRow(const Page& page, const std::string& row) {
    if (const auto at = RowKey<std::size_t>(row, HitPrefix)) {
        const auto* answer = AnswerOf(page);
        if (answer && *at < answer->players.size()) return player_opening::OpeningOf(answer->players[*at]);
    } else if (const auto place = RowKey<std::size_t>(row, RecentPrefix)) {
        if (page.recent && *place < page.recent->size()) return player_opening::OpeningOf((*page.recent)[*place]);
    }
    return std::nullopt;
}

std::optional<Pick> PickOfRow(const Page& page, const std::string& row, bool characterArt) {
    Pick pick;
    pick.characterArt = characterArt;
    if (const auto at = RowKey<std::size_t>(row, HitPrefix)) {
        const auto* answer = AnswerOf(page);
        if (!answer || *at >= answer->players.size()) return std::nullopt;
        const auto& hit = answer->players[*at];
        pick.name = hit.name; pick.country = hit.country; pick.badge = hit.badge; pick.fighter = hit.hasMain ? hit.main : -1;
        pick.detail = SinceWords(hit);
        return pick;
    }
    if (const auto at = RowKey<std::size_t>(row, RecentPrefix)) {
        if (!page.recent || *at >= page.recent->size()) return std::nullopt;
        const auto& recent = (*page.recent)[*at];
        pick.name = recent.name; pick.country = recent.country; pick.fighter = recent.fighter;
        pick.detail = RecentWords(recent, page.now);
        return pick;
    }
    return std::nullopt;
}

void DrawPick(ImDrawList* d, const Pick& pick, ImVec2 min, ImVec2 max) {
    const auto& c = v2::Colors();
    const float s = Scale(), side = max.y - min.y - 16 * s;
    // Their main, flag, class and name, and the words under them.
    const v2::Box picture{ImVec2(min.x + 12 * s, min.y + 8 * s), ImVec2(min.x + 12 * s + side, min.y + 8 * s + side)};
    if (pick.fighter >= 0 && selection::FindFighter(pick.fighter))
        DrawCharacterPortrait(FighterPicture{pick.fighter, pick.characterArt ? FighterArt::Character : FighterArt::Portrait, 0, 0}, picture.min, picture.max, c.frame);
    else d->AddRectFilled(picture.min, picture.max, c.frame);
    const float x = picture.max.x + 14 * s, room = max.x - x - 12 * s, size = 18 * s;
    NoteUserText(pick.name);
    card_parts::DrawPlayerLine(d, x, min.y + 10 * s, room, size, size * 24.f / 22.f, std::string(), pick.name, pick.country, pick.badge);
    if (pick.detail.empty()) return;
    NoteUserText(pick.detail);
    v2::Text(d, ImVec2(x, min.y + 10 * s + size + 8 * s), room, pick.detail, 14 * s, c.muted);
}

} } }
