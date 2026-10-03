#include "PublicRoomsPanel.hxx"
#include "ApplicationShell.hxx"
#include "IdentityRows.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include "../common/TournamentLink.hxx"
#include <imgui.h>
#include <algorithm>
#include <utility>

namespace sf4e { namespace ui {
namespace {
using netplay::tournament::Command;
// The runtime answers every request, if only with a refusal after its own
// limit; this only keeps the panel from waiting on an answer that cannot come.
constexpr double AnswerSeconds = 65;

}

std::string PublicRoomsPanel::FailureText(const std::string& code) {
    static const std::pair<const char*, const char*> table[] = {
        {"room_full", "public.failure.room_full"}, {"banned", "public.failure.banned"},
        {"room_not_found", "public.failure.room_not_found"}, {"room_not_open", "public.failure.room_not_open"},
        {"room_limit", "public.failure.room_limit"},
        {"unsupported_build", "public.failure.unsupported_build"}, {"invalid_name", "public.failure.invalid_name"},
        {"rooms_unavailable", "public.failure.rooms_unavailable"},
    };
    for (const auto& entry : table) if (code == entry.first) return loc::T(entry.second);
    // Everything else is the helper's or the service's own: the same sentences the Ember ID screens use.
    return IdentityPanel::TournamentFailure(code);
}

void PublicRoomsPanel::Create(const std::string& name, int capacity) {
    Command command;
    command.op = Command::Op::RoomCreate;
    command.bridgeId = bridge_; command.roomName = name; command.capacity = capacity;
    Begin(std::move(command));
}

void PublicRoomsPanel::Begin(netplay::tournament::Command command) {
    if (pending_) return;
    Pending pending;
    pending.command = std::move(command);
    pending_ = std::move(pending);
}

void PublicRoomsPanel::OpenLink(const std::string& bridge, const std::string& room, bool direct) {
    link_ = Link{bridge, room, direct};
    said_.clear();
}

// The room a link named, once the Ember ID has answered: it needs the ID and a
// service the player trusts, and says which is missing. A link never makes a
// service trusted. The link is spent either way.
void PublicRoomsPanel::AskForLink(const ShellView& v, bool identityPending) {
    if (identityPending) return;
    const Link link = *link_;
    link_.reset();
    if (!v.identity.known) { Say(FailureText("helper_unavailable")); return; }
    if (v.identity.state != "ready") { Say(loc::T("public.needs_id_detail")); return; }
    if (!FindBridge(v, link.bridge)) { Say(loc::T("public.failure.link_service")); return; }
    // The list on screen is this service's, asked for after the ticket.
    service_ = link.bridge;
    wantList_ = true;
    Command command;
    command.op = Command::Op::RoomTicket;
    command.bridgeId = link.bridge; command.roomId = link.room;
    Begin(std::move(command));
    said_.clear();
}

void PublicRoomsPanel::Update(const ShellView& v, const std::string& screen, const std::string& bridge, bool identityPending,
    const Submit& submit, double now) {
    if (!Owns(screen) && screen != "create") service_.clear();
    if (link_ && link_->direct && Owns(screen) && !Busy()) AskForLink(v, identityPending);
    bridge_ = service_.empty() ? bridge : service_;
    const bool opened = Owns(screen) && screen != lastScreen_;
    lastScreen_ = screen;
    if (opened) wantList_ = true;
    // A refresh's failure is said once; the runtime clears it when the next one is sent.
    if (v.publicRooms.error != listError_) {
        listError_ = v.publicRooms.error;
        if (!listError_.empty() && Owns(screen)) Say(FailureText(listError_));
    }
    // Only these screens show the wait and can act on its answer.
    if (pending_ && !Owns(screen) && screen != "create") pending_.reset();
    if (pending_ && pending_->sent) {
        if (v.publicRooms.answered == pending_->command.request) { pending_.reset(); Joined(v, submit); }
        else if (now - pending_->sentAt > AnswerSeconds) { pending_.reset(); Say(FailureText("unavailable")); }
    }
    ShellAction action;
    action.command.generation = v.session.generation;
    if (pending_ && !pending_->sent) {
        // Beyond every identity the runtime has seen, so no earlier answer can pass for this one's.
        lastRequest_ = (std::max)({lastRequest_, v.publicRooms.answered, v.publicRooms.admitting}) + 1;
        pending_->command.request = lastRequest_;
        action.tournament = pending_->command;
        if (!submit(std::move(action))) { pending_.reset(); Say(loc::T("error.queue_failed")); return; }
        pending_->sent = true; pending_->sentAt = now;
    } else if (wantList_ && Owns(screen) && !bridge_.empty() && !v.publicRooms.loading && !(link_ && link_->direct)) {
        wantList_ = false;
        action.tournament.op = Command::Op::RoomList;
        action.tournament.bridgeId = bridge_;
        action.tournament.request = ++lastRequest_;
        if (!submit(std::move(action))) Say(loc::T("error.queue_failed"));
    }
}

// The answer to a create or ticket: a refusal in words, or the admission to join with.
void PublicRoomsPanel::Joined(const ShellView& v, const Submit& submit) {
    const auto& answer = v.publicRooms;
    if (!answer.failure.empty()) { Say(FailureText(answer.failure)); return; }
    ShellAction join;
    join.command.kind = netplay::CommandKind::JoinInvite;
    join.command.generation = v.session.generation;
    join.command.invitation = answer.admission.invitation;
    join.publicTicket = answer.admission.ticket;
    if (!submit(std::move(join))) Say(loc::T("error.queue_failed"));
}

bool PublicRoomsPanel::TakeSaid(std::string& text) {
    if (said_.empty()) return false;
    text = std::move(said_); said_.clear();
    return true;
}

// Dropping the request is what abandons it: its answer, whenever it comes, is
// to a request the panel no longer holds. The runtime lets the next one replace it.
void PublicRoomsPanel::Conceal() {
    pending_.reset(); link_.reset(); service_.clear(); wantList_ = false; lastScreen_.clear(); said_.clear();
}

bool PublicRoomsPanel::Status(const ShellView& v, const std::string& screen, std::string& status, Tone& tone) const {
    if (pending_) {
        status = loc::T(pending_->command.op == Command::Op::RoomTicket ? "room.joining_status" : "room.creating_status");
        tone = Tone::Pending; return true;
    }
    if (Owns(screen) && v.publicRooms.loading) { status = loc::T("public.loading"); tone = Tone::Pending; return true; }
    return false;
}

std::vector<MenuEntry> PublicRoomsPanel::Rows(const ShellView& v, const std::string& bridge, bool identityPending) const {
    std::vector<MenuEntry> rows;
    // A room being joined or created keeps its Stop row until it is open.
    if (v.session.room == netplay::RoomState::Opening) {
        rows.push_back(ConfirmRow("cancel-open", loc::T("room.stop_joining_action"), loc::T("room.stop_joining"), true));
        return rows;
    }
    const std::string& service = service_.empty() ? bridge : service_;
    if (service.empty()) {
        if (!v.identity.known || identityPending)
            rows.push_back(InfoRow("pr-checking", loc::T("screen.identity"), loc::T("identity.state.checking"), loc::T("identity.state.checking_detail")));
        else {
            rows.push_back(InfoRow("pr-needs-id", loc::T("public.needs_id"), {}, loc::T("public.needs_id_detail")));
            rows.push_back(Row("identity", loc::T("screen.identity"), loc::T("home.identity_detail")));
        }
        return rows;
    }
    const auto& list = v.publicRooms;
    const bool free = v.canOpenRoom && !Busy();
    // A link that arrived while the player was busy waits here for their choice.
    if (link_) rows.push_back(Row("pr-link", loc::T("public.link_row"), loc::T("public.link_row_detail"), free));
    rows.push_back(Row("pr-create", loc::T("public.create"), loc::T("public.create_detail"), free));
    rows.push_back(Row("pr-refresh", loc::T("identity.refresh"), loc::T("public.refresh_detail"), !list.loading));
    rows.push_back(Row("pr-paste", loc::T("public.paste_link"), loc::T("public.paste_link_detail"), free));
    rows.back().hint = loc::T("menu.hint.paste");
    if (list.bridge != service) return rows;
    for (const auto& room : list.rooms) {
        const std::string members = std::to_string(room.members) + "/" + std::to_string(room.capacity);
        const bool inMatch = room.playing > 0;
        auto row = Row("pr-room:" + room.id, room.name,
            inMatch ? loc::Tf("public.room_detail_playing", room.name, room.members, room.capacity, room.region, room.playing) :
                loc::Tf("public.room_detail", room.name, room.members, room.capacity, room.region), free);
        row.value = inMatch ? loc::Tf("public.room_playing", members) : members;
        if (pending_ && pending_->command.op == Command::Op::RoomTicket && pending_->command.roomId == room.id) row.value = loc::T("public.joining_value");
        row.userText = true; row.detailText = DetailText::Name; row.hint = loc::T("online.join");
        rows.push_back(std::move(row));
    }
    if (list.rooms.empty() && list.listed && !list.loading && list.error.empty())
        rows.push_back(InfoRow("pr-none", loc::T("public.none"), {}, loc::T("public.none_detail")));
    return rows;
}

bool PublicRoomsPanel::Activate(const MenuAction& a, MenuNavigation& nav) {
    if (a.id == "identity") { nav.Push("identity"); return true; }
    if (a.id == "pr-refresh") { wantList_ = true; said_.clear(); return true; }
    if (a.id == "pr-link") { if (link_ && !Busy()) link_->direct = true; return true; }
    if (a.id == "pr-paste") {
        // The room link a bot or site gave: its page or the ember: link.
        const char* text = ImGui::GetClipboardText();
        const auto link = tournament_link::ParseRoomPasted(text ? text : "");
        if (!link.Valid()) Say(loc::T("public.failure.link"));
        else if (!Busy()) OpenLink(link.bridgeId, link.roomId, true);
        return true;
    }
    if (a.id.compare(0, 8, "pr-room:") != 0) return false;
    if (Busy()) return true;
    Command command;
    command.op = Command::Op::RoomTicket;
    command.bridgeId = bridge_; command.roomId = a.id.substr(8);
    Begin(std::move(command));
    said_.clear();
    return true;
}
} }
