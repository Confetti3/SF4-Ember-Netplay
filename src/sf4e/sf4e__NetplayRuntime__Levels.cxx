// Fighter levels (docs/design/FIGHTER_LEVELS.md, "Client"): the player's own
// levels. The read goes to the helper on the tournament channel and is
// answered by request id, like rankings; netplay::levels::Requests decides
// what each answer does, and the interface reads the result from the
// published status. Reads come when the menu asks (Command::Op::LevelsMe: the
// profile's own at start and when Profile opens), and twice after a set this PC
// fought ended, to the service the menu last read from.
// Other players' cards in a public room are looked up here too (TickCards), and their profiles, the Find a player
// search and the player's own card are handled in the second half of the file (TickProfiles).
#include "sf4e__NetplayRuntime.hxx"
#include "sf4e__Gamepads.hxx"
#include "../session/TournamentAnswers.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using netplay::tournament::Command;
using nlohmann::json;
using internal::SendBridgeRequest;

std::string CurrentId() { return runtime->room ? runtime->room->Identity().emberId : std::string(); }

// The generation of each table's last set when this player sat in it.
std::vector<std::uint64_t> SetsFought(const room::Snapshot& snapshot) {
	std::vector<std::uint64_t> fought;
	for (const auto& table : snapshot.tables) {
		const auto& set = table.lastSet;
		fought.push_back(snapshot.localMember && (set.p1 == snapshot.localMember || set.p2 == snapshot.localMember) ? set.generation : 0);
	}
	return fought;
}
}

namespace internal {
void DispatchLevels(const Command& command, bool helperReady) {
	if (command.bridgeId.empty()) return;
	auto& requests = runtime->levels;
	requests.Scope(command.bridgeId, CurrentId());
	if (!requests.MayRead()) return;
	// A read the menu asks for answers the reads that were due.
	requests.TakeDue(GetTickCount64());
	const json request = {{"op", "points_me"}, {"bridge_id", command.bridgeId}};
	requests.Sent(helperReady ? SendBridgeRequest(request) : 0, GetTickCount64());
}

bool TakeLevelsAnswer(const session::TournamentAnswer& answer) {
	auto& requests = runtime->levels;
	if (!requests.Awaiting(answer.requestId)) return false;
	const auto now = GetTickCount64();
	if (!answer.ok) {
		const auto failure = answer.reason.empty() ? std::string("unavailable") : answer.reason;
		// A bridge without levels has none to show, and is not asked again until the next trigger.
		if (failure == netplay::levels::Requests::Unavailable) requests.Finish(failure, netplay::levels::Own{}, now);
		else requests.Finish(failure, std::nullopt, now);
		return true;
	}
	auto own = session::DecodeOwnLevels(answer.data);
	requests.Finish(own ? std::string() : "bridge_invalid_response", std::move(own), now);
	return true;
}

void TickLevels(bool helperReady, std::uint64_t nowMs) {
	auto& requests = runtime->levels;
	// Another Ember ID's levels are not shown.
	if (!requests.Bridge().empty() && requests.EmberId() != CurrentId()) requests.Scope({}, {});
	requests.Expire(nowMs);
	if (UserApp::netplay) {
		const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
		requests.NoteSets(snapshot.roomEpoch, SetsFought(snapshot), nowMs);
	}
	if (!helperReady) return;
	// The service is the one the menu last read from: the interface sends the
	// first read, which names it, so nothing is asked before then.
	const auto bridge = requests.Bridge();
	if (bridge.empty() || !requests.MayRead() || !requests.TakeDue(nowMs)) return;
	Command command;
	command.op = Command::Op::LevelsMe;
	command.bridgeId = bridge;
	DispatchLevels(command, helperReady);
}

bool TakeCardsAnswer(const session::TournamentAnswer& answer) {
	auto& cards = runtime->cards;
	if (!cards.Awaiting(answer.requestId)) return false;
	if (!answer.ok) {
		cards.Finish(answer.reason.empty() ? std::string("unavailable") : answer.reason, std::nullopt);
		return true;
	}
	auto decoded = session::DecodeCards(answer.data);
	cards.Finish(decoded ? std::string() : "bridge_invalid_response", std::move(decoded));
	return true;
}

// The cards of a public room's members, to the service own levels are read
// from. A room that is not public shows none: its tokens are not the bridge's.
void TickCards(bool helperReady, std::uint64_t nowMs) {
	auto& cards = runtime->cards;
	cards.Scope(runtime->levels.Bridge());
	cards.Expire(nowMs);
	std::uint64_t epoch = 0;
	std::vector<std::string> tokens;
	std::vector<std::uint64_t> sets;
	if (UserApp::netplay && runtime->room && runtime->room->ServerOwned()) {
		const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
		epoch = snapshot.roomEpoch;
		if (epoch) {
			for (const auto& member : snapshot.members)
				if (member.card.Present()) tokens.push_back(member.card.ToString());
			for (const auto& table : snapshot.tables) sets.push_back(table.lastSet.generation);
		}
	}
	cards.Observe(epoch, tokens, sets, nowMs);
	// The profile open is looked up again after a set in this room ends, as the members' cards are.
	runtime->profiles.Observe(epoch, sets, nowMs);
	if (!helperReady) return;
	const auto batch = cards.Take(nowMs);
	if (batch.empty()) return;
	// A card token names a room member; a friend id the friend the Friends tab has focused.
	json selectors = json::array();
	for (const auto& key : batch) selectors.push_back({{netplay::levels::Cards::IsFriendKey(key) ? "friend" : "card", key}});
	const json request = {{"op", "cards_lookup"}, {"bridge_id", cards.Bridge()}, {"cards", selectors}};
	cards.Sent(SendBridgeRequest(request), batch, nowMs);
}

void DispatchCards(const commands::FriendCardWanted& command) {
	runtime->cards.WantFriend(command.id);
}
} // namespace internal

// ---- Other players' profiles, the Find a player search and the player's own card ----

namespace {
namespace profiles = netplay::profiles;

// A look-up's answer for the ticket it was made under: the one card, or nothing read.
void TakeLookup(profiles::Requests::Ticket ticket, const session::TournamentAnswer& answer) {
	auto& requests = runtime->profiles;
	const auto now = GetTickCount64();
	if (!answer.ok) {
		const auto failure = netplay::BridgeFlights::Reason(answer);
		spdlog::info("Profile: the card was not read: {}", failure);
		requests.FinishLookup(ticket, failure, profiles::Looked{}, now);
		return;
	}
	auto decoded = session::DecodeCards(answer.data);
	profiles::Looked looked;
	if (decoded && decoded->size() == 1) {
		looked.read = true;
		looked.card = std::move((*decoded)[0]);
	}
	if (!looked.read) spdlog::info("Profile: the card answer could not be read");
	requests.FinishLookup(ticket, std::string(), looked, now);
}

// A search's answer for the ticket it was made under, for a name's profile or for Find a player.
void TakeSearch(profiles::Requests::Ticket ticket, const session::TournamentAnswer& answer) {
	auto& requests = runtime->profiles;
	const auto now = GetTickCount64();
	if (!answer.ok) {
		const auto failure = netplay::BridgeFlights::Reason(answer);
		spdlog::info("Profile: the search was not answered: {}", failure);
		requests.FinishSearch(ticket, failure, std::nullopt, now);
		return;
	}
	requests.FinishSearch(ticket, std::string(), session::DecodeSearch(answer.data), now);
}

void SendAsk(const profiles::Requests::Ask& ask, bool helperReady) {
	using Ask = profiles::Requests::Ask;
	const auto& bridge = runtime->profiles.Bridge();
	const bool search = ask.kind == Ask::Kind::Search;
	const json request = search ? profiles::SearchRequest(bridge, ask.value, ask.selector == profiles::Key::Kind::Code) :
		profiles::LookupRequest(bridge, ask.selector, ask.value);
	const auto ticket = ask.ticket;
	const auto handler = [search, ticket](const session::TournamentAnswer& answer) {
		if (search) TakeSearch(ticket, answer);
		else TakeLookup(ticket, answer);
	};
	if (!request.is_null()) {
		internal::SendFlight(request, helperReady, handler);
		return;
	}
	// A request that cannot be made (no selector to look up by) ends at once, as a refusal.
	session::TournamentAnswer refused;
	refused.reason = "invalid_request";
	handler(refused);
}

std::string Trimmed(const std::string& text) {
	const auto first = text.find_first_not_of(" \t");
	if (first == std::string::npos) return {};
	return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// The device Automatic stands for now: the controller this PC plays with.
profiles::Device AutomaticDeviceNow() {
	const auto& device = runtime->input.Selected();
	return profiles::AutomaticDevice(device.type, device.connected, Gamepads::BrandForDevice(device.type, device.index));
}

// What the card shows of how this player plays, as the game measures it now. The connection is read at most once a
// minute; the region is the relay Ember last picked.
profiles::Style OwnStyleNow(std::uint64_t nowMs) {
	auto& signals = runtime->cardSignals;
	if (!signals.linkRead || nowMs - signals.linkMs >= netplay::profiles::Saver::MinGapMs) {
		signals.link = DetectNetworkLink();
		signals.linkRead = true;
		signals.linkMs = nowMs;
	}
	if (runtime->room) {
		const auto relay = NormalizeRelayRegion(runtime->room->Network().relay);
		if (IsRelayRegionCode(relay)) signals.region = relay;
	}
	return profiles::OwnStyle(runtime->preferences, AutomaticDeviceNow(), signals.link, signals.region);
}

// The player's card goes to the service when it changed: at start, and after Edit card saves.
void TickCardSave(bool helperReady, std::uint64_t nowMs) {
	auto& saver = runtime->cardSaver;
	const auto bridge = runtime->levels.Bridge();
	const auto id = CurrentId();
	if (bridge.empty() || id.empty()) return;
	if (!helperReady) return;
	const auto& own = runtime->levels.Published();
	// Only a Discord account has a card.
	if (own.loaded && own.value.kind == netplay::levels::Own::Kind::NoAccount) return;
	const auto bundle = profiles::BundleOf(runtime->preferences, runtime->automaticCountry, OwnStyleNow(nowMs));
	if (!saver.Due(bridge, id, bundle, nowMs)) return;
	saver.Sent(bridge, id, bundle, nowMs);
	spdlog::info("Profile: the card is sent to the service");
	internal::SendFlight(profiles::CardSaveRequest(bridge, bundle), helperReady, [](const session::TournamentAnswer& answer) {
		const auto now = GetTickCount64();
		const auto failure = answer.ok ? std::string() : netplay::BridgeFlights::Reason(answer);
		if (!failure.empty()) spdlog::info("Profile: the card was not saved: {}", failure);
		runtime->cardSaver.Finish(failure);
		if (failure.empty()) runtime->profiles.NoteService(profiles::Service::Offered, now);
		else if (failure == profiles::Unavailable) runtime->profiles.NoteService(profiles::Service::Missing, now);
	});
}
}

namespace internal {
void DispatchProfile(const commands::ProfileOpen& command) {
	if (command.bridge.empty() || CurrentId().empty()) return;
	runtime->profiles.Scope(command.bridge, CurrentId());
	runtime->profiles.Open(command.key, command.known, GetTickCount64());
}

void DispatchProfile(const commands::ProfileClose&) {
	runtime->profiles.Close();
}

// A text under two bytes asks for nothing and changes nothing; an empty one clears the search.
void DispatchProfile(const commands::PlayerSearch& command) {
	if (command.bridge.empty() || CurrentId().empty()) return;
	const auto text = Trimmed(command.text);
	if (text.size() == 1) return;
	runtime->profiles.Scope(command.bridge, CurrentId());
	runtime->profiles.Search(text, command.code, GetTickCount64());
}

void NoteProfileAction(const std::string& token, netplay::levels::Relation relation) {
	runtime->profiles.Refresh();
	if (!token.empty()) runtime->profiles.Relate(token, relation);
}

// The profile open and the search are asked for when due, and the player's own card is saved when it changed.
void TickProfiles(bool helperReady, std::uint64_t nowMs) {
	auto& requests = runtime->profiles;
	// Another Ember ID's profiles are not shown.
	if (!requests.Bridge().empty() && requests.EmberId() != CurrentId()) requests.Scope({}, {});
	requests.Tick(nowMs);
	if (helperReady && !requests.Bridge().empty())
		if (const auto ask = requests.Take(nowMs)) SendAsk(ask, helperReady);
	TickCardSave(helperReady, nowMs);
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
