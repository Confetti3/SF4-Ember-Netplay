#include "PublicRoomsPanel.hxx"
#include "ApplicationShell.hxx"
#include "IdentityRows.hxx"
#include "MenuRows.hxx"
#include "NetworkFeedback.hxx"
#include "PublicRoomsCards.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include "../common/TournamentLink.hxx"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <utility>

namespace sf4e { namespace ui {
namespace {
using netplay::publicrooms::Room;
using netplay::tournament::Command;
// The runtime answers every request, if only with a refusal after its own
// limit; this only keeps the panel from waiting on an answer that cannot come.
constexpr double AnswerSeconds = 65;
// A join that never leaves Idle (the runtime turned it down) is forgotten after this.
constexpr double JoinGraceSeconds = 5;
// Room cards: their height in unscaled pixels, and the narrower one for a narrow list.
constexpr float CardHeight = 100, NarrowCardHeight = 72;

bool Full(const Room& room) { return room.members >= room.capacity; }
bool Joinable(const Room& room) { return !room.locked && !Full(room); }
unsigned FreeSeats(const Room& room) { return room.capacity > room.members ? room.capacity - room.members : 0; }

// Rooms that can be joined come first, near ones before far ones when the
// region is known, then the most free seats, the most players, the oldest, and
// finally the ID so the order never wobbles. The bridge's own order breaks no
// ties that survive these, so it is the input only for a stable sort.
std::vector<const Room*> Sorted(const std::vector<Room>& rooms, const std::string& region) {
    std::vector<const Room*> sorted;
    for (const auto& room : rooms) sorted.push_back(&room);
    std::stable_sort(sorted.begin(), sorted.end(), [&](const Room* a, const Room* b) {
        if (Joinable(*a) != Joinable(*b)) return Joinable(*a);
        if (!region.empty() && (a->region == region) != (b->region == region)) return a->region == region;
        if (FreeSeats(*a) != FreeSeats(*b)) return FreeSeats(*a) > FreeSeats(*b);
        if (a->members != b->members) return a->members > b->members;
        if (a->createdAt != b->createdAt) return a->createdAt < b->createdAt;
        return a->id < b->id;
    });
    return sorted;
}

std::vector<const Room*> Listing(const std::vector<Room>& rooms, const std::string& region, PublicRoomsPanel::Filter filter) {
    auto listing = Sorted(rooms, region);
    const bool near = filter == PublicRoomsPanel::Filter::Near && !region.empty();
    listing.erase(std::remove_if(listing.begin(), listing.end(), [&](const Room* room) {
        return near ? room->region != region : filter == PublicRoomsPanel::Filter::Seats && !Joinable(*room);
    }), listing.end());
    return listing;
}

const char* FilterId(PublicRoomsPanel::Filter filter) {
    return filter == PublicRoomsPanel::Filter::Near ? "near" : filter == PublicRoomsPanel::Filter::Seats ? "seats" : "all";
}
const char* FilterName(PublicRoomsPanel::Filter filter) {
    return loc::T(filter == PublicRoomsPanel::Filter::Near ? "public.filter.near" :
        filter == PublicRoomsPanel::Filter::Seats ? "public.filter.seats" : "public.filter.all");
}

// How long ago the list was heard from the service.
std::string UpdatedText(double seconds) {
    const long long elapsed = seconds > 0 ? static_cast<long long>(seconds) : 0;
    if (elapsed < 10) return loc::T("public.updated_now");
    if (elapsed < 60) return loc::Tf("public.updated_seconds", elapsed);
    return loc::Tf("public.updated_minutes", elapsed / 60);
}

// What a room's row says when its details are known: who hosts it, where, and how full.
std::string RoomDetail(const Room& room) {
    if (room.hostName.empty()) {
        return room.playing > 0 ? loc::Tf("public.room_detail_playing", room.name, room.members, room.capacity, room.region, room.playing) :
            loc::Tf("public.room_detail", room.name, room.members, room.capacity, room.region);
    }
    std::string text = loc::Tf("public.room_detail2", room.hostName, PublicRoomsPanel::RegionLabel(room.region), room.members, room.capacity);
    if (room.playing > 0) text += loc::T("public.room_detail_match");
    if (room.locked) text += loc::T("public.room_detail_locked");
    else if (Full(room)) text += loc::T("public.room_detail_full");
    return text;
}

MenuEntry Wide(MenuEntry entry, float height = 0) {
    entry.wide = true; entry.height = height;
    return entry;
}
}

std::string PublicRoomsPanel::SetupLine(const PublicSetupView& setup) {
    const auto step = setup.step;
    if (setup.Running())
        return loc::T(step == PublicSetupStep::Creating ? "public.setup_step.creating" : step == PublicSetupStep::Finding ? "public.setup_step.finding" :
            step == PublicSetupStep::Trusting ? "public.setup_step.trusting" : "public.setup_step.checking");
    if (step == PublicSetupStep::Failed) return setup.failure.empty() ? std::string(loc::T("identity.failure.timeout")) : setup.failure;
    return {};
}

PublicRoomsPanel::CardPhase PublicRoomsPanel::PhaseOf(const std::string& id) const {
    if (id == "pr-opening") return CardPhase::Opening;
    const bool ticket = pending_ && pending_->command.op == Command::Op::RoomTicket;
    return ticket && id.compare(0, 8, "pr-room:") == 0 && id.compare(8, std::string::npos, pending_->command.roomId) == 0 ? CardPhase::Joining : CardPhase::Idle;
}

std::string PublicRoomsPanel::FailureText(const std::string& code) {
    static const std::pair<const char*, const char*> table[] = {
        {"room_full", "public.failure.room_full"}, {"banned", "public.failure.banned"},
        {"room_not_found", "public.failure.room_not_found"}, {"room_not_open", "public.failure.room_not_open"},
        {"room_limit", "public.failure.room_limit"},
        {"unsupported_build", "public.failure.unsupported_build"}, {"invalid_name", "public.failure.invalid_name"},
        {"rooms_unavailable", "public.failure.rooms_unavailable"},
        {"room_locked", "public.failure.room_locked"}, {"unavailable", "public.failure.unavailable"},
        // The service is the room service here, not an address the player typed.
        {"bridge_unreachable", "public.failure.unavailable"}, {"service_unavailable", "public.failure.unavailable"},
        {"untrusted_signature", "public.failure.untrusted"}, {"binding_mismatch", "public.failure.untrusted"},
        {"internal", "public.failure.internal"}, {"helper_unavailable", "public.failure.helper"},
    };
    for (const auto& entry : table) if (code == entry.first) return loc::T(entry.second);
    // Everything else is the helper's or the service's own: the same sentences the Ember ID screens use.
    return IdentityPanel::TournamentFailure(code);
}

std::string PublicRoomsPanel::RegionLabel(const std::string& code) {
    if (const char* name = RelayRegionName(code)) return name;
    std::string label = code;
    for (auto& c : label) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return label;
}

std::string PublicRoomsPanel::KnownRegion(const ShellView& v) {
    const std::string& relay = v.netReport.relay;
    return relay == "other" ? std::string() : relay;
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

bool PublicRoomsPanel::Join(const std::string& roomId) {
    if (Busy()) return false;
    Command command;
    command.op = Command::Op::RoomTicket;
    command.bridgeId = bridge_; command.roomId = roomId;
    Begin(std::move(command));
    said_.clear();
    return true;
}

void PublicRoomsPanel::Abandon() {
    pending_.reset();
    said_.clear();
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
    if (!Owns(screen) && screen != "create") { service_.clear(); refused_.clear(); }
    if (link_ && link_->direct && Owns(screen) && !Busy()) AskForLink(v, identityPending);
    bridge_ = service_.empty() ? bridge : service_;
    // A setup that just made the service usable shows the rooms without waiting for a refresh.
    if (Owns(screen) && !bridge_.empty() && !hadService_) wantList_ = true;
    hadService_ = Owns(screen) && !bridge_.empty();
    const bool opened = Owns(screen) && screen != lastScreen_;
    lastScreen_ = screen;
    if (opened) wantList_ = true;
    const auto& list = v.publicRooms;
    // Near means nothing without a region: the filter falls back to every room.
    if (filter_ == Filter::Near && KnownRegion(v).empty()) filter_ = Filter::All;
    // A refresh finished, answered or not: the next one by itself is due a while after it.
    if (list.listed != listedSeen_) {
        const bool settled = listedSeen_ != 0 || !list.loading;
        listedSeen_ = list.listed;
        manualRefresh_ = false;
        if (settled) loadedFor_ = list.bridge;
        if (list.error.empty()) listedAt_ = now;
        nextAuto_ = now + (list.error.empty() ? AutoRefreshSeconds : ErrorBackoffSeconds);
    }
    // A refresh's failure is said once; the runtime clears it when the next one is sent.
    // With nothing listed the error card says it instead.
    if (list.error != listError_) {
        listError_ = list.error;
        if (!listError_.empty() && Owns(screen) && !list.rooms.empty()) Say(FailureText(listError_));
    }
    // The room joined is the player's until the session is back to Idle, and Idle
    // before it has left it is only the join not having started yet.
    if (current_) {
        if (v.session.room != netplay::RoomState::Idle) current_->seen = true;
        else if (current_->seen || now - current_->at > JoinGraceSeconds) current_.reset();
    }
    // Only these screens show the wait and can act on its answer.
    if (pending_ && !Owns(screen) && screen != "create") pending_.reset();
    if (pending_ && pending_->sent) {
        if (list.answered == pending_->command.request) { const Command done = pending_->command; pending_.reset(); Joined(v, submit, done, now); }
        else if (now - pending_->sentAt > AnswerSeconds) { pending_.reset(); Say(FailureText("unavailable")); }
    }
    // The list is kept fresh while the player is looking at it and free: not while
    // a request is waiting, a room is being opened or entered, a link waits, or
    // a refresh is already on its way.
    if (Owns(screen) && !bridge_.empty() && !Busy() && !link_ && !list.loading && v.session.room == netplay::RoomState::Idle &&
        now >= nextAuto_) wantList_ = true;
    ShellAction action;
    action.command.generation = v.session.generation;
    if (pending_ && !pending_->sent) {
        // Beyond every identity the runtime has seen, so no earlier answer can pass for this one's.
        lastRequest_ = (std::max)({lastRequest_, list.answered, list.admitting}) + 1;
        pending_->command.request = lastRequest_;
        action.tournament = pending_->command;
        if (!submit(std::move(action))) { pending_.reset(); Say(loc::T("error.queue_failed")); return; }
        pending_->sent = true; pending_->sentAt = now;
    } else if (wantList_ && Owns(screen) && !bridge_.empty() && !list.loading && !(link_ && link_->direct)) {
        wantList_ = false;
        nextAuto_ = now + AutoRefreshSeconds;
        action.tournament.op = Command::Op::RoomList;
        action.tournament.bridgeId = bridge_;
        action.tournament.request = ++lastRequest_;
        if (!submit(std::move(action))) { manualRefresh_ = false; Say(loc::T("error.queue_failed")); }
    }
}

// The answer to a create or ticket: a refusal in words, or the admission to join with.
void PublicRoomsPanel::Joined(const ShellView& v, const Submit& submit, const Command& done, double now) {
    const auto& answer = v.publicRooms;
    if (!answer.failure.empty()) {
        // Full, locked and banned stay true for a while: Quick join does not offer that room again.
        if (done.op == Command::Op::RoomTicket && (answer.failure == "room_full" || answer.failure == "room_locked" || answer.failure == "banned"))
            refused_.insert(done.roomId);
        Say(FailureText(answer.failure));
        return;
    }
    ShellAction join;
    join.command.kind = netplay::CommandKind::JoinInvite;
    join.command.generation = v.session.generation;
    join.command.invitation = answer.admission.invitation;
    join.publicTicket = answer.admission.ticket;
    if (!submit(std::move(join))) { Say(loc::T("error.queue_failed")); return; }
    Current joined;
    joined.bridge = done.bridgeId;
    joined.kind = done.op == Command::Op::RoomCreate ? OpenKind::Create : OpenKind::Join;
    joined.room = answer.admission.room;
    joined.roomId = joined.kind == OpenKind::Create ? joined.room.id : done.roomId;
    // The list knows more of a room than its admission does.
    const auto listed = std::find_if(answer.rooms.begin(), answer.rooms.end(), [&](const Room& room) { return room.id == joined.roomId; });
    if (listed != answer.rooms.end() && answer.bridge == done.bridgeId) joined.room = *listed;
    joined.at = now;
    current_ = std::move(joined);
}

bool PublicRoomsPanel::TakeSaid(std::string& text) {
    if (said_.empty()) return false;
    text = std::move(said_); said_.clear();
    return true;
}

// Dropping the request is what abandons it: its answer, whenever it comes, is
// to a request the panel no longer holds. The runtime lets the next one replace it.
// A room already joined is not the panel's to drop: it is in it.
void PublicRoomsPanel::Conceal() {
    pending_.reset(); service_.clear(); wantList_ = false; lastScreen_.clear(); said_.clear();
    refused_.clear(); manualRefresh_ = false;
}

std::string PublicRoomsPanel::RoomLink() const {
    return current_ ? tournament_link::RoomPageUrl(current_->bridge, current_->roomId) : std::string();
}

double PublicRoomsPanel::OpenSeconds(const ShellView& v, const Room& room, double now) const {
    const auto& list = v.publicRooms;
    if (!list.listedAt || !room.createdAt || room.createdAt > list.listedAt) return -1;
    return static_cast<double>(list.listedAt - room.createdAt) + (std::max)(0.0, now - listedAt_);
}

bool PublicRoomsPanel::Refreshable(const ShellView& v) const {
    return Owns(lastScreen_) && !bridge_.empty() && v.session.room == netplay::RoomState::Idle;
}

bool PublicRoomsPanel::Shortcut(const MenuAction& a, const ShellView& v) {
    if (a.delta != MenuInput::Options || !Refreshable(v)) return false;
    if (!Busy() && !v.publicRooms.loading) Refresh();
    return true;
}

bool PublicRoomsPanel::Status(const ShellView& v, const std::string& screen, std::string& status, Tone& tone, double now) const {
    if (pending_) {
        status = loc::T(pending_->command.op == Command::Op::RoomTicket ? "room.joining_status" : "room.creating_status");
        tone = Tone::Pending; return true;
    }
    if (!Owns(screen)) return false;
    const auto& list = v.publicRooms;
    const bool listed = !bridge_.empty() && list.bridge == bridge_ && loadedFor_ == bridge_;
    // The first look at a service says it is loading; a refresh of a list already
    // shown says so only when the player asked for it.
    if (list.loading && !listed) { status = loc::T("public.loading"); tone = Tone::Pending; return true; }
    if (list.loading && manualRefresh_) { status = loc::T("public.updating"); tone = Tone::Pending; return true; }
    // A failed refresh leaves the list as it was, so the age still says how stale it is.
    if (!status.empty() || !listed || list.rooms.empty()) return false;
    const auto shown = Listing(list.rooms, KnownRegion(v), filter_).size();
    const std::string updated = UpdatedText(now - listedAt_);
    status = shown == list.rooms.size() ? loc::Tf("public.status_listed", list.rooms.size(), updated) :
        loc::Tf("public.status_filtered", shown, list.rooms.size(), updated);
    tone = Tone::Neutral;
    return true;
}

std::optional<std::string> PublicRoomsPanel::QuickPick(const ShellView& v, const std::string& region) const {
    if (v.publicRooms.bridge != bridge_) return std::nullopt;
    for (const Room* room : Sorted(v.publicRooms.rooms, region)) {
        if (!Joinable(*room) || refused_.count(room->id) || (!region.empty() && room->region != region)) continue;
        return room->id;
    }
    return std::nullopt;
}

std::vector<MenuEntry> PublicRoomsPanel::Rows(const ShellView& v, const std::string& bridge, bool identityPending, const PublicSetupView& setup,
    float listWidth) const {
    std::vector<MenuEntry> rows;
    const float cardHeight = listWidth < 480 * Scale() ? NarrowCardHeight : CardHeight;
    // A room being joined or created keeps its Stop row until it is open.
    if (v.session.room == netplay::RoomState::Opening) {
        const bool creating = current_ && current_->kind == OpenKind::Create;
        if (current_) {
            auto opening = InfoRow("pr-opening", current_->room.name, loc::T("public.connecting"), loc::T(creating ? "room.creating_status" : "room.joining_status"));
            opening.userText = true; opening.detailText = DetailText::Name; opening.quiet = true;
            rows.push_back(Wide(std::move(opening), cardHeight));
        }
        rows.push_back(Wide(ConfirmRow("cancel-open", loc::T(creating ? "room.stop_creating_action" : "room.stop_joining_action"),
            loc::T(creating ? "room.stop_creating" : "room.stop_joining"), true)));
        return rows;
    }
    const std::string& service = service_.empty() ? bridge : service_;
    if (service.empty()) {
        // The Ember ID panel runs the setup; its step is the card's line while it runs, and its failure the card's text after.
        const bool running = setup.Running();
        if (!running && (!v.identity.known || identityPending))
            rows.push_back(Wide(InfoRow("pr-checking", loc::T("public.setup_step.checking"), {}, loc::T("identity.state.checking_detail")), 120));
        else {
            const auto& id = v.identity;
            const bool needsId = !running && (setup.step == PublicSetupStep::NeedsId || id.state == "locked" || id.state == "recovery_required" ||
                (id.state == "disabled" && id.passphraseRequired));
            MenuEntry card;
            if (needsId) card = Row("pr-setup-id", loc::T("public.setup_needs_id"), loc::T("public.setup_needs_id_detail"));
            else if (running) {
                // Nothing to press while it works; the card says what it is doing.
                card = ConfirmRow("pr-setup", loc::T("public.setup"), loc::T("public.setup_detail"), false);
                card.value = SetupLine(setup); card.quiet = true;
            } else if (setup.step == PublicSetupStep::Failed) {
                // The player agreed to it already: trying again does not ask again.
                card = Row("pr-setup", loc::T("public.setup_retry"), loc::T("public.setup_detail"));
                card.value = SetupLine(setup);
            } else card = ConfirmRow("pr-setup", loc::T("public.setup"), loc::T("public.setup_detail"), !identityPending);
            rows.push_back(Wide(std::move(card), 150));
            rows.push_back(Wide(Row("identity", loc::T("screen.identity"), loc::T("home.identity_detail"))));
        }
        return rows;
    }
    const auto& list = v.publicRooms;
    const bool free = v.canOpenRoom && !Busy();
    const std::string region = KnownRegion(v);
    const Filter filter = filter_ == Filter::Near && region.empty() ? Filter::All : filter_;
    // The list on screen is this service's, and has been heard from.
    const bool listed = list.bridge == service && loadedFor_ == service;
    // The toolbar: four cells in a row (or two rows of two), then everything else full width.
    rows.push_back(Row("pr-quick", loc::T("public.quick"), loc::T("public.quick_detail"), free && listed));
    rows.back().hint = loc::T("public.quick");
    rows.push_back(Row("pr-create", loc::T("public.create_room"), loc::T("public.create_detail"), free));
    rows.back().hint = loc::T("public.create_room");
    rows.push_back(Row("pr-filter", loc::Tf("public.filter_label", FilterName(filter)), loc::T("public.filter_detail"), free));
    rows.back().hint = loc::T("public.filter");
    rows.back().choices = {
        {"all", loc::T("public.filter.all"), {}, true},
        {"near", loc::T("public.filter.near"), region.empty() ? std::string(loc::T("public.filter_near_unknown")) : std::string(), !region.empty()},
        {"seats", loc::T("public.filter.seats"), {}, true}};
    rows.back().chosen = FilterId(filter);
    rows.push_back(Row("pr-paste", loc::T("public.paste_link"), loc::T("public.paste_link_detail"), free));
    rows.back().hint = loc::T("menu.hint.paste");
    if (Busy()) rows.push_back(Wide(Row("pr-stop", loc::T("public.stop_joining"), loc::T("room.stop_joining"))));
    // A link that arrived while the player was busy waits here for their choice.
    if (link_) rows.push_back(Wide(Row("pr-link", loc::T("public.link_row"), loc::T("public.link_row_detail"), free)));
    if (!listed) {
        rows.push_back(Wide(InfoRow("pr-loading", loc::T("public.loading_card"), {}, {}), 3 * CardHeight + 16));
        return rows;
    }
    const auto shown = Listing(list.rooms, region, filter);
    for (const Room* room : shown) {
        auto row = Row("pr-room:" + room->id, room->name, RoomDetail(*room), free && Joinable(*room));
        row.userText = true; row.detailText = DetailText::Name; row.hint = loc::T("online.join"); row.quiet = true;
        if (PhaseOf(row.id) == CardPhase::Joining) row.value = loc::T("public.joining_value");
        rows.push_back(Wide(std::move(row), PublicRoomBare(*room) ? NarrowCardHeight : cardHeight));
    }
    if (shown.empty()) {
        if (!list.rooms.empty()) {
            // Rooms are open, but the filter hides them: this card shows them all.
            rows.push_back(Wide(Row("pr-show-all", loc::T("public.none_filtered"), {}), 110));
            rows.back().hint = loc::T("public.show_all");
        } else if (!list.error.empty()) {
            // Nothing is listed to keep on screen, so the failure is the card.
            rows.push_back(Wide(Row("pr-error", FailureText(list.error), {}, !list.loading), 110));
            rows.back().hint = loc::T("public.try_again");
        } else {
            rows.push_back(Wide(Row("pr-none", loc::T("public.none_all"), {}, free), 110));
            rows.back().hint = loc::T("public.create_room");
        }
    }
    return rows;
}

bool PublicRoomsPanel::Choose(const MenuAction& a) {
    if (a.id != "pr-filter") return false;
    if (a.text == "all") filter_ = Filter::All;
    else if (a.text == "near") filter_ = Filter::Near;
    else if (a.text == "seats") filter_ = Filter::Seats;
    return true;
}

bool PublicRoomsPanel::Activate(const MenuAction& a, MenuNavigation& nav) {
    if (a.id == "identity") { nav.Push("identity"); return true; }
    if (a.id == "pr-error") { Refresh(); return true; }
    if (a.id == "pr-stop") { Abandon(); return true; }
    // A filter hiding every open room: its card shows them all. (The empty list's
    // card, pr-none, is the shell's own Create.)
    if (a.id == "pr-show-all") { filter_ = Filter::All; said_.clear(); return true; }
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
    Join(a.id.substr(8));
    return true;
}
} }
