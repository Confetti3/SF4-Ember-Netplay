// The tournament organizer, on the bracket site Ember's own service names:
// the invite that makes the player an organizer there, the tournaments they
// run, one tournament's sets as a queue or as its bracket, and the desk's
// actions on a set (call again, hold, decide or void a game). A tournament
// is made and set up here (desk-draft) and moved through its states as the
// site allows, starting its bracket among them. The reads repeat every few
// seconds while a desk screen is open.
#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "../common/Localization.hxx"
#include "../common/JsonDump.hxx"
#include "../common/LocalTime.hxx"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ctime>
#include <iterator>

namespace sf4e { namespace ui {
using netplay::IdentityOp;
using netplay::IdentityRequest;
namespace {
constexpr double DeskReadSeconds = 10;
constexpr const char* TournamentsPath = "/tournaments";
// The queue's filters, in the order Left/Right step through them.
constexpr const char* Filters[] = {"desk.filter.all", "desk.filter.action", "desk.filter.live"};
constexpr int FilterCount = 3;

bool Shown(const netplay::DeskSet& s, int filter) {
    switch (filter) {
    case 1: return s.state == "needs_review";
    case 2: return s.state == "playing" || s.state == "needs_review";
    default: return true;
    }
}
// The helper's or the site's word in the player's language, or as it came
// when this build does not know it.
std::string DeskWord(const std::string& code, std::initializer_list<std::pair<const char*, const char*>> words) {
    for (const auto& word : words) if (code == word.first) return loc::T(word.second);
    return code;
}
std::string StateText(const netplay::DeskSet& s) {
    return DeskWord(s.state.empty() ? "waiting" : s.state, {{"waiting", "desk.state.waiting"}, {"not_ready", "desk.state.not_ready"},
        {"playing", "desk.state.playing"}, {"needs_review", "desk.state.needs_review"}, {"done", "desk.state.done"},
        {"held", "desk.state.held"}});
}
std::string Name(const std::string& player) { return player.empty() ? std::string(loc::T("desk.tbd")) : player; }
std::string Players(const netplay::DeskSet& s) { return loc::Tf("desk.players", Name(s.players[0]), Name(s.players[1])); }
// Drawn games are replayed and never scored: said beside the score.
std::string Score(const netplay::DeskSet& s) {
    if (!s.scored) return {};
    const auto draws = std::count_if(s.games.begin(), s.games.end(),
        [](const netplay::DeskGame& g) { return g.state == "accepted" && g.outcome == "draw"; });
    return draws ? loc::Tf("desk.score_draws", s.score[0], s.score[1], static_cast<int>(draws)) : loc::Tf("desk.score", s.score[0], s.score[1]);
}
const netplay::DeskEvent* FindEvent(const netplay::IdentityView& id, const std::string& event) {
    const auto it = std::find_if(id.deskEvents.begin(), id.deskEvents.end(), [&](const netplay::DeskEvent& e) { return e.id == event; });
    return it == id.deskEvents.end() ? nullptr : &*it;
}
bool Allows(const netplay::DeskEvent& e, const char* action) {
    return std::find(e.actions.begin(), e.actions.end(), action) != e.actions.end();
}
// Before the bracket is made its settings may change.
bool Settable(const netplay::DeskEvent& e) {
    return e.status == "draft" || e.status == "planned" || e.status == "sign_ups" || e.status == "check_in";
}
std::string OnOff(bool on) { return loc::T(on ? "common.on" : "common.off"); }
std::string EventStatus(const std::string& status) {
    return DeskWord(status, {{"draft", "desk.site.draft"}, {"planned", "desk.site.planned"}, {"sign_ups", "desk.site.sign_ups"},
        {"check_in", "desk.site.check_in"}, {"live", "desk.site.live"}, {"finished", "desk.site.finished"},
        {"archived", "desk.site.archived"}, {"cancelled", "desk.site.cancelled"}});
}
// The site's moves this build has words for; starting the bracket has a row of its own.
constexpr std::pair<const char*, const char*> Moves[] = {
    {"plan", "desk.move.plan"}, {"open_sign_ups", "desk.move.open_sign_ups"}, {"close_sign_ups", "desk.move.close_sign_ups"},
    {"open_check_in", "desk.move.open_check_in"}, {"reopen_sign_ups", "desk.move.reopen_sign_ups"},
    {"archive", "desk.move.archive"}, {"cancel", "desk.move.cancel"}};
constexpr int FirstTo[] = {1, 2, 3, 4, 5, 7, 10};
// `at` moved by `days` on the player's calendar, the same clock time across
// a summer time change; 0 (not set) starts tomorrow at 20:00.
std::uint64_t DaysLater(std::uint64_t at, int days) {
    std::tm local = {};
    if (!LocalTm(at ? static_cast<std::time_t>(at) : std::time(nullptr), local)) return 0;
    local.tm_mday += at ? days : 1; local.tm_isdst = -1;
    if (!at) { local.tm_hour = 20; local.tm_min = 0; local.tm_sec = 0; }
    return static_cast<std::uint64_t>(std::mktime(&local));
}
}

bool IdentityPanel::DeskOffered(const ShellView& v) const {
    const auto* ember = EmberBridge(v);
    return ember && deskBridge_ == ember->id;
}

bool IdentityPanel::DeskScreen(const std::string& screen) {
    return screen == "tournament-desk" || screen == "desk-event" || screen == "desk-set" || screen == "desk-draft";
}

void IdentityPanel::DeskSend(const ShellView& v, const std::string& path, const nlohmann::json& command) {
    const auto* ember = EmberBridge(v);
    if (!ember) return;
    IdentityRequest r; r.op = IdentityOp::DeskAct; r.bridge = ember->id; r.target = path; r.command = DumpJson(command);
    message_.clear(); Queue(std::move(r));
}

const netplay::DeskSet* IdentityPanel::DeskSelected(const ShellView& v) const {
    if (v.identity.deskEvent != deskEvent_) return nullptr;
    const auto it = std::find_if(v.identity.deskSets.begin(), v.identity.deskSets.end(),
        [&](const netplay::DeskSet& s) { return s.id == deskSet_; });
    return it == v.identity.deskSets.end() ? nullptr : &*it;
}

void IdentityPanel::DeskPoll(const ShellView& v, const std::string& screen) {
    if (!DeskScreen(screen) || now_ < deskReadAt_ || sent_ || !queue_.empty()) return;
    const auto* ember = EmberBridge(v);
    if (!ember) return;
    deskReadAt_ = now_ + DeskReadSeconds;
    IdentityRequest read; read.op = IdentityOp::DeskRead; read.bridge = ember->id;
    // The tournaments (each one's state, settings and moves), and the sets
    // of the one open, with the games of the set open.
    if (screen != "desk-set") Queue(IdentityRequest(read));
    if (screen == "desk-event" || screen == "desk-set") {
        read.target = deskEvent_;
        if (screen == "desk-set") read.command = deskSet_;
        Queue(std::move(read));
    }
}

void IdentityPanel::DeskRows(const ShellView& v, const std::string& screen, std::vector<MenuEntry>& rows, std::string& title) const {
    const auto& id = v.identity;
    const bool busy = sent_ != 0 || !queue_.empty();
    if (screen == "tournament-desk") {
        title = loc::T("screen.tournament_desk");
        // Tournament Mode first: what it is, and the invite until it is redeemed.
        if (id.deskAccess.known) {
            const bool on = !id.deskAccess.inviteOnly || id.deskAccess.allowed;
            rows.push_back(InfoRow("dk-mode", loc::T("desk.mode"), loc::T(on ? "desk.mode_on" : "desk.mode_invite"),
                loc::T(on ? "desk.mode_on_detail" : "desk.mode_invite_detail")));
            if (!on) {
                rows.push_back(TextRow("dk-invite", loc::T("desk.invite_code"), {}, 11, !busy));
                return;
            }
            rows.push_back(Row("dk-new", loc::T("desk.draft.new"), loc::T("desk.draft.new_detail"), !busy));
        }
        for (const auto& event : id.deskEvents) {
            auto row = Row("dk-event:" + event.id, event.title,
                event.startsAt ? loc::Tf("desk.event.starts", LocalTime(event.startsAt)) : std::string(loc::T("common.not_set")));
            row.value = EventStatus(event.status);
            row.userText = true;
            rows.push_back(std::move(row));
        }
        if (id.deskAccess.known && id.deskEvents.empty())
            rows.push_back(InfoRow("dk-none", loc::T("desk.none"), {}, loc::T("desk.none_detail")));
        rows.push_back(Row("dk-open", loc::T("desk.open_browser"), loc::T("desk.open_browser_detail"), !busy));
        return;
    }
    if (screen == "desk-draft") { DraftRows(v, rows, title, busy); return; }
    const auto* event = FindEvent(id, deskEvent_);
    title = !event ? std::string(loc::T("screen.tournament_desk")) : event->title;
    const bool current = id.deskEvent == deskEvent_;
    if (screen == "desk-event") {
        if (event) {
            rows.push_back(InfoRow("dk-e-status", loc::T("desk.event.state"), EventStatus(event->status), loc::T("desk.site.detail")));
            rows.push_back(InfoRow("dk-e-signed", loc::T("desk.event.signed_up"), std::to_string(event->signedUp), loc::T("desk.event.signed_up_detail")));
            for (const auto& [key, text] : Moves)
                if (Allows(*event, key)) rows.push_back(ConfirmRow(std::string("dk-move:") + key, loc::T(text), loc::T("desk.move.detail"), !busy));
            if (Allows(*event, "start")) rows.push_back(ConfirmRow("dk-start", loc::T("desk.start"), loc::T("desk.start_detail"), !busy));
            if (Settable(*event)) rows.push_back(Row("dk-edit", loc::T("desk.event.edit"), loc::T("desk.event.edit_detail")));
        }
        rows.push_back(Value("dk-view", loc::T("desk.view"), loc::T(deskBracket_ ? "desk.view.bracket" : "desk.view.queue"), loc::T("desk.view_detail")));
        if (!deskBracket_)
            rows.push_back(Value("dk-filter", loc::T("desk.filter"), loc::T(Filters[deskFilter_]), loc::T("desk.filter_detail")));
        if (!current) { rows.push_back(InfoRow("dk-loading", loc::T("desk.loading"), {}, {})); return; }
        const auto setRow = [&](const netplay::DeskSet& s) {
            auto row = Row("dk-set:" + s.id, Players(s), s.roundLabel + "\n\n" + StateText(s));
            row.value = Score(s).empty() ? StateText(s) : Score(s);
            row.userText = true;
            return row;
        };
        if (!deskBracket_) {
            // What needs the organizer comes first, then the order the site gave.
            std::vector<const netplay::DeskSet*> shown;
            for (const auto& s : id.deskSets) if (Shown(s, deskFilter_)) shown.push_back(&s);
            std::stable_partition(shown.begin(), shown.end(), [](const netplay::DeskSet* s) { return s->state == "needs_review"; });
            for (const auto* s : shown) rows.push_back(setRow(*s));
            if (shown.empty()) rows.push_back(InfoRow("dk-empty", loc::T("desk.no_sets"), {}, {}));
            return;
        }
        // The bracket, round by round.
        std::vector<const netplay::DeskSet*> sorted;
        for (const auto& s : id.deskSets) sorted.push_back(&s);
        std::stable_sort(sorted.begin(), sorted.end(), [](const netplay::DeskSet* a, const netplay::DeskSet* b) { return a->round < b->round; });
        std::string heading;
        for (const auto* s : sorted) {
            if (s->roundLabel != heading) {
                heading = s->roundLabel;
                auto head = InfoRow("dk-round:" + std::to_string(rows.size()), heading, {}, {});
                head.userText = true; rows.push_back(std::move(head));
            }
            rows.push_back(setRow(*s));
        }
        if (sorted.empty()) rows.push_back(InfoRow("dk-empty", loc::T("desk.no_sets"), {}, {}));
        return;
    }
    // desk-set: the set, then what the organizer can do with it.
    const auto* s = current ? DeskSelected(v) : nullptr;
    if (!s) { rows.push_back(InfoRow("dk-gone", loc::T("desk.set_gone"), {}, {})); return; }
    auto players = InfoRow("dk-players", Players(*s), Score(*s), s->roundLabel);
    players.userText = true; rows.push_back(std::move(players));
    rows.push_back(InfoRow("dk-state", loc::T("desk.state"), StateText(*s), {}));
    rows.push_back(Row("dk-call", loc::T("desk.call"), loc::T("desk.call_detail"), !busy && !s->held));
    rows.push_back(ConfirmRow("dk-hold", loc::T(s->held ? "desk.release" : "desk.hold"),
        loc::T(s->held ? "desk.release_detail" : "desk.hold_detail"), !busy));
    // A set with an Ember match: each game the organizer can decide or void,
    // and a game they add when one was played outside Ember's count.
    if (s->matchId.empty() || s->revision.empty()) return;
    int number = 0;
    for (const auto& game : s->games) {
        ++number;
        if (game.state == "review") {
            for (int slot = 0; slot < 2; ++slot)
                rows.push_back(ConfirmRow("dk-win:" + std::to_string(slot) + ":" + game.attempt,
                    loc::Tf("desk.decide", number, Name(s->players[slot])), loc::T("desk.decide_detail"), !busy));
            rows.push_back(ConfirmRow("dk-draw:" + game.attempt, loc::Tf("desk.decide_draw", number), loc::T("desk.decide_draw_detail"), !busy));
        } else if (game.state == "accepted") {
            rows.push_back(ConfirmRow("dk-void:" + game.attempt, loc::Tf("desk.void", number), loc::T("desk.void_detail"), !busy));
        }
    }
    if (s->state != "done")
        for (int slot = 0; slot < 2; ++slot)
            rows.push_back(ConfirmRow("dk-add:" + std::to_string(slot), loc::Tf("desk.add", Name(s->players[slot])), loc::T("desk.add_detail"), !busy));
}

// A tournament's setup: its title first, which makes it; then its date,
// sets and check-in, each saved on the site as it is changed; then its
// Discord channel, set on the site's page.
void IdentityPanel::DraftRows(const ShellView& v, std::vector<MenuEntry>& rows, std::string& title, bool busy) const {
    const auto& id = v.identity;
    title = loc::T("desk.draft.new");
    const auto* e = FindEvent(id, deskEvent_);
    if (!e) {
        rows.push_back(TextRow("dk-d-title", loc::T("desk.draft.title"), {}, 60, !busy && deskEvent_.empty()));
        rows.push_back(InfoRow("dk-d-first", loc::T(deskEvent_.empty() ? "desk.draft.title_first" : "desk.loading"), {}, loc::T("desk.draft.saved")));
        return;
    }
    title = e->title;
    const bool can = !busy && Settable(*e);
    auto name = TextRow("dk-d-title", loc::T("desk.draft.title"), e->title, 60, can);
    name.userText = true; rows.push_back(std::move(name));
    const auto day = e->startsAt ? LocalTime(e->startsAt, "%Y-%m-%d") : std::string();
    rows.push_back(Value("dk-d-day", loc::T("desk.draft.date"), day, loc::T("desk.draft.date_detail"), can));
    if (!day.empty()) rows.push_back(Value("dk-d-time", loc::T("desk.draft.time"), LocalTime(e->startsAt, "%H:%M"), loc::T("desk.draft.time_detail"), can));
    rows.push_back(Value("dk-d-ft", loc::T("desk.draft.first_to"), loc::Tf("desk.draft.ft", e->gamesToWin), loc::T("desk.draft.first_to_detail"), can));
    rows.push_back(Value("dk-d-checkin", loc::T("desk.field.check_in"), OnOff(e->checkIn), {}, can));
    rows.push_back(InfoRow("dk-d-discord", loc::T("desk.draft.discord"), loc::T(e->webhook ? "desk.draft.discord_set" : "desk.draft.discord_unset"),
        loc::T("desk.draft.discord_detail")));
    if (e->status == "draft") rows.push_back(ConfirmRow("dk-d-delete", loc::T("desk.stop.draft"), {}, !busy));
    rows.push_back(Row("dk-d-open", loc::T("desk.draft.open_event"), loc::T("desk.draft.open_event_detail")));
}

bool IdentityPanel::DeskActivate(const MenuAction& a, const ShellView& v, MenuNavigation& nav) {
    if (a.id.compare(0, 9, "dk-event:") == 0) {
        deskEvent_ = a.id.substr(9); deskFilter_ = 0; deskBracket_ = false; deskReadAt_ = 0;
        const auto* e = FindEvent(v.identity, deskEvent_);
        nav.Push(e && e->status == "draft" ? "desk-draft" : "desk-event"); return true;
    }
    if (a.id == "dk-new") { deskEvent_.clear(); nav.Push("desk-draft"); return true; }
    if (a.id == "dk-edit") { nav.Push("desk-draft"); return true; }
    if (a.id == "dk-d-open") {
        // Back to the tournament when its setup was opened from there.
        if (nav.Parent() == "desk-event") nav.Return(); else nav.Replace("desk-event");
        deskReadAt_ = 0; return true;
    }
    const auto* ember = EmberBridge(v);
    if (a.id == "dk-d-delete" && ember) {
        IdentityRequest stop; stop.op = IdentityOp::DeskStop; stop.bridge = ember->id; stop.target = deskEvent_;
        message_.clear(); Queue(std::move(stop));
        while (nav.Screen() != "tournament-desk" && nav.Screen() != nav.Root()) nav.Replace({});
        return true;
    }
    const std::string event = std::string(TournamentsPath) + "/" + deskEvent_;
    if (a.id == "dk-start") { DeskSend(v, event + "/state", {{"action", "start"}}); return true; }
    if (a.id.compare(0, 8, "dk-move:") == 0) { DeskSend(v, event + "/state", {{"action", a.id.substr(8)}}); return true; }
    if (a.id.compare(0, 7, "dk-set:") == 0) { deskSet_ = a.id.substr(7); deskReadAt_ = 0; nav.Push("desk-set"); return true; }
    if (a.id == "dk-open" && ember) {
        IdentityRequest open; open.op = IdentityOp::DeskOpen; open.bridge = ember->id;
        message_.clear(); Queue(std::move(open));
        return true;
    }
    const auto* s = DeskSelected(v);
    if (!ember || !s) return a.id.compare(0, 3, "dk-") == 0;
    const std::string set = event + "/sets/" + s->id;
    if (a.id == "dk-call") { DeskSend(v, set + "/call", nlohmann::json::object()); return true; }
    if (a.id == "dk-hold") { DeskSend(v, set + "/hold", {{"held", !s->held}}); return true; }
    if (a.id.compare(0, 7, "dk-win:") != 0 && a.id.compare(0, 8, "dk-void:") != 0 && a.id.compare(0, 7, "dk-add:") != 0 &&
        a.id.compare(0, 8, "dk-draw:") != 0)
        return false;
    nlohmann::json command = {{"reason", "Decided at the tournament desk"}, {"expected_revision", s->revision}};
    if (a.id.compare(0, 8, "dk-void:") == 0) {
        command["kind"] = "void_game"; command["attempt_id"] = a.id.substr(8);
    } else if (a.id.compare(0, 8, "dk-draw:") == 0) {
        command["kind"] = "game_result"; command["draw"] = true; command["attempt_id"] = a.id.substr(8);
    } else {
        command["kind"] = "game_result"; command["winner_slot"] = a.id[7] - '0';
        if (a.id.compare(0, 7, "dk-win:") == 0) command["attempt_id"] = a.id.substr(9);
    }
    DeskSend(v, set + "/adjudications", command);
    return true;
}

bool IdentityPanel::DeskAccept(const MenuAction& a, const ShellView& v) {
    if (a.kind == MenuAction::TextAccepted && a.id == "dk-invite") {
        const auto* ember = EmberBridge(v);
        if (!ember || a.text.empty()) return true;
        IdentityRequest redeem; redeem.op = IdentityOp::DeskRedeem; redeem.bridge = ember->id; redeem.command = a.text;
        message_.clear(); Queue(std::move(redeem));
        return true;
    }
    if (a.kind == MenuAction::TextAccepted && a.id == "dk-d-title") {
        if (a.text.empty()) return true;
        // The first title makes the tournament; its answer is its ID.
        DeskSend(v, deskEvent_.empty() ? std::string(TournamentsPath) : std::string(TournamentsPath) + "/" + deskEvent_, {{"title", a.text}});
        return true;
    }
    if (a.kind != MenuAction::Adjust) return false;
    if (a.id.compare(0, 5, "dk-d-") == 0) {
        const auto* e = FindEvent(v.identity, deskEvent_);
        if (!e) return true;
        const auto change = [&](const nlohmann::json& command) { DeskSend(v, std::string(TournamentsPath) + "/" + deskEvent_, command); };
        const int step = a.delta > 0 ? 1 : -1;
        if (a.id == "dk-d-day") change({{"starts_at", DaysLater(e->startsAt, step)}});
        else if (a.id == "dk-d-time") change({{"starts_at", e->startsAt + step * 900}});
        else if (a.id == "dk-d-checkin") change({{"check_in", !e->checkIn}});
        else if (a.id == "dk-d-ft") {
            const auto at = std::find(std::begin(FirstTo), std::end(FirstTo), e->gamesToWin);
            const auto index = at == std::end(FirstTo) ? 1 : static_cast<int>(at - std::begin(FirstTo));
            const int count = static_cast<int>(std::size(FirstTo));
            change({{"games_to_win", FirstTo[(index + step + count) % count]}});
        }
        return true;
    }
    if (a.id == "dk-view") deskBracket_ = !deskBracket_;
    else if (a.id == "dk-filter") deskFilter_ = (deskFilter_ + (a.delta > 0 ? 1 : FilterCount - 1)) % FilterCount;
    else return false;
    return true;
}
} }
