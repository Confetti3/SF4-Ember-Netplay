// Playing a bridge-run tournament match. TournamentPlay decides what to do;
// this file turns its outputs into helper requests, room commands and room
// changes, and feeds every answer back. It runs after the room's events are
// drained, so a game's end reaches the state machine on the tick it commits.
#include "sf4e__NetplayRuntime.hxx"
#include "../session/TournamentAnswers.hxx"
#include "../common/HexText.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using netplay::tournament::Output;
using netplay::tournament::WatchOutput;
using nlohmann::json;

json Counter(std::uint64_t value) { return value ? json(std::to_string(value)) : json(nullptr); }
json OptionalText(const std::string& text) { return text.empty() ? json(nullptr) : json(text); }

// The helper request for an output, or null for outputs that are not one.
json Request(const Output& output) {
	const auto& play = runtime->tournament;
	switch (output.kind) {
	case Output::Kind::Claim:
		return {{"op", "match_claim"}, {"bridge_id", play.BridgeId()}, {"match_id", play.MatchId()}, {"build", sf4e::sidecarHash},
			{"ready", output.ready}};
	case Output::Kind::Publish:
		return {{"op", "room_publish"}, {"bridge_id", play.BridgeId()}, {"match_id", play.MatchId()},
			{"room", {{"room_id", output.roomId}, {"invitation", output.invitation}, {"lease_id", OptionalText(output.leaseId)},
				{"fence", OptionalText(output.fence)}, {"replaces", OptionalText(output.replaces)}}}};
	case Output::Kind::Prepare:
		return {{"op", "game_prepare"}, {"bridge_id", play.BridgeId()}, {"match_id", play.MatchId()},
			{"match_generation", std::to_string(output.generation)}};
	case Output::Kind::Report:
		return {{"op", "game_report"}, {"bridge_id", play.BridgeId()}, {"match_id", play.MatchId()},
			{"match_generation", std::to_string(output.generation)}, {"result", output.result},
			{"capture_frame", Counter(output.captureFrame)}, {"confirmed_input_frame", Counter(output.confirmedFrame)}};
	case Output::Kind::Forget:
		return {{"op", "match_leave"}, {"match_id", play.MatchId()}};
	default:
		return nullptr;
	}
}

// The helper request for a watcher's output, or null for the room commands.
json WatchRequest(const WatchOutput& output) {
	const auto& watch = runtime->watch;
	switch (output.kind) {
	case WatchOutput::Kind::Watch:
		return {{"op", "match_watch"}, {"bridge_id", watch.BridgeId()}, {"match_id", watch.MatchId()}, {"build", sf4e::sidecarHash}};
	case WatchOutput::Kind::Unwatch:
		return {{"op", "match_unwatch"}, {"bridge_id", watch.BridgeId()}, {"match_id", watch.MatchId()}};
	default:
		return nullptr;
	}
}

const char* KindName(Output::Kind kind) {
	switch (kind) {
	case Output::Kind::Claim: return "claim";
	case Output::Kind::Publish: return "publish";
	case Output::Kind::Prepare: return "prepare";
	case Output::Kind::Report: return "report";
	case Output::Kind::Forget: return "forget";
	default: return "room";
	}
}

// Hands `request` to the helper, and says under which id its answer comes.
bool SendToHelper(const json& request, std::uint64_t& id) {
	std::string text;
	try { text = request.dump(); } catch (const json::exception&) { text.clear(); }
	return !text.empty() && runtime->room && runtime->room->SendTournament(text, &id);
}

// Why a helper request failed, as the state machines are told.
std::string FailureCode(const session::TournamentAnswer& answer) {
	return answer.reason.empty() ? std::string("unavailable") : answer.reason;
}

void SendRequest(const Output& output, std::uint64_t nowMs) {
	std::uint64_t id = 0;
	if (SendToHelper(Request(output), id)) {
		runtime->tournamentRequests[id] = {output.kind, output.generation};
		return;
	}
	spdlog::warn("Tournament: {} request not sent", KindName(output.kind));
	// A report the helper never saw goes again; the state machine stops the
	// match if it never gets saved.
	if (output.kind == Output::Kind::Report) runtime->tournament.OnReportFailed(output.generation, "helper_unavailable", nowMs);
	else if (output.kind != Output::Kind::Forget) runtime->tournament.OnFailure(output.kind, "helper_unavailable", nowMs);
}

void SendWatchRequest(const WatchOutput& output, std::uint64_t nowMs) {
	std::uint64_t id = 0;
	if (SendToHelper(WatchRequest(output), id)) {
		runtime->watchRequests[id] = output.kind;
		return;
	}
	spdlog::warn("Watch: request not sent");
	// An unwatch nobody hears only leaves the bridge to drop the watcher itself.
	if (output.kind == WatchOutput::Kind::Watch) runtime->watch.OnFailure("helper_unavailable", nowMs);
}

void SendPermitReady(const Output& output) {
	if (!runtime->attached || !UserApp::netplay) return;
	auto& client = UserApp::netplay->client;
	const auto& snapshot = client.GetRoomSnapshot();
	room::Action action;
	action.kind = room::ActionKind::PermitReady;
	action.roomEpoch = snapshot.roomEpoch;
	action.revision = snapshot.revision;
	action.table = room::TournamentTable;
	action.tableRevision = snapshot.tables[room::TournamentTable].revision;
	action.matchGeneration = output.generation;
	action.text = output.permitId;
	action.startWindowMs = output.startWindowMs;
	// Sent again until the room shows it, so a failed send needs no retry here.
	if (client.SendRoomAction(action) != session::SendResult::Queued)
		spdlog::info("Tournament: permit for generation {} not sent yet", output.generation);
}

// A room command the runtime would not carry out leaves the state machine
// waiting out its open timeout; the log says why the room never came.
void RoomCommand(netplay::Command command, const char* what, JoinKind join = JoinKind::Private) {
	const auto outcome = DispatchTournamentRoomCommand(std::move(command), join);
	if (outcome == netplay::DispatchOutcome::Dispatched) spdlog::info("Tournament: {}", what);
	else spdlog::warn("Tournament: {} not started (outcome {})", what, static_cast<int>(outcome));
}

void Execute(const Output& output, std::uint64_t nowMs) {
	switch (output.kind) {
	case Output::Kind::Claim:
	case Output::Kind::Publish:
	case Output::Kind::Prepare:
	case Output::Kind::Report:
	case Output::Kind::Forget:
		SendRequest(output, nowMs);
		break;
	case Output::Kind::Host:
		RoomCommand({netplay::CommandKind::HostRoom, {}, {}}, "opening the match's room");
		break;
	case Output::Kind::Join:
		RoomCommand({netplay::CommandKind::JoinInvite, {}, output.invitation}, "joining the match's room");
		break;
	case Output::Kind::Leave: {
		// Not in a room, or already leaving it (the player left by hand).
		const auto room = runtime->controller.GetSnapshot().room;
		if (room == netplay::RoomState::Idle || room == netplay::RoomState::Closing) break;
		RoomCommand({netplay::CommandKind::LeaveRoom, {}, {}}, "leaving the room");
		break;
	}
	case Output::Kind::Bind:
		// The room may be mid-round; the state machine offers it again.
		if (UserApp::server && UserApp::server->BindTournament(output.binding))
			spdlog::info("Tournament: room bound to match {} revision {}", output.binding.matchId, output.binding.bindingRevision);
		break;
	case Output::Kind::PermitReady:
		SendPermitReady(output);
		break;
	}
}

void ExecuteWatch(const WatchOutput& output, std::uint64_t nowMs) {
	switch (output.kind) {
	case WatchOutput::Kind::Watch:
	case WatchOutput::Kind::Unwatch:
		SendWatchRequest(output, nowMs);
		break;
	case WatchOutput::Kind::Join:
		RoomCommand({netplay::CommandKind::JoinInvite, {}, output.invitation}, "joining the match's room as a watcher", JoinKind::Observer);
		break;
	case WatchOutput::Kind::Leave: {
		const auto room = runtime->controller.GetSnapshot().room;
		if (room == netplay::RoomState::Idle || room == netplay::RoomState::Closing) break;
		RoomCommand({netplay::CommandKind::LeaveRoom, {}, {}}, "leaving the watched room");
		break;
	}
	}
}

// The helper's answer to an assignment refresh.
void TakeAssignments(const session::TournamentAnswer& answer) {
	runtime->assignmentRequest = 0;
	runtime->assignmentList.loading = false;
	++runtime->assignmentList.finished;
	if (!answer.ok) {
		runtime->assignmentList.error = answer.reason.empty() ? "unavailable" : answer.reason;
		return;
	}
	auto list = session::DecodeAssignments(answer.data);
	if (!list) {
		runtime->assignmentList.error = "bridge_invalid_response";
		return;
	}
	runtime->assignmentList.error.clear();
	runtime->assignmentList.items = std::move(*list);
}

// The helper's answer to a watcher's ask. An unwatch has nothing awaiting it.
bool TakeWatchAnswer(const session::TournamentAnswer& answer, std::uint64_t nowMs) {
	const auto found = runtime->watchRequests.find(answer.requestId);
	if (found == runtime->watchRequests.end()) return false;
	const auto kind = found->second;
	runtime->watchRequests.erase(found);
	if (kind != WatchOutput::Kind::Watch) return true;
	auto& watch = runtime->watch;
	if (!answer.ok) {
		const auto code = FailureCode(answer);
		spdlog::info("Watch: refused: {}", code);
		watch.OnFailure(code, nowMs);
	} else if (const auto reply = session::DecodeWatchReply(answer.data)) {
		watch.OnReply(*reply, nowMs);
	} else {
		watch.OnFailure("bridge_invalid_response", nowMs);
	}
	return true;
}

void TakeAnswer(const session::TournamentAnswer& answer, std::uint64_t nowMs) {
	if (answer.requestId && answer.requestId == runtime->assignmentRequest) { TakeAssignments(answer); return; }
	if (TakePublicRoomsAnswer(answer) || TakeWatchAnswer(answer, nowMs)) return;
	const auto found = runtime->tournamentRequests.find(answer.requestId);
	if (found == runtime->tournamentRequests.end()) return;
	const auto kind = found->second.kind;
	const auto generation = found->second.generation;
	runtime->tournamentRequests.erase(found);
	auto& play = runtime->tournament;
	if (!answer.ok) {
		const auto code = FailureCode(answer);
		spdlog::info("Tournament: {} refused: {}", KindName(kind), code);
		if (kind == Output::Kind::Report) play.OnReportFailed(generation, code, nowMs);
		else play.OnFailure(kind, code, nowMs);
		return;
	}
	switch (kind) {
	case Output::Kind::Claim:
	case Output::Kind::Publish:
		if (const auto reply = session::DecodeClaimReply(answer.data)) play.OnRoom(kind, *reply, nowMs);
		else play.OnFailure(kind, "bridge_invalid_response", nowMs);
		break;
	case Output::Kind::Prepare:
		if (const auto reply = session::DecodePrepareReply(answer.data)) play.OnPrepare(*reply, nowMs);
		else play.OnFailure(kind, "bridge_invalid_response", nowMs);
		break;
	case Output::Kind::Report:
		play.OnReported(generation);
		break;
	default:
		break;
	}
}

netplay::tournament::RoomView View(bool helperReady) {
	netplay::tournament::RoomView view;
	const auto& state = runtime->controller.GetSnapshot();
	// Also out of a joined room: a room lost under a game keeps its battle.
	view.fighting = netplay::Fighting(state.match);
	view.joined = state.room == netplay::RoomState::Joined && runtime->attached && UserApp::netplay && runtime->room;
	// Not free to open a room counts as opening: the state machine waits
	// rather than asking for a host or join the runtime would refuse.
	view.opening = !view.joined && (state.room != netplay::RoomState::Idle || !helperReady || !AtMainMenu() ||
		UserApp::netplay || UserApp::server || Game::Battle::System::ggpo);
	if (!view.joined) return view;
	view.roomId = HexLower(runtime->room->RoomId());
	view.invitation = runtime->room->Invitation();
	view.authorityWritable = UserApp::server && (!state.coordinated || state.authorityWritable);
	view.snapshot = &UserApp::netplay->client.GetRoomSnapshot();
	return view;
}
}

namespace internal {
void OpenMatchLink(const tournament_link::MatchLink& link) {
	if (!link.Valid()) return;
	auto& opened = runtime->openedLink;
	opened.bridge = link.bridgeId;
	opened.match = link.matchId;
	++opened.sequence;
	spdlog::info("Tournament: a link opened match {}", link.matchId);
}

void OpenConnectLink(const std::string& bridge, bool launched) {
	if (!tournament_link::IsBridgeId(bridge)) return;
	auto& opened = runtime->openedConnect;
	opened.bridge = bridge;
	++opened.sequence;
	const bool idle = AtMainMenu() && runtime->controller.GetSnapshot().room == netplay::RoomState::Idle && !UserApp::netplay;
	opened.confirm = !launched && !idle;
	spdlog::info("Tournament: a link asked to connect Discord on service {}", bridge);
}

void OpenPublicRoomLink(const tournament_link::RoomLink& link, bool launched) {
	if (!link.Valid()) return;
	auto& opened = runtime->openedRoomLink;
	opened.bridge = link.bridgeId;
	opened.room = link.roomId;
	++opened.sequence;
	// A link that started Ember was just clicked; otherwise only at the main
	// menu with no room is the player free to be taken to Public rooms.
	opened.free = launched || (AtMainMenu() && runtime->controller.GetSnapshot().room == netplay::RoomState::Idle && !UserApp::netplay);
	spdlog::info("Public rooms: a link asked for a room");
}

void DispatchTournament(const netplay::tournament::Command& command, bool helperReady) {
	using Op = netplay::tournament::Command::Op;
	const auto now = GetTickCount64();
	auto& play = runtime->tournament;
	switch (command.op) {
	case Op::Refresh: {
		if (command.bridgeId.empty()) return;
		auto& list = runtime->assignmentList;
		if (list.bridge != command.bridgeId) list.items.clear();
		list.bridge = command.bridgeId;
		std::uint64_t id = 0;
		const auto text = json{{"op", "assignment_list"}, {"bridge_id", command.bridgeId}}.dump();
		if (helperReady && runtime->room && runtime->room->SendTournament(text, &id)) {
			runtime->assignmentRequest = id;
			list.loading = true;
			list.error.clear();
		} else {
			list.loading = false;
			list.error = "helper_unavailable";
			++list.finished;
		}
		return;
	}
	case Op::Play: {
		if (command.bridgeId.empty() || command.matchId.empty() ||
			netplay::tournament::HoldsMatchRoom(play.GetPhase(), runtime->watch.GetPhase())) return;
		if (!helperReady || runtime->controller.GetSnapshot().room != netplay::RoomState::Idle) {
			runtime->error = loc::T("runtime.return_main_menu");
			return;
		}
		if (play.SavingReports()) {
			runtime->error = loc::T("runtime.tournament_result_saving");
			return;
		}
		spdlog::info("Tournament: playing match {}", command.matchId);
		// Answers to an earlier match's requests are not this one's.
		runtime->tournamentRequests.clear();
		play.Start(command.bridgeId, command.matchId, now);
		return;
	}
	case Op::Watch: {
		if (command.bridgeId.empty() || command.matchId.empty() ||
			netplay::tournament::HoldsMatchRoom(play.GetPhase(), runtime->watch.GetPhase())) return;
		// A watch acts only on the match's room, so it starts from no room at all.
		if (runtime->controller.GetSnapshot().room != netplay::RoomState::Idle) {
			runtime->error = loc::T("runtime.watch_leave_room");
			return;
		}
		if (!helperReady) {
			runtime->error = loc::T("runtime.return_main_menu");
			return;
		}
		spdlog::info("Watch: watching match {}", command.matchId);
		runtime->watchRequests.clear();
		runtime->watch.Start(command.bridgeId, command.matchId, now);
		return;
	}
	case Op::Stop:
		spdlog::info("Tournament: stopped by the player");
		play.Stop();
		runtime->watch.Stop();
		return;
	case Op::RoomList:
	case Op::RoomCreate:
	case Op::RoomTicket:
		DispatchPublicRooms(command, helperReady);
		return;
	default:
		return;
	}
}

void TickTournament(bool helperReady) {
	ExpirePublicRooms(GetTickCount64());
	if (!runtime->room) return;
	OpenMatchLink(runtime->matchLinks.Take());
	OpenConnectLink(runtime->connectLinks.Take());
	OpenPublicRoomLink(runtime->publicRoomLinks.Take());
	const auto now = GetTickCount64();
	// The helper is not restarted while the game runs, and the binding names
	// its endpoint: without it the match cannot go on from this game.
	if (runtime->helperLossReported) {
		runtime->tournament.Abandon("helper_lost");
		runtime->watch.Abandon("helper_lost");
	}
	// The fighters' link dropped (GGPO warns before it gives up): a game that
	// then ends in an abort ended for the lost connection, whichever fighter's
	// abort the room takes first, so both fighters are told the same.
	if (runtime->match && runtime->matchEntered && !LocalIsSpectator() && Game::Battle::System::DisconnectCountdownMs() >= 0)
		runtime->tournament.OnConnectionLost(runtime->match->Generation());
	for (const auto& answer : runtime->room->TakeTournamentAnswers()) TakeAnswer(answer, now);
	const auto before = runtime->tournament.GetPhase();
	const auto view = View(helperReady);
	for (const auto& output : runtime->tournament.Tick(now, view)) Execute(output, now);
	for (const auto& output : runtime->watch.Tick(now, view)) ExecuteWatch(output, now);
	const auto after = runtime->tournament.GetPhase();
	if (after != before && (after == netplay::tournament::Phase::Finished || after == netplay::tournament::Phase::Failed))
		spdlog::info("Tournament: match {} ended: {}", runtime->tournament.MatchId(), runtime->tournament.Reason());
}

void ObserveTournamentTerminal(const room::Event& event, netplay::MatchResultOutbox::TerminalResult terminal) {
	if (event.table != room::TournamentTable || !event.matchGeneration || !UserApp::netplay ||
		!UserApp::netplay->client.GetRoomSnapshot().tournament.Active()) return;
	if (event.result == room::MatchResult::Abort || event.result == room::MatchResult::Cancel) {
		runtime->tournament.OnTerminal(event.matchGeneration, event.result, 0, 0);
		return;
	}
	// A win or draw is reported only as this game saw it, rollback-confirmed.
	// Otherwise the other fighter's report stands alone and the bridge holds
	// the game for review.
	const auto* capture = runtime->resultOutbox.Captured();
	if (terminal != netplay::MatchResultOutbox::TerminalResult::Confirmed || !capture ||
		capture->generation != event.matchGeneration || capture->table != event.table) {
		spdlog::warn("Tournament: generation {} ended without a confirmed local result; not reporting it", event.matchGeneration);
		return;
	}
	runtime->tournament.OnTerminal(event.matchGeneration, capture->result, capture->captureFrame, capture->confirmedFrame);
}

netplay::tournament::Status TournamentStatus() {
	netplay::tournament::Status status;
	status.list = runtime->assignmentList;
	status.link = runtime->openedLink;
	status.connect = runtime->openedConnect;
	status.roomLink = runtime->openedRoomLink;
	const auto& play = runtime->tournament;
	status.phase = play.GetPhase();
	status.bridgeId = play.BridgeId();
	status.matchId = play.MatchId();
	status.reason = play.Reason();
	status.waitingForOpponent = play.WaitingForOpponent();
	status.waitingForPermit = play.WaitingForPermit();
	const auto& watch = runtime->watch;
	status.watch.phase = watch.GetPhase();
	status.watch.bridgeId = watch.BridgeId();
	status.watch.matchId = watch.MatchId();
	status.watch.reason = watch.Reason();
	status.watch.full = watch.Full();
	status.watch.notice = watch.Notice();
	status.watch.noticeSequence = watch.NoticeSequence();
	return status;
}

bool EndWatchedRoom(netplay::tournament::WatchRoomEnd end) {
	if (!runtime->watch.Active()) return false;
	runtime->watch.OnRoomEnded(end, GetTickCount64());
	return true;
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
