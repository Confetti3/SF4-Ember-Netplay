#include "sf4e__NetplayRuntime.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
// The room has finished this generation at the table: it committed the end of
// this very generation, or the table's phase has already left the game. Not
// IsRuntimeMatchEndCommitted: that is the last game this PC set up successfully,
// and a later one whose setup failed would pass for it.
bool GenerationFinished(const room::Table& table, std::uint64_t generation) {
	return (generation && runtime->committedEndGeneration == generation) || netplay::GenerationEnded(table, generation);
}

// Records the lock release a spectator owes the room after its own setup or
// stream failure of the admitted generation. It reads only the room projection
// and does not depend on a battle having been entered, so it covers a failure
// that happens before GGPO exists, and it runs apart from the generation-scoped
// action that retires the live link. A failure of a game whose end is known
// (checkFinished) owes nothing. Recording the same generation again keeps the
// release already pending, with its queued copy.
bool ArmSpectatorLockRelease(std::uint64_t generation, bool checkFinished) {
	if (!runtime || !UserApp::netplay || !generation) return false;
	const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
	const auto member = std::find_if(snapshot.members.begin(), snapshot.members.end(),
		[&](const room::Member& item) { return item.id == snapshot.localMember; });
	if (member == snapshot.members.end() || member->table < 0 || member->table >= static_cast<std::int8_t>(room::TableCount)) return false;
	const auto table = static_cast<std::uint8_t>(member->table);
	const bool finished = checkFinished && GenerationFinished(snapshot.tables[table], generation);
	if (!netplay::SpectatorFailureOwesLockRelease(member->seat < 0, generation, finished)) return false;
	if (runtime->spectatorLockRelease.Pending() && runtime->spectatorLockRelease.Generation() == generation) return true;
	runtime->spectatorLockRelease.Arm(table, generation, snapshot.roomEpoch, GetTickCount64());
	spdlog::info("Room: spectator failed generation={} table={}; lock release armed", generation, table);
	return true;
}

void ReportMatchAbort() {
	if (!runtime->attached || !runtime->match || !UserApp::netplay) return;
	auto& client = UserApp::netplay->client;
	const auto& snapshot = client.GetRoomSnapshot();
	const auto member = std::find_if(snapshot.members.begin(), snapshot.members.end(),
		[&](const room::Member& item) { return item.id == snapshot.localMember; });
	if (member == snapshot.members.end() || member->table < 0 || member->table >= room::TableCount) return;
	room::Action action;
	action.kind = member->seat >= 0 ? room::ActionKind::AbortMatch : room::ActionKind::Unwatch;
	// A spectator whose own stream or setup failed leaves this game only: it
	// keeps its place at the table but not its lock-in, so a spectator that
	// keeps failing cannot hold the next start. A player's own Stop watching
	// leaves the table.
	action.keepWatching = member->seat < 0;
	action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
	action.table = static_cast<std::uint8_t>(member->table);
	action.tableRevision = snapshot.tables[action.table].revision;
	// The generation whose grant this PC took up, also when its setup failed
	// before the session recorded it as its own.
	action.matchGeneration = runtime->match->AttemptedGeneration();
	// A game whose end is known needs no abort; the authority would only
	// answer WrongGeneration.
	if (GenerationFinished(snapshot.tables[action.table], action.matchGeneration)) {
		spdlog::info("Room: not reporting the abort of finished generation {} table={}", action.matchGeneration, action.table);
		return;
	}
	if (client.SendRoomAction(action) != session::SendResult::Queued) {
		if (!runtime->pendingAbort || runtime->pendingAbort->matchGeneration == action.matchGeneration)
			runtime->pendingAbort.reset(new room::Action(action));
		if (!runtime->pendingAbortDeadline) runtime->pendingAbortDeadline = GetTickCount64() + 30000;
	}
}

// The battle the runtime entered closed after its GGPO session was retired, so
// no match-ended notice came with it (see SessionlessCloseAction).
static void HandleSessionlessClose(std::uint64_t closedGeneration) {
	const auto matchState = runtime->controller.GetSnapshot().match;
	const bool inProgress = matchState == netplay::MatchState::Playing || matchState == netplay::MatchState::Preparing;
	// The runtime tears these down itself and then waits for the session to retire.
	if (runtime->leaveRequested || runtime->replacementPending) return;
	const auto action = netplay::SessionlessCloseAction(runtime->matchEntered, runtime->enteredGeneration, closedGeneration,
		runtime->match->Generation(), runtime->recoveringMatch, inProgress, LocalIsSpectator(), IsRuntimeMatchEndCommitted(),
		runtime->spectatorStreamFailedGeneration == closedGeneration);
	if (action == netplay::SessionlessClose::Ignore) return;
	spdlog::info("Netplay: battle for generation {} closed without its session spectator={} action={}",
		closedGeneration, LocalIsSpectator(), action == netplay::SessionlessClose::Abort ? "abort" :
		action == netplay::SessionlessClose::LeaveGame ? "leave_game" : "end_view");
	if (action != netplay::SessionlessClose::Abort) {
		// The view ends locally rather than through the recovery path. A
		// spectator's own stream failure already queued its lock release when it
		// was recorded (RetrySpectatorLockRelease), so nothing is sent here.
		runtime->match->End();
		Apply(netplay::EventKind::MatchEnded);
		return;
	}
	ReportMatchAbort();
	runtime->readyIntent.Withdraw(); runtime->lobbyEditIntent.Clear();
	CancelDeferredGgpoClose();
	runtime->match->Abort();
	runtime->recoveringMatch = true;
}

// The room's match is one the game cannot take, and nothing of it was
// written. Leave this game as a failed setup does: the room hears of it, the
// session is retired, and the next game enters as usual.
static void RejectMatchEntry() {
	spdlog::warn("Netplay: leaving generation {}: the game cannot take its match", runtime->match->Generation());
	if (LocalIsSpectator()) ArmSpectatorLockRelease(runtime->match->Generation(), true);
	ReportMatchAbort();
	runtime->readyIntent.Withdraw(); runtime->lobbyEditIntent.Clear();
	internal::AbortLocalMatch(loc::T("runtime.unsupported_match"), NoticeSeverity::Warning);
}

// The entry into a started match waits for the main menu. Say so once, and
// have a spectator that misses P1's window sit the game out.
static void ObserveDeferredEntry() {
	const auto generation = runtime->match->Generation();
	const auto now = GetTickCount64();
	if (runtime->entryDeferredGeneration != generation) {
		runtime->entryDeferredGeneration = generation;
		runtime->entryDeferredSinceMs = now;
		spdlog::info("Match: entry deferred, not at the main menu generation={} spectator={}", generation, LocalIsSpectator());
	}
	if (SpectatorPolicy::EntryGate(false, LocalIsSpectator(), now - runtime->entryDeferredSinceMs) !=
		SpectatorPolicy::EntryStep::SitOut) return;
	// P1 has stopped waiting for this spectator, and entering later would join
	// a stream it no longer serves. The room still lists it for the next game.
	spdlog::info("Match: spectator sits out generation {} (not at the main menu within {} ms)", generation, SpectatorPolicy::SyncDeadlineMs);
	runtime->match->End();
}
}

// The room has committed the end of the current match (any result).
bool IsRuntimeMatchEndCommitted() {
	return runtime && runtime->match && runtime->match->Generation() &&
		runtime->committedEndGeneration == runtime->match->Generation();
}

// A match is being prepared or played, or its GGPO session is live for the
// match (not only draining spectators).
bool IsRuntimeMatchLive() {
	if (Game::Battle::System::ggpo && !DrainingSpectators()) return true;
	if (!runtime || !runtime->match) return false;
	return runtime->match->Live();
}

void NotifyRuntimeBattleClosedWithoutSession() {
	// Called from battle teardown: only record it. TickMatch acts on it.
	if (!runtime || !runtime->match || !runtime->matchEntered) return;
	runtime->sessionlessCloseGeneration = runtime->enteredGeneration;
}

void NotifyRuntimeSpectatorStreamFailed() {
	if (!runtime || !runtime->match || !runtime->matchEntered) return;
	runtime->spectatorStreamFailedGeneration = runtime->enteredGeneration;
	// The spectator keeps watching later games but loses its lock-in, and that
	// is owed however the rest of this generation goes: a result committed
	// before the battle closes must not cancel it. Arm it now for the tick.
	ArmSpectatorLockRelease(runtime->enteredGeneration, false);
}

void NotifyRuntimeMatchEnded() {
	// Called from battle teardown: defer owning-object changes until TickRuntime.
	if (!runtime || !runtime->match) return;
	runtime->matchEnded = true;
	if (!UserApp::netplay || runtime->match->Generation() == 0) return;
	const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
	if (!snapshot.roomEpoch || !snapshot.localMember) return;
	const auto member = std::find_if(snapshot.members.begin(), snapshot.members.end(),
		[&](const room::Member& item) { return item.id == snapshot.localMember; });
	if (member == snapshot.members.end() || member->seat < 0 || member->seat >= 2 ||
		member->table < 0 || member->table >= static_cast<std::int8_t>(room::TableCount)) return;
	const auto table = static_cast<std::uint8_t>(member->table);
	const auto generation = runtime->match->Generation();
	// The retry loop in TickRuntime revalidates the table's generation and
	// drops the notice once the table has moved on. Do not also gate on the
	// phase here: a projection that is one checkpoint behind (Ready, or not
	// yet Playing) used to swallow the only MatchFinished this client sends,
	// and a table where both clients hit that never armed its result timer.
	if (snapshot.tables[table].matchGeneration != generation) {
		spdlog::warn("Match finished: table {} projection generation {} != local {}; not reporting",
			table, snapshot.tables[table].matchGeneration, generation);
		return;
	}
	// Keep a native-finish notice separate from resultPending: a successful
	// result callback may already be waiting for its peer confirmation.
	runtime->matchFinishedPending = true;
	runtime->finishActionId=runtime->finishRetryAt=0;
	runtime->matchFinishedAction.reset();
		runtime->matchFinishedGeneration = generation;
	runtime->matchFinishedTable = table;
}

// The spectator cannot reach its terminal acknowledgement (ReleaseFinishedMatch)
// while it still owns GGPO, so once the room committed the end of the match it
// watches, the exit is bounded from that point. Releasing GGPO also re-arms the
// helper deadline in IrohMatchSession, which cannot age while native GGPO owns
// the socket. Called after the outer tick's GGPO poll, so every datagram P1
// sent before its link closed has reached GGPO when the backlog is read. The
// match is over either way, so the exit sends nothing to the room: the table
// already left this generation.
void PollSpectatorExit() {
	if (!runtime || !runtime->match) return;
	if (!runtime->terminalAckPending || !Game::Battle::System::ggpo || !LocalIsSpectator() ||
		runtime->terminalAckGeneration != runtime->match->Generation()) {
		runtime->match->ClearSpectatorExit();
		return;
	}
	runtime->match->ArmSpectatorExit();
	int backlog = 0;
	const bool drained = GGPO_SUCCEEDED(ggpo_get_spectator_backlog(Game::Battle::System::ggpo, &backlog)) && backlog == 0;
	const auto step = runtime->match->SpectatorExitStep(drained);
	if (step == session::MatchTeardownTiming::SpectatorExit::Wait) return;
	spdlog::info("Room: closing the spectator view of finished generation {} source_closed={} backlog={} cut_short={}",
		runtime->match->Generation(), runtime->match->StreamSourceClosed(), backlog,
		step == session::MatchTeardownTiming::SpectatorExit::RetireCutShort);
	RetireFinishedMatch("spectator_finished");
	// Retiring the session clears a transient notice, so this comes after it.
	if (step == session::MatchTeardownTiming::SpectatorExit::RetireCutShort)
		PushAlert(loc::T("runtime.spectator_close_timeout"), NoticeSeverity::Warning);
}

void NotifyRuntimeMatchResult(room::MatchResult result, std::uint64_t captureFrame, std::uint64_t confirmedFrame) {
	// Native observer calls on the outer game tick, never while resimulating.
	if (!runtime || !runtime->match || !UserApp::netplay || LocalIsSpectator() ||
		(result != room::MatchResult::P1Win && result != room::MatchResult::P2Win && result != room::MatchResult::Draw)) return;
	const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
	const auto member = std::find_if(snapshot.members.begin(), snapshot.members.end(),
		[&](const room::Member& item) { return item.id == snapshot.localMember; });
	if (member == snapshot.members.end() || member->table < 0 || member->table >= room::TableCount) return;
	const auto generation = runtime->match->Generation();
	netplay::MatchResultCapture capture;
	capture.roomId = runtime->room->RoomId();
	capture.roomEpoch = snapshot.roomEpoch;
	capture.generation = generation;
	capture.table = static_cast<std::uint8_t>(member->table);
	capture.slot = static_cast<unsigned>(runtime->match->LocalSlot());
	capture.result = result;
	capture.captureFrame = captureFrame;
	capture.confirmedFrame = confirmedFrame;
	if (runtime->resultOutbox.Capture(capture))
		spdlog::info("Match result: captured table={} generation={} slot={} outcome={}",
			capture.table, generation, capture.slot, static_cast<int>(result));
}

bool GetRuntimeMatchEndpoints(RuntimeMatchEndpoints& endpoints) {
	if (!runtime || !runtime->match || !UserApp::netplay || runtime->match->GetPhase() != session::IrohMatchSession::Phase::Started) return false;
	endpoints.localPort = UserApp::netplay->client._ggpoPort;
	endpoints.localSlot = runtime->match->LocalSlot();
	const auto& roster = runtime->match->Roster();
	if (roster.size() > endpoints.remotePorts.size()) return false;
	endpoints.participantCount = roster.size();
	for (std::size_t slot = 0; slot < roster.size(); ++slot) endpoints.remotePorts[slot] = runtime->match->RemotePort(roster[slot]);
	return endpoints.localPort != 0;
}

void ReleaseRuntimePortToGgpo() {
	if (runtime && runtime->match) runtime->match->ReleasePortToGgpo();
}

namespace internal {
// Retire this client from the current game. The room-facing half of leaving a
// match is ReportMatchAbort; this is the local half, and every caller needs
// both deferral state and the session torn down in the same order.
void AbortLocalMatch(const char* reason, NoticeSeverity severity) {
	CancelDeferredGgpoClose();
	if (Game::Battle::System::ggpo) Game::Battle::System::AbortGgpoMatch(reason, severity);
	runtime->match->Abort(); runtime->recoveringMatch = true;
}

// The counterpart for a game that is already over: end it, and any spectator
// drain, so a parked intent is sent once the helper mappings have closed.
void RetireFinishedMatch(const char* label) {
	CancelDeferredGgpoClose();
	Game::Battle::System::RetireGgpoSession(label);
	runtime->match->End();
}

void RetryPendingAbort() {
	if (runtime->pendingAbort && runtime->attached && UserApp::netplay) {
		const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
		const auto& action = *runtime->pendingAbort;
		// Kept while the projection lags the generation; its deadline bounds that.
		const bool current = runtime->match && runtime->match->AttemptedGeneration() == action.matchGeneration &&
			action.table < room::TableCount && snapshot.roomEpoch == action.roomEpoch &&
			!GenerationFinished(snapshot.tables[action.table], action.matchGeneration);
		if (!current) {
			runtime->pendingAbort.reset();
			runtime->pendingAbortDeadline = 0;
		} else if (runtime->controller.GetSnapshot().control == netplay::Health::Healthy &&
			[&]() {
				// A room snapshot may advance its table revision while this exact
				// generation is recovering. Refresh the revision on retry while
				// retaining the immutable room epoch and match-generation fence.
				auto retry = action;
				retry.tableRevision = snapshot.tables[retry.table].revision;
				return UserApp::netplay->client.SendRoomAction(retry);
			}() == session::SendResult::Queued) {
			runtime->pendingAbort.reset();
			runtime->pendingAbortDeadline = 0;
		} else if (runtime->pendingAbortDeadline && GetTickCount64() >= runtime->pendingAbortDeadline) {
			runtime->error = loc::T("runtime.teardown_report_failed");
			runtime->pendingAbort.reset();
			runtime->pendingAbortDeadline = 0;
			const auto matchState = runtime->controller.GetSnapshot().match;
			Apply(matchState == netplay::MatchState::Playing || matchState == netplay::MatchState::Preparing ?
				netplay::EventKind::GameplayLost : netplay::EventKind::ControlLost, runtime->error);
		}
	}
}

// Sends the lock release a spectator's stream failure armed. It reads only the
// room projection, so it does not depend on the battle or on whether the
// generation's result was committed since. A queued release is not finished:
// it is sent again until the authority accepts it (DrainActionReplies) or the
// projection shows the member unlocked.
void RetrySpectatorLockRelease() {
	if (!runtime->spectatorLockRelease.Pending() || !runtime->attached || !UserApp::netplay) return;
	auto& client = UserApp::netplay->client;
	room::Action action;
	switch (runtime->spectatorLockRelease.Next(client.GetRoomSnapshot(), GetTickCount64(), &action)) {
	case netplay::SpectatorLockRelease::Step::Drop:
		spdlog::info("Room: spectator lock release for generation {} lapsed unconfirmed", runtime->spectatorLockRelease.Generation());
		runtime->spectatorLockRelease.Cancel();
		break;
	case netplay::SpectatorLockRelease::Step::Done:
		spdlog::info("Room: spectator lock release for generation {} confirmed by the room", runtime->spectatorLockRelease.Generation());
		runtime->spectatorLockRelease.Cancel();
		break;
	case netplay::SpectatorLockRelease::Step::Send: {
		if (runtime->controller.GetSnapshot().control != netplay::Health::Healthy) break;
		std::uint64_t actionId = 0;
		if (client.SendRoomAction(action, &actionId) == session::SendResult::Queued) {
			spdlog::info("Room: spectator lock release queued generation={} table={} action={}", action.matchGeneration, action.table, actionId);
			runtime->spectatorLockRelease.Queued(actionId, GetTickCount64());
		}
		break;
	}
	default: break;
	}
}

void RetryMatchFinished() {
	if (runtime->matchFinishedPending && runtime->attached && UserApp::netplay) {
		const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
		// A table that already left play (the result reports ended it) would
		// reject the finish with WrongGeneration, so there is nothing to send.
		const auto* table = runtime->matchFinishedTable < room::TableCount ?
			&snapshot.tables[runtime->matchFinishedTable] : nullptr;
		const bool current = runtime->match && runtime->match->Generation() == runtime->matchFinishedGeneration &&
			table && snapshot.roomEpoch && table->matchGeneration == runtime->matchFinishedGeneration &&
			(table->phase == room::TablePhase::Playing || table->phase == room::TablePhase::Paused);
		if (!current) {
			runtime->matchFinishedPending = false;
			runtime->matchFinishedGeneration = 0;
			runtime->finishActionId=runtime->finishRetryAt=0;
			runtime->matchFinishedAction.reset();
		} else if (runtime->controller.GetSnapshot().control == netplay::Health::Healthy && GetTickCount64()>=runtime->finishRetryAt) {
			const auto now=GetTickCount64();
			room::Action action;
			if(runtime->matchFinishedAction) action=*runtime->matchFinishedAction;
			else {
				action.kind = room::ActionKind::MatchFinished;
				action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
				action.table = runtime->matchFinishedTable;
				action.tableRevision = snapshot.tables[action.table].revision;
				action.matchGeneration = runtime->matchFinishedGeneration;
			}
			auto actionId=runtime->finishActionId;
			const auto sent=actionId ? UserApp::netplay->client.RetryMatchFinished(action,actionId) :
				UserApp::netplay->client.SendRoomAction(action,&actionId);
			if(sent==session::SendResult::Queued) {
				if(!runtime->finishActionId) {
					runtime->finishActionId=actionId;
					runtime->matchFinishedAction=action;
				}
				runtime->finishRetryAt=now+netplay::MatchResultOutbox::RetryDelayMs;
			} else if(sent==session::SendResult::NotConnected || sent==session::SendResult::QueueFull)
				runtime->finishRetryAt=now+netplay::MatchResultOutbox::UnsentRetryDelayMs;
		}
	}
}

void PumpResultOutbox() {
	if (runtime->resultOutbox.Pending() && runtime->attached && UserApp::netplay) {
		const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
		const auto* capture = runtime->resultOutbox.Captured();
		netplay::MatchResultRoomView roomView;
		roomView.roomId = runtime->room ? runtime->room->RoomId() : netplay::RandomRoomId{};
		roomView.roomEpoch = snapshot.roomEpoch;
		roomView.revision = snapshot.revision;
		roomView.liveGeneration = runtime->match ? runtime->match->Generation() : 0;
		roomView.healthy = runtime->controller.GetSnapshot().control == netplay::Health::Healthy;
		if (capture && capture->table < room::TableCount) {
			roomView.tableRevision = snapshot.tables[capture->table].revision;
			roomView.matchGeneration = snapshot.tables[capture->table].matchGeneration;
		}
			netplay::MatchResultSubmission submission;
			const auto now = GetTickCount64();
			const auto poll = runtime->resultOutbox.Poll(roomView, now, submission);
			if (poll == netplay::MatchResultOutbox::PollResult::Invalidated) {
				// The captured win no longer matches the room's table. It was
				// discarded with no trace before; say so and log it.
				spdlog::warn("Match result: captured outcome invalidated table={} generation={} live_generation={} table_generation={}",
					capture ? capture->table : -1, capture ? capture->generation : 0, roomView.liveGeneration, roomView.matchGeneration);
				PushAlert(loc::T("runtime.result_not_recorded"), NoticeSeverity::Warning);
			}
			if (poll == netplay::MatchResultOutbox::PollResult::Ready) {
			room::Action action; action.kind = room::ActionKind::RecordResult;
			action.roomEpoch = submission.roomEpoch; action.revision = submission.revision;
			action.table = submission.table; action.tableRevision = submission.tableRevision;
			action.matchGeneration = submission.generation; action.result = submission.result;
			std::uint64_t actionId = submission.actionId;
			const auto sent = actionId ? UserApp::netplay->client.RetryRoomResult(action, actionId) :
				UserApp::netplay->client.SendRoomAction(action, &actionId);
			if (sent == session::SendResult::Queued) {
				if (!submission.actionId) spdlog::info("Match result: report queued table={} generation={} action={}",
					submission.table, submission.generation, actionId);
				runtime->resultOutbox.MarkQueued(actionId, now);
			}
			else if (sent == session::SendResult::NotConnected || sent == session::SendResult::QueueFull)
				runtime->resultOutbox.MarkUnsent(now);
		}
	}
}

void TickMatch() {
	if (runtime->match) {
		diag::ScopedTimer lifecycleTimer(diag::OP_MATCH_LIFECYCLE);
		if (runtime->matchEnded) {
			runtime->matchEnded = false; runtime->match->End();
			Apply(netplay::EventKind::MatchEnded);
		}
		if (runtime->sessionlessCloseGeneration) {
			const auto closedGeneration = runtime->sessionlessCloseGeneration;
			runtime->sessionlessCloseGeneration = 0;
			HandleSessionlessClose(closedGeneration);
		}
		if (runtime->match && !runtime->match->Tick(Game::Battle::System::ggpo != nullptr)) {
			runtime->error = loc::T("runtime.match_connection_lost");
			// A spectator has no seat to lose: a control reconnect that briefly
			// empties the projection must not turn its stream failure into a room exit.
			if (UserApp::netplay && (LocalIsSpectator() || UserApp::netplay->client.GetRoomSnapshot().roomEpoch)) {
				const bool teardownTimedOut = runtime->match->Error() == "match_teardown_timeout";
				if (!runtime->recoveringMatch) {
					// A spectator's setup can fail before GGPO exists, when no stream
					// failure is ever recorded. Its lock release is owed for the
					// admitted generation all the same, and is not the action that
					// retires the link: that one is sent once and lost with the control.
					if (LocalIsSpectator()) ArmSpectatorLockRelease(runtime->match->AttemptedGeneration(), true);
					ReportMatchAbort();
				}
				runtime->readyIntent.Withdraw(); runtime->lobbyEditIntent.Clear();
				CancelDeferredGgpoClose();
				if (Game::Battle::System::ggpo) Game::Battle::System::AbortGgpoMatch(runtime->error.c_str());
				runtime->match->Abort();
				if (teardownTimedOut) {
					runtime->error = loc::T("runtime.teardown_timeout");
                    runtime->controller.Execute({netplay::CommandKind::LeaveRoom,
                        runtime->controller.GetSnapshot().generation, {}});
                    CloseRoom();
				} else runtime->recoveringMatch = true;
			} else {
				const auto matchState = runtime->controller.GetSnapshot().match;
				Apply(matchState == netplay::MatchState::Playing || matchState == netplay::MatchState::Preparing ?
					netplay::EventKind::GameplayLost : netplay::EventKind::ControlLost, runtime->error);
			}
		} else if (runtime->match) {
			const auto phase = runtime->match->GetPhase();
			const bool sessionLive = runtime->match->Live();
			// The entry of an older generation whose battle is gone would make the
			// controller refuse this grant, and nothing else would ever release it.
			if (netplay::StaleMatchEntry(sessionLive, runtime->matchEntered, runtime->enteredGeneration,
				runtime->match->Generation(), Game::Battle::System::ggpo != nullptr)) {
				const auto stale = runtime->controller.GetSnapshot().match;
				if (stale == netplay::MatchState::Playing || stale == netplay::MatchState::Preparing) {
					spdlog::warn("Netplay: generation {} never closed locally (match={}); releasing it for generation {}",
						runtime->enteredGeneration, static_cast<int>(stale), runtime->match->Generation());
					Apply(netplay::EventKind::MatchEnded);
				}
				ResetMatchEntry();
				runtime->recoveringMatch = false;
			}
			const auto state = runtime->controller.GetSnapshot();
			if (sessionLive && (state.match == netplay::MatchState::None || state.match == netplay::MatchState::PostMatch)) {
				Apply(netplay::EventKind::MatchPreparing); ResetMatchEntry();
			}
			// A grant withdrawn before the battle was entered (a spectator whose
			// link came up after the start) has no battle to close; this ends
			// Preparing so the shell reopens.
			if (!sessionLive && !runtime->matchEntered && state.match == netplay::MatchState::Preparing)
				Apply(netplay::EventKind::MatchEnded);
			if (phase == session::IrohMatchSession::Phase::Started && !runtime->matchEntered) {
				Apply(netplay::EventKind::GameplayReady);
				const auto entry = UserApp::EnterAuthorizedMatch();
				runtime->matchEntered = entry == UserApp::MatchEntry::Entered;
				if (runtime->matchEntered) runtime->enteredGeneration = runtime->match->Generation();
				else if (entry == UserApp::MatchEntry::Rejected) RejectMatchEntry();
				else if (!AtMainMenu()) ObserveDeferredEntry();
			}
			if (runtime->matchEntered && GetGgpoSyncPhase() == GgpoSyncPhase::Running) Apply(netplay::EventKind::MatchStarted);
			if (runtime->match && runtime->controller.GetSnapshot().room == netplay::RoomState::Joined &&
				runtime->controller.GetSnapshot().match == netplay::MatchState::Playing) {
				const auto& roster = runtime->match->Roster();
				const auto& members = UserApp::netplay->client._lobbyData.members;
				const bool missing = std::any_of(roster.begin(), roster.end(), [&](const SessionProtocol::ConnectionID& id) {
					return std::none_of(members.begin(), members.end(), [&](const SessionProtocol::MemberData& member) { return member.connId == id; });
				});
				if (missing && !UserApp::netplay->client.GetRoomSnapshot().roomEpoch) Apply(netplay::EventKind::ControlLost, loc::T("runtime.participant_left_room"));
				else if (missing && runtime->participantLeftGeneration != runtime->match->Generation() &&
					runtime->committedEndGeneration != runtime->match->Generation()) {
					// Custom rooms: the authority ends the game itself (MatchEnded/
					// Abort). The survivor was never told why, only that GGPO
					// timed out. Say it once per game.
					runtime->participantLeftGeneration = runtime->match->Generation();
					PushAlert(loc::T("runtime.participant_left"), NoticeSeverity::Warning);
				}
			}
		}
	}
}

void ReleaseFinishedMatch() {
	// IrohMatchSession reaches Idle only after its bounded helper teardown.
	// Keep the custom-room table projection frozen until native GGPO has also
	// released ownership of the game socket; this prevents a late room update
	// from being mistaken for the next local match roster.
	if (runtime->match && UserApp::netplay && !Game::Battle::System::ggpo &&
		runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle)
		UserApp::netplay->client.ReleaseRoomProjection();
	// Queue the receipt acknowledgement only after both durable boundaries
	// have passed. SessionClient retains the authenticated action until its
	// accepted reply, so a control reconnect cannot release the table early.
	if (runtime->terminalAckPending && runtime->terminalOutcomeConsumed && UserApp::netplay &&
		runtime->match && !Game::Battle::System::ggpo &&
		runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle) {
		if (UserApp::netplay->client.AcknowledgeTerminal(runtime->terminalAckTable,
			runtime->terminalAckGeneration) == session::SendResult::Queued) {
			runtime->terminalAckPending = false;
		}
	}
	if (runtime->recoveringMatch && runtime->match && runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle && AtMainMenu()) {
		runtime->recoveringMatch = false; ResetMatchEntry();
		Apply(netplay::EventKind::MatchRecovered, runtime->error);
	}
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
