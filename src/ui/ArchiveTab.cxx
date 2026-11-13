#include "ArchiveTab.hxx"
#include "ApplicationShell.hxx"
#include "CountryFlag.hxx"
#include "GameMenu.hxx"
#include "MenuDialogs.hxx"
#include "MenuRows.hxx"
#include "NameMarks.hxx"
#include "PublicRoomsPanel.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "ArchiveFind.hxx"
#include "ReplaySummaryText.hxx"
#include "../common/LocalTime.hxx"
#include "../common/Localization.hxx"
#include "../common/WatchCaption.hxx"
#include <algorithm>
#include <cstdio>
#include <iterator>

namespace sf4e { namespace ui {
namespace {
using Entry = archivesets::Entry;
using Game = netplay::archive::Game;
using netplay::archive::Filter;
// A set's card: two lines under the players, and the context line.
constexpr int kCardHeight = 88;
const char* const kRoundKeys[] = {"archive.round.top8", "archive.round.pools", "archive.round.winners", "archive.round.losers", "archive.round.grand"};
static_assert(std::size(kRoundKeys) == netplay::archive::RoundCount, "every round the listing filters by has its words");
const std::string kDot = " \xC2\xB7 ";

// A side's name, or its fighter when nobody named it (its player did not share the game).
std::string SideName(const Game& game, int side) { return game.names[side].empty() ? FighterName(game.fighters[side]) : game.names[side]; }
SkillBadge BadgeOf(int band) {
    return band >= 1 && band <= SkillBadge::Classes ? SkillBadge::OfClass(static_cast<std::uint8_t>(band), true) : SkillBadge();
}
NameMarks MarksOf(const Game& game, int side) {
    NameMarks marks;
    marks.country = CountryCode::Parsed(game.countries[side]);
    marks.badge = BadgeOf(game.classes[side]);
    return marks;
}
std::string GamesText(std::size_t count) { return count == 1 ? std::string(loc::T("archive.game_one")) : loc::Tf("archive.games", count); }
std::string Joined(const std::vector<std::string>& parts, const std::string& between) {
    std::string text;
    for (const auto& part : parts) if (!part.empty()) text += (text.empty() ? "" : between) + part;
    return text;
}
std::string RoundLabel(const std::string& round) {
    for (std::size_t i = 0; i < netplay::archive::RoundCount; ++i)
        if (round == netplay::archive::Rounds[i]) return loc::T(kRoundKeys[i]);
    return round;
}
// A tournament set's round as its games name it: "Top 8 · Winners".
std::string RoundOf(const Game& game) { return Joined({game.round, game.phase}, kDot); }
// What a row adds to its players: a tournament set's event and round, and its games' count when there are several.
std::string ContextText(const Entry& entry) {
    std::vector<std::string> parts;
    if (entry.Tournament()) { parts.push_back(entry.First().event); parts.push_back(RoundOf(entry.First())); }
    if (entry.games.size() > 1 && !entry.Tournament()) parts.push_back(GamesText(entry.games.size()));
    return Joined(parts, kDot);
}
std::string FightersLine(const Entry& entry) {
    const Game& first = entry.First();
    if (entry.kind != Entry::Kind::Session) return loc::Tf("replays.fighters", FighterName(first.fighters[0]), FighterName(first.fighters[1]));
    std::vector<std::string> met;
    for (const int fighter : archivesets::FightersMet(entry)) { if (met.size() == 6) break; met.push_back(FighterName(fighter)); }
    return loc::Tf("archive.met", Joined(met, ", "));
}
// The name side `side` of a set's row stands for: a session's player, else the first game's side.
std::string NameOf(const Entry& entry, int side) {
    if (entry.kind == Entry::Kind::Session) return side == 0 ? entry.player : std::string();
    return entry.First().names[side];
}
// The side a set's row leads with: a session's player, else, on a profile's list, the side the listed player played in the
// first game, so their name and score come first.
int LeadOf(const Entry& entry, const std::string& listed) {
    if (entry.kind == Entry::Kind::Session) return archivesets::SideOf(entry.First(), entry.player);
    const Game& first = entry.First();
    return !listed.empty() && first.names[0] != listed && first.names[1] == listed ? 1 : 0;
}
MenuEntry EntryRow(const ShellView& v, const Entry& entry, bool idle, const std::string& listed, bool profile) {
    const Game& first = entry.First();
    const bool session = entry.kind == Entry::Kind::Session;
    const auto score = archivesets::Score(entry);
    const int lead = LeadOf(entry, listed);
    std::string label;
    if (session) label = loc::Tf("archive.session_label", entry.player, entry.games.size());
    else if (score) label = SideName(first, lead) + " " + std::to_string((*score)[lead]) + "-" + std::to_string((*score)[1 - lead]) + " " + SideName(first, 1 - lead);
    else label = loc::Tf("replays.fighters", SideName(first, lead), SideName(first, 1 - lead));
    const std::string context = ContextText(entry);
    MenuEntry row = Row(std::string(ArchiveTab::RowPrefix) + entry.id, label, Joined({context, LocalTime(entry.newest)}, kDot));
    row.userText = true;
    row.detailText = DetailText::Name;
    for (const auto& name : first.names) NoteUserText(name);
    NoteUserText(entry.player);
    const int left = lead;
    const auto known = [&](int side) { return selection::FindFighter(first.fighters[side]) ? first.fighters[side] : -1; };
    row.fighter1 = known(left);
    row.fighter2 = known(1 - left);
    row.value = LocalTime(entry.newest);
    row.height = kCardHeight;
    row.hint = loc::T("archive.watch_hint");
    const bool several = entry.games.size() > 1;
    row.choices.push_back(NeedsMenu("archive-watch", loc::T(several ? "replays.watch_set" : "replays.watch"), loc::T(several ? "replays.watch_set_detail" : "replays.watch_detail"),
        idle, v.replays.ready && v.archiveBrowse.fetching.empty()));
    const std::string names[2] = {NameOf(entry, 0), NameOf(entry, 1)};
    if (profile) {
        // The players of the set other than the one listed: their cards replace the profile (PlayerPage).
        const std::string others[2] = {names[0] != listed ? names[0] : std::string(), names[1] != listed ? names[1] : std::string()};
        for (auto& card : CardChoices(others)) row.choices.push_back(std::move(card));
    } else {
        // Their sets in the archive, and their profiles.
        for (auto& find : FindChoices(names)) row.choices.push_back(std::move(find));
        for (auto& card : CardChoices(names)) row.choices.push_back(std::move(card));
    }
    row.chosen = row.choices.front().enabled || row.choices.size() == 1 ? row.choices.front().id : row.choices[1].id;
    return row;
}
// A choice's words as a value row shows them, `values` being what each stands for.
MenuEntry PickRow(const char* id, const char* label, const char* detail, const std::vector<std::string>& labels, std::size_t at) {
    MenuEntry row = Value(id, loc::T(label), labels[at], loc::T(detail));
    OfferSteps(row, labels);
    return row;
}
std::size_t IndexOf(const std::vector<std::string>& values, const std::string& value) {
    const auto at = std::find(values.begin(), values.end(), value);
    return at == values.end() ? 0 : static_cast<std::size_t>(at - values.begin());
}
std::string Trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}
// The words of a filter, one for each thing it narrows by.
std::vector<std::string> Parts(const Filter& filter) {
    std::vector<std::string> parts;
    // An exact name is the player the sets are of; typed text beside it is the search.
    if (!filter.name.empty()) { NoteUserText(filter.name); parts.push_back(filter.name); }
    if (!filter.text.empty() && filter.text != filter.name) { NoteUserText(filter.text); parts.push_back(filter.text); }
    if (filter.fighter >= 0) parts.push_back(FighterName(filter.fighter));
    if (filter.band > 0) parts.push_back(loc::Tf("archive.filter_class", ClassName(static_cast<std::uint8_t>(filter.band))));
    if (!filter.country.empty()) {
        const auto code = CountryCode::Parsed(filter.country);
        parts.push_back(code.Present() ? CountryName(code) : filter.country);
    }
    if (!filter.event.empty()) {
        if (filter.event != netplay::archive::OnlineEvent) NoteUserText(filter.event);
        parts.push_back(filter.event == netplay::archive::OnlineEvent ? std::string(loc::T("archive.online")) : filter.event);
    }
    if (!filter.round.empty()) parts.push_back(RoundLabel(filter.round));
    if (filter.order == netplay::archive::Order::Class) parts.push_back(loc::T("archive.sort_class"));
    return parts;
}
}

// Whether the end of a list of `rows` sets is near the focused one at `at` (-1: focus is not on a set):
// the last few, or any of a short list.
bool ArchiveTab::NearEnd(int at, std::size_t rows) {
    constexpr std::size_t kAhead = 6;
    return rows <= kAhead || (at >= 0 && static_cast<std::size_t>(at) + kAhead >= rows);
}

int ArchiveTab::FighterOf(const std::string& id) {
    if (id.size() < 4 || id.compare(0, 3, "af-") != 0 || id[3] < '0' || id[3] > '9') return -1;
    const int fighter = std::stoi(id.substr(3));
    return selection::FindFighter(fighter) ? fighter : -1;
}

std::string ArchiveTab::FilterSummary(const Filter& filter) {
    const auto parts = Parts(filter);
    return parts.empty() ? std::string(loc::T("archive.filter_none")) : Joined(parts, kDot);
}

std::vector<std::string> ArchiveTab::Chips() const { return Parts(filter_); }

void ArchiveTab::Browse(Filter filter) {
    filter_ = std::move(filter);
    last_ = commands::ArchiveList();
    retry_ = false;
}

const netplay::archive::Status& ArchiveTab::Slot(const ShellView& v) const { return place_.profile ? v.profileSets : v.archiveBrowse; }

bool ArchiveTab::Heard(const ShellView& v) const { return Slot(v).Answers(bridge_, filter_); }

void ArchiveTab::Regroup(const ShellView& v) {
    const auto& listing = Slot(v).listing;
    if (listing == held_) return;
    held_ = listing;
    entries_ = listing ? archivesets::Group(*listing) : std::vector<Entry>{};
    // The filter's choices are the archive's, whatever the filter is: they stay while another filter's first page loads.
    if (listing && Slot(v).bridge == bridge_) { events_ = listing->events; countries_ = listing->countries; }
}

const Entry* ArchiveTab::Find(const std::string& id) const {
    for (const auto& entry : entries_) if (entry.id == id) return &entry;
    return nullptr;
}

void ArchiveTab::Update(const ShellView& v, const std::string& screen, const std::string& bridge, const std::string& focus, const Submit& submit) {
    bridge_ = bridge;
    Regroup(v);
    // A page is asked for only while the list is on show: the filter's own pages ask for nothing until the player is back.
    if (!IsList(screen) || bridge.empty() || (place_.profile && filter_.name.empty())) return;
    int at = -1;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (std::string(RowPrefix) + entries_[i].id == focus) { at = static_cast<int>(i); break; }
    commands::ArchiveList want;
    want.bridge = bridge;
    want.filter = filter_;
    want.nearEnd = NearEnd(at, entries_.size());
    want.retry = retry_;
    want.profile = place_.profile;
    // Sent once for each want and each listing it is a want of; the runtime decides which page that asks for.
    if (!want.retry && want == last_ && Slot(v).revision == revision_) return;
    retry_ = false;
    if (!submit(want)) return;
    last_ = std::move(want);
    last_.retry = false;
    revision_ = Slot(v).revision;
}

std::vector<std::string> ArchiveTab::EventValues() const {
    std::vector<std::string> values{std::string(), netplay::archive::OnlineEvent};
    for (const auto& title : events_) values.push_back(title);
    if (!filter_.event.empty() && std::find(values.begin(), values.end(), filter_.event) == values.end()) values.push_back(filter_.event);
    return values;
}

std::vector<std::string> ArchiveTab::CountryValues() const {
    std::vector<std::string> values{std::string()};
    for (const auto& code : countries_) values.push_back(code);
    if (!filter_.country.empty() && std::find(values.begin(), values.end(), filter_.country) == values.end()) values.push_back(filter_.country);
    return values;
}

void ArchiveTab::FilterRows(const ShellView& v, std::vector<MenuEntry>& rows) const {
    // Player or event is typed; the rest are words stepped through, which Select lists in full.
    if (place_.profile) {
        // The player is the name the sets are of and stays; the opponent is typed.
        NoteUserText(filter_.name);
        rows.push_back(InfoRow("af-player", loc::T("profile.sets_player"), filter_.name, loc::T("profile.sets_player_detail")));
        rows.back().userText = true;
        NoteUserText(filter_.text);
        rows.push_back(TextRow("af-text", loc::T("profile.sets_opponent"), filter_.text, 40));
        rows.back().detail = loc::T("profile.sets_opponent_detail");
        rows.back().userText = true;
    } else {
        const std::string& typed = filter_.name.empty() ? filter_.text : filter_.name;
        NoteUserText(typed);
        rows.push_back(TextRow("af-text", loc::T("archive.f_text"), typed, 40));
        rows.back().detail = loc::T("archive.f_text_detail");
        rows.back().userText = true;
    }
    rows.push_back(Row("af-fighter", loc::T("archive.f_fighter"), loc::T("archive.f_fighter_detail")));
    rows.back().value = filter_.fighter >= 0 ? FighterName(filter_.fighter) : std::string(loc::T("archive.any_fighter"));
    std::vector<std::string> classes{loc::T("archive.any_class")};
    for (int band = 1; band <= SkillBadge::Classes; ++band) classes.push_back(ClassName(static_cast<std::uint8_t>(band)));
    rows.push_back(PickRow("af-class", "archive.f_class", "archive.f_class_detail", classes, static_cast<std::size_t>(filter_.band)));
    const auto countryValues = CountryValues();
    std::vector<std::string> countries;
    for (const auto& code : countryValues) {
        const auto parsed = CountryCode::Parsed(code);
        countries.push_back(code.empty() ? std::string(loc::T("archive.any_country")) : parsed.Present() ? CountryName(parsed) : code);
    }
    rows.push_back(PickRow("af-country", "archive.f_country", "archive.f_country_detail", countries, IndexOf(countryValues, filter_.country)));
    const auto eventValues = EventValues();
    std::vector<std::string> events;
    for (const auto& title : eventValues) {
        if (!title.empty() && title != netplay::archive::OnlineEvent) NoteUserText(title);
        events.push_back(title.empty() ? std::string(loc::T("archive.any_event")) : title == netplay::archive::OnlineEvent ? std::string(loc::T("archive.online")) : title);
    }
    rows.push_back(PickRow("af-event", "archive.f_event", "archive.f_event_detail", events, IndexOf(eventValues, filter_.event)));
    rows.back().userText = true;
    std::vector<std::string> rounds{loc::T("archive.any_round")};
    for (const char* key : kRoundKeys) rounds.push_back(loc::T(key));
    std::size_t round = 0;
    for (std::size_t i = 0; i < netplay::archive::RoundCount; ++i) if (filter_.round == netplay::archive::Rounds[i]) round = i + 1;
    rows.push_back(PickRow("af-round", "archive.f_round", "archive.f_round_detail", rounds, round));
    const std::vector<std::string> sorts{loc::T("archive.sort_newest"), loc::T("archive.sort_class")};
    rows.push_back(PickRow("af-sort", "archive.f_sort", "archive.f_sort_detail", sorts, filter_.order == netplay::archive::Order::Class ? 1 : 0));
    // How many games the filter keeps, once the listing says.
    if (Heard(v) && Slot(v).listing && Slot(v).listing->matching) {
        const auto& listing = *Slot(v).listing;
        rows.push_back(InfoRow("af-count", loc::T("archive.games_shown"),
            listing.total ? loc::Tf("archive.games_of", *listing.matching, *listing.total) : std::to_string(*listing.matching), ""));
    }
    rows.push_back(Row("af-clear", loc::T("archive.clear"), loc::T("archive.clear_detail"), Narrowed()));
}

std::vector<MenuEntry> ArchiveTab::Rows(const ShellView& v, const std::string& screen, bool idle, std::string& title) const {
    std::vector<MenuEntry> rows;
    if (screen == place_.filter) {
        title = loc::T("archive.filters");
        FilterRows(v, rows);
        return rows;
    }
    if (screen == place_.fighter) {
        // The fighters as a grid of cards, as the Matches filter lists them; one is chosen, or any.
        title = loc::T("archive.f_fighter");
        for (const int id : selection::RosterDisplayOrder) {
            rows.push_back(Row("af-" + std::to_string(id), selection::FindFighter(id)->name, loc::T("replays.fighter_detail")));
            rows.back().key = static_cast<std::uint64_t>(id);
        }
        rows.push_back(Row("af-any", loc::T("archive.any_fighter"), loc::T("archive.any_fighter_detail")));
        rows.back().wide = true;
        return rows;
    }
    title = loc::T("replays.title");
    if (bridge_.empty()) {
        rows.push_back(Row("archive-identity", loc::T("archive.no_service"), loc::T("archive.no_service_detail")));
        return rows;
    }
    // A profile's list with no name to list by has no sets to find.
    if (place_.profile && filter_.name.empty()) {
        rows.push_back(InfoRow("archive-none", loc::T("archive.empty"), "", loc::T("archive.empty_detail")));
        return rows;
    }
    rows.push_back(Row("archive-filters", loc::T("archive.filters"), loc::T("archive.filters_detail")));
    rows.back().value = FilterSummary(filter_);
    rows.back().userText = true;
    // A profile's list says what its sets are found by: the exact name.
    if (place_.profile) {
        rows.push_back(InfoRow("archive-note", loc::Tf("profile.sets_note", filter_.name), "", ""));
        rows.back().userText = true;
    }
    if (Narrowed()) rows.push_back(Row("archive-clear", loc::T("archive.clear"), loc::T("archive.clear_detail")));
    const auto& status = Slot(v);
    using State = netplay::archive::Status::State;
    if (!Heard(v) || status.state == State::Loading) {
        rows.push_back(InfoRow("archive-loading", loc::T("archive.loading"), "", loc::T("archive.loading_detail")));
    } else if (status.state == State::Unsupported) {
        rows.push_back(InfoRow("archive-unsupported", loc::T("archive.unsupported"), "", loc::T("archive.unsupported_detail")));
    } else if (status.state == State::Failed) {
        rows.push_back(Row("archive-retry", loc::T("archive.failed"), loc::Tf("archive.failed_detail", PublicRoomsPanel::FailureText(status.error))));
    } else if (status.Loaded()) {
        for (const auto& entry : entries_) rows.push_back(EntryRow(v, entry, idle, place_.profile ? filter_.name : std::string(), place_.profile));
        if (entries_.empty() && (status.loadingMore || !status.listing->next.empty()))
            rows.push_back(InfoRow("archive-loading", loc::T("archive.loading"), "", loc::T("archive.loading_detail")));
        else if (entries_.empty())
            rows.push_back(InfoRow("archive-none", loc::T(Narrowed() ? "archive.none" : "archive.empty"), "", loc::T(Narrowed() ? "archive.none_detail" : "archive.empty_detail")));
        if (status.loadingMore) rows.push_back(InfoRow("archive-more", loc::T("archive.loading_more"), "", loc::T("archive.loading_more_detail")));
        else if (status.moreFailed) rows.push_back(Row("archive-more-retry", loc::T("archive.more_failed"), loc::T("archive.more_failed_detail")));
    }
    return rows;
}

bool ArchiveTab::Activate(const MenuAction& a, MenuNavigation& nav) {
    if (a.id == "archive-filters") { nav.Push(place_.filter); return true; }
    if (a.id == "archive-identity") { nav.Push("identity"); return true; }
    if (a.id == "archive-retry" || a.id == "archive-more-retry") { retry_ = true; return true; }
    if (a.id == "archive-clear" || a.id == "af-clear") {
        const std::string kept = place_.profile ? filter_.name : std::string();
        filter_ = Filter().Ordered(filter_.order);
        filter_.name = kept;
        return true;
    }
    if (a.id == "af-fighter") { nav.Push(place_.fighter); return true; }
    if (a.id == "af-any") { filter_.fighter = -1; nav.Return(); return true; }
    if (const int fighter = FighterOf(a.id); fighter >= 0) { filter_.fighter = fighter; nav.Return(); return true; }
    return false;
}

bool ArchiveTab::Adjust(const MenuAction& a) {
    const auto step = [&](const std::vector<std::string>& values, std::string& current) {
        const int at = static_cast<int>(IndexOf(values, current)) + a.delta;
        current = values[static_cast<std::size_t>((std::max)(0, (std::min)(static_cast<int>(values.size()) - 1, at)))];
    };
    // Typing replaces the search, which is then a substring one: an exact name is dropped.
    if (a.id == "af-text") {
        if (a.kind == MenuAction::TextAccepted) { filter_.text = Trimmed(a.text); if (!place_.profile) filter_.name.clear(); }
        return true;
    }
    if (a.id == "af-class") { filter_.band = (std::max)(0, (std::min)(static_cast<int>(SkillBadge::Classes), filter_.band + a.delta)); return true; }
    if (a.id == "af-country") { step(CountryValues(), filter_.country); return true; }
    if (a.id == "af-event") { step(EventValues(), filter_.event); return true; }
    if (a.id == "af-round") {
        std::vector<std::string> rounds{std::string()};
        for (const char* round : netplay::archive::Rounds) rounds.push_back(round);
        step(rounds, filter_.round);
        return true;
    }
    if (a.id == "af-sort") {
        filter_.order = a.delta > 0 ? netplay::archive::Order::Class : netplay::archive::Order::Newest;
        return true;
    }
    return false;
}

void ArchiveTab::Shortcut(const MenuAction& a, const ShellView&, const std::string& screen, MenuNavigation& nav) {
    if (!IsList(screen) || bridge_.empty()) return;
    if (a.delta == MenuInput::Fighter) nav.Push(place_.filter);
    else if (a.delta == MenuInput::Options) retry_ = true;
}

std::vector<LegendHint> ArchiveTab::Hints(const ShellView& v, const std::string& screen) const {
    if (!IsList(screen) || bridge_.empty()) return {};
    using State = netplay::archive::Status::State;
    const auto& status = Slot(v);
    const bool failed = Heard(v) && (status.state == State::Failed || status.state == State::Unsupported || status.moreFailed);
    return ShortcutHints({{MenuInput::Fighter, loc::T("archive.filters")}, {MenuInput::Options, loc::T(failed ? "archive.retry" : "legend.refresh")}});
}

// The set's games as a queue of downloads, each with the names and classes the game was shared with, which
// are drawn over it as a replay's own are (replay::WatchCaption).
std::vector<QueuedGame> ArchiveTab::Games(const Entry& entry, const ShellView& v) const {
    std::vector<QueuedGame> games;
    for (const auto* game : entry.games) {
        QueuedGame queued;
        queued.bridge = Slot(v).bridge.empty() ? bridge_ : Slot(v).bridge;
        queued.url = game->url;
        queued.digest = game->digest;
        const std::string names[2] = {game->names[0], game->names[1]};
        const SkillBadge classes[2] = {BadgeOf(game->classes[0]), BadgeOf(game->classes[1])};
        queued.caption = replay::WatchCaption(names, classes, v.preferences.classEmblems);
        games.push_back(std::move(queued));
    }
    return games;
}

bool ArchiveTab::Choose(const MenuAction& a, const ShellView& v, const Submit& submit, MenuNavigation& nav, WatchQueue& queue, std::string& error) {
    const Entry* entry = IsRow(a.id) ? Find(a.id.substr(std::string(RowPrefix).size())) : nullptr;
    if (!entry) return false;
    const std::string names[2] = {NameOf(*entry, 0), NameOf(*entry, 1)};
    if (const auto name = FoundName(a.text, names)) {
        OpenFor(*name, nav);
    } else if (a.text == "archive-watch" && v.replays.ready && v.archiveBrowse.fetching.empty()) {
        // The first game now; each next one from the Next game row on Replays, where the log comes back to.
        queue.Fill(Games(*entry, v));
        if (!submit(queue.Start(v))) error = loc::T("error.queue_failed");
    }
    return true;
}

std::optional<player_opening::Opening> ArchiveTab::PlayerOf(const MenuAction& a) const {
    const auto chosen = IsRow(a.id) ? CardSide(a.text) : std::nullopt;
    if (!chosen) return std::nullopt;
    const Entry* entry = Find(a.id.substr(std::string(RowPrefix).size()));
    if (!entry) return std::nullopt;
    // The sides are the first game's, as the row's choices name them; a session names only the one player, on whichever side
    // they played in that game.
    const Game& first = entry->First();
    const bool session = entry->kind == Entry::Kind::Session;
    if (session && *chosen != 0) return std::nullopt;
    const int side = session ? archivesets::SideOf(first, entry->player) : *chosen;
    const std::string& name = session ? entry->player : first.names[side];
    if (name.empty()) return std::nullopt;
    return player_opening::OpeningOfSide(name, CountryCode::Parsed(first.countries[side]), BadgeOf(first.classes[side]), first.fighters[side]);
}

bool ArchiveTab::Status(const ShellView& v, const std::string& screen, std::string& status, Tone& tone) const {
    if (!(Owns(screen) || IsList(screen) || screen == place_.filter || screen == place_.fighter) || !status.empty()) return false;
    const auto& answer = v.archiveBrowse;
    if (!answer.fetching.empty()) { status = loc::T("archive.downloading"); tone = Tone::Pending; return true; }
    if (answer.fetchError.empty()) return false;
    status = loc::Tf("archive.download_failed", PublicRoomsPanel::FailureText(answer.fetchError));
    tone = Tone::Error;
    return true;
}

bool ArchiveTab::DrawCard(const ShellView&, const MenuEntry& e, ImVec2 min, ImVec2 max) const {
    if (!IsRow(e.id)) return false;
    const Entry* entry = Find(e.id.substr(std::string(RowPrefix).size()));
    if (!entry) return false;
    auto* d = ImGui::GetWindowDrawList();
    const float s = Scale();
    const auto& c = v2::Colors();
    const Game& first = entry->First();
    const bool session = entry->kind == Entry::Kind::Session;
    // The right end: when the set was played.
    float right = max.x - 16 * s;
    const std::string when = LocalTime(entry->newest);
    const float whenWidth = ImGui::CalcTextSize(when.c_str()).x;
    d->AddText(ImVec2(right - whenWidth, min.y + (max.y - min.y - ImGui::GetFontSize()) * .5f), c.muted, when.c_str());
    right -= whenWidth + 14 * s;
    // The fighters' faces, a session's player first (and a profile's player).
    const int left = LeadOf(*entry, place_.profile ? filter_.name : std::string());
    const float face = max.y - min.y - 16 * s;
    float x = min.x + 16 * s;
    for (int i = 0; i < 2; ++i) {
        const int fighter = first.fighters[i == 0 ? left : 1 - left];
        if (!selection::FindFighter(fighter)) continue;
        DrawCharacterPortrait(fighter, ImVec2(x, min.y + 8 * s), ImVec2(x + face, min.y + 8 * s + face), c.frame);
        x += face + 4 * s;
    }
    if (x > min.x + 16 * s) x += 8 * s;
    // The players, each after the flag and class they have, with the score between them.
    const float top = min.y + 10 * s;
    ImGui::PushFont(HeadingFont());
    const float size = ImGui::GetFontSize();
    for (const auto& name : first.names) NoteUserText(name);
    NoteUserText(entry->player);
    if (session) {
        const NameMarks marks = MarksOf(first, left);
        const float room = (right - x) * .6f - NameMarksWidth(marks, size);
        const std::string name = FitLabel(entry->player, room);
        float at = x + DrawNameMarks(d, x, top, size, marks).width;
        d->AddText(ImVec2(at, top), c.text, name.c_str());
        at += ImGui::CalcTextSize(name.c_str()).x + 12 * s;
        d->AddText(ImVec2(at, top), c.accent, GamesText(entry->games.size()).c_str());
    } else {
        const auto score = archivesets::Score(*entry);
        const std::string mid = score ? std::to_string((*score)[left]) + "-" + std::to_string((*score)[1 - left]) : std::string(loc::T("archive.vs"));
        const NameMarks marksA = MarksOf(first, left), marksB = MarksOf(first, 1 - left);
        const float room = (right - x - ImGui::CalcTextSize(mid.c_str()).x - 24 * s) * .5f;
        const std::string a = FitLabel(SideName(first, left), room - NameMarksWidth(marksA, size)), b = FitLabel(SideName(first, 1 - left), room - NameMarksWidth(marksB, size));
        float at = x + DrawNameMarks(d, x, top, size, marksA).width;
        d->AddText(ImVec2(at, top), c.text, a.c_str());
        at += ImGui::CalcTextSize(a.c_str()).x + 12 * s;
        d->AddText(ImVec2(at, top), c.accent, mid.c_str());
        at += ImGui::CalcTextSize(mid.c_str()).x + 12 * s;
        at += DrawNameMarks(d, at, top, size, marksB).width;
        d->AddText(ImVec2(at, top), c.text, b.c_str());
    }
    const float below = top + size + 4 * s;
    ImGui::PopFont();
    // Under them: the fighters, then the event and round of a tournament set, or how many games it holds.
    d->AddText(ImVec2(x, below), c.text, FitLabel(FightersLine(*entry), right - x).c_str());
    const std::string context = ContextText(*entry);
    if (!context.empty()) { NoteUserText(context); d->AddText(ImVec2(x, below + ImGui::GetFontSize() + 3 * s), c.muted, FitLabel(context, right - x).c_str()); }
    return true;
}

void ArchiveTab::DrawDetail(const ShellView&, const std::string& id, MenuNavigation& nav, const std::vector<MenuEntry>& rows) const {
    const Entry* entry = Find(id.substr(std::string(RowPrefix).size()));
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; });
    if (!entry || row == rows.end()) return;
    const float s = Scale();
    const auto& c = v2::Colors();
    const Game& first = entry->First();
    ImGui::PushFont(HeadingFont());
    for (const auto& name : first.names) NoteUserText(name);
    NoteUserText(entry->player);
    ImGui::TextWrapped("%s", row->label.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 6 * s));
    if (entry->Tournament() && !first.event.empty()) { NoteUserText(first.event); v2::Fact(loc::T("archive.fact.event"), first.event); }
    if (entry->Tournament() && !RoundOf(first).empty()) v2::Fact(loc::T("archive.fact.round"), RoundOf(first));
    v2::Fact(loc::T("replays.fact.played"), LocalTime(entry->newest));
    v2::Fact(loc::T("archive.fact.games"), std::to_string(entry->games.size()));
    std::uint64_t bytes = 0;
    for (const auto* game : entry->games) bytes += game->size;
    if (bytes) v2::Fact(loc::T("archive.fact.size"), loc::Tf("archive.size_kb", (bytes + 1023) / 1024));
    // Each game with its fighters, the first few of them.
    constexpr std::size_t kMostLines = 8;
    for (std::size_t i = 0; i < entry->games.size() && i < kMostLines; ++i) {
        const Game& game = *entry->games[i];
        v2::Fact(loc::Tf("archive.game_n", i + 1), loc::Tf("replays.fighters", FighterName(game.fighters[0]), FighterName(game.fighters[1])));
    }
    // Watch now opens the row's choices, as Select does.
    ImGui::Dummy(ImVec2(0, 6 * s));
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(c.accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(c.accentLight));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(c.ink));
    ImGui::PushFont(HeadingFont());
    if (ImGui::Button(loc::T("replays.watch"), ImVec2(-1, 52 * s))) nav.Ask(id, rows);
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
}
} }
