#include "CardSets.hxx"
#include "ReplaySummaryText.hxx"
#include "ShellView.hxx"
#include "../common/Localization.hxx"
#include "../common/WatchCaption.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
namespace card_sets {
namespace {
const char* const kDot = " \xC2\xB7 ";
const char* const kEnDash = " \xE2\x80\x93 ";

SkillBadge BadgeOf(int band) {
    return band >= 1 && band <= SkillBadge::Classes ? SkillBadge::OfClass(static_cast<std::uint8_t>(band), true) : SkillBadge();
}

std::string Joined(const std::vector<std::string>& parts) {
    std::string text;
    for (const auto& part : parts) if (!part.empty()) text += (text.empty() ? "" : kDot) + part;
    return text;
}
}

player_card::SetRow RowOf(const archivesets::Entry& entry, const std::string& name, std::uint64_t now) {
    player_card::SetRow row;
    row.row = std::string(RowPrefix) + entry.id;
    const auto& last = entry.Last();
    const int side = archivesets::SideOf(last, name), other = 1 - side;
    row.mine = selection::FindFighter(last.fighters[side]) ? last.fighters[side] : -1;
    row.theirs = selection::FindFighter(last.fighters[other]) ? last.fighters[other] : -1;
    row.opponent = last.names[other].empty() ? FighterName(last.fighters[other]) : last.names[other];
    row.country = CountryCode::Parsed(last.countries[other]);
    row.badge = BadgeOf(last.classes[other]);
    std::string result;
    if (const auto score = archivesets::Score(entry)) {
        // The score is the first game's first player's, then the second's: this player's side first.
        const int first = archivesets::SideOf(entry.First(), name);
        result = std::to_string((*score)[first]) + kEnDash + std::to_string((*score)[1 - first]);
    } else {
        result = entry.games.size() == 1 ? std::string(loc::T("archive.game_one")) : loc::Tf("archive.games", entry.games.size());
    }
    std::string context = loc::T("card.set_online");
    if (entry.Tournament()) context = Joined({entry.First().event, Joined({entry.First().round, entry.First().phase})});
    row.line = Joined({result, context, player_card::WhenWords(now, entry.newest)});
    return row;
}

player_card::SetsView ViewOf(const netplay::archive::Status& status, bool answers, const std::vector<archivesets::Entry>& entries, const std::string& name,
    std::uint64_t now) {
    using State = netplay::archive::Status::State;
    using Sets = player_card::SetsView;
    Sets view;
    if (!answers || status.state == State::Loading || status.state == State::Idle) { view.state = Sets::State::Reading; return view; }
    if (status.state == State::Unsupported) { view.state = Sets::State::Unavailable; view.allSets = false; return view; }
    if (status.state == State::Failed || !status.listing) { view.state = Sets::State::Unreachable; return view; }
    for (const auto& entry : entries) view.rows.push_back(RowOf(entry, name, now));
    // The first page may hold none of this name's sets while more pages follow: that is still reading.
    if (view.rows.empty()) view.state = status.loadingMore || !status.listing->next.empty() ? Sets::State::Reading : Sets::State::None;
    else view.state = Sets::State::Rows;
    return view;
}
}

void CardSets::Regroup(const ShellView& v) {
    const auto& listing = v.profileSets.listing;
    if (listing == held_) return;
    held_ = listing;
    entries_ = listing ? archivesets::Group(*listing) : std::vector<archivesets::Entry>{};
}

void CardSets::Update(const ShellView& v, const std::string& bridge, const std::string& name, bool shown, const Submit& submit) {
    bridge_ = bridge;
    name_ = name;
    Regroup(v);
    if (!shown) { visiting_ = false; return; }
    // Coming on show asks again, so the sets are the newest.
    if (!visiting_) retry_ = true;
    visiting_ = true;
    if (bridge.empty() || name.empty()) return;
    commands::ArchiveList want;
    want.bridge = bridge;
    want.filter = netplay::archive::Filter::Named(name);
    want.profile = true;
    want.retry = retry_;
    // Sent once for each want and each listing it is a want of; the runtime decides which page, if any, that asks for.
    if (!want.retry && want == last_ && v.profileSets.revision == revision_) return;
    retry_ = false;
    if (!submit(want)) return;
    last_ = std::move(want);
    last_.retry = false;
    revision_ = v.profileSets.revision;
}

player_card::SetsView CardSets::View(const ShellView& v) const {
    const bool answers = !bridge_.empty() && !name_.empty() && v.profileSets.Answers(bridge_, netplay::archive::Filter::Named(name_));
    if (bridge_.empty() || name_.empty()) {
        player_card::SetsView none;
        none.state = player_card::SetsView::State::None;
        return none;
    }
    return card_sets::ViewOf(v.profileSets, answers, entries_, name_, v.unixNow);
}

bool CardSets::Activate(const std::string& id, const ShellView& v, const Submit& submit, WatchQueue& queue, std::string& error) {
    if (!card_sets::IsRow(id)) return false;
    const std::string wanted = id.substr(std::string(card_sets::RowPrefix).size());
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const archivesets::Entry& e) { return e.id == wanted; });
    if (entry == entries_.end() || !v.replays.ready || !v.profileSets.fetching.empty() || !v.archiveBrowse.fetching.empty()) return true;
    std::vector<QueuedGame> games;
    for (const auto* game : entry->games) {
        QueuedGame queued;
        queued.bridge = v.profileSets.bridge.empty() ? bridge_ : v.profileSets.bridge;
        queued.url = game->url;
        queued.digest = game->digest;
        const std::string names[2] = {game->names[0], game->names[1]};
        const SkillBadge classes[2] = {card_sets::BadgeOf(game->classes[0]), card_sets::BadgeOf(game->classes[1])};
        queued.caption = replay::WatchCaption(names, classes, v.preferences.classEmblems);
        games.push_back(std::move(queued));
    }
    // The first game now; each next one from the Next game row on Replays.
    queue.Fill(std::move(games));
    if (!submit(queue.Start(v))) error = loc::T("error.queue_failed");
    return true;
}

} }
