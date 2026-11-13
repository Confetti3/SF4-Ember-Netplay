#include "PlayerCard.hxx"
#include "CardParts.hxx"
#include "MenuRows.hxx"
#include "ShellView.hxx"
#include "SignInSites.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <string>

namespace sf4e { namespace ui {
namespace player_card {
std::vector<int> PlayedFighters(const Own& own) {
    std::vector<int> played;
    for (int fighter = 0; fighter < static_cast<int>(own.fighters.size()); ++fighter)
        if (own.fighters[fighter].points) played.push_back(fighter);
    std::sort(played.begin(), played.end(), [&](int a, int b) {
        const auto &x = own.fighters[a], &y = own.fighters[b];
        return x.points != y.points ? x.points > y.points : a < b;
    });
    return played;
}

Groups Grouped(const Own& own, int main) {
    Groups groups;
    groups.played = PlayedFighters(own);
    for (const int fighter : selection::RosterDisplayOrder)
        if (!own.fighters[fighter].points) groups.unplayed.push_back(fighter);
    // The main leads its own group, wherever its points would put it.
    for (auto* group : {&groups.played, &groups.unplayed}) {
        const auto at = std::find(group->begin(), group->end(), main);
        if (at != group->end()) std::rotate(group->begin(), at, at + 1);
    }
    return groups;
}

float Part(const Level& level) {
    // A fighter with no points has no tier from the bridge yet: an empty meter, not the top.
    if (!level.points && !level.next) return 0.f;
    if (!level.next || level.next <= level.floor) return 1.f;
    const std::uint64_t done = (std::max)(level.points, level.floor) - level.floor;
    return (std::min)(1.f, static_cast<float>(done) / static_cast<float>(level.next - level.floor));
}

void Fill(PlayerCardView& view, const netplay::Answered<Own>& answer, int main) {
    view.fighters.clear();
    view.main = view.account = {};
    view.facts = {};
    const bool hasMain = main >= 0 && main < static_cast<int>(Own{}.fighters.size());
    view.mainFighter = hasMain ? main : -1;
    // Whatever the answer, the card shows the main, at 0 points when there are none.
    if (!answer.loaded) {
        // A read that failed with nothing before it has no points to show.
        view.levels = answer.loading || answer.error.empty() ? CardLevels::Pending : CardLevels::Unreachable;
        if (hasMain) view.fighters.push_back({main, 0, 0, 0});
        return;
    }
    const Own& own = answer.value;
    view.levels = own.kind == Own::Kind::NoAccount ? CardLevels::NoAccount : own.kind == Own::Kind::Counted ? CardLevels::Counted : CardLevels::Unavailable;
    if (view.levels != CardLevels::Counted) {
        if (hasMain) view.fighters.push_back({main, 0, 0, 0});
        return;
    }
    view.account = own.account;
    if (hasMain) view.main = own.fighters[main];
    for (const int fighter : TopFighters(PlayedFighters(own), hasMain ? main : -1))
        view.fighters.push_back({fighter, own.fighters[fighter].points, own.fighters[fighter].tier, own.fighters[fighter].games});
    // The account counts every game, those that named no fighter too, so its
    // games and wins are the card's, not a sum over the fighter rows.
    for (const auto& level : own.fighters) view.facts.fightersPlayed += level.points ? 1 : 0;
    view.facts.games = own.account.games;
    const std::uint64_t wins = own.account.wins;
    view.facts.winPercent = view.facts.games ? static_cast<int>((wins * 100 + view.facts.games / 2) / view.facts.games) : 0;
}

std::string GamesWords(std::uint64_t games) { return loc::Tf(loc::SingularCount(games) ? "level.game_one" : "level.games", games); }

const char* NoLevelWords(const PlayerCardView& card) {
    switch (card.levels) {
    case CardLevels::Pending: return "level.loading";
    case CardLevels::NoAccount: return "level.no_account";
    case CardLevels::Unavailable: return "level.unavailable";
    case CardLevels::Unreachable: return "level.unreachable";
    case CardLevels::Counted: return card.facts.fightersPlayed ? nullptr : "level.none";
    }
    return nullptr;
}

Progress ProgressOf(const std::string& name, const Level& level, bool account) {
    Progress row;
    const bool started = level.points > 0;
    row.label = started ? name + " \xC2\xB7 " + v2::TierName(v2::FrameTier(level.tier)) : name;
    row.part = Part(level);
    if (level.next > level.points && (started || account)) row.words = loc::Tf("level.points_to", level.next - level.points, v2::TierName(v2::FrameTier(level.tier + 1)));
    else row.words = loc::T(started ? "level.top" : "level.not_played");
    return row;
}

std::vector<Progress> ProgressRows(const PlayerCardView& view) {
    std::vector<Progress> rows;
    if (view.mainFighter >= 0) rows.push_back(ProgressOf(selection::FindFighter(view.mainFighter)->name, view.main, false));
    rows.push_back(ProgressOf(loc::T("card.account"), view.account, true));
    return rows;
}
}

void DrawFighterEdge(ImDrawList* d, v2::Box box, std::uint64_t points, int tier) {
    if (!points) return;
    const float inset = 3 * Scale();
    d->AddRect(ImVec2(box.min.x + inset, box.min.y + inset), ImVec2(box.max.x - inset, box.max.y - inset), v2::TierColor(v2::FrameTier(tier)).stripe, 0, 0, Hairline());
}

player_card::Size CardSize(float bodyHeight) {
    const float s = Scale();
    return player_card::SizeOf(v2::Narrow(ImGui::GetIO().DisplaySize.x, s), bodyHeight / s);
}

namespace {
using player_card::FullCardView;
using player_card::Metrics;

// What one frame of the Full card draws with.
struct Ctx {
    ImDrawList* d;
    const FullCardView& v;
    const Metrics& m;
    FighterArt art;
    float alpha, s;
    const std::vector<MenuEntry>& rows;
    MenuNavigation& nav;
    MenuAction& action;
    bool changed;

    const MenuEntry* Find(const std::string& id) const {
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; });
        return row == rows.end() ? nullptr : &*row;
    }
    // Places the row `id` in `box` (pointer, pad and keys reach it there). False when there is no such row.
    bool Place(const std::string& id, v2::Box box, bool& focused, bool& hovered) const {
        const MenuEntry* row = Find(id);
        if (!row) return false;
        hovered = PressEntry(nav, rows, *row, box, action, changed).hovered;
        focused = row->id == nav.Focus();
        return true;
    }
};

// The row `id` among `rows` as a button in `box`, pressed through PressEntry.
// With `glyph` (a pad button's name) the label goes after that button's glyph.
// False when there is no such row.
bool ButtonRow(const Ctx& c, v2::Box box, const std::string& id, v2::ButtonKind kind, const char* glyph) {
    const MenuEntry* row = c.Find(id);
    if (!row) return false;
    const auto& colors = v2::Colors();
    const float s = c.s;
    bool focused = false, hovered = false;
    c.Place(id, box, focused, hovered);
    if (!glyph) {
        v2::Button(c.d, box, row->label, kind, focused, hovered, row->enabled);
        return true;
    }
    v2::Button(c.d, box, std::string(), kind, focused, hovered, row->enabled);
    const float side = 26 * s, font = 17 * s, whole = side + 10 * s + card_parts::Across(row->label, font, HeadingFont()), left = box.min.x + (box.W() - whole) * .5f;
    DrawPromptGlyph(c.d, glyph, ImVec2(left, box.min.y + (box.H() - side) * .5f), side, MenuArt(), s);
    c.d->AddText(HeadingFont(), font, ImVec2(left + side + 10 * s, box.min.y + (box.H() - font) * .5f), row->enabled ? colors.text : colors.muted, row->label.c_str());
    return true;
}

// The hero's buttons that have a row: how tall they stand with the gap over the first and between.
float ButtonsHeight(const Ctx& c) {
    float height = 0;
    int shown = 0;
    for (const auto& button : c.v.buttons) {
        if (!c.Find(button.row)) continue;
        height += (shown++ ? c.m.buttonGapNext : c.m.buttonGap) * c.s + c.m.button * c.s;
    }
    return height;
}

// The buttons in a column from `y`, the gap over the first. Returns the bottom.
float DrawButtons(const Ctx& c, float x, float y, float width) {
    int shown = 0;
    for (const auto& button : c.v.buttons) {
        if (!c.Find(button.row)) continue;
        y += (shown++ ? c.m.buttonGapNext : c.m.buttonGap) * c.s;
        ButtonRow(c, v2::Box{ImVec2(x, y), ImVec2(x + width, y + c.m.button * c.s)}, button.row, button.kind, button.glyph);
        y += c.m.button * c.s;
    }
    return y;
}

// The player's fighter large, the tier a 1 px line, a shade across its foot with its word and name and no number; with no
// fighter, the ground and the way to choose a main.
void DrawHero(const Ctx& c, v2::Box hero) {
    const auto& colors = v2::Colors();
    const float s = c.s;
    if (c.v.hero < 0) {
        c.d->AddRectFilled(hero.min, hero.max, colors.frame);
        const float w = (std::min)(hero.W() - 32 * s, 280 * s), h = 48 * s, left = hero.min.x + (hero.W() - w) * .5f, top = hero.min.y + (hero.H() - h) * .5f;
        ButtonRow(c, v2::Box{ImVec2(left, top), ImVec2(left + w, top + h)}, ChooseMainRow, v2::ButtonKind::Primary, nullptr);
        return;
    }
    DrawCharacterPortrait(FighterPicture{c.v.hero, c.art, 0, 0}, hero.min, hero.max, colors.frame);
    const v2::Box foot{ImVec2(hero.min.x, hero.max.y - 56 * s), hero.max};
    c.d->AddRectFilled(foot.min, foot.max, v2::color::Alpha(colors.ink, .82f));
    v2::Text(c.d, ImVec2(foot.min.x + 14 * s, foot.min.y + 8 * s), foot.W() - 28 * s, c.v.heroWord, c.m.heroWordText * s, colors.muted, HeadingFont());
    v2::Text(c.d, ImVec2(foot.min.x + 14 * s, foot.min.y + 26 * s), foot.W() - 28 * s, c.v.heroName, c.m.heroNameText * s, colors.text, HeadingFont());
    DrawFighterEdge(c.d, hero, c.v.heroPoints, c.v.heroTier);
}

// The title row (the worn title on its plate, 12 px after it the worn icon on its tier's plate; left out with neither), the
// name with the flag before it and the class emblem after, the line about the player and, for a friend, where they are.
// Returns the y under them.
float DrawIdentity(const Ctx& c, float x, float y, float room) {
    const auto& colors = v2::Colors();
    const float s = c.s, line = c.m.titleLine * s;
    const auto& worn = c.v.worn;
    if (worn.Any()) {
        float at = x;
        if (!worn.title.empty()) {
            NoteUserText(worn.title);
            at += v2::TitlePlate(c.d, ImVec2(x, y), room - (worn.icon >= 0 ? line + 12 * s : 0), line, worn.title, worn.titleTier) + 12 * s;
        }
        if (worn.icon >= 0) card_parts::DrawIconPlate(c.d, ImVec2(at, y), line, line * .64f, worn.icon, worn.iconTier);
        y += line + c.m.titleGap * s;
    }
    const float nameRow = c.m.nameRow * s, nameSize = c.m.nameText * s;
    card_parts::DrawPlayerLine(c.d, x, y + (nameRow - nameSize) * .5f, room, nameSize, c.m.emblem * s, std::string(), c.v.name, c.v.country, c.v.badge);
    y += nameRow;
    if (!c.v.about.empty()) { v2::Text(c.d, ImVec2(x, y), room, c.v.about, c.m.aboutText * s, colors.muted); y += c.m.aboutRow * s; }
    if (c.v.presenceMark != player_card::PresenceMark::None) {
        card_parts::DrawPresence(c.d, ImVec2(x + 6 * s, y + 11 * s), 6 * s, c.v.presenceMark);
        v2::Text(c.d, ImVec2(x + 20 * s, y), room - 20 * s, c.v.presence, c.m.aboutText * s, colors.text);
        y += c.m.aboutRow * s;
    }
    return y;
}

// Plays and Looking for: on one line, the second wrapping, or each on its own line.
float DrawChips(const Ctx& c, float x, float y, float room, bool stacked) {
    if (c.v.plays.empty() && c.v.looking.empty()) return y;
    const float row = c.m.chipRow * c.s;
    return y + (stacked ? card_parts::DrawGroupsStacked(c.d, ImVec2(x, y), room, row, c.m.groupText, 6 * c.s, c.v.plays, c.v.looking)
                        : card_parts::DrawGroupsTogether(c.d, ImVec2(x, y), room, row, c.m.groupText, c.m.groupGap * c.s, 4 * c.s, c.v.plays, c.v.looking));
}

// FP and BP, the SF4 line left-aligned under the title: on the player's own card each with its meter and the words after it
// (narrow: under the line, across the width). Returns the y under them.
float DrawPoints(const Ctx& c, float x, float y, float room, bool narrow) {
    const auto& colors = v2::Colors();
    const auto& points = c.v.points;
    if (!points.shown) return y;
    const float s = c.s, digits = c.m.digits * s, gap = c.m.pointsGap * s, words = c.m.wordsText * s;
    if (!points.hero.meter) return y + v2::LevelLines(c.d, ImVec2(x, y), digits, gap, points.hero.points, points.account.points, c.alpha);
    const struct { const player_card::PointsLine* line; const char* label; } lines[2] = {{&points.hero, "points.label"}, {&points.account, "points.account_label"}};
    if (narrow) {
        for (const auto& one : lines) {
            v2::Level(c.d, ImVec2(x, y), digits, one.line->points, one.label, c.alpha);
            y += digits + 8 * s;
            v2::Meter(c.d, {ImVec2(x, y), ImVec2(x + room, y + 10 * s)}, one.line->part, 0.f);
            y += 10 * s + 6 * s;
            v2::Text(c.d, ImVec2(x, y), room, one.line->words, words, colors.muted);
            y += words + 14 * s;
        }
        return y;
    }
    // One column of numbers and labels, as LevelLines draws it; each line's meter starts 24 px after its label.
    v2::LevelLines(c.d, ImVec2(x, y), digits, gap, points.hero.points, points.account.points, c.alpha);
    const float end = (std::max)(v2::LevelWidth(points.hero.points, digits, "points.label"), v2::LevelWidth(points.account.points, digits, "points.account_label"));
    const float meterX = x + end + 24 * s, meterWidth = (std::max)(40 * s, (std::min)(c.m.meter * s, x + room - meterX - 16 * s - 120 * s));
    for (int i = 0; i < 2; ++i) {
        const float top = y + i * (digits + gap), mid = top + (digits - 10 * s) * .5f, wordsX = meterX + meterWidth + 16 * s;
        v2::Meter(c.d, {ImVec2(meterX, mid), ImVec2(meterX + meterWidth, mid + 10 * s)}, lines[i].line->part, 0.f);
        v2::Text(c.d, ImVec2(wordsX, top + (digits - words) * .5f), x + room - wordsX, lines[i].line->words, words, colors.muted);
    }
    return y + 2 * digits + gap;
}

// The words for a points state with nothing to show, and the way to Discord when there is no account. Returns the y under them.
float DrawWords(const Ctx& c, float x, float y, float room) {
    if (c.v.levelWords) y += v2::Paragraph(c.d, ImVec2(x, y), room, loc::T(c.v.levelWords), c.m.noteText * c.s, v2::Colors().muted);
    const float after = DrawRowButton(DiscordConnectScreen, ImVec2(x, y), (std::min)(room, 360 * c.s), c.rows, c.nav, c.action, c.changed);
    return after > y ? after + 8 * c.s : y;
}

// One fighter of the strip: the picture with the tier line, its points on it and "Main" on the main's, and under it the
// name and, on the player's own card, the games.
void DrawStripTile(const Ctx& c, v2::Box box, const player_card::StripTile& tile, float textBlock, bool focused, bool hovered) {
    const auto& colors = v2::Colors();
    const float s = c.s, pad = 8 * s, side = box.W() - 2 * pad;
    v2::CardGround(c.d, box, focused, hovered);
    const v2::Box picture{ImVec2(box.min.x + pad, box.min.y + pad), ImVec2(box.max.x - pad, box.max.y - pad - textBlock)};
    DrawCharacterPortrait(FighterPicture{tile.fighter, c.art, 0, 0}, picture.min, picture.max, colors.frame);
    v2::LevelBand(c.d, picture, tile.points, c.alpha);
    DrawFighterEdge(c.d, picture, tile.points, tile.tier);
    if (tile.isMain) v2::Chip(c.d, ImVec2(picture.min.x + 6 * s, picture.min.y + 6 * s), loc::T("fighters.main"), colors.accentLight);
    const float y = picture.max.y + 8 * s;
    v2::Text(c.d, ImVec2(box.min.x + pad, y), side, selection::FindFighter(tile.fighter)->name, c.m.tileNameText * s, colors.text, HeadingFont());
    if (!tile.games.empty()) v2::Text(c.d, ImVec2(box.min.x + pad, y + c.m.tileNameText * s + 5 * s), side, tile.games, c.m.tileGamesText * s, colors.muted);
    if (focused) v2::Focus(c.d, box.min, box.max);
}

// "TOP FIGHTERS" and the tiles: wide, as tall as the column leaves down to `limit`; narrow, three across the width with
// square pictures. Returns the y under them.
float DrawStrip(const Ctx& c, float x, float y, float room, float limit, bool narrow) {
    if (c.v.strip.empty()) return y;
    const float s = c.s, between = c.m.tileGap * s;
    v2::SectionHeading(c.d, ImVec2(x, y), loc::T("card.top_fighters"));
    y += c.m.heading * s;
    const float width = narrow ? (room - 2 * between) / 3 : c.m.tileWidth * s;
    const bool games = !c.v.strip.front().games.empty();
    const float textBlock = 8 * s + c.m.tileNameText * s + (games ? 5 * s + c.m.tileGamesText * s : 0) + 8 * s;
    const float height = narrow ? width + textBlock : (std::max)(textBlock + 60 * s, limit - y);
    int shown = 0;
    for (const auto& tile : c.v.strip) {
        const float left = x + shown++ * (width + between);
        const v2::Box box{ImVec2(left, y), ImVec2(left + width, y + height)};
        bool focused = false, hovered = false;
        if (!c.Place(tile.row, box, focused, hovered)) continue;
        DrawStripTile(c, box, tile, textBlock, focused, hovered);
    }
    return y + height;
}

// "AWARDS" with "28 of 40" at its right, and up to four families: wide at their own size, narrow sharing the width. With none, the
// points state's words or the words for no award yet. Left out for a card without profiles.
float DrawAwards(const Ctx& c, float x, float y, float room, bool narrow) {
    const auto& colors = v2::Colors();
    const auto& awards = c.v.awards;
    if (awards.total == 0) return y;
    const float s = c.s;
    v2::SectionHeading(c.d, ImVec2(x, y), loc::T("card.awards"));
    const std::string count = loc::Tf("card.of", awards.earned, awards.total);
    const float countWidth = card_parts::Across(count, 14 * s);
    v2::Text(c.d, ImVec2(x + room - countWidth, y), countWidth + 1, count, 14 * s, colors.muted);
    y += c.m.heading * s;
    if (awards.tiles.empty()) {
        const char* words = c.v.levelWords && c.v.levels != CardLevels::Counted ? c.v.levelWords : "card.no_awards";
        return y + v2::Paragraph(c.d, ImVec2(x, y), room, loc::T(words), c.m.noteText * s, colors.muted);
    }
    const float gap = c.m.familyGap * s, width = narrow ? (room - 3 * gap) / 4 : c.m.familyWidth * s, height = c.m.familyHeight * s;
    int shown = 0;
    for (const auto& tile : awards.tiles) {
        const float left = x + shown++ * (width + gap);
        const v2::Box box{ImVec2(left, y), ImVec2(left + width, y + height)};
        bool focused = false, hovered = false;
        if (!c.Place(player_card::CardAwardRow(tile.family->id), box, focused, hovered)) { card_parts::DrawFamilyTile(c.d, box, tile, c.m, false, false); continue; }
        card_parts::DrawFamilyTile(c.d, box, tile, c.m, focused, hovered);
    }
    return y + height;
}

// Where the player's own card names the most played opponent, and another's the record between you.
float DrawRival(const Ctx& c, float x, float y, float room) {
    using Kind = player_card::Rival::Kind;
    const auto& rival = c.v.rival;
    const float s = c.s;
    if (rival.kind == Kind::Versus) return y + v2::FactCells(c.d, ImVec2(x, y), room, rival.cells, 3, c.m.recordValueText);
    if (rival.kind == Kind::None) return y;
    v2::SectionHeading(c.d, ImVec2(x, y), loc::T("card.most_played"));
    y += c.m.heading * s;
    if (rival.kind == Kind::MostPlayedNone) return y + v2::Paragraph(c.d, ImVec2(x, y), room, loc::T("card.most_played_none"), c.m.noteText * s, v2::Colors().muted);
    const v2::Box box{ImVec2(x, y), ImVec2(x + room, y + c.m.rowHeight * s)};
    bool focused = false, hovered = false;
    c.Place(player_card::MostPlayedRow, box, focused, hovered);
    card_parts::DrawRivalRow(c.d, box, rival, c.m, focused, hovered);
    return box.max.y;
}

// "RECENT SETS": the rows the size shows, or the words for why there are none, and All sets under them. Returns the y under them.
float DrawSets(const Ctx& c, float x, float y, float room) {
    using State = player_card::SetsView::State;
    const auto& sets = c.v.sets;
    const float s = c.s, rowHeight = c.m.rowHeight * s, rowGap = c.m.rowGap * s;
    v2::SectionHeading(c.d, ImVec2(x, y), loc::T("card.recent_sets"));
    y += c.m.heading * s;
    const auto note = [&](const char* id) { y += v2::Paragraph(c.d, ImVec2(x, y), room, loc::T(id), c.m.noteText * s, v2::Colors().muted) + rowGap; };
    if (sets.state == State::Reading) {
        for (int i = 0; i < c.v.setRows; ++i) {
            card_parts::DrawReadingRow(c.d, v2::Box{ImVec2(x, y), ImVec2(x + room, y + rowHeight)}, i == 0 ? std::string(loc::T("card.sets_loading")) : std::string(), c.m.rowLineText);
            y += rowHeight + rowGap;
        }
    } else if (sets.state == State::Rows) {
        for (std::size_t i = 0; i < sets.rows.size() && i < static_cast<std::size_t>(c.v.setRows); ++i) {
            const v2::Box box{ImVec2(x, y), ImVec2(x + room, y + rowHeight)};
            bool focused = false, hovered = false;
            c.Place(sets.rows[i].row, box, focused, hovered);
            card_parts::DrawSetRow(c.d, box, sets.rows[i], c.art, c.m, focused, hovered);
            y += rowHeight + rowGap;
        }
    } else if (sets.state == State::None) note("card.sets_none");
    else if (sets.state == State::Unavailable) note("card.sets_unavailable");
    else note("card.sets_unreachable");
    if (sets.allSets) {
        const v2::Box box{ImVec2(x, y), ImVec2(x + room, y + c.m.linkRow * s)};
        bool focused = false, hovered = false;
        c.Place(player_card::AllSetsRow, box, focused, hovered);
        card_parts::DrawLinkRow(c.d, box, loc::T("card.all_sets"), 16, focused, hovered);
        y = box.max.y;
    }
    return y;
}

// Record, then the points state's words and the way to Discord. Returns the y under them.
float DrawRecord(const Ctx& c, float x, float y, float room, std::size_t across) {
    if (c.v.record.empty()) return y;
    return y + v2::FactCells(c.d, ImVec2(x, y), room, c.v.record, across, c.m.recordValueText);
}

// Wide and compact: the hero with its buttons at its foot, the middle column and the right column, each to the body's foot.
float DrawColumns(const Ctx& c, ImVec2 origin, float width, float height) {
    const float s = c.s, bottom = origin.y + height - s, gutter = c.m.gutter * s, gap = c.m.buttonGap * s;
    const float hero = c.m.hero * s, right = c.m.right * s, middle = (std::max)(0.f, (std::min)(c.m.middle * s, width - hero - right - 2 * gutter));
    const v2::Box heroBox{origin, ImVec2(origin.x + hero, bottom - ButtonsHeight(c))};
    DrawHero(c, heroBox);
    DrawButtons(c, origin.x, heroBox.max.y, hero);

    float x = origin.x + hero + gutter, y = origin.y;
    y = DrawIdentity(c, x, y, middle) + gap * .75f;
    if (!c.v.plays.empty() || !c.v.looking.empty()) y = DrawChips(c, x, y, middle, false) + gap;
    if (c.v.points.shown) y = DrawPoints(c, x, y, middle, false) + gap;
    if (!c.v.record.empty()) y = DrawRecord(c, x, y, middle, 4) + gap * 1.1f;
    y = DrawWords(c, x, y, middle);
    DrawStrip(c, x, y, middle, bottom, false);

    x += middle + gutter;
    y = origin.y;
    const float section = c.m.heading * s;
    const float afterAwards = DrawAwards(c, x, y, right, false);
    y = afterAwards > y ? afterAwards + section : y;
    const float afterRival = DrawRival(c, x, y, right);
    y = afterRival > y ? afterRival + gap : y;
    DrawSets(c, x, y, right);
    return height;
}

// Narrow: one column that scrolls. The band (the hero and, beside it or under it, the title row, name and about), another's
// buttons, the chips, points, record, most played or the record between you, the strip, the awards, three recent sets, and the
// player's own buttons at the end.
float DrawNarrow(const Ctx& c, ImVec2 origin, float width) {
    const float s = c.s, gap = 16 * s;
    const bool beside = width >= 560 * s, buttonsFirst = !c.v.buttons.empty() && c.v.buttons.front().kind == v2::ButtonKind::Primary;
    const v2::Box hero{origin, ImVec2(origin.x + c.m.hero * s, origin.y + 260 * s)};
    DrawHero(c, hero);
    float y = hero.max.y;
    if (beside) y = (std::max)(y, DrawIdentity(c, hero.max.x + 16 * s, origin.y, width - hero.W() - 16 * s));
    else y = DrawIdentity(c, origin.x, y + 12 * s, width);
    y += gap;
    if (buttonsFirst) y = DrawButtons(c, origin.x, y - c.m.buttonGap * s, width) + gap;
    if (!c.v.plays.empty() || !c.v.looking.empty()) y = DrawChips(c, origin.x, y, width, true) + gap;
    if (c.v.points.shown) y = DrawPoints(c, origin.x, y, width, true) + gap * .5f;
    if (!c.v.record.empty()) y = DrawRecord(c, origin.x, y, width, 2) + gap;
    y = DrawWords(c, origin.x, y, width);
    const float afterRival = DrawRival(c, origin.x, y, width);
    y = afterRival > y ? afterRival + gap : y;
    const float afterStrip = DrawStrip(c, origin.x, y, width, 0, true);
    y = afterStrip > y ? afterStrip + gap : y;
    const float afterAwards = DrawAwards(c, origin.x, y, width, true);
    y = afterAwards > y ? afterAwards + gap : y;
    y = DrawSets(c, origin.x, y, width) + gap * .5f;
    if (!buttonsFirst) y = DrawButtons(c, origin.x, y - c.m.buttonGap * s, width) + gap;
    return y - origin.y;
}
}

float DrawFullCard(const player_card::FullCardView& card, FighterArt art, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action,
    bool changed, float bodyHeight) {
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x;
    const auto size = CardSize(bodyHeight);
    const Ctx c{ImGui::GetWindowDrawList(), card, player_card::MetricsOf(size), art, card.levels == CardLevels::Pending ? .4f : 1.f, s, rows, nav, action, changed};
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float total = size == player_card::Size::Narrow ? DrawNarrow(c, origin, width) : DrawColumns(c, origin, width, bodyHeight);
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, total));
    return total;
}

// Connect Discord, the way to points and friends a player without an account has.
MenuEntry ConnectDiscordRow() {
    auto connect = Row(DiscordConnectScreen, loc::T("screen.connect_discord"), loc::T("rankings.connect_detail"));
    connect.value = loc::T("identity.discord_none");
    connect.hint = loc::T("screen.connect_discord");
    return connect;
}

float DrawRowButton(const char* id, ImVec2 at, float width, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed) {
    const float s = Scale();
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; });
    if (row == rows.end()) return at.y;
    const v2::Box box{ImVec2(at.x, at.y + 8 * s), ImVec2(at.x + width, at.y + 8 * s + 48 * s)};
    const bool hovered = PressEntry(nav, rows, *row, box, action, changed).hovered;
    v2::Button(ImGui::GetWindowDrawList(), box, row->label, v2::ButtonKind::Primary, row->id == nav.Focus(), hovered, row->enabled);
    return box.max.y;
}

float DrawNameLine(ImDrawList* d, float x, float y, float room, float row, float size, const std::string& name, CountryCode country, const SkillBadge& badge) {
    card_parts::DrawPlayerLine(d, x, y + (row - size) * .5f, room, size, size * 24.f / 22.f, std::string(), name, country, badge);
    return row;
}

namespace {
// The Panel drawn (`d`) or only measured (null): its height from `origin` across `width`.
float LayoutPanel(ImDrawList* d, ImVec2 origin, float width, const OtherCardView& card) {
    const auto& c = v2::Colors();
    const float s = Scale();
    const bool compact = ImGui::GetIO().DisplaySize.y / s <= 720;
    const float side = (compact ? 80 : 112) * s, gap = 16 * s, digits = (compact ? 22 : 26) * s, row = 28 * s;
    if (d) {
        const v2::Box tile{origin, ImVec2(origin.x + side, origin.y + side)};
        if (card.fighter >= 0) DrawCharacterPortrait(FighterPicture{card.fighter, card.art, 0, 0}, tile.min, tile.max, c.frame);
        else d->AddRectFilled(tile.min, tile.max, c.frame);
        DrawFighterEdge(d, tile, card.points ? card.fighterPoints : 0, card.fighterTier);
    }
    // How the viewer stands with the player is a chip at the card's top right.
    const float x = origin.x + side + gap, right = origin.x + width, chip = card.chip.empty() ? 0 : v2::ChipWidth(card.chip);
    float y = origin.y;
    if (d) {
        DrawNameLine(d, x, y, right - x - (chip > 0 ? chip + 12 * s : 0), 30 * s, 22 * s, card.name, card.country, card.badge);
        if (chip > 0) v2::Chip(d, ImVec2(right - chip, origin.y), card.chip, card.chipTone);
    }
    y += 30 * s;
    if (!card.title.empty() || card.icon >= 0) {
        y += 8 * s;
        float plate = 0;
        if (d && !card.title.empty()) {
            NoteUserText(card.title);
            plate = v2::TitlePlate(d, ImVec2(x, y), right - x - (card.icon >= 0 ? row + 8 * s : 0), row, card.title, card.titleTier);
        }
        if (d && card.icon >= 0) card_parts::DrawIconPlate(d, ImVec2(x + (plate > 0 ? plate + 8 * s : 0), y), row, row * .64f, card.icon, card.iconTier);
        y += row;
    }
    // FP and BP under the plate, left-aligned with it.
    if (card.points) {
        y += 8 * s;
        if (card.accountKnown) {
            if (d) v2::LevelLines(d, ImVec2(x, y), digits, 8 * s, card.fighterPoints, card.accountPoints);
            y += 2 * digits + 8 * s;
        } else {
            if (d) v2::Level(d, ImVec2(x, y), digits, card.fighterPoints, "points.label");
            y += digits;
        }
    }
    y = (std::max)(y, origin.y + side);
    if (!card.record.empty()) {
        y += 12 * s;
        if (d) v2::Text(d, ImVec2(origin.x, y), width, card.record, 14 * s, c.muted);
        y += 20 * s;
    }
    if (!card.plays.empty() || !card.looking.empty()) {
        y += 10 * s;
        const float groups = d ? card_parts::DrawGroupsStacked(d, ImVec2(origin.x, y), width, row, player_card::TextFloor, 8 * s, card.plays, card.looking) :
            card_parts::GroupsStackedHeight(width, row, player_card::TextFloor, 8 * s, card.plays, card.looking);
        y += groups;
    }
    if (card.awards.total > 0) {
        y += 10 * s;
        const float icon = (compact ? 28 : 32) * s;
        float at = origin.x;
        for (const auto& tile : card.awards.tiles) {
            if (d) card_parts::DrawIconPlate(d, ImVec2(at, y), icon, icon * .62f, tile.family->icon, tile.tier);
            at += icon + 8 * s;
        }
        if (d) {
            const std::string count = loc::Tf("card.of", card.awards.earned, card.awards.total);
            v2::Text(d, ImVec2(at + (card.awards.tiles.empty() ? 0 : -2 * s), y + (icon - 14 * s) * .5f), right - at, count, 14 * s, c.muted);
        }
        y += icon;
    }
    return y - origin.y;
}
}

float OtherCardHeight(const OtherCardView& card, float width, bool ground) {
    const float pad = ground ? 14 * Scale() : 0;
    return LayoutPanel(nullptr, ImVec2(0, 0), width - 2 * pad, card) + 2 * pad;
}

float DrawOtherCard(ImDrawList* d, v2::Box box, const OtherCardView& card, bool ground) {
    const auto& c = v2::Colors();
    const float s = Scale(), pad = ground ? 14 * s : 0, height = OtherCardHeight(card, box.W(), ground);
    if (ground) {
        d->AddRectFilled(box.min, ImVec2(box.max.x, box.min.y + height), v2::PanelGround(), 4 * s);
        d->AddRect(box.min, ImVec2(box.max.x, box.min.y + height), c.border, 4 * s, 0, s);
    }
    LayoutPanel(d, ImVec2(box.min.x + pad, box.min.y + pad), box.W() - 2 * pad, card);
    return height;
}

OtherCardView OwnAsOthers(const PlayerCardView& card) {
    OtherCardView other;
    other.name = card.name;
    other.country = card.country;
    other.badge = card.profile.marks.badge;
    other.title = card.profile.title;
    other.titleTier = card.profile.tier;
    other.fighter = card.mainFighter;
    other.fighterPoints = card.main.points;
    other.fighterTier = card.main.tier;
    other.accountPoints = card.account.points;
    return other;
}

OtherCardView PanelOf(const player_card::Subject& subject, FighterArt art) {
    OtherCardView panel;
    panel.name = subject.name;
    panel.country = subject.country;
    panel.badge = subject.badge;
    panel.fighter = subject.fighter >= 0 ? subject.fighter : subject.main;
    panel.art = art;
    panel.points = subject.levels == CardLevels::Counted;
    panel.fighterPoints = subject.hero.points;
    panel.fighterTier = subject.hero.tier;
    panel.accountPoints = subject.account.points;
    if (!subject.profiled) return panel;
    // The worn title and icon and the shown families are drawn at the level the card's counters earn, never one claimed.
    const auto worn = player_card::WornOnCard(subject.counters, subject.worn);
    panel.title = worn.title;
    panel.titleTier = worn.titleTier;
    panel.icon = worn.icon;
    panel.iconTier = worn.iconTier;
    if (panel.points) panel.record = player_card::RecordLine(subject.counters);
    panel.plays = player_card::PlaysChips(subject.style);
    panel.looking = player_card::LookingChips(subject.style);
    panel.awards = player_card::AwardsOf(subject.counters, subject.pins, subject.friendsCounted);
    return panel;
}
} }
