// The tournament organizer: organizer access on each of Ember's own
// service's sites, the web desk (in this PC's browser, or approving another
// device), the events the organizer runs, an event's sets as a queue or as
// its bracket, and the desk's actions on a set (call again, hold, decide or
// void a game), and the organizer's own events made and run from here: a
// draft set up on its site and marked ready (desk-draft), its bracket
// started, and stopped (desk-stop). The reads repeat every few seconds
// while a desk screen is open.
#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "../common/Localization.hxx"
#include "../common/JsonDump.hxx"
#include "../common/LocalTime.hxx"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iterator>

namespace sf4e { namespace ui {
using netplay::IdentityOp;
using netplay::IdentityRequest;
namespace {
constexpr double DeskReadSeconds = 10;
constexpr const char* EventsPath = "/v1/organizer/events/";
// The queue's filters, in the order Left/Right step through them.
constexpr const char* Filters[] = {"desk.filter.all", "desk.filter.winners", "desk.filter.losers", "desk.filter.action", "desk.filter.live"};
constexpr int FilterCount = 5;

bool NeedsAction(const netplay::DeskSet& s) {
    return s.state == "needs_review" || s.state == "needs_organizer";
}
bool Shown(const netplay::DeskSet& s, int filter) {
    switch (filter) {
    case 1: return s.side == "winners";
    case 2: return s.side == "losers";
    case 3: return NeedsAction(s);
    case 4: return s.state == "playing";
    default: return true;
    }
}
// The bridge's word for a set or an event in the player's language, or as
// it came when this build does not know it.
std::string DeskWord(const std::string& code, std::initializer_list<std::pair<const char*, const char*>> words) {
    for (const auto& word : words) if (code == word.first) return loc::T(word.second);
    return code;
}
std::string StateText(const netplay::DeskSet& s) {
    return DeskWord(s.state.empty() ? "waiting" : s.state, {{"waiting", "desk.state.waiting"}, {"not_ready", "desk.state.not_ready"},
        {"called", "desk.state.called"}, {"playing", "desk.state.playing"}, {"needs_review", "desk.state.needs_review"},
        {"needs_organizer", "desk.state.needs_organizer"}, {"reporting", "desk.state.reporting"}, {"done", "desk.state.done"},
        {"held", "desk.state.held"}});
}
std::string SideText(const std::string& side) {
    return loc::T(side == "losers" ? "desk.side.losers" : side == "grand_final" ? "desk.side.grand_final" : "desk.side.winners");
}
// "Winners · Winners Semi-Final · Top 8", from what the site said.
std::string Position(const netplay::DeskSet& s) {
    std::string text = SideText(s.side);
    if (!s.roundLabel.empty()) text += " · " + s.roundLabel;
    if (!s.phase.empty()) text += " · " + s.phase;
    return text;
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
// Brackets read winners, losers, then the grand final, each by round.
int SideOrder(const std::string& side) { return side == "winners" ? 0 : side == "losers" ? 1 : 2; }
const netplay::DeskEvent* FindEvent(const netplay::IdentityView& id, const std::string& event) {
    const auto it = std::find_if(id.deskEvents.begin(), id.deskEvents.end(), [&](const netplay::DeskEvent& e) { return e.id == event; });
    return it == id.deskEvents.end() ? nullptr : &*it;
}
std::string OnOff(bool on) { return loc::T(on ? "common.on" : "common.off"); }
std::string EventStatus(const std::string& status) {
    return DeskWord(status, {{"draft", "desk.status.draft"}, {"ready", "desk.status.ready"}, {"running", "desk.status.running"}});
}
// The create form's settings this build has words for, and their values
// (as JSON text); the game offers only these, the rest stay as the site sets them.
constexpr std::pair<const char*, const char*> FieldWords[] = {
    {"format", "desk.field.format"}, {"grand_final", "desk.field.grand_final"}, {"third_place_match", "desk.field.third_place"},
    {"open_signup", "desk.field.open_signup"}, {"check_in", "desk.field.check_in"}};
constexpr std::pair<const char*, const char*> ValueWords[] = {
    {"\"single elimination\"", "desk.value.single"}, {"\"double elimination\"", "desk.value.double"},
    {"\"round robin\"", "desk.value.round_robin"}, {"\"swiss\"", "desk.value.swiss"}, {"\"\"", "desk.value.reset"},
    {"\"single match\"", "desk.value.one_match"}, {"\"skip\"", "desk.value.no_final"}, {"3", "desk.value.yes"},
    {"true", "desk.value.yes"}, {"false", "desk.value.no"}};
template<std::size_t N> const char* WordFor(const std::pair<const char*, const char*> (&words)[N], const std::string& key) {
    for (const auto& [code, word] : words) if (key == code) return word;
    return nullptr;
}
// A field's values this build can name.
std::vector<std::string> NamedValues(const std::vector<std::string>& values) {
    std::vector<std::string> named;
    for (const auto& value : values) if (WordFor(ValueWords, value)) named.push_back(value);
    return named;
}
constexpr int FirstTo[] = {1, 2, 3, 4, 5, 7, 10};
int GamesToWin(const netplay::DeskEvent& e) {
    const auto settings = nlohmann::json::parse(e.settings, nullptr, false);
    const auto games = settings.is_object() ? settings.value("rounds", nlohmann::json::object()).value("games_to_win", nlohmann::json()) : nlohmann::json();
    return games.is_number_integer() ? games.get<int>() : 2;
}
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
    return ember && sitesBridge_ == ember->id
        && std::any_of(sites_.begin(), sites_.end(), [](const netplay::IdentitySignInSite& s) { return s.organizer; });
}

bool IdentityPanel::DeskScreen(const std::string& screen) {
    return screen == "tournament-desk" || screen == "desk-event" || screen == "desk-set" || screen == "desk-device" ||
        screen == "desk-draft" || screen == "desk-stop";
}

std::vector<const netplay::IdentitySignInSite*> IdentityPanel::DeskSites(const ShellView& v) const {
    std::vector<const netplay::IdentitySignInSite*> held;
    const auto* ember = EmberBridge(v);
    for (const auto& site : sites_) {
        const auto found = ember ? accounts_.find({SignInSource::Site, site.id, ember->id}) : accounts_.end();
        if (site.organizer && found != accounts_.end() && found->second.organizer) held.push_back(&site);
    }
    return held;
}

const netplay::IdentitySignInSite* IdentityPanel::DraftSite(const ShellView& v) const {
    const auto held = DeskSites(v);
    const auto it = std::find_if(held.begin(), held.end(), [&](const auto* site) { return site->id == draft_.site; });
    return it != held.end() ? *it : held.empty() ? nullptr : held.front();
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
    if (!DeskScreen(screen) || screen == "desk-device" || now_ < deskReadAt_ || sent_ || !queue_.empty()) return;
    const auto* ember = EmberBridge(v);
    if (!ember) return;
    const auto* e = FindEvent(v.identity, deskEvent_);
    // A draft's service, once known, lists its events to pick from once.
    if (screen == "desk-draft" && e && e->tournament.empty()) {
        const auto* site = DraftSite(v);
        if (site && !site->create && draft_.listed != site->id) DeskSiteRead(v);
    }
    deskReadAt_ = now_ + DeskReadSeconds;
    IdentityRequest read; read.op = IdentityOp::DeskRead; read.bridge = ember->id;
    // The events (each one's state and settings), and the sets of the one
    // open; a running event's settings no longer change.
    if (screen != "desk-set" && !(screen == "desk-event" && e && e->status == "running")) Queue(IdentityRequest(read));
    if (screen == "desk-event" || screen == "desk-set") { read.target = deskEvent_; Queue(std::move(read)); }
    // The desk's first screen also says who is signed in on the web.
    if (screen == "tournament-desk") { IdentityRequest web; web.op = IdentityOp::DeskWeb; web.bridge = ember->id; Queue(std::move(web)); }
}

void IdentityPanel::DeskRows(const ShellView& v, const std::string& screen, std::vector<MenuEntry>& rows, std::string& title) const {
    const auto& id = v.identity;
    const bool busy = sent_ != 0 || !queue_.empty();
    if (screen == "desk-device") {
        // Another device's browser shows a code; it is typed here. The code
        // only signs in the browser that shows it, but sign-ins stay off stream.
        title = loc::T("desk.open_phone");
        rows.push_back(InfoRow("dk-offstream", loc::T("desk.offstream"), {}, loc::T("desk.offstream_detail")));
        if (const auto* ember = EmberBridge(v)) {
            auto at = InfoRow("dk-device-at", loc::T("desk.code_at"), ember->origin + "/desk", loc::T("desk.code_at_detail"));
            at.userText = true; rows.push_back(std::move(at));
        }
        rows.push_back(TextRow("dk-device-code", loc::T("desk.device_code"), {}, 9, !busy));
        // What asked with the code, and the number its page shows among two
        // others: picking it proves the code came from that page, not from
        // someone who sent the player a code of their own.
        const auto& asked = id.deskDevice;
        if (deskCode_.empty() || asked.choices.empty()) return;
        const auto minutes = asked.requestedAt ? (static_cast<std::uint64_t>(std::time(nullptr)) - std::min<std::uint64_t>(asked.requestedAt, std::time(nullptr))) / 60 : 0;
        auto who = InfoRow("dk-device-who", loc::T("desk.device_asked"), asked.device.empty() ? loc::T("desk.device_unknown") : asked.device,
            loc::Tf("desk.device_asked_detail", static_cast<int>(minutes)));
        who.userText = true; rows.push_back(std::move(who));
        for (const int number : asked.choices)
            rows.push_back(Row("dk-pick:" + std::to_string(number), loc::Tf("desk.device_pick", number), loc::T("desk.device_pick_detail"), !busy));
        return;
    }
    if (screen == "tournament-desk") {
        title = loc::T("screen.tournament_desk");
        // Tournament Mode first: what it is, and on a service that takes
        // organizers by invitation, the invite until it is redeemed.
        if (id.deskAccess.known) {
            const bool on = !id.deskAccess.inviteOnly || id.deskAccess.allowed;
            rows.push_back(InfoRow("dk-mode", loc::T("desk.mode"), loc::T(on ? "desk.mode_on" : "desk.mode_invite"),
                loc::T(on ? "desk.mode_on_detail" : "desk.mode_invite_detail")));
            if (!on) {
                rows.push_back(TextRow("dk-invite", loc::T("desk.invite_code"), {}, 11, !busy));
                return;
            }
        }
        // Organizer access, per site of Ember's own service: allow it, or
        // drop it; a site not linked yet is linked on its own screen first.
        const auto* ember = EmberBridge(v);
        for (const auto& site : sites_) {
            if (!site.organizer || !ember) continue;
            const auto found = accounts_.find({SignInSource::Site, site.id, ember->id});
            const bool linked = found != accounts_.end() && !found->second.user.empty();
            const auto name = SiteName(site.id);
            if (!linked) {
                rows.push_back(InfoRow("dk-org-link:" + site.id, loc::Tf("desk.org_site", name), loc::T("desk.org_unlinked"), loc::Tf("desk.org_unlinked_detail", name)));
            } else if (found->second.organizer) {
                auto held = ConfirmRow("dk-org-remove:" + site.id, loc::Tf("desk.org_site", name), loc::T("desk.org_remove_detail"), !busy);
                held.value = loc::T("site.organizer_held"); rows.push_back(std::move(held));
            } else {
                auto allow = Row("dk-org:" + site.id, loc::Tf("desk.org_site", name), loc::Tf("desk.org_allow_detail", name), !busy);
                allow.value = loc::T("desk.org_allow"); rows.push_back(std::move(allow));
            }
            rows.back().userText = true;
        }
        // The desk itself once any site grants organizer access.
        const bool held = !DeskSites(v).empty();
        if (!held && id.deskEvents.empty()) return;
        if (held) rows.push_back(Row("dk-new", loc::T("desk.draft.new"), loc::T("desk.draft.new_detail"), !busy));
        for (const auto& event : id.deskEvents) {
            auto row = Row("dk-event:" + event.id, event.title, loc::Tf("desk.event_detail", event.site));
            row.value = EventStatus(event.status);
            row.userText = true;
            rows.push_back(std::move(row));
        }
        if (id.deskEvents.empty())
            rows.push_back(InfoRow("dk-none", loc::T("desk.none"), {}, loc::T("desk.none_detail")));
        // The web desk from here: in this PC's browser, or approving another device.
        rows.push_back(Row("dk-open", loc::T("desk.open_browser"), loc::T("desk.open_browser_detail"), !busy));
        rows.push_back(Row("dk-phone", loc::T("desk.open_phone"), loc::T("desk.open_phone_detail"), !busy));
        // Web desk sessions: how many, the newest, and signing them all out.
        const auto& web = id.webSessions;
        rows.push_back(InfoRow("dk-web", loc::T("desk.web"), std::to_string(web.size()),
            web.empty() ? std::string(loc::T("desk.web_none")) : loc::Tf("desk.web_detail", web.front().browser, web.front().site)));
        if (!web.empty()) rows.push_back(ConfirmRow("dk-web-end", loc::T("desk.web_end"), loc::T("desk.web_end_detail"), !busy));
        return;
    }
    if (screen == "desk-draft") { DraftRows(v, rows, title, busy); return; }
    if (screen == "desk-stop") {
        const auto* e = FindEvent(id, deskEvent_);
        title = loc::T("desk.stop");
        if (!e) {
            rows.push_back(InfoRow("dk-stopped", loc::T("desk.stop.done"), {}, {}));
            rows.push_back(Row("dk-stop-home", loc::T("desk.stop.home"), {}));
            return;
        }
        title = loc::Tf("desk.stop.title", e->title);
        const auto name = SiteName(e->site);
        rows.push_back(InfoRow("dk-stop-what", loc::T("desk.stop.what"), {}, loc::T("desk.stop.what_detail")));
        if (e->createdOnSite)
            rows.push_back(ConfirmRow("dk-stop-site", loc::Tf("desk.stop.site", name), loc::Tf("desk.stop.site_detail", name), !busy));
        rows.push_back(ConfirmRow("dk-stop-keep", loc::T(e->tournament.empty() ? "desk.stop.draft" : "desk.stop.keep"),
            e->tournament.empty() ? std::string() : loc::Tf("desk.stop.keep_detail", name), !busy));
        rows.push_back(Row("dk-stop-back", loc::T("desk.stop.back"), {}));
        return;
    }
    const auto* event = FindEvent(id, deskEvent_);
    title = !event ? std::string(loc::T("screen.tournament_desk")) : event->title;
    const bool current = id.deskEvent == deskEvent_;
    if (screen == "desk-event") {
        if (event) {
            rows.push_back(InfoRow("dk-e-status", loc::T("desk.event.state"), EventStatus(event->status),
                event->startsAt ? loc::Tf("desk.event.starts", LocalTime(event->startsAt)) : std::string(loc::T("common.not_set"))));
            if (event->signUps)
                rows.push_back(InfoRow("dk-e-signed", loc::T("desk.event.signed_up"), std::to_string(event->signedUp), loc::T("desk.event.signed_up_detail")));
        }
        // The event's state on its site and the moves it allows now (the
        // bracket site says them; Challonge and start.gg do not).
        const auto& site = id.deskSiteState;
        const bool known = event && !site.state.empty() && site.tournament == event->tournament;
        if (known) {
            static const std::pair<const char*, const char*> states[] = {
                {"draft", "desk.site.draft"}, {"planned", "desk.site.planned"}, {"sign_ups", "desk.site.sign_ups"},
                {"check_in", "desk.site.check_in"}, {"live", "desk.site.live"}, {"finished", "desk.site.finished"},
                {"archived", "desk.site.archived"}, {"cancelled", "desk.site.cancelled"}};
            std::string word = site.state;
            for (const auto& [key, text] : states) if (site.state == key) word = loc::T(text);
            const auto name = SiteName(event->site);
            auto state = InfoRow("dk-site-state", loc::Tf("desk.site.on", name), word, loc::T("desk.site.detail"));
            state.userText = true; rows.push_back(std::move(state));
            static const std::pair<const char*, const char*> moves[] = {
                {"plan", "desk.move.plan"}, {"open_sign_ups", "desk.move.open_sign_ups"}, {"close_sign_ups", "desk.move.close_sign_ups"},
                {"open_check_in", "desk.move.open_check_in"}, {"reopen_sign_ups", "desk.move.reopen_sign_ups"},
                {"archive", "desk.move.archive"}, {"cancel", "desk.move.cancel"}};
            for (const auto& action : site.actions)
                for (const auto& [key, text] : moves)
                    if (action == key) rows.push_back(ConfirmRow("dk-move:" + action, loc::T(text), loc::Tf("desk.move.detail", name), !busy));
        }
        if (event) {
            // Starting the bracket, on a site that starts it from Ember: a site
            // with states allows it when its moves say so, greyed with the reason until then.
            if (event->startsOnSite && event->status != "draft") {
                const bool can = !known || std::find(site.actions.begin(), site.actions.end(), "start") != site.actions.end();
                rows.push_back(ConfirmRow("dk-start", loc::T("desk.start"), can ? loc::T("desk.start_detail") : loc::T("desk.start_not_yet"), !busy && can));
            }
            rows.push_back(Row("dk-edit", loc::T("desk.event.edit"), loc::T("desk.event.edit_detail")));
            rows.push_back(Row("dk-stop", loc::T("desk.stop"), loc::T("desk.stop_detail")));
        }
        rows.push_back(Value("dk-view", loc::T("desk.view"), loc::T(deskBracket_ ? "desk.view.bracket" : "desk.view.queue"), loc::T("desk.view_detail")));
        if (!deskBracket_)
            rows.push_back(Value("dk-filter", loc::T("desk.filter"), loc::T(Filters[deskFilter_]), loc::T("desk.filter_detail")));
        if (!current) { rows.push_back(InfoRow("dk-loading", loc::T("desk.loading"), {}, {})); return; }
        const auto setRow = [&](const netplay::DeskSet& s) {
            auto row = Row("dk-set:" + s.id, Players(s), Position(s) + "\n\n" + StateText(s));
            row.value = Score(s).empty() ? StateText(s) : Score(s);
            row.userText = true;
            return row;
        };
        if (!deskBracket_) {
            // What needs the organizer comes first, then the order the site gave.
            std::vector<const netplay::DeskSet*> shown;
            for (const auto& s : id.deskSets) if (Shown(s, deskFilter_)) shown.push_back(&s);
            std::stable_partition(shown.begin(), shown.end(), [](const netplay::DeskSet* s) { return NeedsAction(*s); });
            for (const auto* s : shown) rows.push_back(setRow(*s));
            if (shown.empty()) rows.push_back(InfoRow("dk-empty", loc::T("desk.no_sets"), {}, {}));
            return;
        }
        std::vector<const netplay::DeskSet*> sorted;
        for (const auto& s : id.deskSets) sorted.push_back(&s);
        std::stable_sort(sorted.begin(), sorted.end(), [](const netplay::DeskSet* a, const netplay::DeskSet* b) {
            if (SideOrder(a->side) != SideOrder(b->side)) return SideOrder(a->side) < SideOrder(b->side);
            return std::abs(a->round) < std::abs(b->round);
        });
        std::string heading;
        for (const auto* s : sorted) {
            const std::string round = SideText(s->side) + (s->roundLabel.empty() ? std::string() : " · " + s->roundLabel);
            if (round != heading) {
                heading = round;
                auto head = InfoRow("dk-round:" + std::to_string(rows.size()), round, {}, s->phase);
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
    auto players = InfoRow("dk-players", Players(*s), Score(*s), Position(*s));
    players.userText = true; rows.push_back(std::move(players));
    std::string called = s->calledAt ? loc::T(s->posted == "posted" ? "desk.called_posted" : s->posted == "failed" ? "desk.called_failed" :
        !s->posted.empty() ? "desk.called_waiting" : "desk.called") : loc::T("desk.not_called");
    rows.push_back(InfoRow("dk-state", loc::T("desk.state"), StateText(*s), called));
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
    if (s->state != "done" && s->state != "reporting")
        for (int slot = 0; slot < 2; ++slot)
            rows.push_back(ConfirmRow("dk-add:" + std::to_string(slot), loc::Tf("desk.add", Name(s->players[slot])), loc::T("desk.add_detail"), !busy));
}

// A draft, a section at a time: details, its service and rules, sign-ups
// and Discord, then ready. Each change is saved on the service as it is made.
void IdentityPanel::DraftRows(const ShellView& v, std::vector<MenuEntry>& rows, std::string& title, bool busy) const {
    const auto& id = v.identity;
    title = loc::T("desk.draft.new");
    const auto* e = FindEvent(id, deskEvent_);
    if (!e) {
        // A new tournament starts with its title; the rest is set once the draft exists.
        rows.push_back(TextRow("dk-d-title", loc::T("desk.draft.title"), {}, 60, !busy && deskEvent_.empty()));
        rows.push_back(InfoRow("dk-d-first", loc::T(deskEvent_.empty() ? "desk.draft.title_first" : "desk.loading"), {}, loc::T("desk.draft.saved")));
        return;
    }
    title = e->title;
    const auto heading = [&](const char* key) { rows.push_back(InfoRow(std::string("dk-h:") + key, loc::T(key), {}, {})); };
    heading("desk.draft.details");
    auto name = TextRow("dk-d-title", loc::T("desk.draft.title"), e->title, 60, !busy);
    name.userText = true; rows.push_back(std::move(name));
    const auto day = e->startsAt ? LocalTime(e->startsAt, "%Y-%m-%d") : std::string();
    rows.push_back(Value("dk-d-day", loc::T("desk.draft.date"), day,
        loc::T("desk.draft.date_detail"), !busy));
    if (!day.empty()) rows.push_back(Value("dk-d-time", loc::T("desk.draft.time"), LocalTime(e->startsAt, "%H:%M"), loc::T("desk.draft.time_detail"), !busy));

    heading("desk.draft.service");
    if (!e->tournament.empty()) {
        auto on = InfoRow("dk-d-on", loc::Tf("desk.site.on", SiteName(e->site)), e->name.empty() ? e->tournament : e->name, loc::T("desk.draft.on_detail"));
        on.userText = true; rows.push_back(std::move(on));
    } else if (const auto* site = DraftSite(v)) {
        const auto siteName = SiteName(site->id);
        auto pick = Value("dk-d-site", loc::T("desk.draft.site"), siteName,
            site->create ? loc::Tf("desk.draft.site_creates", siteName) : loc::Tf("desk.draft.site_pick", siteName), !busy);
        pick.userText = true; rows.push_back(std::move(pick));
        if (site->create) {
            for (const auto& [field, values] : *site->create) {
                if (!WordFor(FieldWords, field) || NamedValues(values).empty()) continue;
                const auto picked = draft_.fields.find(field);
                rows.push_back(Value("dk-d-field:" + field, loc::T(WordFor(FieldWords, field)),
                    picked == draft_.fields.end() ? loc::Tf("desk.draft.as_site", siteName) : std::string(loc::T(WordFor(ValueWords, picked->second))), {}, !busy));
            }
            rows.push_back(ConfirmRow("dk-d-create", loc::Tf("desk.draft.create", siteName), loc::T("desk.draft.create_detail"), !busy));
        } else {
            const auto& listed = id.deskSiteEvents;
            for (const auto& [event, eventName] : listed.site == site->id ? listed.events : decltype(listed.events){}) {
                auto row = Row("dk-d-pick:" + event, eventName, loc::Tf("desk.draft.pick_detail", siteName), !busy);
                row.userText = true; rows.push_back(std::move(row));
            }
            if (listed.site != site->id || listed.events.empty())
                rows.push_back(InfoRow("dk-d-none", loc::T("desk.draft.none_listed"), {}, loc::Tf("desk.draft.none_listed_detail", siteName)));
        }
    } else {
        rows.push_back(InfoRow("dk-d-nosite", loc::T("desk.draft.no_site"), {}, loc::T("desk.draft.no_site_detail")));
    }
    rows.push_back(Value("dk-d-ft", loc::T("desk.draft.first_to"), loc::Tf("desk.draft.ft", GamesToWin(*e)), loc::T("desk.draft.first_to_detail"), !busy));

    heading("desk.draft.people");
    if (!e->tournament.empty())
        rows.push_back(Value("dk-d-signups", loc::T("desk.draft.sign_ups"), OnOff(e->signUps), loc::T("desk.draft.sign_ups_detail"), !busy));
    rows.push_back(Value("dk-d-public", loc::T("desk.draft.public"), OnOff(e->isPublic), loc::T("desk.draft.public_detail"), !busy));
    rows.push_back(InfoRow("dk-d-discord", loc::T("desk.draft.discord"), loc::T(e->webhook ? "desk.draft.discord_set" : "desk.draft.discord_unset"),
        loc::T("desk.draft.discord_detail")));
    if (e->webhook)
        rows.push_back(Value("dk-d-results", loc::T("desk.draft.results"), OnOff(e->postResults), loc::T("desk.draft.results_detail"), !busy));

    heading("desk.draft.ready_step");
    if (e->status == "draft") {
        const bool can = !e->tournament.empty() && e->startsAt;
        rows.push_back(ConfirmRow("dk-d-ready", loc::T("desk.draft.ready"),
            can ? loc::Tf("desk.draft.ready_detail", LocalTime(e->startsAt)) : std::string(loc::T("desk.draft.ready_needs")), !busy && can));
        rows.push_back(Row("dk-stop", loc::T("desk.stop.draft"), {}));
    } else {
        rows.push_back(Row("dk-d-open", loc::T("desk.draft.open_event"), loc::T("desk.draft.open_event_detail")));
    }
}

void IdentityPanel::DeskSiteRead(const ShellView& v) {
    const auto* ember = EmberBridge(v);
    const auto* event = FindEvent(v.identity, deskEvent_);
    if (!ember || !event) return;
    IdentityRequest read; read.bridge = ember->id;
    if (!event->tournament.empty()) {
        read.op = IdentityOp::DeskSiteState; read.site = event->site; read.target = event->tournament;
    } else {
        // A draft not on a site yet: its service's events to pick from, on a
        // site where the organizer makes them.
        const auto* site = DraftSite(v);
        if (!site || site->create) return;
        read.op = IdentityOp::DeskSiteEvents; read.site = draft_.listed = site->id;
    }
    if (!read.site.empty()) Queue(std::move(read));
}

bool IdentityPanel::DeskActivate(const MenuAction& a, const ShellView& v, MenuNavigation& nav) {
    if (a.id.compare(0, 8, "dk-pick:") == 0) {
        const auto* ember = EmberBridge(v);
        if (!ember || deskCode_.empty()) return true;
        IdentityRequest approve; approve.op = IdentityOp::DeskApprove; approve.bridge = ember->id; approve.command = deskCode_;
        approve.number = static_cast<std::uint8_t>(std::atoi(a.id.c_str() + 8));
        message_.clear(); Queue(std::move(approve));
        return true;
    }
    if (a.id.compare(0, 9, "dk-event:") == 0) {
        deskEvent_ = a.id.substr(9); deskFilter_ = 0; deskBracket_ = false; deskReadAt_ = 0;
        draft_ = {};
        DeskSiteRead(v);
        const auto* e = FindEvent(v.identity, deskEvent_);
        nav.Push(e && e->status == "draft" ? "desk-draft" : "desk-event"); return true;
    }
    if (a.id == "dk-new") { deskEvent_.clear(); draft_ = {}; nav.Push("desk-draft"); return true; }
    if (a.id == "dk-edit") { nav.Push("desk-draft"); return true; }
    if (a.id == "dk-stop") { nav.Push("desk-stop"); return true; }
    if (a.id == "dk-stop-back") { nav.Return(); return true; }
    if (a.id == "dk-stop-home") {
        while (nav.Screen() != "tournament-desk" && nav.Screen() != nav.Root()) nav.Replace({});
        deskReadAt_ = 0; return true;
    }
    if (a.id == "dk-d-open") {
        // Back to the event when its draft screen was opened from there.
        if (nav.Parent() == "desk-event") nav.Return(); else nav.Replace("desk-event");
        deskReadAt_ = 0; DeskSiteRead(v); return true;
    }
    const std::string event = EventsPath + deskEvent_;
    if (a.id == "dk-stop-site" || a.id == "dk-stop-keep") {
        const auto* ember = EmberBridge(v);
        if (!ember) return true;
        IdentityRequest stop; stop.op = IdentityOp::DeskStop; stop.bridge = ember->id; stop.target = deskEvent_;
        stop.onSite = a.id == "dk-stop-site";
        message_.clear(); Queue(std::move(stop)); return true;
    }
    if (a.id == "dk-d-ready") { DeskSend(v, event + "/ready", nlohmann::json::object()); return true; }
    if (a.id == "dk-start") { deskMoved_ = true; DeskSend(v, event + "/start", nlohmann::json::object()); return true; }
    if (a.id.compare(0, 10, "dk-d-pick:") == 0) {
        if (const auto* site = DraftSite(v)) DeskSend(v, event, {{"site", site->id}, {"tournament", a.id.substr(10)}});
        return true;
    }
    if (a.id == "dk-d-create") {
        const auto* site = DraftSite(v);
        if (!site) return true;
        nlohmann::json fields = nlohmann::json::object();
        for (const auto& [field, value] : draft_.fields) fields[field] = nlohmann::json::parse(value, nullptr, false);
        DeskSend(v, event + "/create", {{"site", site->id}, {"fields", fields}});
        return true;
    }
    if (a.id.compare(0, 8, "dk-move:") == 0) {
        // A move of the event on its site, which needs the game's proof.
        deskMoved_ = true; DeskSend(v, event + "/transitions/" + a.id.substr(8), nlohmann::json::object());
        return true;
    }
    if (a.id.compare(0, 7, "dk-set:") == 0) { deskSet_ = a.id.substr(7); nav.Push("desk-set"); return true; }
    if (a.id.compare(0, 7, "dk-org:") == 0) { OrganizerAccess(v, a.id.substr(7), true); return true; }
    if (a.id.compare(0, 14, "dk-org-remove:") == 0) { OrganizerAccess(v, a.id.substr(14), false); return true; }
    if (a.id == "dk-phone") { message_.clear(); deskCode_.clear(); nav.Push("desk-device"); return true; }
    const auto* ember = EmberBridge(v);
    if (a.id == "dk-open" && ember) {
        IdentityRequest handoff; handoff.op = IdentityOp::DeskOpen; handoff.bridge = ember->id;
        message_.clear(); Queue(std::move(handoff));
        return true;
    }
    if (a.id == "dk-web-end" && ember) {
        IdentityRequest end; end.op = IdentityOp::DeskWebEnd; end.bridge = ember->id;
        message_.clear(); Queue(std::move(end)); return true;
    }
    const auto* s = DeskSelected(v);
    if (!ember || !s) return a.id.compare(0, 3, "dk-") == 0;
    std::string target;
    nlohmann::json command;
    if (a.id == "dk-call" || a.id == "dk-hold") {
        target = EventsPath + deskEvent_ + "/sets/" + s->id;
        command = a.id == "dk-call" ? nlohmann::json{{"call", true}} : nlohmann::json{{"held", !s->held}};
    } else if (a.id.compare(0, 7, "dk-win:") == 0 || a.id.compare(0, 8, "dk-void:") == 0 || a.id.compare(0, 7, "dk-add:") == 0 ||
               a.id.compare(0, 8, "dk-draw:") == 0) {
        target = EventsPath + deskEvent_ + "/matches/" + s->matchId + "/adjudications";
        command = {{"reason", "Decided at the tournament desk"}, {"expected_revision", s->revision}};
        if (a.id.compare(0, 8, "dk-void:") == 0) {
            command["kind"] = "void_game"; command["attempt_id"] = a.id.substr(8);
        } else if (a.id.compare(0, 8, "dk-draw:") == 0) {
            command["kind"] = "game_result"; command["draw"] = true; command["attempt_id"] = a.id.substr(8);
        } else {
            command["kind"] = "game_result"; command["winner_slot"] = a.id[7] - '0';
            if (a.id.compare(0, 7, "dk-win:") == 0) command["attempt_id"] = a.id.substr(9);
        }
    } else {
        return false;
    }
    DeskSend(v, target, command);
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
    if (a.kind == MenuAction::TextAccepted && a.id == "dk-device-code") {
        const auto* ember = EmberBridge(v);
        if (!ember || a.text.empty()) return true;
        // Read first: the player approves only once they see what asked.
        deskCode_ = a.text;
        IdentityRequest info; info.op = IdentityOp::DeskCodeInfo; info.bridge = ember->id; info.command = a.text;
        message_.clear(); Queue(std::move(info));
        return true;
    }
    if (a.kind == MenuAction::TextAccepted && a.id == "dk-d-title") {
        if (a.text.empty()) return true;
        // The first title makes the draft; its answer is the draft's ID.
        DeskSend(v, deskEvent_.empty() ? std::string("/v1/organizer/drafts") : EventsPath + deskEvent_, {{"title", a.text}});
        return true;
    }
    if (a.kind != MenuAction::Adjust) return false;
    if (a.id.compare(0, 5, "dk-d-") == 0) {
        const auto* e = FindEvent(v.identity, deskEvent_);
        if (!e) return true;
        const auto change = [&](const nlohmann::json& command) { DeskSend(v, EventsPath + deskEvent_, command); };
        const int step = a.delta > 0 ? 1 : -1;
        if (a.id == "dk-d-day") change({{"starts_at", DaysLater(e->startsAt, step)}});
        else if (a.id == "dk-d-time") change({{"starts_at", e->startsAt + step * 900}});
        else if (a.id == "dk-d-signups") change({{"sign_ups", !e->signUps}});
        else if (a.id == "dk-d-public") change({{"public", !e->isPublic}});
        else if (a.id == "dk-d-results") change({{"post_results", !e->postResults}});
        else if (a.id == "dk-d-ft") {
            // Only the first-to changes; the event's other settings stay as they are.
            auto settings = nlohmann::json::parse(e->settings, nullptr, false);
            if (!settings.is_object()) settings = nlohmann::json::object();
            const int games = GamesToWin(*e);
            const auto at = std::find(std::begin(FirstTo), std::end(FirstTo), games);
            const auto index = at == std::end(FirstTo) ? 1 : static_cast<int>(at - std::begin(FirstTo));
            const int count = static_cast<int>(std::size(FirstTo));
            if (!settings["rounds"].is_object()) settings["rounds"] = nlohmann::json::object();
            settings["rounds"]["games_to_win"] = FirstTo[(index + step + count) % count];
            change({{"settings", settings}});
        } else if (a.id == "dk-d-site") {
            const auto held = DeskSites(v);
            const auto* site = DraftSite(v);
            if (held.empty() || !site) return true;
            const auto at = static_cast<int>(std::find(held.begin(), held.end(), site) - held.begin());
            const int count = static_cast<int>(held.size());
            draft_.site = held[(at + step + count) % count]->id; draft_.fields.clear();
            DeskSiteRead(v);
        } else if (a.id.compare(0, 11, "dk-d-field:") == 0) {
            // As the site sets it, then each of the site's values in turn.
            const auto* site = DraftSite(v);
            if (!site || !site->create) return true;
            const auto field = a.id.substr(11);
            const auto found = std::find_if(site->create->begin(), site->create->end(), [&](const auto& f) { return f.first == field; });
            if (found == site->create->end()) return true;
            const auto values = NamedValues(found->second);
            if (values.empty()) return true;
            const auto picked = draft_.fields.find(field);
            const int count = static_cast<int>(values.size()) + 1;
            int at = 0;
            if (picked != draft_.fields.end()) at = static_cast<int>(std::find(values.begin(), values.end(), picked->second) - values.begin()) + 1;
            at = (at + step + count) % count;
            if (at == 0) draft_.fields.erase(field); else draft_.fields[field] = values[at - 1];
        }
        return true;
    }
    if (a.id == "dk-view") deskBracket_ = !deskBracket_;
    else if (a.id == "dk-filter") deskFilter_ = (deskFilter_ + (a.delta > 0 ? 1 : FilterCount - 1)) % FilterCount;
    else return false;
    return true;
}
} }
