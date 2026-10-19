// The tournament desk: the events the organizer runs through Ember's own
// service, an event's sets as a queue or as its bracket, and the desk's
// actions on a set (call again, hold, decide or void a game). Shown while the
// Ember ID holds organizer access on one of the service's sites. The reads
// repeat every few seconds while a desk screen is open.
#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "../common/Localization.hxx"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>

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
std::string Score(const netplay::DeskSet& s) { return s.scored ? loc::Tf("desk.score", s.score[0], s.score[1]) : std::string(); }
// Brackets read winners, losers, then the grand final, each by round.
int SideOrder(const std::string& side) { return side == "winners" ? 0 : side == "losers" ? 1 : 2; }
}

bool IdentityPanel::DeskHeld(const ShellView& v) const {
    const auto* ember = EmberBridge(v);
    return ember && std::any_of(accounts_.begin(), accounts_.end(), [&](const auto& entry) {
        return std::get<0>(entry.first) == SignInSource::Site && std::get<2>(entry.first) == ember->id && entry.second.organizer;
    });
}

bool IdentityPanel::DeskScreen(const std::string& screen) {
    return screen == "tournament-desk" || screen == "desk-event" || screen == "desk-set";
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
    if (screen != "tournament-desk") read.target = deskEvent_;
    Queue(std::move(read));
    // The desk's first screen also says who is signed in on the web.
    if (screen == "tournament-desk") { IdentityRequest web; web.op = IdentityOp::DeskWeb; web.bridge = ember->id; Queue(std::move(web)); }
}

void IdentityPanel::DeskRows(const ShellView& v, const std::string& screen, std::vector<MenuEntry>& rows, std::string& title) const {
    const auto& id = v.identity;
    const bool busy = sent_ != 0 || !queue_.empty();
    if (screen == "tournament-desk") {
        title = loc::T("screen.tournament_desk");
        for (const auto& event : id.deskEvents) {
            auto row = Row("dk-event:" + event.id, event.title, loc::Tf("desk.event_detail", event.site));
            row.value = DeskWord(event.status, {{"draft", "desk.status.draft"}, {"ready", "desk.status.ready"}, {"running", "desk.status.running"}});
            row.userText = true;
            rows.push_back(std::move(row));
        }
        if (id.deskEvents.empty())
            rows.push_back(InfoRow("dk-none", loc::T("desk.none"), {}, loc::T("desk.none_detail")));
        // Web desk sessions: how many, the newest, and signing them all out.
        const auto& web = id.webSessions;
        rows.push_back(InfoRow("dk-web", loc::T("desk.web"), std::to_string(web.size()),
            web.empty() ? std::string(loc::T("desk.web_none")) : loc::Tf("desk.web_detail", web.front().browser, web.front().site)));
        if (!web.empty()) rows.push_back(ConfirmRow("dk-web-end", loc::T("desk.web_end"), loc::T("desk.web_end_detail"), !busy));
        return;
    }
    const auto event = std::find_if(id.deskEvents.begin(), id.deskEvents.end(),
        [&](const netplay::DeskEvent& e) { return e.id == deskEvent_; });
    title = event == id.deskEvents.end() ? std::string(loc::T("screen.tournament_desk")) : event->title;
    const bool current = id.deskEvent == deskEvent_;
    if (screen == "desk-event") {
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
        } else if (game.state == "accepted") {
            rows.push_back(ConfirmRow("dk-void:" + game.attempt, loc::Tf("desk.void", number), loc::T("desk.void_detail"), !busy));
        }
    }
    if (s->state != "done" && s->state != "reporting")
        for (int slot = 0; slot < 2; ++slot)
            rows.push_back(ConfirmRow("dk-add:" + std::to_string(slot), loc::Tf("desk.add", Name(s->players[slot])), loc::T("desk.add_detail"), !busy));
}

bool IdentityPanel::DeskActivate(const MenuAction& a, const ShellView& v, MenuNavigation& nav) {
    if (a.id.compare(0, 9, "dk-event:") == 0) {
        deskEvent_ = a.id.substr(9); deskFilter_ = 0; deskBracket_ = false; deskReadAt_ = 0;
        nav.Push("desk-event"); return true;
    }
    if (a.id.compare(0, 7, "dk-set:") == 0) { deskSet_ = a.id.substr(7); nav.Push("desk-set"); return true; }
    const auto* ember = EmberBridge(v);
    if (a.id == "dk-web-end" && ember) {
        IdentityRequest end; end.op = IdentityOp::DeskWebEnd; end.bridge = ember->id;
        message_.clear(); Queue(std::move(end)); return true;
    }
    const auto* s = DeskSelected(v);
    if (!ember || !s) return a.id.compare(0, 3, "dk-") == 0;
    IdentityRequest r; r.op = IdentityOp::DeskAct; r.bridge = ember->id;
    nlohmann::json command;
    if (a.id == "dk-call" || a.id == "dk-hold") {
        r.target = EventsPath + deskEvent_ + "/sets/" + s->id;
        command = a.id == "dk-call" ? nlohmann::json{{"call", true}} : nlohmann::json{{"held", !s->held}};
    } else if (a.id.compare(0, 7, "dk-win:") == 0 || a.id.compare(0, 8, "dk-void:") == 0 || a.id.compare(0, 7, "dk-add:") == 0) {
        r.target = EventsPath + deskEvent_ + "/matches/" + s->matchId + "/adjudications";
        command = {{"reason", "Decided at the tournament desk"}, {"expected_revision", s->revision}};
        if (a.id.compare(0, 8, "dk-void:") == 0) {
            command["kind"] = "void_game"; command["attempt_id"] = a.id.substr(8);
        } else {
            command["kind"] = "game_result"; command["winner_slot"] = a.id[7] - '0';
            if (a.id.compare(0, 7, "dk-win:") == 0) command["attempt_id"] = a.id.substr(9);
        }
    } else {
        return false;
    }
    r.command = command.dump();
    message_.clear();
    Queue(std::move(r));
    return true;
}

bool IdentityPanel::DeskAccept(const MenuAction& a) {
    if (a.kind != MenuAction::Adjust) return false;
    if (a.id == "dk-view") deskBracket_ = !deskBracket_;
    else if (a.id == "dk-filter") deskFilter_ = (deskFilter_ + (a.delta > 0 ? 1 : FilterCount - 1)) % FilterCount;
    else return false;
    return true;
}
} }
