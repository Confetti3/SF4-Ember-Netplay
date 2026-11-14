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

// The answer to the look-up of cards that went as `flight`.
void TakeCards(std::uint64_t flight, const session::TournamentAnswer& answer) {
	auto& cards = runtime->cards;
	if (!answer.ok) {
		cards.Finish(flight, netplay::BridgeFlights::Reason(answer), std::nullopt);
		return;
	}
	auto decoded = session::DecodeCards(answer.data);
	cards.Finish(flight, decoded ? std::string() : "bridge_invalid_response", std::move(decoded));
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

// The cards of a public room's members, to the service own levels are read
// from. A room that is not public shows none: its tokens are not the bridge's.
void TickCards(bool helperReady, std::uint64_t nowMs) {
	auto& cards = runtime->cards;
	cards.Scope(runtime->levels.Bridge());
	std::uint64_t epoch = 0;
	std::vector<std::string> tokens;
	std::vector<netplay::levels::LastSet> sets;
	if (UserApp::netplay && runtime->room && runtime->room->ServerOwned()) {
		const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
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
	if (!helperReady) return;
	const auto batch = cards.Take(nowMs);
	if (batch.empty()) return;
	// A card token names a room member; a friend id the friend the Friends tab has focused.
	json selectors = json::array();
	for (const auto& key : batch) selectors.push_back({{netplay::levels::Cards::IsFriendKey(key) ? "friend" : "card", key}});
	const json request = {{"op", "cards_lookup"}, {"bridge_id", cards.Bridge()}, {"cards", selectors}};
	const auto flight = cards.Begin(batch, nowMs);
	SendFlight(request, helperReady, [flight](const session::TournamentAnswer& answer) { TakeCards(flight, answer); });
}

void DispatchCards(const commands::FriendCardWanted& command) {
	runtime->cards.WantFriend(command.id);
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
