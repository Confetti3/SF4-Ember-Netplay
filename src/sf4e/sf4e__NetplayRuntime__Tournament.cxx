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
using nlohmann::json;

json Counter(std::uint64_t value) { return value ? json(std::to_string(value)) : json(nullptr); }
json OptionalText(const std::string& text) { return text.empty() ? json(nullptr) : json(text); }

// The helper request for an output, or null for outputs that are not one.
json Request(const Output& output) {
	const auto& play = runtime->tournament;
	switch (output.kind) {
	case Output::Kind::Claim:
		return {{"op", "match_claim"}, {"bridge_id", play.BridgeId()}, {"match_id", play.MatchId()}, {"build", sf4e::sidecarHash}};
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

void SendRequest(const Output& output, std::uint64_t nowMs) {
	const auto request = Request(output);
	std::uint64_t id = 0;
	std::string text;
	try { text = request.dump(); } catch (const json::exception&) { text.clear(); }
	if (!text.empty() && runtime->room && runtime->room->SendTournament(text, &id)) {
		runtime->tournamentRequests[id] = output.kind;
		return;
	}
	spdlog::warn("Tournament: {} request not sent", KindName(output.kind));
	// A report the helper never saw is lost; the room says so.
	if (output.kind == Output::Kind::Report) runtime->tournament.OnFailure(output.kind, "report_not_saved", nowMs);
	else if (output.kind != Output::Kind::Forget) runtime->tournament.OnFailure(output.kind, "helper_unavailable", nowMs);
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
	// Sent again until the room shows it, so a failed send needs no retry here.
	if (client.SendRoomAction(action) != session::SendResult::Queued)
		spdlog::info("Tournament: permit for generation {} not sent yet", output.generation);
}

// A room command the runtime would not carry out leaves the state machine
// waiting out its open timeout; the log says why the room never came.
void RoomCommand(netplay::Command command, const char* what) {
	const auto outcome = DispatchTournamentRoomCommand(std::move(command));
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
	case Output::Kind::Leave:
		if (runtime->controller.GetSnapshot().room == netplay::RoomState::Idle) break;
		RoomCommand({netplay::CommandKind::LeaveRoom, {}, {}}, "leaving the room");
		break;
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

// The helper's answer to an assignment refresh.
void TakeAssignments(const session::TournamentAnswer& answer) {
	runtime->assignmentRequest = 0;
	runtime->assignmentList.loading = false;
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

// How long a match link waits for the helper and the Ember ID: the bridge
// keeps it for a minute. And how long a sent redeem waits for its answer.
constexpr ULONGLONG HandoffWaitMs = 55000;
constexpr ULONGLONG HandoffAnswerMs = 30000;

void HandoffDone(const std::string& matchId, const std::string& error) {
	auto& result = runtime->handoffResult;
	result.pending = false;
	result.match = matchId;
	result.error = error;
	++result.sequence;
	runtime->handoffRequest = 0;
	if (error.empty()) spdlog::info("Tournament: a match link named match {}", matchId);
	else spdlog::info("Tournament: a match link could not be opened: {}", error);
}

// The answer to a redeem: the match the link names.
void TakeHandoff(const session::TournamentAnswer& answer) {
	if (!answer.ok) { HandoffDone({}, answer.reason.empty() ? std::string("unavailable") : answer.reason); return; }
	const auto found = answer.data.find("match_id");
	const bool valid = found != answer.data.end() && found->is_string() &&
		found->get<std::string>().size() == 40 && found->get<std::string>().compare(0, 4, "emt_") == 0;
	if (!valid) { HandoffDone({}, "bridge_invalid_response"); return; }
	HandoffDone(found->get<std::string>(), {});
}

// Sends a waiting match link once the helper and the Ember ID can redeem it.
void RedeemHandoff(bool helperReady) {
	auto& pending = runtime->pendingHandoff;
	const auto now = GetTickCount64();
	// An answer that never comes ends the wait instead of leaving the link
	// shown as opening; its late answer, if any, is then ignored.
	if (runtime->handoffRequest && now - runtime->handoffSentMs > HandoffAnswerMs) HandoffDone({}, "unavailable");
	if (!pending.Valid() || runtime->handoffRequest) return;
	if (now - runtime->handoffArrivedMs > HandoffWaitMs) {
		WipeText(pending.code);
		pending = {};
		HandoffDone({}, "handoff_expired");
		return;
	}
	if (!helperReady || runtime->room->Identity().state != "ready") return;
	auto text = json{{"op", "handoff_redeem"}, {"bridge_id", pending.bridgeId}, {"handoff", pending.code}}.dump();
	std::uint64_t id = 0;
	const bool sent = runtime->room->SendTournament(text, &id);
	WipeText(text);
	runtime->handoffResult.bridge = pending.bridgeId;
	WipeText(pending.code);
	pending = {};
	if (sent) { runtime->handoffRequest = id; runtime->handoffSentMs = now; }
	else HandoffDone({}, "helper_unavailable");
}

void TakeAnswer(const session::TournamentAnswer& answer, std::uint64_t nowMs) {
	if (answer.requestId && answer.requestId == runtime->assignmentRequest) { TakeAssignments(answer); return; }
	if (answer.requestId && answer.requestId == runtime->handoffRequest) { TakeHandoff(answer); return; }
	const auto found = runtime->tournamentRequests.find(answer.requestId);
	if (found == runtime->tournamentRequests.end()) return;
	const auto kind = found->second;
	runtime->tournamentRequests.erase(found);
	auto& play = runtime->tournament;
	if (!answer.ok) {
		const auto code = answer.reason.empty() ? std::string("unavailable") : answer.reason;
		spdlog::info("Tournament: {} refused: {}", KindName(kind), code);
		play.OnFailure(kind, code, nowMs);
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
		play.OnReported();
		break;
	default:
		break;
	}
}

netplay::tournament::RoomView View(bool helperReady) {
	netplay::tournament::RoomView view;
	const auto& state = runtime->controller.GetSnapshot();
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
void QueueTournamentHandoff(tournament_link::Handoff handoff) {
	if (!handoff.Valid()) return;
	WipeText(runtime->pendingHandoff.code);
	runtime->pendingHandoff = std::move(handoff);
	runtime->handoffArrivedMs = GetTickCount64();
	runtime->handoffResult.pending = true;
	runtime->handoffResult.bridge = runtime->pendingHandoff.bridgeId;
	spdlog::info("Tournament: a match link arrived");
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
		}
		return;
	}
	case Op::Play: {
		const auto phase = play.GetPhase();
		const bool active = phase == netplay::tournament::Phase::Claiming || phase == netplay::tournament::Phase::Opening ||
			phase == netplay::tournament::Phase::InRoom;
		if (command.bridgeId.empty() || command.matchId.empty() || active) return;
		if (!helperReady || runtime->controller.GetSnapshot().room != netplay::RoomState::Idle) {
			runtime->error = loc::T("runtime.return_main_menu");
			return;
		}
		spdlog::info("Tournament: playing match {}", command.matchId);
		// Answers to an earlier match's requests are not this one's.
		runtime->tournamentRequests.clear();
		play.Start(command.bridgeId, command.matchId, now);
		return;
	}
	case Op::Stop:
		spdlog::info("Tournament: stopped by the player");
		play.Stop();
		return;
	case Op::Redeem: {
		auto handoff = tournament_link::ParsePasted(command.handoff, command.bridgeId);
		if (handoff.Valid()) QueueTournamentHandoff(std::move(handoff));
		else HandoffDone({}, "invalid_link");
		return;
	}
	default:
		return;
	}
}

void TickTournament(bool helperReady) {
	if (!runtime->room) return;
	QueueTournamentHandoff(runtime->handoffLinks.Take());
	RedeemHandoff(helperReady);
	const auto now = GetTickCount64();
	// The helper is not restarted while the game runs, and the binding names
	// its endpoint: without it the match cannot go on from this game.
	if (runtime->helperLossReported) runtime->tournament.Abandon("helper_lost");
	for (const auto& answer : runtime->room->TakeTournamentAnswers()) TakeAnswer(answer, now);
	const auto before = runtime->tournament.GetPhase();
	for (const auto& output : runtime->tournament.Tick(now, View(helperReady))) Execute(output, now);
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
	status.handoff = runtime->handoffResult;
	const auto& play = runtime->tournament;
	status.phase = play.GetPhase();
	status.bridgeId = play.BridgeId();
	status.matchId = play.MatchId();
	status.reason = play.Reason();
	status.waitingForOpponent = play.WaitingForOpponent();
	status.waitingForPermit = play.WaitingForPermit();
	return status;
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
