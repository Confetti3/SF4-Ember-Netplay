// Fighter levels (docs/design/FIGHTER_LEVELS.md, "Client"): the player's own
// levels. The read goes to the helper on the tournament channel and its answer
// comes back through flights, like rankings; netplay::levels::Requests decides
// what each answer does, and the interface reads the result from the
// published status. Reads come when the menu asks (Command::Op::LevelsMe: the
// profile's own at start and when Profile opens), and twice after a set this PC
// fought ended, to the service the menu last read from.
// Other players' cards in a public room are looked up here too (TickCards); their profiles, the Find a player search
// and the player's own card are in sf4e__NetplayRuntime__Profiles.cxx.
#include "sf4e__NetplayRuntime.hxx"
#include "../common/ProductIdentity.hxx"
#include "../session/TournamentAnswers.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using netplay::tournament::Command;
using nlohmann::json;

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

// The answer to the read of the player's own levels that went as `flight`.
void TakeLevels(std::uint64_t flight, const session::TournamentAnswer& answer) {
	auto& requests = runtime->levels;
	const auto now = GetTickCount64();
	if (!answer.ok) {
		const auto failure = netplay::BridgeFlights::Reason(answer);
		// A bridge without levels has none to show, and is not asked again until the next trigger.
		if (failure == netplay::levels::Requests::Unavailable) requests.Finish(flight, failure, netplay::levels::Own{}, now);
		else requests.Finish(flight, failure, std::nullopt, now);
		return;
	}
	auto own = session::DecodeOwnLevels(answer.data);
	requests.Finish(flight, own ? std::string() : "bridge_invalid_response", std::move(own), now);
}

// The answer to the look-up of `cards` that went as `flight`.
void TakeCards(netplay::levels::Cards& cards, std::uint64_t flight, const session::TournamentAnswer& answer) {
	if (!answer.ok) {
		cards.Finish(flight, netplay::BridgeFlights::Reason(answer), std::nullopt);
		return;
	}
	auto decoded = session::DecodeCards(answer.data);
	cards.Finish(flight, decoded ? std::string() : "bridge_invalid_response", std::move(decoded));
}

// Sends the look-up `cards` has due, to the bridge it is scoped to: a room member's token as a card, a friend id as a
// friend. `TakeCards` gets the answer back to `cards`.
void SendCards(netplay::levels::Cards& cards, bool helperReady, std::uint64_t nowMs) {
	const auto batch = cards.Take(nowMs);
	if (batch.empty()) return;
	json selectors = json::array();
	for (const auto& key : batch) selectors.push_back({{netplay::levels::Cards::IsFriendKey(key) ? "friend" : "card", key}});
	const json request = {{"op", "cards_lookup"}, {"bridge_id", cards.Bridge()}, {"cards", selectors}};
	const auto flight = cards.Begin(batch, nowMs);
	internal::SendFlight(request, helperReady, [&cards, flight](const session::TournamentAnswer& answer) { TakeCards(cards, flight, answer); });
}

// The cards friends' look-ups are kept in: the room's own when friends' cards go to the same bridge as members' (they
// ride in the same look-up), else their own, on the account bridge.
netplay::levels::Cards& FriendCardsOf(const netplay::Bridges& bridges) {
	using netplay::Ask;
	return bridges.Together(Ask::FriendCard, Ask::MemberCard) ? runtime->cards : runtime->friendCards;
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
	const auto flight = requests.Begin();
	SendFlight(request, helperReady, [flight](const session::TournamentAnswer& answer) { TakeLevels(flight, answer); });
}

void TickLevels(bool helperReady, std::uint64_t nowMs) {
	auto& requests = runtime->levels;
	// Another Ember ID's levels are not shown.
	if (!requests.Bridge().empty() && requests.EmberId() != CurrentId()) requests.Scope({}, {});
	if (RuntimeSession::netplay) {
		const auto& snapshot = RuntimeSession::netplay->client.GetRoomSnapshot();
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

// The cards of a public room's members, to the service own levels are read
// from. A room that is not public shows none: its tokens are not the bridge's.
// Friends' cards go where netplay::ServiceOf sends them: with the members' when
// that is the same bridge, else to the account bridge in a look-up of their own.
void TickCards(bool helperReady, std::uint64_t nowMs) {
	using netplay::Ask;
	const auto bridges = RuntimeBridges(runtime->levels.Bridge());
	auto& cards = runtime->cards;
	cards.Scope(bridges.Of(Ask::MemberCard));
	runtime->friendCards.Scope(bridges.Together(Ask::FriendCard, Ask::MemberCard) ? std::string() : bridges.Of(Ask::FriendCard));
	std::uint64_t epoch = 0;
	std::vector<std::string> tokens;
	std::vector<netplay::levels::LastSet> sets;
	if (RuntimeSession::netplay && runtime->room && runtime->room->ServerOwned()) {
		const auto& snapshot = RuntimeSession::netplay->client.GetRoomSnapshot();
		epoch = snapshot.roomEpoch;
		if (epoch) {
			const auto cardOf = [&](room::MemberId id) {
				for (const auto& member : snapshot.members)
					if (member.id == id && member.card.Present()) return member.card.ToString();
				return std::string();
			};
			for (const auto& member : snapshot.members)
				if (member.card.Present()) tokens.push_back(member.card.ToString());
			for (const auto& table : snapshot.tables) sets.push_back({table.lastSet.generation, cardOf(table.lastSet.p1), cardOf(table.lastSet.p2)});
		}
	}
	cards.Observe(epoch, tokens, sets, nowMs);
	// The profile open is looked up again after a set its player fought ends, as their card in the room is.
	runtime->profiles.Observe(epoch, sets, nowMs);
	// The account bridge's friends' cards have no room: its epoch stays 0, and only the friends wanted are kept.
	runtime->friendCards.Observe(0, {}, {}, nowMs);
	if (!helperReady) return;
	// A card token names a room member; a friend id the friend the Friends tab has focused.
	SendCards(cards, helperReady, nowMs);
	SendCards(runtime->friendCards, helperReady, nowMs);
}

void DispatchCards(const commands::FriendCardWanted& command) {
	FriendCardsOf(RuntimeBridges(runtime->levels.Bridge())).WantFriend(command.id);
}

std::shared_ptr<const netplay::levels::CardMap> ShownCards() {
	auto& shown = runtime->shownCards;
	const auto& members = runtime->cards.Published();
	const auto& friends = runtime->friendCards.Published();
	// Friends' cards of their own are none while they ride with the members': the members' map is the view's as it is.
	if (friends->empty()) return members;
	if (shown.members != members || shown.friends != friends || !shown.both) {
		auto both = std::make_shared<netplay::levels::CardMap>(*members);
		both->insert(friends->begin(), friends->end());
		shown = {members, friends, std::move(both)};
	}
	return shown.both;
}

netplay::Bridges RuntimeBridges(const std::string& game) {
	return {game, ember::SeparateAccount() ? runtime->accountBridge : game};
}

void DispatchAccount(const commands::AccountService& command) {
	if (!ember::SeparateAccount() || command.bridge == runtime->accountBridge) return;
	spdlog::info("Account service {}", command.bridge.empty() ? "not trusted" : "named");
	runtime->accountBridge = command.bridge;
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
