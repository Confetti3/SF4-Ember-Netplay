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

#include <ctime>
#include <functional>

namespace sf4e { namespace NetplayFacade {
namespace {
namespace friends = netplay::friends;
using friends::Command;
using nlohmann::json;
using internal::SendBridgeRequest;

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

void SendAction(const Command& command, bool helperReady) {
	// The room offered is judged as the invite goes, not as the press was made.
	const auto request = friends::ActionRequest(command, command.kind == Command::Kind::Invite ? Present().offer : friends::Offer());
	if (request.is_null()) { runtime->error = loc::T("friends.nothing_to_offer"); return; }
	runtime->friends.SentAction(helperReady ? SendBridgeRequest(request) : 0, command.kind, GetTickCount64());
}

// One of the bridge's actions: sent now, or held for the sync or action in flight.
void Act(const Command& command, bool helperReady) {
	auto& requests = runtime->friends;
	if (requests.MayAct()) SendAction(command, helperReady);
	else if (!requests.Hold(command)) runtime->error = loc::T("friends.busy");
}

// A Join, a Watch or an invite: a private room's invitation goes the way a Discord one does, a public room's by a visit.
void GoToFriend(const Command& command, std::uint64_t generation) {
	const auto& state = *runtime->friends.Published().value;
	const bool invite = command.kind == Command::Kind::TakeInvite;
	const auto where = invite ? friends::ToInvite(state, command.id, static_cast<std::uint64_t>(std::time(nullptr))) :
		friends::ToFriend(state, command.id, command.kind == Command::Kind::Watch);
	if (!where.Any()) { runtime->error = loc::T(DestinationKey(where.refusal, invite)); return; }
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
	runtime->friendVisit.Start(command.bridgeId, where, generation, busy, GetTickCount64());
	spdlog::info("Friends: {} a public room, {}", command.kind == Command::Kind::Watch ? "watching at" : "joining", busy ? "asking first" : "free to go");
}

// The player's answer to the question a visit asks, as the Discord one's is taken.
void AnswerVisit(const Command& command, std::uint64_t generation) {
	auto& visit = runtime->friendVisit;
	if (!visit.Active() || command.revision != visit.Revision()) return;
	if (!command.flag) { visit.Cancel(); return; }
	if (AtMainMenu() && CurrentGates().discordCanSwitch && !Game::Battle::System::ggpo &&
		visit.Confirm(generation, command.revision, GetTickCount64()))
		runtime->offlineRequested = false;
}

// The room ticket answered: join with it, or say why not.
void TakeTicket(const session::TournamentAnswer& answer) {
	auto& visit = runtime->friendVisit;
	const auto now = GetTickCount64();
	const auto admission = answer.ok ? session::DecodeAdmission(answer.data) : std::nullopt;
	if (admission) {
		commands::JoinRoom join;
		join.generation = runtime->controller.GetSnapshot().generation;
		join.invitation = admission->invitation;
		join.publicTicket = admission->ticket;
		if (SubmitRuntimeCommand(std::move(join))) { visit.Admitted(true, now); return; }
	}
	spdlog::info("Friends: room ticket not taken: {}", !answer.ok ? answer.reason : admission ? std::string("queue_failed") : std::string("bridge_invalid_response"));
	runtime->error = loc::T("friends.cannot_join");
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
	case friends::Visit::Step::Ticket: {
		const json request = {{"op", "room_ticket"}, {"bridge_id", visit.Bridge()}, {"room_id", visit.RoomId()}, {"build", sf4e::sidecarHash}};
		const auto id = helperReady ? SendBridgeRequest(request) : 0;
		visit.Ticketed(id, nowMs);
		if (!id) runtime->error = loc::T("friends.cannot_join");
		break;
	}
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
	case friends::Visit::Step::None:
		break;
	}
}
}

namespace internal {
void DispatchFriends(const Command& command, const netplay::Generation& pressed, bool helperReady) {
	if (command.bridgeId.empty() || CurrentId().empty()) return;
	auto& requests = runtime->friends;
	requests.Scope(command.bridgeId, CurrentId());
	switch (command.kind) {
	case Command::Kind::Visit:
		requests.Visit(GetTickCount64());
		return;
	case Command::Kind::Leave:
		requests.Leave();
		return;
	case Command::Kind::Join:
	case Command::Kind::Watch:
	case Command::Kind::TakeInvite:
	case Command::Kind::AnswerVisit:
		// Made for the room the player saw: not for the one they are in by now.
		if (!(runtime->controller.GetSnapshot().generation == pressed)) return;
		if (command.kind == Command::Kind::AnswerVisit) AnswerVisit(command, pressed.room);
		else GoToFriend(command, pressed.room);
		return;
	default:
		Act(command, helperReady);
		return;
	}
}

bool TakeSocialAnswer(const session::TournamentAnswer& answer) {
	auto& requests = runtime->friends;
	const auto now = GetTickCount64();
	if (runtime->friendVisit.Awaiting(answer.requestId)) { TakeTicket(answer); return true; }
	if (!requests.Awaiting(answer.requestId)) return false;
	if (!answer.ok) {
		requests.Finish(answer.reason.empty() ? std::string("unavailable") : answer.reason, std::nullopt, now);
		return true;
	}
	friends::State state;
	if (session::DecodeFriendsState(answer.data, state)) requests.Finish({}, std::move(state), now);
	else requests.Finish("bridge_invalid_response", std::nullopt, now);
	return true;
}

void TickSocial(bool helperReady, std::uint64_t nowMs) {
	auto& requests = runtime->friends;
	const auto id = CurrentId();
	// Another Ember ID's friends are not shown.
	if (!requests.Bridge().empty() && requests.EmberId() != id) requests.Scope({}, {});
	// The service is the one the interface names with its first command; until then the one levels are read from.
	if (requests.Bridge().empty() && !id.empty() && !runtime->levels.Bridge().empty()) requests.Scope(runtime->levels.Bridge(), id);
	requests.Expire(nowMs);
	const auto presented = Present();
	runtime->friendsCanInvite = presented.offer.Any();
	requests.NotePresence(std::hash<std::string>()(presented.report.ToJson().dump()));
	if (auto held = requests.TakeHeld()) SendAction(*held, helperReady);
	else if (helperReady && requests.SyncDue(nowMs))
		requests.SentSync(SendBridgeRequest(friends::SyncRequest(requests.Bridge(), sf4e::sidecarHash, OwnListing(), presented.report)), nowMs);
	TickVisit(helperReady, nowMs);
}

void SayFriendsGoodbye() {
	auto& requests = runtime->friends;
	if (!requests.Syncing() || requests.Published().value->kind != friends::State::Kind::Listed) return;
	friends::Report leaving;
	leaving.kind = friends::Report::Kind::Offline;
	SendBridgeRequest(friends::SyncRequest(requests.Bridge(), sf4e::sidecarHash, OwnListing(), leaving));
}

friends::View FriendsStatus() {
	const auto& requests = runtime->friends;
	friends::View view;
	view.bridge = requests.Bridge();
	view.state = requests.Published();
	view.busy = requests.Busy();
	view.acting = requests.Acting();
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
