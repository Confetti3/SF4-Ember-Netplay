// Friends (docs/design/FRIENDS.md, "Client"): the player's friend list on the
// bridge. The helper's friends_sync and friends_act go on the tournament
// channel and are answered by request id, like levels; netplay::friends::
// Requests decides what each answer does and when the next sync is due, and
// the interface reads the result from the published view (FriendsStatus).
//
// This file builds what friends are told of the player (the presence each
// sync carries), carries out the interface's commands, and goes to a friend:
// a public room by the ticket the Public rooms list's Join asks for, with a
// Watch of the friend's table once in; a private room's invitation through
// the Discord join's own path (discord::PendingInvite), which asks first when
// the player is busy. netplay::friends::Visit holds the waiting.
#include "sf4e__NetplayRuntime.hxx"
#include "../common/HexText.hxx"
#include "../netplay/FriendWire.hxx"
#include "../session/SocialAnswers.hxx"
#include "../ui/PublicRoomsPanel.hxx"

#include <ctime>
#include <functional>

namespace sf4e { namespace NetplayFacade {
namespace {
namespace friends = netplay::friends;
using nlohmann::json;

std::string CurrentId() { return runtime->room ? runtime->room->Identity().emberId : std::string(); }

const room::Snapshot& CurrentRoom() {
	static const room::Snapshot none;
	return runtime->attached && UserApp::netplay ? UserApp::netplay->client.GetRoomSnapshot() : none;
}

// What the player is doing now as friends are told, and the room that can be
// offered them. The description is discord::Describe's, made with friends'
// own inputs: shown whatever the Discord setting says, and with the room's
// invitation whatever the Discord setting for invitations says. A public
// room's invitation would admit nobody, and a match's room is never offered.
struct Presented {
	friends::Report report;
	friends::Offer offer;
};

Presented Present() {
	const auto control = runtime->controller.GetSnapshot();
	const auto& current = CurrentRoom();
	discord::PresenceInput input(control, current);
	input.ready = runtime->eventSystemReady;
	input.offline = runtime->offlineRequested || (input.ready && !AtMainMenu() && control.room == netplay::RoomState::Idle);
	input.show = true;
	input.invites = true;
	input.now = static_cast<std::uint64_t>(std::time(nullptr));
	friends::RoomIdentity identity;
	identity.isPublic = runtime->room && current.serverOwned;
	if (identity.isPublic) identity.publicId = HexLower(runtime->room->RoomId());
	else if (runtime->room && !netplay::tournament::HoldsMatchRoom(runtime->tournament.GetPhase(), runtime->watch.GetPhase())) {
		input.secret = runtime->room->DiscordInvitation();
		if (!discord::TicketMetadata(input.secret, input.party, input.expires)) input.secret.clear();
	}
	const auto described = discord::Describe(input);
	return {friends::ReportOf(described, current, identity), friends::OfferOf(described, identity)};
}

// What friends see of the player: the name and flag the board lists, and the main fighter.
friends::Listing OwnListing() {
	const auto& preferences = runtime->preferences;
	friends::Listing listing;
	listing.name = netplay::rankings::ListingName(preferences.displayName);
	listing.country = ShownCountry(preferences.country, runtime->automaticCountry).ToString();
	listing.main = preferences.mainFighter;
	return listing;
}

// The words for a code the helper or the bridge refused with, in the player's language.
const char* RefusalKey(const std::string& code) {
	static const std::pair<const char*, const char*> table[] = {
		{"unknown_code", "friends.unknown_code"}, {"friends_full", "friends.full"}, {"request_limit", "friends.request_limit"},
		{"card_expired", "friends.card_expired"}, {"own_card", "friends.own_card"}, {"code_too_soon", "friends.code_too_soon"},
		{"invite_unavailable", "friends.invite_unavailable"}, {"no_discord_account", "friends.no_account"},
		{"friends_unavailable", "friends.unavailable"},
	};
	for (const auto& entry : table) if (code == entry.first) return entry.second;
	return "friends.failed";
}

// Why a Join, a Watch or an invite leads nowhere.
const char* DestinationKey(friends::Destination::Refusal refusal, bool invite) {
	using Refusal = friends::Destination::Refusal;
	switch (refusal) {
	case Refusal::Gone: return invite ? "friends.invite_gone" : "friends.cannot_join";
	case Refusal::PrivateRoom: return "friends.private_invite_only";
	case Refusal::NotFighting: return "friends.cannot_watch";
	default: return "friends.cannot_join";
	}
}

// A sync's or an action's answer, for the flight it began (netplay::friends::Requests::Finish).
void TakeAnswer(std::uint64_t token, const session::TournamentAnswer& answer) {
	auto& requests = runtime->friends;
	const auto now = GetTickCount64();
	if (!answer.ok) { requests.Finish(token, netplay::BridgeFlights::Reason(answer), std::nullopt, now); return; }
	friends::State state;
	if (session::DecodeFriendsState(answer.data, state)) requests.Finish(token, {}, std::move(state), now);
	else requests.Finish(token, "bridge_invalid_response", std::nullopt, now);
}

// Hands `request` to the helper as the sync (`action` false) or the action in flight.
void Send(const json& request, bool action, bool helperReady, std::uint64_t nowMs) {
	auto& requests = runtime->friends;
	const auto token = action ? requests.BeginAction() : requests.BeginSync(nowMs);
	internal::SendFlight(request, helperReady, [token](const session::TournamentAnswer& answer) { TakeAnswer(token, answer); });
}

void SendAction(const commands::FriendAction& action, bool helperReady) {
	// The room offered is judged as the invite goes, not as the press was made.
	const bool invite = std::holds_alternative<commands::FriendInvite>(action);
	const auto request = friends::ActionRequest(action, invite ? Present().offer : friends::Offer());
	// Refused like an answer would be, so it is counted and the panel never reports it done.
	if (request.is_null()) { runtime->friends.Refuse("nothing_to_offer"); return; }
	Send(request, true, helperReady, GetTickCount64());
}

// A Join, a Watch or an invite: a private room's invitation goes the way a Discord one does, a public room's by a visit.
void GoToFriend(const std::string& bridge, const friends::Destination& where, bool watch, std::uint64_t generation) {
	const bool busy = BusyForInvitation(runtime->controller.GetSnapshot());
	if (where.kind == friends::Destination::Kind::PrivateInvite) {
		runtime->friendVisit.Cancel();
		std::string party;
		std::uint64_t expires = 0;
		if (!discord::TicketMetadata(where.secret, party, expires)) { runtime->error = loc::T("discord.invitation_invalid"); return; }
		runtime->discordInvite.OfferFromFriend(where.secret, party, expires, generation, busy);
		spdlog::info("Friends: invitation {}", busy ? "waits for an answer" : "pending");
		return;
	}
	runtime->discordInvite.Cancel();
	runtime->friendVisit.Start(bridge, where, generation, busy, GetTickCount64());
	spdlog::info("Friends: {} a public room, {}", watch ? "watching at" : "joining", busy ? "asking first" : "free to go");
}

// Where a Join, a Watch or a taken invite leads, or why nowhere; false when nowhere.
bool Resolve(const friends::Destination& where, bool invite) {
	if (where.Any()) return true;
	runtime->error = loc::T(DestinationKey(where.refusal, invite));
	return false;
}

// The player's answer to the question a visit asks, as the Discord one's is taken.
void AnswerVisit(const commands::FriendAnswerVisit& command) {
	auto& visit = runtime->friendVisit;
	if (!visit.Active() || command.revision != visit.Revision()) return;
	if (!command.switchRoom) { visit.Cancel(); return; }
	if (AtMainMenu() && CurrentGates().discordCanSwitch && !Game::Battle::System::ggpo &&
		visit.Confirm(command.generation.room, command.revision, GetTickCount64()))
		runtime->offlineRequested = false;
}

// The room ticket answered, for the visit it was asked for: join with it, or say why not. The ticket is asked
// for, decoded and refused as a public room's Join has it (PublicRooms.cxx), so its words are those.
void TakeTicket(std::uint64_t revision, const std::string& failure, std::optional<netplay::publicrooms::Admission> admission) {
	auto& visit = runtime->friendVisit;
	if (!visit.Ticketing(revision)) return;
	const auto now = GetTickCount64();
	if (admission) {
		commands::JoinRoom join;
		join.generation = runtime->controller.GetSnapshot().generation;
		join.invitation = admission->invitation;
		join.publicTicket = admission->ticket;
		if (SubmitRuntimeCommand(std::move(join))) { visit.Admitted(true, now); return; }
	}
	spdlog::info("Friends: room ticket not taken: {}", failure.empty() ? std::string("queue_failed") : failure);
	runtime->error = failure.empty() ? loc::T("error.queue_failed") : ui::PublicRoomsPanel::FailureText(failure);
	visit.Admitted(false, now);
}

// Whether the room this PC is in takes an action now: joined, healthy, writable.
bool RoomTakesActions(bool helperReady, const netplay::Snapshot& state, const room::Snapshot& current) {
	return helperReady && state.room == netplay::RoomState::Joined && state.control == netplay::Health::Healthy &&
		state.recovery == netplay::Recovery::None && (!state.coordinated || state.authorityWritable) && !current.closed &&
		current.roomEpoch && room::FindMember(current, current.localMember);
}

// The visit to a friend's public room: leave what the player said to leave, ask for the ticket, watch once in.
void TickVisit(bool helperReady, std::uint64_t nowMs) {
	auto& visit = runtime->friendVisit;
	if (!visit.Active()) return;
	// A Discord invitation that came meanwhile is the player's latest.
	if (runtime->discordInvite.Active()) { visit.Cancel(); return; }
	const auto control = runtime->controller.GetSnapshot();
	const auto& current = CurrentRoom();
	const auto gates = CurrentGates();
	friends::Surroundings here;
	here.nowMs = nowMs;
	here.generation = control.generation.room;
	here.safeMenu = AtMainMenu() && gates.discordCanSwitch && !Game::Battle::System::ggpo;
	here.idle = control.room == netplay::RoomState::Idle;
	here.canJoin = gates.canOpenRoom;
	if (runtime->room && current.serverOwned && control.room == netplay::RoomState::Joined) {
		here.roomId = HexLower(runtime->room->RoomId());
		here.canAct = RoomTakesActions(helperReady, control, current);
	}
	switch (visit.Tick(here)) {
	case friends::Visit::Step::Leave:
		// Another room replaces the one being watched, as Leave does.
		runtime->watch.Stop();
		SubmitRuntimeCommand(commands::LeaveRoom{control.generation, 0});
		visit.LeaveQueued();
		break;
	case friends::Visit::Step::Ticket:
		internal::RequestTicket(visit.Bridge(), visit.RoomId(), helperReady,
			[revision = visit.Revision()](const std::string& failure, std::optional<netplay::publicrooms::Admission> admission) {
				TakeTicket(revision, failure, std::move(admission));
			});
		break;
	case friends::Visit::Step::Watch: {
		const auto table = static_cast<std::size_t>(visit.WatchTable());
		room::Action action;
		action.kind = room::ActionKind::Watch;
		action.protocolVersion = room::ProtocolVersion;
		action.roomEpoch = current.roomEpoch;
		action.revision = current.revision;
		action.table = static_cast<std::uint8_t>(table);
		action.tableRevision = current.tables[table].revision;
		std::uint64_t actionId = 0;
		const bool sent = UserApp::netplay && UserApp::netplay->client.SendRoomAction(action, &actionId) == session::SendResult::Queued;
		spdlog::info("Friends: watch of table {} {}", table, sent ? "sent" : "not sent");
		if (!sent) runtime->error = loc::T("friends.cannot_watch");
		break;
	}
	case friends::Visit::Step::Ended:
		spdlog::info("Friends: visit ended without arriving");
		runtime->error = loc::T("friends.cannot_join");
		break;
	case friends::Visit::Step::EndedInRoom:
		// In the room already: the words of a room that could not be joined would be untrue.
		spdlog::info("Friends: visit ended in the room without a seat to watch from");
		runtime->error = loc::T("friends.visit_in_room");
		break;
	case friends::Visit::Step::None:
		break;
	}
}
}

namespace internal {
void DispatchFriends(const commands::FriendsVisit& command) {
	if (command.bridge.empty() || CurrentId().empty()) return;
	runtime->friends.Scope(command.bridge, CurrentId());
	runtime->friends.Visit(GetTickCount64());
}

void DispatchFriends(const commands::FriendsLeave& command) {
	if (command.bridge.empty() || CurrentId().empty()) return;
	runtime->friends.Scope(command.bridge, CurrentId());
	runtime->friends.Leave();
}

// One of the bridge's actions: sent now, or held for the sync or action in flight.
void DispatchFriends(commands::FriendAction action, bool helperReady) {
	const auto& bridge = commands::BridgeOf(action);
	if (bridge.empty()) return;
	auto& requests = runtime->friends;
	// No Ember ID to act as: refused, as an answer would be, so the panel is told.
	if (CurrentId().empty()) { requests.Refuse(netplay::BridgeFlights::Unavailable); return; }
	requests.Scope(bridge, CurrentId());
	if (requests.MayAct()) SendAction(action, helperReady);
	else if (!requests.Hold(std::move(action))) runtime->error = loc::T("friends.busy");
}

namespace {
// Made for the room the player saw: not for the one they are in by now.
bool Pressed(const netplay::Generation& pressed) { return runtime->controller.GetSnapshot().generation == pressed; }
}

void DispatchFriends(const commands::FriendJoin& command) {
	if (command.bridge.empty() || CurrentId().empty() || !Pressed(command.generation)) return;
	runtime->friends.Scope(command.bridge, CurrentId());
	const auto where = friends::ToFriend(*runtime->friends.Published().value, command.friendId, false);
	if (Resolve(where, false)) GoToFriend(command.bridge, where, false, command.generation.room);
}

void DispatchFriends(const commands::FriendWatch& command) {
	if (command.bridge.empty() || CurrentId().empty() || !Pressed(command.generation)) return;
	runtime->friends.Scope(command.bridge, CurrentId());
	const auto where = friends::ToFriend(*runtime->friends.Published().value, command.friendId, true);
	if (Resolve(where, false)) GoToFriend(command.bridge, where, true, command.generation.room);
}

void DispatchFriends(const commands::FriendTakeInvite& command) {
	if (command.bridge.empty() || CurrentId().empty() || !Pressed(command.generation)) return;
	runtime->friends.Scope(command.bridge, CurrentId());
	const auto where = friends::ToInvite(*runtime->friends.Published().value, command.invite, static_cast<std::uint64_t>(std::time(nullptr)));
	if (Resolve(where, true)) GoToFriend(command.bridge, where, false, command.generation.room);
}

void DispatchFriends(const commands::FriendAnswerVisit& command) {
	if (command.bridge.empty() || CurrentId().empty() || !Pressed(command.generation)) return;
	runtime->friends.Scope(command.bridge, CurrentId());
	AnswerVisit(command);
}

void TickSocial(bool helperReady, std::uint64_t nowMs) {
	auto& requests = runtime->friends;
	const auto id = CurrentId();
	// Another Ember ID's friends are not shown.
	if (!requests.Bridge().empty() && requests.EmberId() != id) requests.Scope({}, {});
	// The service is the one the interface names with its first command; until then the one levels are read from.
	if (requests.Bridge().empty() && !id.empty() && !runtime->levels.Bridge().empty()) requests.Scope(runtime->levels.Bridge(), id);
	requests.LapseVisit(nowMs);
	const auto presented = Present();
	runtime->friendsCanInvite = presented.offer.Any();
	requests.NotePresence(std::hash<std::string>()(presented.report.ToJson().dump()));
	if (auto held = requests.TakeHeld()) SendAction(*held, helperReady);
	else if (helperReady && requests.SyncDue(nowMs))
		Send(friends::SyncRequest(requests.Bridge(), sf4e::sidecarHash, OwnListing(), presented.report), false, helperReady, nowMs);
	TickVisit(helperReady, nowMs);
}

void SayFriendsGoodbye() {
	auto& requests = runtime->friends;
	if (!requests.Syncing() || requests.Published().value->kind != friends::State::Kind::Listed) return;
	friends::Report leaving;
	leaving.kind = friends::Report::Kind::Offline;
	internal::SendBridgeRequest(friends::SyncRequest(requests.Bridge(), sf4e::sidecarHash, OwnListing(), leaving));
}

friends::View FriendsStatus() {
	const auto& requests = runtime->friends;
	friends::View view;
	view.bridge = requests.Bridge();
	view.state = requests.Published();
	view.busy = requests.Busy();
	view.refusal = requests.Refusal();
	if (!view.refusal.empty()) view.refusalText = loc::T(RefusalKey(view.refusal));
	view.refusals = requests.Refusals();
	view.canInvite = runtime->friendsCanInvite;
	view.visiting = requests.Visiting();
	const auto& visit = runtime->friendVisit;
	view.visit.active = visit.Active();
	view.visit.confirm = visit.Confirming();
	view.visit.watch = visit.WatchTable() >= 0;
	view.visit.name = visit.Name();
	view.visit.revision = visit.Revision();
	return view;
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
