#include "PublicRoomsPanel.hxx"
#include "ApplicationShell.hxx"
#include "AreaNames.hxx"
#include "IdentityRows.hxx"
#include "MenuRows.hxx"
#include "PublicRoomsCards.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include "../common/TournamentLink.hxx"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>

namespace sf4e { namespace ui {
namespace {
using netplay::publicrooms::Room;
using netplay::publicrooms::RoomFilter;
using PublicList = netplay::publicrooms::Status;
using netplay::tournament::Command;
// The runtime answers every request, if only with a refusal after its own
// limit; this only keeps the panel from waiting on an answer that cannot come.
constexpr double AnswerSeconds = 65;
// A join that never leaves Idle (the runtime turned it down) is forgotten after this.
constexpr double JoinGraceSeconds = 5;
// Room cards: their height in unscaled pixels, and the narrower one for a narrow list.
constexpr float CardHeight = 100, NarrowCardHeight = 72;

unsigned FreeSeats(const Room& room) { return room.capacity > room.members ? room.capacity - room.members : 0; }

// How near a room is to the player in `home`: 2 in the same area, 1 next to it,
// 0 farther, without an area, or while the player's own is not set.
int Closeness(Area home, const Room& room) {
    return !AreaNear(home, room.area) ? 0 : room.area == home ? 2 : 1;
}

// Rooms that can be joined come first, then those in the player's own area,
// then those next to it, then the most free seats, the most players, the
// oldest, and finally the ID so the order never wobbles. The bridge's own order
// breaks no ties that survive these, so it is the input only for a stable sort.
bool Before(const Room& a, const Room& b, Area home) {
    if (a.Joinable() != b.Joinable()) return a.Joinable();
    if (Closeness(home, a) != Closeness(home, b)) return Closeness(home, a) > Closeness(home, b);
    if (FreeSeats(a) != FreeSeats(b)) return FreeSeats(a) > FreeSeats(b);
    if (a.members != b.members) return a.members > b.members;
    if (a.createdAt != b.createdAt) return a.createdAt < b.createdAt;
    return a.id < b.id;
}

// The order the last list was sorted into, as positions in its rooms, kept
// while the same panel asks again for the same refresh and area: the screen
// asks every frame, and the list changes once per refresh. `listed` is the
// refresh count; the size and a pass over the neighbours (no allocation, one
// comparison per room) guard against a list that was replaced without it.
struct SortedOrder {
    const void* owner = nullptr;
    std::uint64_t listed = 0;
    Area home = Area::None;
    std::vector<std::size_t> order;
};
SortedOrder sortedOrder;

std::vector<const Room*> Sorted(const void* owner, const PublicList& list, Area home) {
    const auto& rooms = list.rooms;
    auto& cached = sortedOrder;
    bool current = cached.owner == owner && cached.listed == list.listed && cached.home == home && cached.order.size() == rooms.size();
    for (std::size_t i = 1; current && i < cached.order.size(); ++i)
        if (Before(rooms[cached.order[i]], rooms[cached.order[i - 1]], home)) current = false;
    if (!current) {
        cached.owner = owner;
        cached.listed = list.listed;
        cached.home = home;
        cached.order.resize(rooms.size());
        std::iota(cached.order.begin(), cached.order.end(), std::size_t(0));
        std::stable_sort(cached.order.begin(), cached.order.end(), [&](std::size_t a, std::size_t b) {
            return Before(rooms[a], rooms[b], home);
        });
    }
    std::vector<const Room*> sorted;
    sorted.reserve(cached.order.size());
    for (std::size_t index : cached.order) sorted.push_back(&rooms[index]);
    return sorted;
}

std::vector<const Room*> Listing(const void* owner, const PublicList& list, Area home, const RoomFilter& filter) {
    auto listing = Sorted(owner, list, home);
    listing.erase(std::remove_if(listing.begin(), listing.end(), [&](const Room* room) { return !Matches(filter, *room, home); }), listing.end());
    return listing;
}

// The filters page's value rows, which Left and Right step; the list's Filters row opens the page,
// and Clear filters is a row on the page and a card on a list the filter has emptied.
constexpr const char *FilterAreaRow = "pr-filter-area", *AreaNearRow = "pr-area-near", *AreaAllRow = "pr-area-all",  *FilterTypeRow = "pr-filter-type", *FilterSeatsRow = "pr-filter-seats",
    *FilterWiredRow = "pr-filter-wired", *ClearFiltersCard = "pr-clear-filters", *ClearFiltersRow = "pr-filters-clear", *FiltersRow = "pr-filters";
const std::string AreaRowPrefix = "pr-area:";
constexpr float FiltersRowHeight = 40;

// The area filter in a few words: "All areas", "Near me", the one area's name or "3 areas".
std::string PlaceValue(const RoomFilter& filter) {
    if (filter.scope == RoomFilter::Place::All) return loc::T("public.filter_area_all");
    if (filter.scope == RoomFilter::Place::Near) return loc::T("public.filter_region_near");
    if (filter.areas.Count() > 1) return loc::Tf("public.filter_areas_count", filter.areas.Count());
    for (const auto& row : areas::Table)
        if (filter.areas.Has(row.area)) return AreaName(row.area);
    return {};
}

std::string RoomTypeValue(const RoomFilter& filter) {
    const auto wanted = WantedType(filter.type);
    return wanted ? std::string(RoomTypeName(*wanted)) : std::string(loc::T("public.filter_type_all"));
}

// What the list's Filters row says is on: "All rooms", or the settings that narrow it, such as "Near me · Classic · Wired only".
std::string FilterSummary(const RoomFilter& filter) {
    if (!filter.Active()) return loc::T("public.filter_summary_all");
    std::string text;
    const auto add = [&](const std::string& part) { text += (text.empty() ? "" : " \xC2\xB7 ") + part; };
    if (filter.scope != RoomFilter::Place::All) add(PlaceValue(filter));
    if (filter.type != RoomFilter::Type::All) add(RoomTypeValue(filter));
    if (filter.openSeats) add(loc::T("public.filter_summary_seats"));
    if (filter.wiredOnly) add(loc::T("public.filter_wired_only"));
    return text;
}

}

std::string PublicRoomsPanel::UpdatedText(double seconds) {
    const long long elapsed = seconds > 0 ? static_cast<long long>(seconds) : 0;
    if (elapsed < 10) return loc::T("public.updated_now");
    if (elapsed < 60) return loc::Tf("public.updated_seconds", elapsed);
    return loc::Tf("public.updated_minutes", elapsed / 60);
}

namespace {

// What a room's row says: who hosts it and how full it is, where its creator
// plays from when they said, and whether a match is on.
std::string RoomDetail(const Room& room) {
    const std::string area = room.area == Area::None ? std::string() : " " + loc::Tf("public.room_detail_area", AreaName(room.area));
    if (room.hostName.empty()) {
        std::string text = loc::Tf("public.room_detail_seats", room.name, room.members, room.capacity) + area;
        if (room.playing > 0) text += "\n" + loc::Tf("public.room_detail_matches", room.playing);
        return text;
    }
    std::string text = loc::Tf("public.room_detail_host", room.hostName, room.members, room.capacity) + area;
    if (room.playing > 0) text += loc::T("public.room_detail_match");
    if (room.locked) text += loc::T("public.room_detail_locked");
    else if (room.Full()) text += loc::T("public.room_detail_full");
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
        // The player's own limit, which waiting does not lift, apart from the room server's.
        {"room_limit_own_room", "public.failure.room_limit_own_room"}, {"room_limit_network", "public.failure.room_limit_network"},
        {"unsupported_build", "public.failure.unsupported_build"}, {"invalid_name", "public.failure.invalid_name"},
        {"rooms_unavailable", "public.failure.rooms_unavailable"},
        {"room_locked", "public.failure.room_locked"}, {"unavailable", "public.failure.unavailable"},
        {"own_delay_unsupported", "public.failure.own_delay_unsupported"},
        // The service is the room service here, not an address the player typed.
        {"bridge_unreachable", "public.failure.unavailable"}, {"service_unavailable", "public.failure.unavailable"},
        {"untrusted_signature", "public.failure.untrusted"}, {"binding_mismatch", "public.failure.untrusted"},
        {"internal", "public.failure.internal"}, {"helper_unavailable", "public.failure.helper"},
    };
    for (const auto& entry : table) if (code == entry.first) return loc::T(entry.second);
    // Everything else is the helper's or the service's own: the same sentences the Ember ID screens use.
    return IdentityPanel::TournamentFailure(code);
}

Area PublicRoomsPanel::HomeArea(const ShellView& v) {
    return OwnArea(v.preferences.area, v.automaticArea);
}

bool PublicRoomsPanel::AreasListed(const ShellView& v) {
    const auto& rooms = v.publicRooms.rooms;
    return std::any_of(rooms.begin(), rooms.end(), [](const Room& room) { return room.area != Area::None; });
}

Area PublicRoomsPanel::FilterHome(const ShellView& v) {
    return AreasListed(v) ? HomeArea(v) : Area::None;
}

void PublicRoomsPanel::Create(const std::string& name, int capacity, const netplay::TableSetup& tables, const room::Charter& charter) {
    if (pending_) return;
    Command command;
    command.op = Command::Op::RoomCreate;
    command.bridgeId = bridge_; command.roomName = name; command.capacity = capacity;
    command.ownDelay = charter.delayPolicy == room::DelayPolicy::Own;
    Begin(std::move(command));
    pending_->createdRules = tables;
    pending_->createdCharter = charter;
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
    // Moving between the list and its filters page is not opening the list.
    const bool opened = Owns(screen) && !Owns(lastScreen_);
    lastScreen_ = screen;
    if (opened) wantList_ = true;
    const auto& list = v.publicRooms;
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
        if (list.answered == pending_->command.request) { const Pending done = *pending_; pending_.reset(); Joined(v, submit, done, now); }
        else if (now - pending_->sentAt > AnswerSeconds) { pending_.reset(); Say(FailureText("unavailable")); }
    }
    // The list is kept fresh while the player is looking at it and free: not while
    // a request is waiting, a room is being opened or entered, a link waits, or
    // a refresh is already on its way.
    if (Owns(screen) && !bridge_.empty() && !Busy() && !link_ && !list.loading && v.session.room == netplay::RoomState::Idle &&
        now >= nextAuto_) wantList_ = true;
    commands::Tournament action;
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
void PublicRoomsPanel::Joined(const ShellView& v, const Submit& submit, const Pending& answered, double now) {
    const Command& done = answered.command;
    const auto& answer = v.publicRooms;
    if (!answer.failure.empty()) {
        // Full, locked and banned stay true for a while: Quick join does not offer that room again.
        if (done.op == Command::Op::RoomTicket && (answer.failure == "room_full" || answer.failure == "room_locked" || answer.failure == "banned"))
            refused_.insert(done.roomId);
        Say(FailureText(answer.failure));
        return;
    }
    commands::JoinRoom join;
    join.generation = v.session.generation;
    join.invitation = answer.admission.invitation;
    join.publicTicket = answer.admission.ticket;
    if (done.op == Command::Op::RoomCreate) {
        join.createdRules = answered.createdRules;
        join.charter = answered.createdCharter;
    }
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

bool PublicRoomsPanel::Status(const ShellView& v, const std::string& screen, std::string& status, Tone& tone, double now, const RoomFilter& filter) const {
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
    const auto shown = Listing(this, list, FilterHome(v), filter).size();
    const std::string updated = UpdatedText(now - listedAt_);
    status = shown == list.rooms.size() ? loc::Tf("public.status_listed", list.rooms.size(), updated) :
        loc::Tf("public.status_filtered", shown, list.rooms.size(), updated);
    tone = Tone::Neutral;
    return true;
}

std::optional<std::string> PublicRoomsPanel::QuickPick(const ShellView& v, Area home, const RoomFilter& filter) const {
    if (v.publicRooms.bridge != bridge_) return std::nullopt;
    // A room near the player first; a room that names no area is near no one, so the filter's own rooms follow.
    const RoomFilter nearFirst = filter.ForQuickJoin();
    const auto sorted = Sorted(this, v.publicRooms, home);
    for (const RoomFilter& wanted : {nearFirst, filter}) {
        for (const Room* room : sorted)
            if (room->Joinable() && !refused_.count(room->id) && Matches(wanted, *room, home)) return room->id;
        if (nearFirst == filter) break;
    }
    return std::nullopt;
}

std::vector<MenuEntry> PublicRoomsPanel::Rows(const ShellView& v, const std::string& bridge, bool identityPending, const PublicSetupView& setup,
    float listWidth, const RoomFilter& filter, bool unranked) const {
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
    const Area home = FilterHome(v);
    // The list on screen is this service's, and has been heard from.
    const bool listed = list.bridge == service && loadedFor_ == service;
    // The toolbar: three cells in a row (or two rows), then the filter's rows and everything else full width.
    rows.push_back(Row("pr-quick", loc::T("public.quick"), loc::T("public.quick_detail"), free && listed));
    rows.back().hint = loc::T("public.quick");
    rows.push_back(Row("pr-create", loc::T("public.create_room"), loc::T("public.create_detail"), free));
    rows.back().hint = loc::T("public.create_room");
    rows.push_back(Row("pr-paste", loc::T("public.paste_link"), loc::T("public.paste_link_detail"), free));
    rows.back().hint = loc::T("menu.hint.paste");
    if (Busy()) rows.push_back(Wide(Row("pr-stop", loc::T("public.stop_joining"), loc::T("room.stop_joining"))));
    // A link that arrived while the player was busy waits here for their choice.
    if (link_) rows.push_back(Wide(Row("pr-link", loc::T("public.link_row"), loc::T("public.link_row_detail"), free)));
    // The filter is one row; its page has the values.
    rows.push_back(Wide(Row(FiltersRow, loc::T("public.filters"), loc::T("public.filters_detail"), free), FiltersRowHeight));
    rows.back().value = FilterSummary(filter);
    // Above the rooms, and when there are none: games count for rank only with a Discord account linked.
    if (unranked) {
        auto notice = Row(RankedNoticeRow, loc::T("public.ranked_needs_discord"), loc::T("public.ranked_needs_discord_detail"));
        notice.hint = loc::T("screen.connect_discord");
        rows.push_back(Wide(std::move(notice), listWidth < 480 * Scale() ? 120 : 96));
    }
    if (!listed) {
        rows.push_back(Wide(InfoRow("pr-loading", loc::T("public.loading_card"), {}, {}), 3 * CardHeight + 16));
        return rows;
    }
    const auto shown = Listing(this, list, home, filter);
    for (const Room* room : shown) {
        auto row = Row("pr-room:" + room->id, room->name, RoomDetail(*room), free && room->Joinable());
        row.userText = true; row.detailText = DetailText::Name; row.hint = loc::T("online.join"); row.quiet = true;
        if (PhaseOf(row.id) == CardPhase::Joining) row.value = loc::T("public.joining_value");
        rows.push_back(Wide(std::move(row), PublicRoomBare(*room) ? NarrowCardHeight : cardHeight));
    }
    if (shown.empty()) {
        if (!list.rooms.empty()) {
            // Rooms are open, but the filter hides them: this card clears it.
            rows.push_back(Wide(Row(ClearFiltersCard, loc::T("public.none_filtered"), {}), 110));
            rows.back().hint = loc::T("public.clear_filters");
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

std::vector<MenuEntry> PublicRoomsPanel::FilterRows(const RoomFilter& filter, Area home, bool areasListed, bool enabled) {
    const bool nearUnknown = filter.scope == RoomFilter::Place::Near && home == Area::None;
    const bool nearUnlisted = filter.scope == RoomFilter::Place::Near && home != Area::None && !areasListed;
    std::vector<MenuEntry> rows;
    rows.push_back(Value(FilterAreaRow, loc::T("public.filter_area"), PlaceValue(filter),
        loc::T(nearUnknown ? "public.filter_near_unset" : nearUnlisted ? "public.filter_near_unlisted" : "public.filter_area_detail"), enabled));
    rows.back().opens = true; rows.back().hint = loc::T("public.filter_areas_pick"); rows.back().adjustHint = loc::T("public.filter_areas_quick");
    rows.push_back(Value(FilterTypeRow, loc::T("public.filter_type"), RoomTypeValue(filter), loc::T("public.filter_type_detail"), enabled));
    rows.push_back(Value(FilterSeatsRow, loc::T("public.filter_seats"), loc::T(filter.openSeats ? "common.on" : "common.off"),
        loc::T("public.filter_seats_detail"), enabled));
    rows.push_back(Value(FilterWiredRow, loc::T("public.filter_wired"), loc::T(filter.wiredOnly ? "public.filter_wired_only" : "public.filter_wired_all"),
        loc::T("public.filter_wired_detail"), enabled));
    rows.push_back(Row(ClearFiltersRow, loc::T("public.clear_filters"), loc::T("public.clear_filters_detail"), enabled && filter.Active()));
    return rows;
}

std::vector<MenuEntry> PublicRoomsPanel::AreaRows(const RoomFilter& filter, Area home, bool areasListed, bool enabled) {
    const auto onOff = [](bool on) { return loc::T(on ? "common.on" : "common.off"); };
    std::vector<MenuEntry> rows;
    rows.push_back(Value(AreaNearRow, loc::T("public.filter_region_near"), onOff(filter.scope == RoomFilter::Place::Near),
        loc::T(home == Area::None ? "public.filter_near_unset" : !areasListed ? "public.filter_near_unlisted" : "public.filter_near_detail"), enabled));
    for (const auto& row : areas::Table)
        rows.push_back(Value(AreaRowPrefix + row.code, AreaName(row.area), onOff(filter.areas.Has(row.area)),
            loc::Tf("public.filter_area_one_detail", AreaName(row.area)), enabled));
    rows.push_back(Row(AreaAllRow, loc::T("public.filter_areas_all"), loc::T("public.filter_areas_all_detail"), enabled && filter.scope != RoomFilter::Place::All));
    return rows;
}

bool PublicRoomsPanel::ChangeFilter(const MenuAction& a, Area home, RoomFilter& filter) {
    if (a.kind == MenuAction::Activate && (a.id == ClearFiltersCard || a.id == ClearFiltersRow)) { filter = RoomFilter(); return true; }
    if (a.kind == MenuAction::Activate && a.id == AreaAllRow) { filter.SetNear(false); return true; }
    if (a.kind != MenuAction::Adjust) return false;
    if (a.id == FilterAreaRow) filter.StepNear(home);
    else if (a.id == AreaNearRow) filter.SetNear(a.delta > 0);
    else if (a.id.compare(0, AreaRowPrefix.size(), AreaRowPrefix) == 0) filter.SetArea(AreaFromCode(a.id.substr(AreaRowPrefix.size())), a.delta > 0);
    else if (a.id == FilterTypeRow) filter.StepType(a.delta);
    else if (a.id == FilterSeatsRow) filter.openSeats = a.delta > 0;
    else if (a.id == FilterWiredRow) filter.wiredOnly = a.delta > 0;
    else return false;
    return true;
}

bool PublicRoomsPanel::Activate(const MenuAction& a, MenuNavigation& nav) {
    if (a.id == "identity") { nav.Push("identity"); return true; }
    if (a.id == FiltersRow) { nav.Push(FiltersScreen); return true; }
    if (a.id == FilterAreaRow) { nav.Push(AreasScreen); return true; }
    if (a.id == "pr-error") { Refresh(); return true; }
    if (a.id == "pr-stop") { Abandon(); return true; }
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
