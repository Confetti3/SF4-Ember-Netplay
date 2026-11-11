// Fighter levels (docs/design/FIGHTER_LEVELS.md, "Client"): the player's own
// levels. The read goes to the helper on the tournament channel and is
// answered by request id, like rankings; netplay::levels::Requests decides
// what each answer does, and the interface reads the result from the
// published status. Reads come when the menu asks (Command::Op::LevelsMe),
// once at start, and twice after a set this PC fought ended.
#include "sf4e__NetplayRuntime.hxx"
#include "../session/TournamentAnswers.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using netplay::tournament::Command;
using nlohmann::json;
using internal::SendBridgeRequest;

std::string CurrentId() { return runtime->room ? runtime->room->Identity().emberId : std::string(); }

// The service the automatic reads go to: the one the menu last read from, or
// before it has, Ember's own built-in one once the player's ID is ready.
std::string AutomaticBridge() {
	if (!runtime->levels.Bridge().empty()) return runtime->levels.Bridge();
	if (!runtime->room) return {};
	const auto& identity = runtime->room->Identity();
	if (identity.state != "ready") return {};
	for (const auto& bridge : identity.bridges) if (bridge.builtin) return bridge.id;
	return {};
}

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
	const json request = {{"op", "levels_me"}, {"bridge_id", command.bridgeId}};
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
	const auto bridge = AutomaticBridge();
	if (bridge.empty() || !requests.MayRead() || !requests.TakeDue(nowMs)) return;
	Command command;
	command.op = Command::Op::LevelsMe;
	command.bridgeId = bridge;
	DispatchLevels(command, helperReady);
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
