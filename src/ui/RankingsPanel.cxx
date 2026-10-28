#include "RankingsPanel.hxx"
#include "ApplicationShell.hxx"
#include "IdentityPanel.hxx"
#include "MenuRows.hxx"
#include "NameMarks.hxx"
#include "CountryFlag.hxx"
#include "PublicRoomsPanel.hxx"
#include "SignInSites.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cmath>

namespace sf4e { namespace ui {
namespace {
using netplay::rankings::Standing;
using netplay::tournament::Command;
using Kind = Standing::Kind;
// Card heights, unscaled.
constexpr float CardHeight = 110, StateCardHeight = 120, HeaderHeight = 40, PlayerHeight = 34;

MenuEntry Card(std::string id, std::string label, std::string detail, float height) {
    auto card = InfoRow(std::move(id), std::move(label), {}, std::move(detail));
    card.height = height;
    return card;
}

std::string WaitText(std::uint64_t seconds) {
    const auto wait = seconds >= 3600 ? loc::Tf("rankings.hours", (seconds + 3599) / 3600)
        : loc::Tf("rankings.minutes", (std::max<std::uint64_t>)(1, (seconds + 59) / 60));
    return loc::Tf("rankings.change_wait", wait);
}

// A listing as the Take part confirmation names it: the name, with the country's name when there is one.
std::string ListingText(const netplay::rankings::Listing& listing) {
    return listing.country.Present() ? loc::Tf("rankings.name_country", listing.name, CountryName(listing.country)) : listing.name;
}

// The standing card's heading and line, by where the player stands.
std::pair<std::string, std::string> StandingWords(const Standing& standing) {
    switch (standing.kind) {
    case Kind::NoAccount: return {loc::T("rankings.no_account"), loc::T("rankings.no_account_detail")};
    case Kind::OptedOut: return {loc::T("rankings.off"), loc::T("rankings.off_detail")};
    default: break;
    }
    if (!standing.classesShown) return {loc::T("rankings.hidden"), loc::T("rankings.hidden_detail")};
    switch (standing.badge.kind) {
    case SkillBadge::Kind::Developer: return {loc::T("class.developer_tip"), loc::T("rankings.developer_detail")};
    case SkillBadge::Kind::Class:
        return {ClassName(standing.badge.band), loc::T(standing.badge.settled ? "rankings.settled_detail" : "rankings.faint_detail")};
    default: return {loc::T("rankings.placing"), loc::T("rankings.placing_detail")};
    }
}

// Taking part, with classes shown, and not a developer: the board can list them.
bool Boardable(const Standing& standing) {
    return standing.kind == Kind::Ranked && standing.classesShown && standing.badge.kind != SkillBadge::Kind::Developer;
}
}

std::string RankingsPanel::FailureText(const std::string& code) {
    if (code == "ranking_unavailable") return loc::T("rankings.failure.unavailable");
    if (code == "choice_too_soon") return loc::T("rankings.failure.too_soon");
    if (code == "no_discord_account") return loc::T("rankings.no_account");
    return PublicRoomsPanel::FailureText(code);
}

netplay::rankings::Listing RankingsPanel::OwnListing(const netplay::PlayerPreferences& preferences, const CountryCode& automaticCountry) {
    return {netplay::rankings::ListingName(preferences.displayName), ShownCountry(preferences.country, automaticCountry)};
}

std::string RankingsPanel::EntryValue(const ShellView& v, const std::string& bridge) {
    const auto& status = v.rankings;
    if (bridge.empty() || status.bridge != bridge || !status.standing.loaded) return {};
    const auto& standing = status.standing.value;
    if (standing.kind == Kind::NoAccount) return loc::T("identity.discord_none");
    if (standing.kind == Kind::OptedOut) return loc::T("rankings.off");
    if (!standing.classesShown) return {};
    switch (standing.badge.kind) {
    case SkillBadge::Kind::Developer: return loc::T("class.developer");
    case SkillBadge::Kind::Class: return ClassName(standing.badge.band);
    default: return loc::T("rankings.placing");
    }
}

const Standing* RankingsPanel::Current(const ShellView& v) const {
    const auto& status = v.rankings;
    return !bridge_.empty() && status.bridge == bridge_ && status.standing.loaded ? &status.standing.value : nullptr;
}

// Seconds until taking part or not may change again, counted from when the standing arrived.
static std::uint64_t ChangeWait(const netplay::rankings::Status& status) {
    const double left = static_cast<double>(status.standing.value.changeIn) - status.AgeSeconds(status.standing);
    return left > 0 ? static_cast<std::uint64_t>(std::ceil(left)) : 0;
}

void RankingsPanel::Update(const ShellView& v, const std::string& screen, const std::string& bridge,
    const netplay::rankings::Listing& own, const Submit& submit) {
    const auto& status = v.rankings;
    const bool owned = Owns(screen);
    // Rankings and its board are one visit: coming back from the board reads nothing again.
    if (owned && !Owns(lastScreen_)) { wantStanding_ = true; listingSent_ = false; }
    if (screen == BoardScreen && lastScreen_ != BoardScreen) wantBoard_ = true;
    lastScreen_ = screen;
    if (owned) { bridge_ = bridge; own_ = own; }
    // Leaving drops what was not asked yet; a choice already on its way is
    // still answered, and said, wherever the player is.
    else { wantStanding_ = wantBoard_ = false; if (pending_ && !pending_->sent) pending_.reset(); }
    TakeAnswers(v, owned);
    if (!owned || bridge_.empty()) return;
    ShellAction action;
    action.command.generation = v.session.generation;
    if (pending_ && !pending_->sent) {
        // Beyond every identity the runtime has seen, so no earlier answer can pass for this one's.
        lastRequest_ = (std::max)({lastRequest_, status.chose, status.choosing}) + 1;
        pending_->command.request = lastRequest_;
        action.tournament = pending_->command;
        if (!submit(std::move(action))) { pending_.reset(); Say(loc::T("error.queue_failed")); return; }
        pending_->sent = true;
        return;
    }
    // A read waits for the choice in flight, whose answer is the standing after it.
    const bool standing = wantStanding_ && !status.standing.loading && !status.choosing,
        board = wantBoard_ && screen == BoardScreen && !status.board.loading;
    if (!standing && !board) return;
    (standing ? wantStanding_ : wantBoard_) = false;
    action.tournament.op = standing ? Command::Op::RankingStanding : Command::Op::RankingBoard;
    action.tournament.bridgeId = bridge_;
    if (!submit(std::move(action))) Say(loc::T("error.queue_failed"));
}

// A value installed, by a read or a choice, may be published to the board; a
// failed read keeps the value and says why once. A choice is answered
// wherever the player is, and after Ember was hidden. The runtime answers
// every request, if only as unavailable after its own limit.
void RankingsPanel::TakeAnswers(const ShellView& v, bool owned) {
    const auto& status = v.rankings;
    const bool ours = !bridge_.empty() && status.bridge == bridge_;
    if (status.standing.revision != standingRevision_) {
        standingRevision_ = status.standing.revision;
        const auto* standing = Current(v);
        if (owned && standing && Boardable(*standing) && !listingSent_ && !pending_ && (!standing->listed || standing->listing != own_)) {
            listingSent_ = true;
            Choose(true);
        }
    }
    if (status.standing.finished != standingSeen_) {
        standingSeen_ = status.standing.finished;
        if (ours && !status.standing.error.empty() && status.standing.loaded) Say(FailureText(status.standing.error));
    }
    if (status.board.finished != boardSeen_) {
        boardSeen_ = status.board.finished;
        if (ours && !status.board.error.empty() && status.board.loaded) Say(FailureText(status.board.error));
    }
    if (!pending_ || !pending_->sent) return;
    if (status.chose == pending_->command.request) {
        const auto& failure = status.choiceFailure;
        if (!failure.empty()) Say(FailureText(failure));
        // A refused change names no wait and no account: the standing does. A
        // choice that went through answered with the standing after it.
        if (failure == "choice_too_soon" || failure == "no_discord_account") wantStanding_ = true;
        else if (failure.empty()) wantStanding_ = false;
        pending_.reset();
    }
}

void RankingsPanel::Choose(bool ranked) {
    Pending pending;
    pending.command.op = Command::Op::RankingChoose;
    pending.command.bridgeId = bridge_;
    pending.command.ranked = ranked;
    // Only a player who takes part is listed: as the Take part confirmation named them.
    if (ranked) {
        pending.command.listingName = own_.name;
        pending.command.listingCountry = own_.country.ToString();
    }
    pending_ = std::move(pending);
}

std::vector<MenuEntry> RankingsPanel::Rows(const ShellView& v, const std::string& screen, bool identityPending, std::string& title) const {
    title = loc::T(screen == BoardScreen ? "rankings.board_title" : "rankings.title");
    if (bridge_.empty()) {
        // Rankings come from public room games: their setup makes the service usable.
        if (!v.identity.known || identityPending)
            return {Card("rk-checking", loc::T("public.setup_step.checking"), loc::T("identity.state.checking_detail"), StateCardHeight)};
        return {Card("rk-needs-setup", loc::T("rankings.needs_setup"), {}, StateCardHeight),
            Row("rk-setup", loc::T("screen.public_rooms"), loc::T("online.public_detail"))};
    }
    return screen == BoardScreen ? BoardRows(v) : StandingRows(v, identityPending);
}

std::vector<MenuEntry> RankingsPanel::StandingRows(const ShellView& v, bool identityPending) const {
    const auto& status = v.rankings;
    const auto* standing = Current(v);
    if (!standing) {
        const bool failed = status.bridge == bridge_ && !status.standing.error.empty() && !status.standing.loading;
        if (!failed) return {Card("rk-loading", loc::T("rankings.loading"), {}, StateCardHeight)};
        if (status.standing.error == "ranking_unavailable")
            return {Card("rk-unavailable", FailureText(status.standing.error), {}, StateCardHeight)};
        auto retry = Row("rk-error", FailureText(status.standing.error), {});
        retry.height = StateCardHeight; retry.hint = loc::T("public.try_again");
        return {retry};
    }
    const bool busy = pending_.has_value() || status.standing.loading || identityPending;
    const auto words = StandingWords(*standing);
    std::vector<MenuEntry> rows{Card("rk-standing", words.first, words.second, CardHeight)};
    const auto wait = ChangeWait(status);
    // Taking part or not: confirmed, since it holds for a day.
    const auto choice = [&](const char* id, const char* label, std::string detail) {
        auto row = ConfirmRow(id, loc::T(label), wait ? WaitText(wait) : std::move(detail), !busy && !wait);
        row.quiet = wait != 0;
        row.hint = loc::T(label);
        return row;
    };
    switch (standing->kind) {
    case Kind::NoAccount: {
        auto connect = Row(DiscordConnectScreen, loc::T("screen.connect_discord"), loc::T("rankings.connect_detail"));
        connect.value = loc::T("identity.discord_none"); connect.hint = loc::T("screen.connect_discord");
        rows.push_back(std::move(connect));
        break;
    }
    case Kind::OptedOut:
        rows.push_back(choice("rk-join", "rankings.take_part", loc::Tf("rankings.take_part_detail", ListingText(own_))));
        break;
    case Kind::Ranked: {
        if (Boardable(*standing)) {
            // The bridge's own listing, not the local name: what the board really shows.
            auto listed = InfoRow("rk-board-as", loc::T("rankings.board_as"),
                pending_ ? std::string(loc::T("public.updating")) : standing->listed ? standing->listing.name : std::string(), loc::T("rankings.board_as_detail"));
            rows.push_back(std::move(listed));
        }
        auto players = Row("rk-players", loc::T("rankings.players"), standing->classesShown ? loc::T("rankings.players_detail") : loc::T("rankings.hidden_detail"),
            standing->classesShown);
        players.quiet = !standing->classesShown; players.hint = loc::T("rankings.players");
        rows.push_back(std::move(players));
        rows.push_back(choice("rk-stop", "rankings.stop", loc::T("rankings.stop_detail")));
        break;
    }
    }
    auto how = Row("rk-how", loc::T("rankings.how"), loc::T("rankings.how_text"));
    how.reading = true; how.hint = loc::T("rankings.how");
    rows.push_back(std::move(how));
    return rows;
}

std::vector<MenuEntry> RankingsPanel::BoardRows(const ShellView& v) const {
    const auto& status = v.rankings;
    const auto* standing = Current(v);
    const auto* board = status.bridge == bridge_ && status.board.loaded ? status.board.value.get() : nullptr;
    std::vector<MenuEntry> rows;
    // You lead the board, whether or not it lists you: it says why not.
    if (standing) {
        const auto words = StandingWords(*standing);
        const bool own = board && std::any_of(board->begin(), board->end(), [](const netplay::rankings::BoardEntry& e) { return e.own; });
        const bool beyond = Boardable(*standing) && standing->badge.HasClass() && standing->badge.settled && standing->listed && board && !own;
        auto you = InfoRow("rkb-you", loc::T("rankings.you"), words.first,
            beyond ? loc::Tf("rankings.you_beyond_cap", netplay::rankings::MaxBoard) : words.second);
        you.height = HeaderHeight;
        rows.push_back(std::move(you));
    }
    if (standing && !standing->classesShown) { rows.push_back(Card("rkb-hidden", loc::T("rankings.hidden_detail"), {}, StateCardHeight)); return rows; }
    if (!board) {
        const bool failed = status.bridge == bridge_ && !status.board.error.empty() && !status.board.loading;
        if (!failed) { rows.push_back(Card("rkb-loading", loc::T("rankings.loading_board"), {}, StateCardHeight)); return rows; }
        auto retry = Row("rkb-error", FailureText(status.board.error), {});
        retry.height = StateCardHeight; retry.hint = loc::T("public.try_again");
        rows.push_back(std::move(retry));
        return rows;
    }
    if (board->empty()) { rows.push_back(Card("rkb-empty", loc::T("rankings.empty"), {}, StateCardHeight)); return rows; }
    // A header per class, highest first, then its players. Left and Right move
    // between headers, so any row is a few presses from the top.
    std::vector<std::size_t> headers;
    for (std::size_t i = 0; i < board->size(); ++i) {
        const auto band = (*board)[i].band;
        if (!i || (*board)[i - 1].band != band) {
            headers.push_back(rows.size());
            const auto count = std::count_if(board->begin() + i, board->end(), [&](const netplay::rankings::BoardEntry& e) { return e.band == band; });
            auto header = InfoRow("rkb-class:" + std::to_string(band), ClassName(band), std::to_string(count), loc::T("rankings.jump"));
            header.height = HeaderHeight; header.key = band;
            rows.push_back(std::move(header));
        }
        const auto& entry = (*board)[i];
        std::string detail = loc::Tf("class.tip", ClassName(entry.band));
        if (entry.listing.country.Present()) detail += "\n" + CountryName(entry.listing.country);
        auto player = InfoRow("rkb-p:" + std::to_string(i), entry.listing.name, {}, std::move(detail));
        player.userText = true; player.detailText = DetailText::Name; player.height = PlayerHeight; player.valueHighlighted = entry.own;
        player.left = rows[headers.back()].id; player.key = i;
        rows.push_back(std::move(player));
    }
    // Each row's Right is the next header; a header's Left is the previous one, the top one's You.
    for (std::size_t h = 0; h < headers.size(); ++h) {
        const std::string next = h + 1 < headers.size() ? rows[headers[h + 1]].id : std::string();
        const std::size_t end = h + 1 < headers.size() ? headers[h + 1] : rows.size();
        for (std::size_t r = headers[h]; r < end; ++r) rows[r].right = next;
        rows[headers[h]].left = h ? rows[headers[h - 1]].id : rows.front().id;
    }
    return rows;
}

bool RankingsPanel::Activate(const MenuAction& a, const ShellView& v, MenuNavigation& nav) {
    if (a.id == "rk-setup") { nav.Push("public-rooms"); return true; }
    if (a.id == "rk-error") { wantStanding_ = true; return true; }
    if (a.id == "rkb-error") { wantBoard_ = true; return true; }
    if (a.id == "rk-stop" || a.id == "rk-join") { if (!pending_) Choose(a.id == "rk-join"); return true; }
    if (a.id == "rk-players") {
        nav.Push(BoardScreen);
        // The board opens on your own row when the last one listed you.
        if (const auto* board = v.rankings.bridge == bridge_ ? v.rankings.board.value.get() : nullptr)
            for (std::size_t i = 0; i < board->size(); ++i)
                if ((*board)[i].own) { nav.Prefer("rkb-p:" + std::to_string(i)); break; }
        return true;
    }
    return false;
}

bool RankingsPanel::Shortcut(const MenuAction& a) {
    if (a.delta != MenuInput::Options || !Refreshable()) return false;
    (lastScreen_ == BoardScreen ? wantBoard_ : wantStanding_) = true;
    return true;
}

bool RankingsPanel::Status(const ShellView& v, const std::string& screen, std::string& status, Tone& tone) const {
    if (!Owns(screen) || bridge_.empty()) return false;
    const auto& rankings = v.rankings;
    const bool ours = rankings.bridge == bridge_;
    if (pending_) { status = loc::T("public.updating"); tone = Tone::Pending; return true; }
    if (screen == Screen) {
        if (!rankings.standing.loading) return false;
        status = loc::T(Current(v) ? "public.updating" : "rankings.loading");
        tone = Tone::Pending;
        return true;
    }
    const auto* board = ours && rankings.board.loaded ? rankings.board.value.get() : nullptr;
    if (rankings.board.loading) { status = loc::T(board ? "public.updating" : "rankings.loading_board"); tone = Tone::Pending; return true; }
    if (!status.empty() || !board || board->empty()) return false;
    status = loc::Tf("rankings.status_board", board->size(), PublicRoomsPanel::UpdatedText(rankings.AgeSeconds(rankings.board)));
    tone = Tone::Neutral;
    return true;
}

bool RankingsPanel::TakeSaid(std::string& text) {
    if (said_.empty()) return false;
    text = std::move(said_); said_.clear();
    return true;
}

// A choice already sent is kept: its answer is taken, and said, when Ember shows again.
void RankingsPanel::Conceal() {
    if (pending_ && !pending_->sent) pending_.reset();
    wantStanding_ = wantBoard_ = false; lastScreen_.clear();
}
} }
