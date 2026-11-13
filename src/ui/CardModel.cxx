#include "CardModel.hxx"
#include "CountryFlag.hxx"
#include "NameMarks.hxx"
#include "PlayerCard.hxx"
#include "../common/LocalTime.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cstddef>
#include <cstdio>

namespace sf4e { namespace ui { namespace player_card {
namespace aw = netplay::awards;
namespace profiles = netplay::profiles;

namespace {
const char* const kEnDash = " \xE2\x80\x93 ";
const char* const kDot = " \xC2\xB7 ";

// The year and month of a unix time in UTC, "2026-03": a card says the month a player came, and the bridge
// serves the first second of it in UTC, so it is not read in this PC's zone.
std::string MonthOf(std::uint64_t seconds) {
    const std::int64_t days = static_cast<std::int64_t>(seconds / 86400) + 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t dayOfEra = days - era * 146097;
    const std::int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const std::int64_t shifted = (5 * dayOfYear + 2) / 153;
    const std::int64_t month = shifted < 10 ? shifted + 3 : shifted - 9;
    const std::int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02d", static_cast<int>(year), static_cast<int>(month));
    return text;
}

constexpr Metrics kWide = {
    440, 32, 840, 496,
    34, 10, 44, 36, 22, 15, 32,
    30, 13, 24,
    30, 320, 13, 8,
    13, 17,
    24, 264, 12, 15, 13,
    112, 160, 16, 64, 44, 7, 3, 13,
    72, 8, 48, 16, 14, 48,
    48, 16, 8,
    13, 18, 14,
    5};
constexpr Metrics kCompact = {
    260, 24, 524, 400,
    30, 6, 40, 30, 20, 14, 28,
    28, 13, 24,
    24, 150, 13, 8,
    13, 16,
    22, 160, 12, 14, 13,
    88, 120, 16, 48, 32, 6, 3, 13,
    52, 6, 40, 14, 13, 40,
    44, 12, 8,
    13, 18, 14,
    3};
constexpr Metrics kNarrow = {
    200, 16, 0, 0,
    30, 6, 40, 30, 20, 14, 28,
    28, 13, 24,
    22, 0, 13, 8,
    13, 16,
    22, 0, 12, 14, 13,
    0, 128, 12, 48, 32, 6, 3, 13,
    56, 6, 40, 14, 13, 44,
    48, 12, 8,
    13, 18, 14,
    3};
}

const Metrics& MetricsOf(Size size) { return size == Size::Wide ? kWide : size == Size::Compact ? kCompact : kNarrow; }

float Metrics::SmallestText() const {
    return (std::min)({nameText, aboutText, groupText, wordsText, recordKeyText, recordValueText, tileNameText, tileGamesText, familyTitleText,
        rowNameText, rowLineText, heroWordText, heroNameText, noteText});
}

std::vector<Chip> PlaysChips(const profiles::Style& style) {
    std::vector<Chip> chips;
    switch (style.device) {
    case profiles::Device::Pad: chips.push_back({Chip::Kind::Device, loc::T("style.pad")}); break;
    case profiles::Device::Stick: chips.push_back({Chip::Kind::Device, loc::T("style.stick")}); break;
    case profiles::Device::Leverless: chips.push_back({Chip::Kind::Device, loc::T("style.leverless")}); break;
    case profiles::Device::Keyboard: chips.push_back({Chip::Kind::Device, loc::T("style.keyboard")}); break;
    case profiles::Device::None: break;
    }
    if (style.link == profiles::Link::Wired) chips.push_back({Chip::Kind::Link, loc::T("style.wired")});
    else if (style.link == profiles::Link::WiFi) chips.push_back({Chip::Kind::Link, loc::T("style.wifi")});
    if (!style.region.empty()) {
        const char* name = style.region == "use1" ? loc::T("network.region_use1") : style.region == "usw1" ? loc::T("network.region_usw1") :
            style.region == "euc1" ? loc::T("network.region_euc1") : style.region == "aps1" ? loc::T("network.region_aps1") : loc::T("network.region_other");
        chips.push_back({Chip::Kind::Region, name});
    }
    return chips;
}

std::vector<Chip> LookingChips(const profiles::Style& style) {
    std::vector<Chip> chips;
    const char* const words[profiles::LookingKinds] = {loc::T("style.casuals"), loc::T("style.long_sets"), loc::T("style.ranked"),
        loc::T("style.practice"), loc::T("style.new"), loc::T("style.teach")};
    for (std::size_t i = 0; i < profiles::LookingKinds; ++i)
        if (style.looking & profiles::LookingBit[i]) chips.push_back({Chip::Kind::Looking, words[i]});
    return chips;
}

std::vector<Chip> GiveWay(std::vector<Chip> plays, const Fits& fits) {
    for (const auto kind : {Chip::Kind::Region, Chip::Kind::Link}) {
        if (fits(plays)) break;
        plays.erase(std::remove_if(plays.begin(), plays.end(), [&](const Chip& chip) { return chip.kind == kind; }), plays.end());
    }
    return plays;
}

std::string LevelTitle(const aw::Level& level) {
    return level.band ? loc::Tf("class.tip", ClassName(static_cast<std::uint8_t>(level.band))) : std::string(loc::T(level.title));
}

namespace {
FamilyTile TileOf(const aw::Family& family, int level) {
    FamilyTile tile;
    tile.family = &family;
    tile.level = level;
    tile.tier = v2::FrameTier(aw::LevelAt(family, level)->tier);
    tile.title = LevelTitle(*aw::LevelAt(family, level));
    return tile;
}
}

std::vector<FamilyTile> ShownFamilies(const aw::Counters& counters, const std::vector<std::string>& pins, std::size_t most) {
    std::vector<FamilyTile> shown;
    for (const auto& pin : pins) {
        if (shown.size() >= most) break;
        const aw::Family* family = aw::Find(pin);
        if (!family || std::any_of(shown.begin(), shown.end(), [&](const FamilyTile& tile) { return tile.family == family; })) continue;
        if (const int level = aw::LevelOf(counters, *family)) shown.push_back(TileOf(*family, level));
    }
    std::vector<FamilyTile> rest;
    for (const auto& family : aw::Catalog) {
        const int level = aw::LevelOf(counters, family);
        if (level > 0 && std::none_of(shown.begin(), shown.end(), [&](const FamilyTile& tile) { return tile.family == &family; })) rest.push_back(TileOf(family, level));
    }
    // Highest tier first, then how far along the family's levels it is, then the catalog's order (the sort is stable).
    std::stable_sort(rest.begin(), rest.end(), [](const FamilyTile& a, const FamilyTile& b) {
        if (a.tier != b.tier) return a.tier > b.tier;
        return a.level * static_cast<int>(b.family->count) > b.level * static_cast<int>(a.family->count);
    });
    for (auto& tile : rest) {
        if (shown.size() >= most) break;
        shown.push_back(std::move(tile));
    }
    return shown;
}

WornView WornOnCard(const aw::Counters& counters, const profiles::Worn& worn) {
    WornView view;
    if (const auto title = aw::WornOf(counters, worn.title)) {
        view.title = LevelTitle(title.Current());
        view.titleTier = v2::FrameTier(title.Current().tier);
    }
    if (const auto icon = aw::WornOf(counters, worn.icon)) {
        view.icon = icon.family->icon;
        view.iconTier = v2::FrameTier(icon.Current().tier);
        view.family = icon.family->id;
    }
    return view;
}

AwardsView AwardsOf(const aw::Counters& counters, const std::vector<std::string>& pins, bool friends) {
    AwardsView view;
    aw::Counters seen = counters;
    if (!friends) seen.friends = 0;
    view.tiles = ShownFamilies(seen, pins);
    view.earned = aw::LevelsEarned(seen);
    view.total = static_cast<int>(aw::LevelTotal());
    if (!friends)
        if (const auto* family = aw::Find("friends")) view.total -= static_cast<int>(family->count);
    return view;
}

PointsLine MeterLine(const netplay::levels::Level& level, bool account) {
    PointsLine line;
    line.points = level.points;
    line.meter = true;
    line.part = Part(level);
    if (level.points == 0 && !account) {
        line.words = loc::T("level.not_played");
        return line;
    }
    const std::string tier = v2::TierName(v2::FrameTier(level.tier));
    line.words = level.next > level.points ? loc::Tf("card.tier_to", tier, level.next - level.points, v2::TierName(v2::FrameTier(level.tier + 1))) : loc::Tf("card.tier_top", tier);
    return line;
}

std::vector<v2::TagView::Fact> RecordCells(const aw::Counters& c, bool winRate) {
    const std::uint64_t percent = c.games ? (c.wins * 100 + c.games / 2) / c.games : 0;
    return {{loc::T("card.counted_games"), std::to_string(c.games)},
        winRate ? v2::TagView::Fact{loc::T("card.win_rate"), std::to_string(percent) + "%"} : v2::TagView::Fact{loc::T("card.wins"), std::to_string(c.wins)},
        {loc::T("card.best_streak"), std::to_string(c.bestStreak)},
        {loc::T("card.fighters_played"), loc::Tf("card.of", c.fightersPlayed, selection::FighterCount)}};
}

std::string RecordLine(const aw::Counters& c) { return loc::Tf("card.record_line", c.games, c.wins, c.bestStreak); }

std::vector<int> TopFighters(const std::vector<int>& byPoints, int main, std::size_t most) {
    std::vector<int> top;
    if (main >= 0 && main < selection::FighterCount) top.push_back(main);
    for (const int fighter : byPoints) {
        if (top.size() >= most) break;
        if (fighter != main) top.push_back(fighter);
    }
    return top;
}

std::string WhenWords(std::uint64_t now, std::uint64_t at) {
    if (!at) return {};
    const std::uint64_t seconds = now > at ? now - at : 0;
    if (now && seconds < 24 * 3600) return loc::Tf("friends.hours_ago", (std::max<std::uint64_t>)(1, seconds / 3600));
    if (now && seconds < 3 * 24 * 3600) return loc::Tf("friends.days_ago", seconds / (24 * 3600));
    return LocalTime(at, "%Y-%m-%d");
}

namespace {
const char* LevelWordsOf(CardLevels levels, bool played, bool detailed) {
    switch (levels) {
    case CardLevels::Pending: return "level.loading";
    case CardLevels::NoAccount: return "level.no_account";
    case CardLevels::Unavailable: return "level.unavailable";
    case CardLevels::Unreachable: return "level.unreachable";
    case CardLevels::Counted: return played || !detailed ? nullptr : "level.none";
    }
    return nullptr;
}

std::string About(const Subject& s) {
    const bool country = s.country.Present(), since = s.profiled && s.since;
    if (country && since) return loc::Tf("card.about", CountryName(s.country), MonthOf(s.since));
    if (country) return CountryName(s.country);
    if (since) return loc::Tf("card.since", MonthOf(s.since));
    return {};
}

std::string RelationWords(const Subject& s) {
    using netplay::levels::Relation;
    switch (s.relation) {
    case Relation::Friend: return s.friendSince ? loc::Tf("card.since_date", LocalTime(s.friendSince, "%Y-%m-%d")) : std::string(loc::T("card.relation"));
    case Relation::Sent: return loc::T("friends.request_sent");
    case Relation::Received: return loc::T("card.they_asked");
    case Relation::Blocked: return loc::T("friends.blocked_member");
    case Relation::None: break;
    }
    return loc::T("card.not_friends");
}
}

FullCardView FullOf(const Subject& s, Size size) {
    const Metrics& m = MetricsOf(size);
    FullCardView v;
    v.size = size;
    v.levels = s.levels;
    v.name = s.name;
    v.country = s.country;
    v.badge = s.badge;
    v.about = About(s);
    v.presenceMark = s.presenceMark;
    v.presence = s.presence;
    v.setRows = m.setRows;
    v.sets = s.sets;
    v.buttons = s.buttons;
    v.hero = s.fighter >= 0 ? s.fighter : s.main;
    if (const auto* fighter = selection::FindFighter(v.hero)) v.heroName = fighter->name;
    else v.hero = -1;
    v.heroWord = loc::T(s.playing ? "card.playing" : "profile.main_badge");
    v.heroPoints = s.hero.points;
    v.heroTier = s.hero.tier;
    if (s.profiled) {
        v.worn = WornOnCard(s.counters, s.worn);
        v.plays = PlaysChips(s.style);
        v.looking = LookingChips(s.style);
        v.record = RecordCells(s.counters, s.detailed);
        v.awards = AwardsOf(s.counters, s.pins, s.friendsCounted);
    }
    const bool counted = s.levels == CardLevels::Counted;
    v.points.shown = counted;
    if (counted) {
        v.points.hero = s.detailed ? MeterLine(s.hero, false) : PointsLine{s.hero.points, false, 0, std::string()};
        v.points.account = s.detailed ? MeterLine(s.account, true) : PointsLine{s.account.points, false, 0, std::string()};
    }
    std::vector<int> byPoints;
    for (const auto& played : s.played) byPoints.push_back(played.fighter);
    for (const int fighter : TopFighters(byPoints, s.main)) {
        StripTile tile;
        tile.fighter = fighter;
        tile.isMain = fighter == s.main;
        tile.row = CardFighterRow(fighter);
        for (const auto& played : s.played)
            if (played.fighter == fighter) { tile.points = played.points; tile.tier = played.tier; if (s.detailed) tile.games = GamesWords(played.games); }
        if (s.detailed && tile.games.empty()) tile.games = GamesWords(0);
        v.strip.push_back(std::move(tile));
    }
    if (s.detailed) {
        if (s.hasMostPlayed) {
            v.rival.kind = Rival::Kind::MostPlayed;
            v.rival.player = s.mostPlayed;
            v.rival.line = loc::Tf("card.most_played_line", s.mostPlayed.won, s.mostPlayed.lost, s.mostPlayed.won + s.mostPlayed.lost);
        } else if (counted && s.counters.games == 0) v.rival.kind = Rival::Kind::MostPlayedNone;
    } else if (s.versus && s.profiled) {
        v.rival.kind = Rival::Kind::Versus;
        v.rival.cells = {{loc::Tf("card.you_and", s.name), s.hasRecord ? std::to_string(s.record.won) + kEnDash + std::to_string(s.record.lost) : std::string("-")},
            {loc::T("friends.last_played"), s.hasRecord && s.record.lastAt ? WhenWords(s.now, s.record.lastAt) : std::string("-")},
            {loc::T("card.relation"), RelationWords(s)}};
    }
    v.levelWords = LevelWordsOf(s.levels, !s.played.empty(), s.detailed);
    v.notes = s.notes;
    return v;
}

Subject OwnSubject(const OwnInputs& in) {
    Subject s;
    s.detailed = true;
    s.name = in.name;
    s.country = in.country;
    s.badge = in.badge;
    s.since = in.since;
    s.main = s.fighter = in.main >= 0 && in.main < selection::FighterCount ? in.main : -1;
    s.worn = in.worn;
    s.pins = in.pins;
    s.style = in.style;
    s.friendsCounted = true;
    static const netplay::levels::Own none{};
    const auto* answer = in.levels;
    if (!answer || !answer->loaded) {
        s.levels = answer && !answer->loading && !answer->error.empty() ? CardLevels::Unreachable : CardLevels::Pending;
        s.counters = aw::CountersOf(none, in.friends);
        return s;
    }
    const auto& own = answer->value;
    s.levels = own.kind == netplay::levels::Own::Kind::NoAccount ? CardLevels::NoAccount : own.kind == netplay::levels::Own::Kind::Counted ? CardLevels::Counted : CardLevels::Unavailable;
    s.counters = aw::CountersOf(own, in.friends);
    if (s.levels != CardLevels::Counted) return s;
    s.account = own.account;
    if (s.main >= 0) s.hero = own.fighters[s.main];
    for (const int fighter : PlayedFighters(own)) s.played.push_back({fighter, own.fighters[fighter].points, own.fighters[fighter].tier, own.fighters[fighter].games});
    s.hasMostPlayed = own.hasMostPlayed;
    s.mostPlayed = own.mostPlayed;
    return s;
}

Subject CardSubject(const netplay::levels::Card& card, const profiles::Known& known, int fighter, bool playing, bool profiled) {
    Subject s;
    s.name = card.name.empty() ? known.name : card.name;
    s.country = card.country.Present() ? card.country : known.country;
    s.badge = card.badge == SkillBadge() ? known.badge : card.badge;
    s.since = card.hasSince ? card.since : 0;
    s.main = card.hasMain ? card.main : -1;
    s.fighter = fighter >= 0 ? fighter : known.fighter >= 0 ? known.fighter : s.main;
    s.playing = playing;
    s.worn = card.worn;
    s.pins = card.pins;
    if (card.hasStyle) s.style = card.style;
    s.counters = aw::CountersOf(card);
    s.profiled = profiled;
    s.account.points = card.account.points;
    s.account.tier = card.account.tier;
    std::vector<netplay::levels::CardFighter> byPoints = card.fighters;
    std::sort(byPoints.begin(), byPoints.end(), [](const netplay::levels::CardFighter& a, const netplay::levels::CardFighter& b) {
        return a.points != b.points ? a.points > b.points : a.fighter < b.fighter;
    });
    for (const auto& one : byPoints) {
        if (!one.points) continue;
        s.played.push_back({one.fighter, one.points, one.tier, 0});
        if (one.fighter == s.fighter) { s.hero.points = one.points; s.hero.tier = one.tier; }
    }
    s.versus = true;
    s.hasRecord = card.hasVersus;
    s.record = card.versus;
    s.relation = card.relation;
    return s;
}

std::vector<MenuEntry> CardRows(const FullCardView& v) {
    std::vector<MenuEntry> rows;
    for (const auto& tile : v.strip) {
        auto row = Row(tile.row, selection::FindFighter(tile.fighter)->name, loc::Tf("level.value", tile.points) + (tile.games.empty() ? std::string() : kDot + tile.games));
        row.hint = loc::T("profile.tab.fighters");
        rows.push_back(std::move(row));
    }
    for (const auto& tile : v.awards.tiles) {
        auto row = Row(CardAwardRow(tile.family->id), tile.title, std::string(loc::T(tile.family->name)) + kDot + v2::TierName(tile.tier));
        row.hint = loc::T("screen.awards");
        rows.push_back(std::move(row));
    }
    if (v.rival.kind == Rival::Kind::MostPlayed) {
        auto row = Row(MostPlayedRow, v.rival.player.name, v.rival.line);
        row.userText = true;
        row.hint = loc::T("room.member_card");
        rows.push_back(std::move(row));
    }
    if (v.sets.state == SetsView::State::Rows) {
        for (std::size_t i = 0; i < v.sets.rows.size() && i < static_cast<std::size_t>(v.setRows); ++i) {
            const auto& set = v.sets.rows[i];
            auto row = Row(set.row, loc::Tf("card.set_vs", set.opponent), set.line);
            row.userText = true;
            row.detailText = DetailText::Name;
            row.hint = loc::T("archive.watch_hint");
            rows.push_back(std::move(row));
        }
    }
    if (v.sets.allSets && v.sets.shown) {
        auto row = Row(AllSetsRow, loc::T("card.all_sets"), std::string());
        row.hint = loc::T("card.all_sets");
        rows.push_back(std::move(row));
    }
    return rows;
}

} } }
