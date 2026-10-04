// The bridge's public rooms: the list, and the admission (invitation and signed
// ticket) that creating or joining one returns. Requests go to the helper on
// the tournament channel and are answered by request id; the interface asks
// through DispatchPublicRooms, reads the result from the published status, and
// joins with the admission itself (a JoinInvite carrying the ticket).
#include "sf4e__NetplayRuntime.hxx"
#include "../session/TournamentAnswers.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using netplay::tournament::Command;
using nlohmann::json;
// The helper bounds its own bridge requests; this only ends a wait for one it lost.
constexpr std::uint64_t GiveUpMs = 60000;

// Sends one request; the id the helper will answer under, or 0 when it was not sent.
std::uint64_t Send(const json& request) {
	std::uint64_t id = 0;
	std::string text;
	try { text = request.dump(); } catch (const json::exception&) { return 0; }
	return runtime->room && runtime->room->SendTournament(text, &id) ? id : 0;
}

void FinishList(const std::string& error) {
	auto& rooms = runtime->publicRooms;
	runtime->roomListRequest = 0;
	rooms.loading = false;
	rooms.error = error;
	++rooms.listed;
}

void FinishAdmission(const std::string& failure, std::optional<netplay::publicrooms::Admission> admission = std::nullopt) {
	auto& rooms = runtime->publicRooms;
	runtime->admissionRequest = 0;
	rooms.answered = rooms.admitting;
	rooms.admitting = 0;
	rooms.failure = failure;
	rooms.admission = admission ? std::move(*admission) : netplay::publicrooms::Admission();
}

std::string Reason(const session::TournamentAnswer& answer) { return answer.reason.empty() ? "unavailable" : answer.reason; }

void DispatchList(const Command& command, bool helperReady) {
	auto& rooms = runtime->publicRooms;
	if (command.bridgeId.empty() || rooms.loading) return;
	if (rooms.bridge != command.bridgeId) rooms.rooms.clear();
	rooms.bridge = command.bridgeId;
	const auto id = helperReady ? Send({{"op", "room_list"}, {"bridge_id", command.bridgeId}, {"build", sf4e::sidecarHash}}) : 0;
	if (!id) { FinishList("helper_unavailable"); return; }
	runtime->roomListRequest = id;
	runtime->roomListSentMs = GetTickCount64();
	rooms.loading = true;
	rooms.error.clear();
}

// The one create or ticket request there is: a new one supersedes the one in
// flight, whose helper answer then names no request and is dropped, so an
// abandoned admission is never published. Every request is answered, if only
// with a refusal, under the identity the interface gave it.
void DispatchAdmission(const Command& command, bool helperReady) {
	if (!command.request) return;
	auto& rooms = runtime->publicRooms;
	runtime->admissionRequest = 0;
	rooms.admitting = command.request;
	const bool create = command.op == Command::Op::RoomCreate;
	if (command.bridgeId.empty()) { FinishAdmission("rooms_unavailable"); return; }
	if (create ? command.roomName.empty() || command.capacity < 2 : command.roomId.empty()) {
		FinishAdmission(create ? "invalid_name" : "room_not_found");
		return;
	}
	const auto request = create ?
		json{{"op", "room_create"}, {"bridge_id", command.bridgeId}, {"name", command.roomName},
			{"capacity", command.capacity}, {"build", sf4e::sidecarHash}} :
		json{{"op", "room_ticket"}, {"bridge_id", command.bridgeId}, {"room_id", command.roomId}, {"build", sf4e::sidecarHash}};
	const auto id = helperReady ? Send(request) : 0;
	if (!id) { FinishAdmission("helper_unavailable"); return; }
	runtime->admissionRequest = id;
	runtime->admissionSentMs = GetTickCount64();
	spdlog::info("Public rooms: {} requested", create ? "create" : "ticket");
}
}

namespace internal {
void DispatchPublicRooms(const Command& command, bool helperReady) {
	if (command.op == Command::Op::RoomList) DispatchList(command, helperReady);
	else DispatchAdmission(command, helperReady);
}

bool TakePublicRoomsAnswer(const session::TournamentAnswer& answer) {
	if (!answer.requestId) return false;
	if (answer.requestId == runtime->roomListRequest) {
		auto list = answer.ok ? session::DecodeRoomList(answer.data) : std::nullopt;
		if (list) {
			runtime->publicRooms.rooms = std::move(list->rooms);
			runtime->publicRooms.listedAt = list->listedAt;
		}
		FinishList(!answer.ok ? Reason(answer) : list ? std::string() : "bridge_invalid_response");
		return true;
	}
	if (answer.requestId == runtime->admissionRequest) {
		auto admission = answer.ok ? session::DecodeAdmission(answer.data) : std::nullopt;
		if (!answer.ok) spdlog::info("Public rooms: refused: {}", Reason(answer));
		FinishAdmission(!answer.ok ? Reason(answer) : admission ? std::string() : "bridge_invalid_response", std::move(admission));
		return true;
	}
	return false;
}

void ExpirePublicRooms(std::uint64_t nowMs) {
	if (runtime->roomListRequest && nowMs - runtime->roomListSentMs >= GiveUpMs) FinishList("unavailable");
	if (runtime->admissionRequest && nowMs - runtime->admissionSentMs >= GiveUpMs) FinishAdmission("unavailable");
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
