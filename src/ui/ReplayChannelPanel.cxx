#include "ReplayChannelPanel.hxx"
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "NameMarks.hxx"
#include "CountryFlag.hxx"
#include "PublicRoomsPanel.hxx"
#include "ReplaySummaryText.hxx"
#include "SignInSites.hxx"
#include "Theme.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplayProvenance.hxx"
#include <algorithm>
#include <iterator>

namespace sf4e { namespace ui {
namespace {
using netplay::channel::Sharing;
using netplay::tournament::Command;
constexpr float StateCardHeight = 120;

MenuEntry Card(std::string id, std::string label, std::string detail) {
    auto card = InfoRow(std::move(id), std::move(label), {}, std::move(detail));
    card.height = StateCardHeight;
    return card;
}

std::string FighterName(int id) {
    const auto* fighter = selection::FindFighter(id);
    return fighter ? fighter->name : loc::T("common.unavailable");
}

// A side as the row's detail names it: the player, their fighter and their class when they had one.
std::string SideText(const netplay::channel::Side& side) {
    std::string mark;
    switch (side.badge.kind) {
    case SkillBadge::Kind::Class: mark = ClassName(side.badge.band); break;
    case SkillBadge::Kind::Developer: mark = loc::T("class.developer"); break;
    case SkillBadge::Kind::Placing: mark = loc::T("rankings.placing"); break;
    default: break;
    }
    std::string text = mark.empty() ? loc::Tf("channel.side", side.name, FighterName(side.fighter))
        : loc::Tf("channel.side_class", side.name, FighterName(side.fighter), mark);
    if (side.country.Present()) text += " (" + CountryName(side.country) + ")";
    return text;
}

// The fighters the filter steps through, in the game's own roster order.
std::vector<int> FilterChoices() {
    std::vector<int> choices{-1};
    choices.insert(choices.end(), std::begin(selection::RosterDisplayOrder), std::end(selection::RosterDisplayOrder));
    return choices;
}
}

std::string ReplayChannelPanel::FailureText(const std::string& code) {
    if (code == "replay_channel_unavailable") return loc::T("channel.failure.unavailable");
    if (code == "replay_not_found") return loc::T("channel.failure.not_found");
    if (code == "no_discord_account") return loc::T("channel.connect_detail");
    return PublicRoomsPanel::FailureText(code);
}

const netplay::channel::Page* ReplayChannelPanel::Current(const ShellView& v) const {
    const auto& status = v.replayChannel;
    return !bridge_.empty() && status.bridge == bridge_ && status.page.loaded && status.fighter == fighter_ ? status.page.value.get() : nullptr;
}

void ReplayChannelPanel::Update(const ShellView& v, const std::string& screen, const std::string& bridge, const Submit& submit) {
    const auto& status = v.replayChannel;
    const bool owned = Owns(screen);
    // Opening the screen reads the channel, and the choice too while it says
    // no account: the player may have connected Discord since.
    if (owned && lastScreen_ != Screen) wantPage_ = wantSharing_ = true;
    lastScreen_ = screen;
    bridge_ = bridge;
    // A choice or download not sent yet is dropped on leaving; one on its way is still answered.
    if (!owned) { wantPage_ = wantMore_ = false; if (pending_ && !pending_->sent) pending_.reset(); }
    TakeAnswers(v, submit);
    if (bridge_.empty()) return;
    ShellAction action;
    action.command.generation = v.session.generation;
    action.tournament.bridgeId = bridge_;
    // The choice: the runtime reads it again by itself once it has the
    // service; the menu names the service, and on opening the screen or
    // Options reads it now while it says no account, since connecting Discord
    // changes it.
    const bool noAccount = status.sharing.loaded && status.sharing.value == Sharing::NoAccount;
    if (!owned || (status.sharing.loaded && !noAccount)) wantSharing_ = false;
    if (Introduce(v, submit) || (wantSharing_ && !status.sharing.loading && !status.choosing)) {
        if (wantSharing_ && status.bridge == bridge_) {
            wantSharing_ = false;
            auto read = action;
            read.tournament.op = Command::Op::ReplaySharing;
            submit(std::move(read));
        }
        return;
    }
    if (pending_ && !pending_->sent) {
        lastRequest_ = (std::max)({lastRequest_, status.chose, status.choosing, status.downloaded, status.downloading}) + 1;
        pending_->command.request = lastRequest_;
        action.tournament = pending_->command;
        if (!submit(std::move(action))) { pending_.reset(); Say(loc::T("error.queue_failed"), true); return; }
        pending_->sent = true;
        return;
    }
    if (!owned || (!wantPage_ && !wantMore_) || status.page.loading) return;
    action.tournament.op = Command::Op::ReplayList;
    action.tournament.fighter = fighter_;
    action.tournament.more = !wantPage_ && wantMore_;
    wantPage_ = wantMore_ = false;
    if (!submit(std::move(action))) Say(loc::T("error.queue_failed"), true);
}

// Names the usable service to the runtime with a read of the choice, once for
// each service and Ember ID: the runtime reads it again by itself from then on.
bool ReplayChannelPanel::Introduce(const ShellView& v, const Submit& submit) {
    const std::string scope = bridge_ + "\n" + v.identity.emberId;
    if (bridge_.empty() || v.replayChannel.bridge == bridge_ || introduced_ == scope) return false;
    introduced_ = scope;
    ShellAction read;
    read.command.generation = v.session.generation;
    read.tournament.op = Command::Op::ReplaySharing;
    read.tournament.bridgeId = bridge_;
    submit(std::move(read));
    return true;
}

void ReplayChannelPanel::Hidden(const ShellView& v, const std::string& bridge, const Submit& submit) {
    bridge_ = bridge;
    Introduce(v, submit);
}

void ReplayChannelPanel::TakeAnswers(const ShellView& v, const Submit& submit) {
    const auto& status = v.replayChannel;
    const bool ours = !bridge_.empty() && status.bridge == bridge_;
    if (status.page.finished != pageSeen_) {
        pageSeen_ = status.page.finished;
        if (ours && !status.page.error.empty() && status.page.loaded) Say(FailureText(status.page.error), true);
        // A filter changed while a page was on its way, which the runtime then
        // did not take: the page for the filter shown is asked for now.
        if (ours && status.page.error.empty() && status.fighter != fighter_ && Owns(lastScreen_)) wantPage_ = true;
    }
    if (!pending_ || !pending_->sent) return;
    const auto request = pending_->command.request;
    if (pending_->command.op == Command::Op::ReplayShare && status.chose == request) {
        if (!status.choiceFailure.empty()) Say(FailureText(status.choiceFailure), true);
        else if (status.sharing.loaded && status.sharing.value == Sharing::Off) Say(loc::T("channel.shared_off"), false);
        pending_.reset();
    } else if (pending_->command.op == Command::Op::ReplayDownload && status.downloaded == request) {
        const bool watch = pending_->watch;
        pending_.reset();
        if (!status.downloadFailure.empty()) { Say(FailureText(status.downloadFailure), true); return; }
        // The game plays it only from the main menu; otherwise it waits in the channel's folder.
        if (!watch || !v.replays.ready || v.session.room != netplay::RoomState::Idle) { Say(loc::T("channel.downloaded"), false); return; }
        ShellAction play;
        play.command.generation = v.session.generation;
        play.replay = {replay::Mode::Watch, status.downloadedFile};
        if (!submit(std::move(play))) Say(loc::T("error.queue_failed"), true);
    }
}

std::vector<MenuEntry> ReplayChannelPanel::Rows(const ShellView& v, bool identityPending, bool idle, std::string& title) const {
    title = loc::T("channel.title");
    const auto& status = v.replayChannel;
    if (bridge_.empty()) {
        // The channel lives on the service public rooms use: their setup makes it usable.
        if (!v.identity.known || identityPending)
            return {Card("rc-checking", loc::T("public.setup_step.checking"), loc::T("identity.state.checking_detail"))};
        return {Card("rc-needs-setup", loc::T("channel.needs_setup"), {}),
            Row("rc-setup", loc::T("screen.public_rooms"), loc::T("online.public_detail"))};
    }
    std::vector<MenuEntry> rows;
    const bool ours = status.bridge == bridge_;
    // A service without the channel has nothing to choose or list: its card
    // alone, with a short heading so the reason fits in every language.
    const auto missing = [&](const std::string& error, bool loaded) { return !loaded && error == "replay_channel_unavailable"; };
    if (ours && (missing(status.sharing.error, status.sharing.loaded) || missing(status.page.error, status.page.loaded)))
        return {Card("rc-unavailable", loc::T("common.unavailable"), FailureText("replay_channel_unavailable"))};
    // Sharing first: what happens to the player's own games.
    if (ours && status.sharing.loaded && status.sharing.value == Sharing::NoAccount) {
        auto connect = Row(DiscordConnectScreen, loc::T("screen.connect_discord"), loc::T("channel.connect_detail"));
        connect.value = loc::T("identity.discord_none"); connect.hint = loc::T("screen.connect_discord");
        rows.push_back(std::move(connect));
    } else if (ours && status.sharing.loaded) {
        const bool on = status.sharing.value == Sharing::On;
        std::string detail = loc::T("channel.share_detail");
        if (on && status.queued) detail += "\n\n" + loc::Tf("channel.queued", status.queued);
        rows.push_back(Value("rc-share", loc::T("channel.share"), loc::T(on ? "common.on" : "common.off"), std::move(detail),
            !pending_ && !status.choosing && !identityPending));
    } else {
        // Read again a while after a failure; until then it says so.
        const bool failed = ours && !status.sharing.loading && !status.sharing.error.empty();
        rows.push_back(InfoRow("rc-share-reading", loc::T("channel.share"), loc::T(failed ? "common.unavailable" : "public.updating"),
            failed ? FailureText(status.sharing.error) : std::string(loc::T("channel.share_detail"))));
    }
    rows.push_back(Value("rc-fighter", loc::T("channel.fighter"), fighter_ < 0 ? loc::T("channel.all_fighters") : FighterName(fighter_),
        loc::T("channel.fighter_detail")));
    const auto* page = Current(v);
    if (!page) {
        const bool failed = ours && !status.page.error.empty() && !status.page.loading;
        if (!failed) { rows.push_back(Card("rc-loading", loc::T("channel.loading"), {})); return rows; }
        auto retry = Row("rc-error", FailureText(status.page.error), {});
        retry.height = StateCardHeight; retry.hint = loc::T("public.try_again");
        rows.push_back(std::move(retry));
        return rows;
    }
    if (page->entries.empty()) { rows.push_back(Card("rc-empty", loc::T("channel.empty"), loc::T("channel.empty_detail"))); return rows; }
    const bool playable = idle && v.replays.ready;
    for (const auto& entry : page->entries) {
        const std::string names[2] = {entry.sides[0].name, entry.sides[1].name};
        const int fighters[2] = {entry.sides[0].fighter, entry.sides[1].fighter};
        auto row = Row("rc:" + entry.id, ReplayRowLabel(replayfiles::ReplayDateLabel(entry.playedAt), names, fighters), {}, !pending_);
        row.userText = true;
        for (const auto& name : names) NoteUserText(name);
        row.fighter1 = selection::FindFighter(fighters[0]) ? fighters[0] : -1;
        row.fighter2 = selection::FindFighter(fighters[1]) ? fighters[1] : -1;
        if (entry.winner >= 0) row.value = loc::Tf("channel.won", names[entry.winner]);
        row.detail = SideText(entry.sides[0]) + "\n" + SideText(entry.sides[1]) + "\n" +
            loc::T(entry.publicRoom ? "channel.public_room" : "channel.private_room") + "\n\n" +
            loc::T(playable ? "channel.watch_detail" : "channel.download_detail");
        row.detailText = DetailText::Name;
        row.hint = loc::T(playable ? "channel.watch" : "channel.download");
        rows.push_back(std::move(row));
    }
    if (page->more) rows.push_back(Row("rc-more", loc::T("channel.more"), loc::T("channel.more_detail"), !status.page.loading));
    return rows;
}

bool ReplayChannelPanel::Activate(const MenuAction& a, const ShellView& v, MenuNavigation& nav) {
    if (a.id == "rc-setup") { nav.Push("public-rooms"); return true; }
    if (a.id == "rc-error") { wantPage_ = true; return true; }
    if (a.id == "rc-more") { wantMore_ = true; return true; }
    if (a.id.compare(0, 3, "rc:") == 0) {
        if (pending_) return true;
        Pending pending;
        pending.command.op = Command::Op::ReplayDownload;
        pending.command.bridgeId = bridge_;
        pending.command.replayId = a.id.substr(3);
        pending.watch = v.replays.ready && v.session.room == netplay::RoomState::Idle;
        pending_ = std::move(pending);
        return true;
    }
    return false;
}

void ReplayChannelPanel::Accept(const MenuAction& a) {
    if (a.id == "rc-share") {
        if (pending_) return;
        Pending pending;
        pending.command.op = Command::Op::ReplayShare;
        pending.command.bridgeId = bridge_;
        pending.command.sharing = a.delta > 0;
        pending_ = std::move(pending);
    } else if (a.id == "rc-fighter") {
        Step(fighter_, FilterChoices(), a.delta);
        wantPage_ = true;
    }
}

bool ReplayChannelPanel::Shortcut(const MenuAction& a) {
    if (a.delta != MenuInput::Options || !Refreshable()) return false;
    wantPage_ = wantSharing_ = true;
    return true;
}

bool ReplayChannelPanel::Status(const ShellView& v, const std::string& screen, std::string& status, Tone& tone) const {
    if (!Owns(screen) || bridge_.empty()) return false;
    const auto& channel = v.replayChannel;
    if (pending_) {
        status = loc::T(pending_->command.op == Command::Op::ReplayDownload ? "channel.downloading" : "public.updating");
        tone = Tone::Pending;
        return true;
    }
    if (channel.page.loading) { status = loc::T(Current(v) ? "public.updating" : "channel.loading"); tone = Tone::Pending; return true; }
    const auto* page = Current(v);
    if (!status.empty() || !page || page->entries.empty()) return false;
    status = loc::Tf("channel.status", page->entries.size(), PublicRoomsPanel::UpdatedText(channel.AgeSeconds(channel.page)));
    tone = Tone::Neutral;
    return true;
}

bool ReplayChannelPanel::TakeSaid(std::string& text, bool& error) {
    if (said_.empty()) return false;
    text = std::move(said_); said_.clear(); error = saidError_;
    return true;
}

void ReplayChannelPanel::Conceal() {
    if (pending_ && !pending_->sent) pending_.reset();
    wantPage_ = wantMore_ = false; lastScreen_.clear();
}
} }
