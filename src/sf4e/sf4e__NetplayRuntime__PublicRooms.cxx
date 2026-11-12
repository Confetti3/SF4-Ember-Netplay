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
using internal::SendBridgeRequest;
using netplay::BridgeFlights;

void FinishList(const std::string& error) {
	auto& rooms = runtime->publicRooms;
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

void TakeList(const session::TournamentAnswer& answer) {
	auto list = answer.ok ? session::DecodeRoomList(answer.data) : std::nullopt;
	if (list) {
		runtime->publicRooms.rooms = std::move(list->rooms);
		runtime->publicRooms.listedAt = list->listedAt;
	}
	FinishList(!answer.ok ? BridgeFlights::Reason(answer) : list ? std::string() : "bridge_invalid_response");
}

// Asks for a room_create or room_ticket and decodes what comes back, however it ended: what both a
// public room's Join and a friend's visit read (`failure` empty when `admission` is there).
std::uint64_t SendAdmission(const json& request, bool helperReady, internal::AdmissionDone done) {
	return internal::SendFlight(request, helperReady, [done = std::move(done)](const session::TournamentAnswer& answer) {
		auto admission = answer.ok ? session::DecodeAdmission(answer.data) : std::nullopt;
		if (!answer.ok) spdlog::info("Public rooms: refused: {}", BridgeFlights::Reason(answer));
		done(!answer.ok ? BridgeFlights::Reason(answer) : admission ? std::string() : "bridge_invalid_response", std::move(admission));
	});
}

json TicketRequest(const std::string& bridgeId, const std::string& roomId) {
	return {{"op", "room_ticket"}, {"bridge_id", bridgeId}, {"room_id", roomId}, {"build", sf4e::sidecarHash}};
}

void DispatchList(const Command& command, bool helperReady) {
	auto& rooms = runtime->publicRooms;
	if (command.bridgeId.empty() || rooms.loading) return;
	if (rooms.bridge != command.bridgeId) rooms.rooms.clear();
	rooms.bridge = command.bridgeId;
	rooms.loading = true;
	rooms.error.clear();
	// A request the helper cannot take ends at once, through the same handler.
	internal::SendFlight({{"op", "room_list"}, {"bridge_id", command.bridgeId}, {"build", sf4e::sidecarHash}}, helperReady, TakeList);
}

// The one create or ticket request there is: a new one supersedes the one in
// flight, whose helper answer then names no request and is dropped, so an
// abandoned admission is never published. Every request is answered, if only
// with a refusal, under the identity the interface gave it.
void DispatchAdmission(const Command& command, bool helperReady) {
	if (!command.request) return;
	auto& rooms = runtime->publicRooms;
	// The flight it supersedes is forgotten, so its answer is dropped.
	runtime->flights.Forget(runtime->admissionRequest);
	runtime->admissionRequest = 0;
	rooms.admitting = command.request;
	const bool create = command.op == Command::Op::RoomCreate;
	if (command.bridgeId.empty()) { FinishAdmission("rooms_unavailable"); return; }
	if (create ? command.roomName.empty() || command.capacity < 2 : command.roomId.empty()) {
		FinishAdmission(create ? "invalid_name" : "room_not_found");
		return;
	}
	auto request = create ?
		json{{"op", "room_create"}, {"bridge_id", command.bridgeId}, {"name", command.roomName},
			{"capacity", command.capacity}, {"build", sf4e::sidecarHash}} :
		TicketRequest(command.bridgeId, command.roomId);
	// Only a room that asks for each player's own delay says so; the helper reads an absent key as the shared one.
	if (create && command.ownDelay) request["own_delay"] = true;
	runtime->admissionRequest = SendAdmission(request, helperReady, [](const std::string& failure, std::optional<netplay::publicrooms::Admission> admission) {
		FinishAdmission(failure, std::move(admission));
	});
	if (runtime->admissionRequest) spdlog::info("Public rooms: {} requested", create ? "create" : "ticket");
}
}

namespace internal {
std::uint64_t SendBridgeRequest(const json& request) {
	std::uint64_t id = 0;
	std::string text;
	try { text = request.dump(); } catch (const json::exception&) { return 0; }
	return runtime->room && runtime->room->SendTournament(text, &id) ? id : 0;
}

std::uint64_t SendFlight(const json& request, bool helperReady, netplay::BridgeFlights::Handler handler) {
	return runtime->flights.Begin(helperReady ? SendBridgeRequest(request) : 0, std::move(handler), GetTickCount64());
}

void DispatchPublicRooms(const Command& command, bool helperReady) {
	if (command.op == Command::Op::RoomList) DispatchList(command, helperReady);
	else DispatchAdmission(command, helperReady);
}

void RequestTicket(const std::string& bridgeId, const std::string& roomId, bool helperReady, AdmissionDone done) {
	// Not the interface's one admission: a visit's ticket neither replaces it nor is replaced by it.
	SendAdmission(TicketRequest(bridgeId, roomId), helperReady, std::move(done));
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
