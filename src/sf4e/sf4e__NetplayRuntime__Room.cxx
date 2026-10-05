#include "sf4e__NetplayRuntime.hxx"
#include "../session/IdentityEvents.hxx"
#include "../platform/Utf8.hxx"
#include <cwchar>

namespace sf4e { namespace NetplayFacade {
namespace {
std::string CurrentProbePeer(std::uint64_t& revision) {
    revision=0;
    if (!runtime || !runtime->attached || !UserApp::netplay || !UserApp::server) return {};
    const auto& snapshot=UserApp::netplay->client.GetRoomSnapshot();
    const auto local=std::find_if(snapshot.members.begin(),snapshot.members.end(),[&](const room::Member& member){return member.id==snapshot.localMember;});
    if (local==snapshot.members.end() || local->table<0 || local->table>=room::TableCount || local->seat<0 || local->seat>1) return {};
    const auto& table=snapshot.tables[local->table];
    const auto opponent=local->seat==0 ? table.p2 : table.p1;
    const auto peer=UserApp::server->roomPeerIdentities.find(opponent);
    if (peer==UserApp::server->roomPeerIdentities.end()) return {};
    revision=table.revision; return peer->second;
}

// A command is dispatched fresh from the interface, retried from its parked
// intent, or sent by tournament play on the player's behalf.
enum class Attempt { Fresh, Retry, Tournament };
using netplay::DispatchOutcome;
// Chat and table actions (Queue, Watch, Unready and the like) park apart.
static Intent& RoomActionIntent(const RuntimeCommand& command) {
	return command.roomAction.kind == room::ActionKind::Chat ? runtime->chatIntent : runtime->roomActionIntent;
}
// Holds a command the room cannot take yet under its intent's budget.
static DispatchOutcome Defer(Intent& intent, const RuntimeCommand& command, Intent::Budget budget = Intent::Budget::Timed) {
	intent.Defer(command, command.command.generation, GetTickCount64(), budget);
	return DispatchOutcome::Deferred;
}

// Where an export writes: a new file in the settings folder's backups folder,
// which the helper creates. Empty when the settings folder is unknown.
std::string IdentityBackupPath() {
	const auto folder = netplay::SettingsStore::DefaultDirectory();
	if (folder.empty()) return {};
	const auto now = std::time(nullptr);
	std::tm local = {};
	if (localtime_s(&local, &now)) return {};
	wchar_t name[64] = {};
	if (!std::wcsftime(name, 64, L"ember-id-%Y%m%d-%H%M%S.backup", &local)) return {};
	return platform::WideToUtf8(folder + L"\\identity-backups\\" + name);
}

// Sends one identity or bridge request to the helper, or records why not.
// Endpoint-level like the connection check: no room generation applies. Key
// changes wait for the main menu outside a room; bridge and key use waits for
// no live game.
void DispatchIdentity(netplay::IdentityRequest& request, bool helperReady) {
	using netplay::IdentityOp;
	runtime->identityTicket = request.ticket; runtime->identityRequest = 0; runtime->identityRefusal = "";
	const auto op = request.op;
	const bool readOnly = op == IdentityOp::Status || op == IdentityOp::BridgeList ||
		op == IdentityOp::BridgeInspect || op == IdentityOp::LinkList || op == IdentityOp::DiscordStatus;
	const bool changesKey = op == IdentityOp::Enable || op == IdentityOp::Import || op == IdentityOp::Reset;
	if (!helperReady || !runtime->room) { runtime->identityRefusal = "identity.refused.helper"; return; }
	if (changesKey && !GetRuntimeSnapshotShared()->canEditPreferences) { runtime->identityRefusal = "identity.refused.leave_room"; return; }
	if (!readOnly && !changesKey && Game::Battle::System::ggpo) { runtime->identityRefusal = "identity.refused.match"; return; }
	if (op == IdentityOp::Export && (request.path = IdentityBackupPath()).empty()) { runtime->identityRefusal = "identity.refused.no_folder"; return; }
	auto text = session::BuildTournamentRequest(request);
	std::uint64_t id = 0;
	const bool sent = !text.empty() && runtime->room->SendTournament(text, &id);
	WipeText(text);
	if (sent) runtime->identityRequest = id;
	else runtime->identityRefusal = "identity.refused.helper";
}

// Validates and applies one command. Fresh presses come from the interface
// queue; retries come straight from a parked intent, revalidated here.
static DispatchOutcome Dispatch(RuntimeCommand command, bool helperReady, Attempt attempt) {
	if (command.identity.op != netplay::IdentityOp::None) {
		DispatchIdentity(command.identity, helperReady);
		return DispatchOutcome::Dropped;
	}
	if (command.tournament.op != netplay::tournament::Command::Op::None) {
		DispatchTournament(command.tournament, helperReady);
		return DispatchOutcome::Dropped;
	}
	// Validate ownership before even a deferred GGPO teardown side effect.
	if (!(command.command.generation == runtime->controller.GetSnapshot().generation)) return DispatchOutcome::Dropped;
    if (command.discordAction != discord::InviteAction::None) {
        if (!runtime->discordInvite.Matches(command.discordRevision)) return DispatchOutcome::Dropped;
        if (command.discordAction == discord::InviteAction::Cancel) runtime->discordInvite.Cancel();
        else if (AtMainMenu() && GetRuntimeSnapshotShared()->discordCanSwitch && !Game::Battle::System::ggpo &&
            runtime->discordInvite.Confirm(command.command.generation.room, command.discordRevision))
            runtime->offlineRequested=false;
        return DispatchOutcome::Dropped;
    }
    if (command.previewSoundVolume >= 0) {
        PlayChallengerCall((std::min)(command.previewSoundVolume, 100));
        return DispatchOutcome::Dropped;
    }
    if (command.shortInvitation) {
        // The answer, or the failure, reaches the interface through the snapshot.
        if (runtime->room) runtime->room->RequestShortInvitation();
        return DispatchOutcome::Dropped;
    }
    if (command.inputAction != input::Action::None) {
        if (command.inputAction == input::Action::Cancel) { runtime->input.Cancel(); runtime->inputInitialized=true; return DispatchOutcome::Dropped; }
        const auto current = runtime->controller.GetSnapshot();
        if (!AtMainMenu() || !GetRuntimeSnapshotShared()->canChangeController || runtime->readyIntent.Parked() ||
            current.readyPending || (current.match != netplay::MatchState::None && current.match != netplay::MatchState::PostMatch)) return DispatchOutcome::Dropped;
        if (command.inputAction == input::Action::BeginCapture) runtime->input.Begin();
        else if (command.inputAction == input::Action::UseKeyboard) {
            runtime->input.Cancel();
            for (const auto& device : runtime->inputDevices) if (device.type == Dimps::Pad::PADTYPE_RAWINPUT) {
                runtime->input.Adopt(device); runtime->inputInitialized = true;
                input::AssignToSide(device, 0, false); break;
            }
        }
        return DispatchOutcome::Dropped;
    }
    if (command.service != platform::ServiceAction::None) {
        if (command.service == platform::ServiceAction::InstallUpdate ||
            ((command.service == platform::ServiceAction::OpenUpdater || command.service == platform::ServiceAction::OpenRecovery) && !GetRuntimeSnapshotShared()->canEditPreferences)) return DispatchOutcome::Dropped;
        platform::DiagnosticsView diagnostics;
        const auto state = runtime->controller.GetSnapshot();
        diagnostics.room = static_cast<int>(state.room); diagnostics.match = static_cast<int>(state.match);
        diagnostics.control = static_cast<int>(state.control); diagnostics.gameplay = static_cast<int>(state.gameplay);
        diagnostics.helperReady = helperReady; diagnostics.verificationAvailable = state.verificationAvailable;
        diagnostics.pingMs = GetStatus().pingMs;
        FillNetworkDiagnostics(diagnostics);
        diagnostics.selectedDelay=GetRuntimeSnapshotShared()->selectedDelay;
        diagnostics.recoveryCheckpointBuildsAvailable=static_cast<bool>(UserApp::server);
        if(UserApp::server) diagnostics.recoveryCheckpointBuilds=UserApp::server->RecoveryCheckpointBuilds();
        diagnostics.performanceEnabled=diag::Enabled();
        diagnostics.traceDropped=runtime->trace.Dropped();
        diagnostics.traceLastWriteMs=runtime->trace.LastWriteMs();
        diagnostics.logDropped=Platform::AsyncLogDropped();
        if(diagnostics.performanceEnabled) {
            const auto& performance=diag::G();
            const int ops[]={diag::OP_COMPLETE_OUTER_CALL,diag::OP_OUTER_TICK,diag::OP_RUNTIME_TICK,diag::OP_SESSION_CLIENT_STEP,
                diag::OP_SESSION_SERVER_STEP,diag::OP_GGPO_IDLE,diag::OP_ROLLBACK_CALLBACK,
                diag::OP_SAVE_TOTAL,diag::OP_LOAD_TOTAL,diag::OP_PACING_WAIT,
                diag::OP_DIAGNOSTIC_ENQUEUE,diag::OP_TRACE_ENQUEUE,
                diag::OP_FREE_TOTAL,diag::OP_RESTORE_EFFECT,diag::OP_RESTORE_VFX};
            static_assert(sizeof(ops)/sizeof(ops[0])==platform::DiagnosticTimingCount, "diagnostic timing operations must stay fixed");
            for(std::size_t i=0;i<platform::DiagnosticTimingCount;++i) {
                const auto& stat=performance.ops[ops[i]];
                diagnostics.timings[i]={stat.count,stat.hitchCounts[1],stat.MeanMs(),stat.maxMs};
            }
            diagnostics.rollbackCallbacks=performance.totalRollbackCallbacks;
            diagnostics.predictionStalls=performance.predictionStalls;
            diagnostics.predictionSkippedFrames=performance.skipReasons[diag::SKIP_PREDICTION_THRESHOLD];
        }
        if (!runtime->services.Request(command.service, diagnostics)) runtime->error = loc::T("runtime.operation_busy");
        return DispatchOutcome::Dropped;
    }
	const auto kind = command.command.kind;
    // Recheck queued invite work after callbacks and controller changes.
    // A replacement or Cancel must also invalidate a queued Join/Leave.
    if (command.discordRevision) {
        if (!runtime->discordInvite.Matches(command.discordRevision) || !AtMainMenu() ||
            !GetRuntimeSnapshotShared()->discordCanSwitch || Game::Battle::System::ggpo) return DispatchOutcome::Dropped;
        if (runtime->discordInvite.Expired(static_cast<std::uint64_t>(std::time(nullptr)))) {
            runtime->discordInvite.Cancel();
            runtime->error=loc::T("discord.invitation_expired");
            return DispatchOutcome::Dropped;
        }
        if (kind == netplay::CommandKind::JoinInvite && (!runtime->input.Ready() || !helperReady)) return DispatchOutcome::Dropped;
    }
	if (kind == netplay::CommandKind::HostRoom && !command.preferences.Valid()) return DispatchOutcome::Dropped;
	if (kind == netplay::CommandKind::RoomAction) {
		if (!runtime->attached || !UserApp::netplay || !runtime->match) return DispatchOutcome::Dropped;
		// A lock-in the player asks for now is newer than a stream failure, so a
		// release still waiting for it must not undo it.
		runtime->spectatorLockRelease.Observe(command.roomAction);
		// One parked action of each kind, and the newest press wins: an older
		// one retried after this press would undo it (Queue, then Unqueue).
		if (attempt == Attempt::Fresh) RoomActionIntent(command).Clear();
		const auto action = command.roomAction.kind;
		const bool changingTable = action == room::ActionKind::Queue || action == room::ActionKind::Unqueue ||
			action == room::ActionKind::Watch || action == room::ActionKind::Unwatch;
		if (changingTable && Game::Battle::System::ggpo) {
			const bool localSpectator = LocalIsSpectator();
			const bool immediateSpectatorAction = localSpectator &&
				(action == room::ActionKind::Unqueue || action == room::ActionKind::Unwatch);
			if (immediateSpectatorAction) {
				// Removing a queued spectator preserves the current spectator
				// stream. Unwatch is sent before local GGPO retirement so P1
				// receives game_peer_end while the generation is still current.
				if (UserApp::netplay->client.SendRoomAction(command.roomAction) != session::SendResult::Queued) {
					runtime->error = loc::T("runtime.spectator_action_retrying");
					return Defer(runtime->roomActionIntent, command);
				}
				// The player chose to leave: a passing note, not an error.
				if (action == room::ActionKind::Unwatch) AbortLocalMatch(loc::T("runtime.leaving_spectator"), NoticeSeverity::Info);
				return DispatchOutcome::Dispatched;
			}
			if (runtime->controller.GetSnapshot().match == netplay::MatchState::Playing && !localSpectator) return DispatchOutcome::Dropped;
			// The match teardown this starts ends by itself, so the wait has no budget.
			Defer(runtime->roomActionIntent, command, Intent::Budget::Untimed);
			if (DrainingSpectators()) RetireFinishedMatch("iroh_room_action");
			else AbortLocalMatch(loc::T("runtime.returning_room"), NoticeSeverity::Info);
			return DispatchOutcome::Deferred;
		}
	}
	if (kind == netplay::CommandKind::SavePreferences &&
		(!GetRuntimeSnapshotShared()->canEditPreferences || !command.preferences.Valid())) return DispatchOutcome::Dropped;
	if (kind == netplay::CommandKind::SetLobbySettings &&
		(!CanEditLobby() || !command.preferences.lobby.Valid())) return DispatchOutcome::Dropped;
	if (kind == netplay::CommandKind::SetLobbySettings && runtime->match &&
		(runtime->match->GetPhase() != session::IrohMatchSession::Phase::Idle || Game::Battle::System::ggpo)) {
		// P1 may edit the next match before pressing Ready. Use the same
		// explicit post-match drain/retirement boundary as rematch readiness.
		Defer(runtime->lobbyEditIntent, command);
		RetireFinishedMatch("iroh_lobby_settings");
		return DispatchOutcome::Deferred;
	}
	if (kind == netplay::CommandKind::Ready || kind == netplay::CommandKind::Rematch) {
		// A press is never silently dropped. Something the player must fix
		// fails immediately and visibly; everything the room is still
		// finishing (drain, fence, receipt, result) parks the press under
		// one budget and resubmits it here once the gate reopens.
		const auto publishedShared = GetRuntimeSnapshotShared();
		const auto& published = *publishedShared;
		// A set that ended while the press waited can rotate this player into the
		// queue. The seat is gone, so the press quietly ends with it.
		if (attempt == Attempt::Retry && published.session.room == netplay::RoomState::Joined &&
			(published.localSlot < 0 || published.localSlot > 1)) {
			runtime->readyIntent.Clear();
			return DispatchOutcome::Dropped;
		}
		auto* client = UserApp::netplay ? &UserApp::netplay->client : nullptr;
		const bool inFlight = runtime->readyIntent.Parked() || runtime->controller.GetSnapshot().readyPending ||
			(client && (client->_outstandingReadyRequestNumber != -1 ||
				(published.localSlot >= 0 && published.localSlot < 2 && client->LocalSelectionLocked(published.localSlot))));
		if (inFlight) return DispatchOutcome::Dropped;
		const char* refusal = nullptr;
		if (!runtime->input.Ready()) refusal = loc::T("runtime.ready.assign_controller");
		else if (!selection::IsRandomStage(command.stage) && !selection::FindStage(command.stage)) refusal = loc::T("runtime.ready.stage_unavailable");
		else if (command.character.charaID >= 44) refusal = loc::T("runtime.ready.fighter_unavailable");
		else if (!selection::Available(selection::FromNative(command.character), published.lobbySettings.editionSelect,
			Dimps::Selection::ReadAvailability(command.character.charaID)))
			refusal = loc::T("runtime.ready.selection_unavailable");
		else if (published.session.room != netplay::RoomState::Joined || published.localSlot < 0 || published.localSlot > 1)
			refusal = loc::T("runtime.ready.take_seat");
		if (refusal) { FailReady(refusal); return DispatchOutcome::Dropped; }
		// A parked Rematch may drain after the session left PostMatch; the
		// controller accepts Ready in either state.
		if (kind == netplay::CommandKind::Rematch && runtime->controller.GetSnapshot().match != netplay::MatchState::PostMatch)
			command.command.kind = netplay::CommandKind::Ready;
		runtime->readyIntent.Arm(GetTickCount64(), command.command.generation);
		const bool draining = runtime->match &&
			(runtime->match->GetPhase() != session::IrohMatchSession::Phase::Idle || Game::Battle::System::ggpo);
		if (draining || !published.readyGate) {
			Defer(runtime->readyIntent, command);
			// Ready is the explicit handoff from post-match spectator draining.
			if (draining) RetireFinishedMatch("iroh_rematch");
			return DispatchOutcome::Deferred;
		}
	}
	if ((kind == netplay::CommandKind::HostRoom || kind == netplay::CommandKind::JoinInvite) &&
		(!helperReady || !AtMainMenu() || UserApp::netplay || UserApp::server || Game::Battle::System::ggpo)) {
		runtime->error = loc::T("runtime.return_main_menu"); return DispatchOutcome::Dropped;
	}
	if (kind == netplay::CommandKind::ReplaceRoom && !CanBeginReplacement()) return DispatchOutcome::Dropped;
	// Leaving the room by hand stops the tournament match too, or it would
	// only open the room again.
	if (kind == netplay::CommandKind::LeaveRoom && attempt != Attempt::Tournament) runtime->tournament.Stop();
	const auto decision = runtime->controller.Execute(command.command);
	if (!decision.accepted) {
		// The authority checkpoint fence is transient. Dropping a fenced command
		// silently discarded a press the interface had already accepted, which
		// produced repeated pressing until one attempt landed between updates.
		// Ready and room actions park the newest intent and retry it once
		// writable, including after a match recovery that ends within the
		// budget. Every other fenced command is reported (ledger H-006).
		if (kind == netplay::CommandKind::Ready || kind == netplay::CommandKind::Rematch) {
			// The gate closed between publish and execute: park the press
			// under its existing budget rather than losing it.
			if (!runtime->readyIntent.Parked()) return Defer(runtime->readyIntent, command);
		} else if (decision.refusal == netplay::Refusal::Fenced) {
			if (kind == netplay::CommandKind::RoomAction) return Defer(RoomActionIntent(command), command);
			runtime->error = loc::T("runtime.room_catchup_timeout");
		}
		return DispatchOutcome::Dropped;
	}
	if (kind == netplay::CommandKind::RoomAction && command.roomAction.kind == room::ActionKind::Unready)
		runtime->readyIntent.Clear();
    if (command.discordRevision) {
        if (kind == netplay::CommandKind::JoinInvite) runtime->discordInvite.Cancel();
        else if (kind == netplay::CommandKind::LeaveRoom) runtime->discordInvite.LeaveQueued();
    }
	auto outcome = DispatchOutcome::Dispatched;
	switch (decision.effect) {
	case netplay::Effect::SendRoomAction: {
		const auto sent = UserApp::netplay->client.SendRoomAction(command.roomAction);
		// A full queue or a control reconnect is transient: keep the intent
		// under its budget rather than making the player press again.
		if (sent == session::SendResult::NotConnected || sent == session::SendResult::QueueFull)
			outcome = Defer(RoomActionIntent(command), command);
		else if (sent != session::SendResult::Queued) runtime->error = loc::T("runtime.room_action_failed");
		break;
	}
	case netplay::Effect::SavePreferences:
        // UI drafts cannot replace the game-thread-owned match record.
        command.preferences.record=runtime->preferences.record;
		if (OverlayPrefs::SavePlayerPreferences(command.preferences)) {
			runtime->preferences = command.preferences;
			runtime->displayName = command.preferences.displayName;
			runtime->error.clear();
		} else runtime->error = loc::T("runtime.preferences_queue_failed");
		break;
	case netplay::Effect::SetLobbySettings: {
		const auto& settings = command.preferences.lobby;
		if (UserApp::netplay->client.Lobby_SetSettings(settings.editionSelect, settings.roundCount,
			{0, static_cast<short>(settings.roundTime)}, false) == session::SendResult::Queued) {
			runtime->pendingLobbySettings.reset(new netplay::LobbySettings(settings));
			runtime->lobbySettingsDeadline = GetTickCount64() + 15000;
		} else runtime->error = loc::T("runtime.lobby_settings_send_failed");
		break;
	}
	case netplay::Effect::HostRoom:
	case netplay::Effect::JoinInvite: {
		if (decision.effect == netplay::Effect::HostRoom) {
            command.preferences.record=runtime->preferences.record;
			runtime->preferences = command.preferences;
			if (!OverlayPrefs::SavePlayerPreferences(runtime->preferences))
				runtime->error = loc::T("runtime.room_defaults_save_failed");
		}
		runtime->displayName = runtime->preferences.displayName;
		runtime->error.clear(); runtime->offlineRequested = false;
		// A public room's admission carries the bridge's signed ticket, which the
		// helper presents to the room host; a ticket that is not an object is no admission.
		const auto ticket = command.publicTicket.empty() ? nlohmann::json() : nlohmann::json::parse(command.publicTicket, nullptr, false);
		runtime->publicJoin = decision.effect == netplay::Effect::JoinInvite && !command.publicTicket.empty();
		const bool started = decision.effect == netplay::Effect::HostRoom ? runtime->room->Host(sf4e::sidecarHash) :
			command.publicTicket.empty() ? runtime->room->Join(decision.invitation, sf4e::sidecarHash) :
			ticket.is_object() && runtime->room->JoinPublic(decision.invitation, ticket, sf4e::sidecarHash);
		if (!started) Apply(netplay::EventKind::RoomFailed, loc::T("runtime.room_open_failed"));
		break;
	}
	case netplay::Effect::CloseSession:
        if(IsRuntimeRecoveryEnabled() && runtime->attached && UserApp::netplay &&
            UserApp::netplay->client.GetRoomSnapshot().localMember) {
            runtime->leaveRequested=true;
            CancelDeferredGgpoClose();
            Game::Battle::System::RetireGgpoSession("leave_room");
            if(runtime->match) runtime->match->Abort();
        } else CloseRoom();
        break;
    case netplay::Effect::ReplaceRoom:
        runtime->replacementPending=true;
        CancelDeferredGgpoClose();
        Game::Battle::System::RetireGgpoSession("replacement_room");
        if(runtime->match) runtime->match->BeginReplacement();
        break;
    case netplay::Effect::CheckConnection: {
        if(!GetRuntimeSnapshotShared()->canProbe) break;
        if(runtime->room->Probe().status=="checking") break;
        std::uint64_t revision=0; const auto peer=CurrentProbePeer(revision);
        if(peer.empty() || !runtime->room->RequestProbe(peer,runtime->nextProbeRequest++,revision,command.command.benchmark))
            runtime->error=loc::T("runtime.connection_check_unavailable");
        else if(runtime->error==loc::T("runtime.connection_check_unavailable"))
            runtime->error.clear();
        break;
    }
    case netplay::Effect::ApplyDelay: {
        if(GetRuntimeSnapshotShared()->delayLocked) break;
        int selected=command.selectedDelay;
        if(selected==-1) {
            std::uint64_t revision=0; const auto peer=CurrentProbePeer(revision);
            const auto& probe=runtime->room->Probe();
            if(peer.empty() || probe.peer!=peer || probe.pairRevision!=revision) break;
            selected=probe.recommended;
        }
        if(selected<0 || selected>MaximumInputDelay) break;
        selected=PlayableInputDelay(selected);
        runtime->selectedDelay=selected;
        UserApp::netplay->client.SetSelectedDelay(selected);
        runtime->preferences.inputDelay=selected;
        if(!OverlayPrefs::SavePlayerPreferences(runtime->preferences)) runtime->error=loc::T("runtime.delay_save_failed");
        break;
    }
	case netplay::Effect::SendReady: {
		auto& client = UserApp::netplay->client;
        client.SetSelectedDelay(runtime->selectedDelay);
		bool sent = client.PreBattle_SetChara(command.character) == session::SendResult::Queued;
		if (!client._lobbyData.members.empty() && client._lobbyData.members[0].connId == client._cid) {
			sent = client.PreBattle_SetEnv(sf4e::localRand()) == session::SendResult::Queued && sent;
			sent = client.PreBattle_SetStage(selection::ResolveStage(command.stage, sf4e::localRand(), command.randomStageExcluded)) == session::SendResult::Queued && sent;
		}
		if (!sent || client.Lobby_Ready() != session::SendResult::Queued) {
			FailReady(loc::T("runtime.match_settings_send_failed"));
			Apply(netplay::EventKind::ControlLost, loc::T("runtime.match_settings_send_failed"));
		}
		break;
	}
	case netplay::Effect::StartOffline:
		if (UserApp::netplay || UserApp::server) ShutdownNetplay(true);
		runtime->offlineRequested = true;
		break;
	default: break;
	}
	return outcome;
}

// A sent Ready completes when the committed seat flag arrives, or when the
// table has already moved on to preparing the match. Legacy lobbies have no
// seat flag; the acknowledged request is the commit.
static void ObserveReadyCommit() {
	if (!runtime->readyIntent.AwaitingCommit() || !runtime->attached || !UserApp::netplay ||
		runtime->controller.GetSnapshot().readyPending) return;
	const auto& client = UserApp::netplay->client;
	const auto& room = client.GetRoomSnapshot();
	if (!room.roomEpoch) {
		if (client._outstandingReadyRequestNumber == -1) runtime->readyIntent.Commit();
		return;
	}
	const auto place = room::PlaceOf(room, room.localMember);
	if (place.kind != room::Place::Kind::Seat) return;
	const auto& table = room.tables[place.table];
	if (table.ready[place.seat] || table.phase != room::TablePhase::Waiting) runtime->readyIntent.Commit();
}

// Retries a parked command through the same dispatch as a fresh press. The
// intent settles on the attempt's outcome: a deferral keeps its budget
// running, anything else completes it according to its policy.
static void Retry(Intent& intent, bool helperReady) {
	if (auto command = intent.Take()) intent.Settle(Dispatch(std::move(*command), helperReady, Attempt::Retry));
}
}

namespace internal {
// A Ready press that cannot be honoured ends its intent and is announced once.
void FailReady(const char* reason) {
	runtime->readyIntent.Clear();
	runtime->error = reason; runtime->readyFailure = reason; ++runtime->readyFailureSequence;
	spdlog::warn("Ready failed: {}", reason);
}

DispatchOutcome DispatchTournamentRoomCommand(netplay::Command command) {
	RuntimeCommand runtimeCommand;
	runtimeCommand.command = std::move(command);
	runtimeCommand.command.generation = runtime->controller.GetSnapshot().generation;
	runtimeCommand.preferences = runtime->preferences;
	const bool helperReady = runtime->helper && runtime->helper->State() == platform::HelperState::Connected;
	return Dispatch(std::move(runtimeCommand), helperReady, Attempt::Tournament);
}

void DrainCommands(bool helperReady) {
	RuntimeCommand command;
	for (int budget = 0; budget < 8 && runtime->commands->TryPop(command); ++budget)
		Dispatch(std::move(command), helperReady, Attempt::Fresh);
}

void DrainRoomEvents() {
	// The helper was already polled at the top of this tick (and again by
	// the recovery runtime when coordination is active); a reply to a command
	// sent above cannot arrive within the same tick, so a third poll here
	// only repeated the checkpoint pump and message parsing.
	if (runtime->attached && UserApp::netplay) {
		room::Event event;
        while (UserApp::netplay->client.TakeRoomEvent(event)) {
            if(event.kind==room::Event::Kind::MatchEnded) {
				const auto resultTerminal = runtime->room ? runtime->resultOutbox.ObserveTerminal(runtime->room->RoomId(),
					UserApp::netplay->client.GetRoomSnapshot().roomEpoch, event.table, event.matchGeneration, event.result) :
					netplay::MatchResultOutbox::TerminalResult::Unrelated;
				ObserveTournamentTerminal(event, resultTerminal);
				if(event.matchGeneration==runtime->matchFinishedGeneration && event.table==runtime->matchFinishedTable) {
					runtime->matchFinishedPending=false;
					runtime->finishActionId=runtime->finishRetryAt=0;
					runtime->matchFinishedAction.reset();
				}
				// The room's terminal receipt is an authenticated, per-recipient
				// lifecycle record. Receiving this event only starts local
				// consumption; AcknowledgeTerminal is queued below after profile
				// persistence and native/helper retirement.
				if (event.terminalReplay && event.matchGeneration && runtime->room) {
					runtime->terminalAckPending = true;
					runtime->terminalOutcomeConsumed = true; // spectators and aborts need no profile write
					runtime->terminalAckTable = event.table;
					runtime->terminalAckGeneration = event.matchGeneration;
					const auto* capture = runtime->resultOutbox.Captured();
					const bool countedResult = event.result == room::MatchResult::P1Win ||
						event.result == room::MatchResult::P2Win;
					const bool profileMatch = countedResult && capture &&
						resultTerminal == netplay::MatchResultOutbox::TerminalResult::Confirmed;
					// Profile persistence is completed by the poll below, which
					// waits for the writer to report the revision on disk.
					if (profileMatch) runtime->terminalOutcomeConsumed = false;
				}
				if (runtime->match && event.matchGeneration && event.matchGeneration == runtime->match->Generation())
					runtime->committedEndGeneration = event.matchGeneration;
				spdlog::log(runtime->matchEndLog.First(event.table, event.matchGeneration) ? spdlog::level::info : spdlog::level::debug,
					"Room: match ended table={} generation={} result={} replay={}",
					event.table, event.matchGeneration, static_cast<int>(event.result), event.terminalReplay);
            }
			if (event.kind == room::Event::Kind::ResultDisputed)
				spdlog::warn("Match result: disputed table={} generation={} reporter={} result={}",
					event.table, event.matchGeneration, event.member, static_cast<int>(event.result));
			if (!runtime->match || event.kind != room::Event::Kind::MatchEnded ||
				event.matchGeneration != runtime->match->Generation() ||
				(event.result != room::MatchResult::Abort && event.result != room::MatchResult::Cancel)) continue;
			runtime->readyIntent.Withdraw(); runtime->lobbyEditIntent.Clear();
			AbortLocalMatch(loc::T("runtime.table_game_cancelled"));
		}
	}
}

void PersistTerminalOutcome() {
	// Record the confirmed outcome in the profile and hold the terminal receipt
	// until the settings writer reports that revision on disk. Anything that
	// stops the write from landing releases the receipt instead, since it
	// gates the whole table's next match (MatchResultOutbox::PersistProfile).
	const auto* capturedResult = runtime->resultOutbox.Captured();
	if (runtime->terminalAckPending && !runtime->terminalOutcomeConsumed && runtime->room && capturedResult &&
		runtime->terminalAckGeneration == capturedResult->generation &&
		runtime->terminalAckTable == capturedResult->table && capturedResult->slot < 2 &&
		(capturedResult->result == room::MatchResult::P1Win || capturedResult->result == room::MatchResult::P2Win)) {
		netplay::ProfileStore store;
		store.queue = [] { return OverlayPrefs::QueuePlayerPreferences(runtime->preferences); };
		store.saved = [](std::uint64_t revision) { return OverlayPrefs::PlayerPreferencesSaved(revision); };
		store.failed = [] { return !OverlayPrefs::PersistenceError().empty(); };
		using Persistence = netplay::MatchResultOutbox::ProfilePersistence;
		switch (runtime->resultOutbox.PersistProfile(runtime->preferences.record, store, GetTickCount64())) {
		case Persistence::Waiting: return;
		case Persistence::Saved: spdlog::info("Match result: profile record saved"); break;
		case Persistence::NotRequired: break;
		case Persistence::Released:
			spdlog::warn("Match result: profile record not saved, releasing the match: {}",
				OverlayPrefs::PersistenceError().empty() ? std::string("write refused or timed out") : OverlayPrefs::PersistenceError());
			runtime->error = loc::T("runtime.match_record_not_saved");
			break;
		}
		runtime->terminalOutcomeConsumed = true;
	}
}

void DrainActionReplies() {
	if (runtime->attached && UserApp::netplay) {
        SessionClient::ActionReply reply;
        while (UserApp::netplay->client.TakeActionReply(reply)) {
            if(reply.actionId==runtime->leaveActionId && reply.accepted) runtime->leaveAcknowledged=true;
			// Only the authority's acceptance ends a spectator's lock release.
			if (reply.accepted) runtime->spectatorLockRelease.Acknowledged(reply.actionId);
			// A refused Ready fails now, with the room's reason, instead of
			// sitting as "Readying up..." until the intent timeout blames the
			// previous match.
			if (!reply.accepted && !reply.superseded && reply.kindKnown && reply.kind == room::ActionKind::Ready &&
				runtime->readyIntent.Armed()) {
				const auto& text = UserApp::netplay->client.RoomError();
				FailReady(text.empty() ? loc::T("runtime.ready.refused") : loc::T(text.c_str()));
			}
			if (runtime->resultOutbox.ObserveReply(reply.actionId, reply.accepted,
				reply.reason == room::RejectReason::DuplicateResult))
				spdlog::info("Match result: report acknowledged action={} duplicate={}",
					reply.actionId, reply.reason == room::RejectReason::DuplicateResult);
			// WrongGeneration means the table already left this game (the
			// opponent's result closed it first); the authority will never
			// accept the finish report, and retrying it every 500 ms made a
			// checkpoint proposal per retry for the rest of the lobby wait.
			if (reply.actionId==runtime->finishActionId && (reply.accepted || reply.reason==room::RejectReason::WrongGeneration)) {
				runtime->matchFinishedPending=false;
				runtime->finishActionId=runtime->finishRetryAt=0;
				runtime->matchFinishedAction.reset();
			}
        }
    }
}

void ConfirmLobbySettings() {
	if (runtime->pendingLobbySettings && runtime->attached && UserApp::netplay) {
		const auto& lobby = UserApp::netplay->client._lobbyData;
		netplay::LobbySettings current;
		current.editionSelect = lobby.editionSelect; current.roundCount = lobby.roundCount; current.roundTime = lobby.roundTime.integral;
		if (current == *runtime->pendingLobbySettings) {
			runtime->preferences.lobby = current;
			if (!OverlayPrefs::SavePlayerPreferences(runtime->preferences)) runtime->error = loc::T("runtime.lobby_defaults_save_failed");
			runtime->pendingLobbySettings.reset();
		} else if (GetTickCount64() >= runtime->lobbySettingsDeadline) {
			runtime->pendingLobbySettings.reset();
			runtime->error = loc::T("runtime.lobby_settings_rejected");
		}
	}
}

void ResolvePendingIntents(bool helperReady) {
	const auto currentGeneration = runtime->controller.GetSnapshot().generation;
	const auto now = GetTickCount64();
	runtime->roomActionIntent.DropStale(currentGeneration);
	runtime->chatIntent.DropStale(currentGeneration);
	runtime->readyIntent.DropStale(currentGeneration);
	runtime->lobbyEditIntent.DropStale(currentGeneration);
	// A set that ended while a Ready or Rematch was parked can rotate this
	// player into the queue. The seat is gone, so the press ends with it
	// before its gate or budget is consulted.
	if (runtime->readyIntent.Parked()) {
		const auto publishedShared = GetRuntimeSnapshotShared();
		if (publishedShared->session.room == netplay::RoomState::Joined &&
			(publishedShared->localSlot < 0 || publishedShared->localSlot > 1)) runtime->readyIntent.Clear();
	}
	// A match recovery pauses a room action's budget; it restarts once the
	// recovery resolves, which is when the action can be submitted (H-006).
	// Say so when it gives up rather than applying a stale intent.
	for (auto* intent : {&runtime->roomActionIntent, &runtime->chatIntent})
		if (intent->Expired(now, runtime->recoveringMatch)) {
			intent->Clear();
			runtime->error = loc::T("runtime.room_catchup_timeout");
		}
	// A Ready ends without a notice once the room shows its seat is gone (the
	// player stood up, or was removed). Left armed, it ran out its budget and
	// blamed the previous match. Only the committed room decides this: a leave
	// the room refuses keeps the press.
	if (runtime->attached && UserApp::netplay &&
		netplay::DropReadyWithoutSeat(runtime->readyIntent, UserApp::netplay->client.GetRoomSnapshot()))
		spdlog::info("Ready: dropped, the seat it was pressed from is gone");
	// A parked Ready or lobby edit that never gets its turn is reported, not
	// forgotten: the player pressed it and GGPO was already retired for it.
	if (runtime->readyIntent.Expired(now)) FailReady(loc::T("runtime.ready.previous_match_timeout"));
	if (runtime->lobbyEditIntent.Expired(now)) {
		runtime->lobbyEditIntent.Clear();
		runtime->error = loc::T("runtime.previous_match_close_timeout");
	}
	const auto& controlState = runtime->controller.GetSnapshot();
	const bool healthyRoomControl = controlState.control == netplay::Health::Healthy &&
        (!controlState.coordinated || controlState.authorityWritable);
	const bool matchIdle = runtime->match && runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle;
	if (const auto* parked = runtime->roomActionIntent.Parked()) {
		// A spectator leaves while its stream still runs: Unwatch must reach
		// P1 before local GGPO retirement. Everything else waits for the menu.
		const bool spectatorExit = runtime->attached && UserApp::netplay && LocalIsSpectator() && Game::Battle::System::ggpo &&
			(parked->roomAction.kind == room::ActionKind::Unqueue || parked->roomAction.kind == room::ActionKind::Unwatch);
		if (healthyRoomControl && !runtime->recoveringMatch && (spectatorExit || (AtMainMenu() && matchIdle)))
			Retry(runtime->roomActionIntent, helperReady);
	}
	if (runtime->chatIntent.Parked() && healthyRoomControl && !runtime->recoveringMatch && AtMainMenu() && matchIdle)
		Retry(runtime->chatIntent, helperReady);
	// A sent Ready keeps its budget; the pump clears it on commit or failure.
	if (runtime->readyIntent.Parked() && healthyRoomControl && GetRuntimeSnapshotShared()->readyGate &&
		!Game::Battle::System::ggpo && matchIdle)
		Retry(runtime->readyIntent, helperReady);
	if (runtime->lobbyEditIntent.Parked() && healthyRoomControl && matchIdle)
		Retry(runtime->lobbyEditIntent, helperReady);
	if (runtime->attached && UserApp::netplay && runtime->controller.GetSnapshot().readyPending &&
		UserApp::netplay->client._outstandingReadyRequestNumber == -1) Apply(netplay::EventKind::ReadyAcknowledged);
	ObserveReadyCommit();
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
