#include "sf4e__NetplayRuntime.hxx"
#include "sf4e__ReplayStore.hxx"
#include "sf4e__ReplayPlayback.hxx"
#include "sf4e__Gamepads.hxx"
#include "../netplay/PublishCadence.hxx"
#include "../netplay/LatencyBoard.hxx"
#include "../common/HelperFailure.hxx"
#include "sf4e__CustomContent.hxx"

namespace sf4e { namespace NetplayFacade {
namespace internal {
void FillNetworkDiagnostics(platform::DiagnosticsView& view) {
    if(!runtime || !runtime->room) return;
    const auto& probe=runtime->room->Probe();
    view.probeState=probe.status.empty()?0:probe.status=="checking"?1:
        probe.status=="ready"||probe.status=="complete"?2:probe.status=="invalidated"?3:probe.status=="timed_out"?5:probe.status=="local_overload"?6:4;
    view.probeRoute=ClassifyRoute(probe.route);
    view.probeRelay=RelayRegion(probe.route);
    view.netReport=runtime->room->Network();
    view.udpPort=runtime->room->LocalUdpPort();
    view.probeFailure=probe.failureReason;
    view.sent=probe.sent;view.expected=probe.expected;
    view.benchmark=probe.benchmark;view.replies=probe.samples;view.missed=probe.lost;
    view.p50Us=probe.p50RttUs;view.p95Us=probe.p95RttUs;view.p99Us=probe.p99RttUs;view.jitterUs=probe.jitterUs;
    if(runtime->match) for(const auto& entry:runtime->room->Games()) {
        const auto& game=entry.second;
        if(game.generation!=runtime->match->Generation()) continue;
        const auto route=ClassifyRoute(game.route);
        if(route==RouteKind::Direct) ++view.directLinks;
        if(route==RouteKind::Relayed) ++view.relayedLinks;
        view.routeChanges+=game.routeChanges;view.localDrops+=game.localDrops;view.sendPressure+=game.congestionEvents;
    }
}

std::string StickyText() {
    switch (runtime->stickyError) {
    case StickyError::StartFailed: return loc::T("runtime.network_start_failed");
    case StickyError::HelperUnavailable:
        switch (netplay::ClassifyHelperFailure(runtime->stickyCode)) {
        case netplay::HelperFailure::Missing: return loc::T("runtime.network_helper_missing");
        case netplay::HelperFailure::Blocked: return loc::T("runtime.network_helper_blocked");
        default: return loc::Tf("runtime.network_helper_unavailable", runtime->stickyCode);
        }
    default: return {};
    }
}
void RaiseStickyError(StickyError kind, std::uint32_t code) {
    runtime->stickyError = kind; runtime->stickyCode = code;
    runtime->error = runtime->stickyText = StickyText();
}

// The networking errors stay until networking is back; the others expire.
bool StickyRuntimeError(const std::string& error) {
	return runtime->stickyError != StickyError::None && error == runtime->stickyText;
}
} // namespace internal

namespace {
// The session client reports catalog ids; anything else is an internal code
// that must not reach the status line.
std::string RoomErrorText(const std::string& error) {
    if (error.find('.') != std::string::npos) return loc::T(error.c_str());
    static std::string logged;
    if (logged != error) { logged = error; spdlog::warn("Room error without a message: {}", error); }
    return loc::T("runtime.room_request_failed");
}
static void FillRoomView(RuntimeSnapshot& snapshot) {
	if (runtime->attached && UserApp::netplay) {
		snapshot.room = UserApp::netplay->client.GetRoomSnapshot();
		// A class is the bridge's word only in a room this client joined as
		// public, which it knows from its own join; a private room's host
		// writes its own snapshot, so any class in one is dropped here. So are
		// all of them for a player who hides class emblems on their screen.
		if (!runtime->room || !runtime->room->ServerOwned() || !runtime->preferences.classEmblems)
			for (auto& member : snapshot.room.members) member.badge = SkillBadge();
		snapshot.roomReceivedMs = UserApp::netplay->client.RoomSnapshotReceivedMs();
		snapshot.roomNotices.assign(runtime->roomNotices.begin(), runtime->roomNotices.end());
		if (snapshot.room.roomEpoch) {
			for (const auto& member : snapshot.room.members) {
				netplay::MemberView view(member.name);
				view.role = member.seat >= 0 ? netplay::MemberRole::Player : netplay::MemberRole::Spectator;
				view.ready = member.seat >= 0 && member.seat < 2 && member.table >= 0 &&
					member.table < static_cast<std::int8_t>(room::TableCount) &&
					snapshot.room.tables[member.table].ready[member.seat];
				snapshot.members.push_back(std::move(view));
			}
		} else {
			for (const auto& member : UserApp::netplay->client._lobbyData.members) {
				netplay::MemberView view(member.name);
				const auto slot = snapshot.members.size();
				view.role = slot < 2 ? netplay::MemberRole::Player : netplay::MemberRole::Spectator;
				view.ready = slot < 2 && UserApp::netplay->client._matchData.readyMessageNum[slot] != -1;
				snapshot.members.push_back(std::move(view));
			}
		}
		const auto& client = UserApp::netplay->client;
		snapshot.lobbySettings.editionSelect = client._lobbyData.editionSelect;
		snapshot.lobbySettings.roundCount = client._lobbyData.roundCount;
		snapshot.lobbySettings.roundTime = client._lobbyData.roundTime.integral;
		// What the player may do (localSlot, the gates, the delay) is ComputeGates';
		// this fills in what the interface shows beside it.
		const room::Member* localMember = nullptr;
		if (snapshot.room.roomEpoch) {
			const auto local = std::find_if(snapshot.room.members.begin(), snapshot.room.members.end(),
				[&](const room::Member& member) { return member.id == snapshot.room.localMember; });
			if (local != snapshot.room.members.end()) localMember = &*local;
			if (!client.RoomError().empty()) snapshot.helperError = RoomErrorText(client.RoomError());
		}
		if (snapshot.room.roomEpoch) {
			const bool seated = localMember && localMember->table >= 0 && localMember->table < room::TableCount &&
				localMember->seat >= 0 && localMember->seat < 2;
			const auto* table = seated ? &snapshot.room.tables[localMember->table] : nullptr;
            if (table) {
                snapshot.lobbySettings.editionSelect = table->rules.editionSelect;
                snapshot.lobbySettings.roundCount = table->rules.roundCount;
                snapshot.lobbySettings.roundTime = table->rules.roundTime;
                snapshot.autoDelayMeasured=runtime->preferences.autoInputDelay && AutoDelayMeasured();
                const int otherSeat=localMember->seat ? 0 : 1;
                snapshot.opponentDelay=table->ready[otherSeat] ? table->inputDelay[otherSeat] : -1;
                if (runtime->room && UserApp::server) {
                    const auto opponent=localMember->seat ? table->p1 : table->p2;
                    const auto peer=UserApp::server->roomPeerIdentities.find(opponent);
                    const auto& probe=runtime->room->Probe();
                    if (peer!=UserApp::server->roomPeerIdentities.end() && probe.peer==peer->second &&
                        probe.pairRevision==table->revision) {
                        snapshot.probeRoute=ClassifyRoute(probe.route);
                        snapshot.probeRelay=RelayRegion(probe.route);
                        const auto opponentMember=std::find_if(snapshot.room.members.begin(),snapshot.room.members.end(),
                            [&](const room::Member& m){return m.id==opponent;});
                        if(opponentMember!=snapshot.room.members.end()){snapshot.probeOpponent=opponentMember->name;snapshot.probeOpponentNat=opponentMember->nat;}
                        snapshot.probeP50Us=probe.p50RttUs; snapshot.probeP95Us=probe.p95RttUs; snapshot.probeP99Us=probe.p99RttUs;
                        snapshot.probeJitterUs=probe.jitterUs; snapshot.probeBenchmark=probe.benchmark;
                        snapshot.probeStatus=probe.status; snapshot.probeSamples=probe.samples; snapshot.probeLost=probe.lost;
                        snapshot.probeSent=probe.sent; snapshot.probeExpected=probe.expected;
                        snapshot.recommendedDelay=probe.recommended;
                    }
                }
            }
		}
	}
}

static void FillLockReasons(RuntimeSnapshot& snapshot) {
    // Report the actual gate; a pending transition is not the same as Ready.
    if (!snapshot.canEditSelection) {
        snapshot.selectionLockReason = !snapshot.atMainMenu ? loc::T("runtime.lock.return_to_menu_fighter") :
            runtime->pendingAbort ? loc::T("runtime.lock.previous_game_closing") :
			snapshot.room.localTerminalPending ? loc::T("runtime.lock.teardown") :
			runtime->recoveringMatch || snapshot.session.control != netplay::Health::Healthy ? loc::T("runtime.lock.room_recovering") :
            snapshot.session.readyPending || runtime->readyIntent.Parked() ? loc::T("runtime.lock.ready_pending") :
            loc::T("runtime.lock.selection_update");
    }
}

static void PublishDiscordPresence(const RuntimeSnapshot& snapshot) {
    if (runtime->discordClient && runtime->discordClient->State() == platform::HelperState::Connected) {
        const auto nowTick=GetTickCount64();
        // A public room admits by ticket, so its invitation alone would admit nobody.
        const bool offersInvitation = runtime->room && !snapshot.room.serverOwned;
        // A tournament match's room, played or watched, is never offered to friends. Keyed to
        // what this game is doing, not the room's binding: it also covers the window where the
        // match's room is open but not bound yet.
        const bool holdsMatchRoom = offersInvitation &&
            netplay::tournament::HoldsMatchRoom(runtime->tournament.GetPhase(), runtime->watch.GetPhase());
        // The invitation changes on a helper renewal without a room revision, so it is keyed
        // by content.
        std::uint64_t invitationHash = 1469598103934665603ULL;
        if (offersInvitation)
            for (unsigned char c : runtime->room->DiscordInvitation()) { invitationHash ^= c; invitationHash *= 1099511628211ULL; }
        // Every input the message below is built from, except the clock: a secret that
        // expires under an unchanged key is cleared by the companion itself.
        const std::array<std::uint64_t,11> key{{runtime->eventSystemReady,
            runtime->offlineRequested || (runtime->eventSystemReady && !snapshot.atMainMenu && snapshot.session.room==netplay::RoomState::Idle),
            static_cast<std::uint64_t>(runtime->preferences.discordPresence) | (static_cast<std::uint64_t>(runtime->preferences.discordInvites)<<1),
            snapshot.session.generation.room,snapshot.room.revision,
            static_cast<std::uint64_t>(snapshot.session.room) | (static_cast<std::uint64_t>(snapshot.session.match)<<8) |
                (static_cast<std::uint64_t>(snapshot.session.control)<<16),
            snapshot.room.localMember,static_cast<std::uint64_t>(snapshot.atMainMenu),
            snapshot.room.roomEpoch,invitationHash,
            static_cast<std::uint64_t>(offersInvitation) | (static_cast<std::uint64_t>(holdsMatchRoom)<<1) |
                (static_cast<std::uint64_t>(snapshot.room.serverOwned)<<2)}};
        const bool publishDue=nowTick-runtime->discordLastPublish>=1000;
        if (runtime->discordPresenceKeyValid && key==runtime->discordPresenceKey) {
            // Nothing changed. The companion exits after 15 s without a message, so the
            // message it already has is sent again at the same 1 s cadence, as it is.
            if (publishDue && runtime->discordClient->Send(runtime->discordPublished)) runtime->discordLastPublish=nowTick;
            return;
        }
        discord::PresenceInput input(snapshot.session, snapshot.room);
        input.ready = runtime->eventSystemReady;
        input.offline = runtime->offlineRequested || (input.ready && !snapshot.atMainMenu &&
            snapshot.session.room == netplay::RoomState::Idle);
        input.show = runtime->preferences.discordPresence; input.invites = runtime->preferences.discordInvites;
        input.now = static_cast<std::uint64_t>(std::time(nullptr));
        if (offersInvitation) {
            input.secret = runtime->room->DiscordInvitation();
            if (!discord::TicketMetadata(input.secret,input.party,input.expires)) input.secret.clear();
            if (holdsMatchRoom) input.secret.clear();
        }
        const auto value = discord::Describe(input);
        const auto message = nlohmann::json{{"type","presence"},{"epoch",snapshot.session.generation.room},
            {"show",value.show},{"activity",value.activity},{"party",value.party},{"size",value.size},
            {"capacity",value.capacity},{"secret",value.secret},{"expires",value.expires}}.dump();
        if (message != runtime->discordPublished || publishDue) {
            if (runtime->discordClient->Send(message)) {
                runtime->discordPublished=message; runtime->discordLastPublish=nowTick;
                runtime->discordPresenceKey=key;runtime->discordPresenceKeyValid=true;
            }
        } else {
            // The same message the companion already has: the cached copy stands for this key.
            runtime->discordPresenceKey=key;runtime->discordPresenceKeyValid=true;
        }
    }
}

static void TraceSnapshot(const RuntimeSnapshot& snapshot) {
    {
        diag::ScopedTimer traceTimer(diag::OP_TRACE_ENQUEUE);
        TraceFields fields;
        fields.room = static_cast<int>(snapshot.session.room);
        fields.match = static_cast<int>(snapshot.session.match);
        fields.control = static_cast<int>(snapshot.session.control);
        fields.recovery = static_cast<int>(snapshot.session.recovery);
        fields.router = runtime->room ? static_cast<int>(runtime->room->GetState()) : -1;
        if (runtime->room) fields.routerError = runtime->room->Error();
        fields.matchPhase = runtime->match ? static_cast<int>(runtime->match->GetPhase()) : -1;
        if (runtime->match) fields.matchError = runtime->match->LastFailure();
        fields.nativeSocket = Game::Battle::System::ggpo != nullptr;
        fields.resultPending = runtime->resultOutbox.Pending();
        fields.finishPending = runtime->matchFinishedPending;
        fields.leavePending = runtime->leaveRequested;
        fields.terminalPending = runtime->terminalAckPending;
        fields.authorityWritable = !snapshot.session.coordinated || snapshot.session.authorityWritable;
        fields.readyRequested = snapshot.readyRequested; fields.readyGate = snapshot.readyGate;
        if (runtime->room) fields.probe = runtime->room->Probe().status;
        fields.probeFailure = runtime->room ? runtime->room->Probe().failureReason : 0U;
        fields.probeRoute = snapshot.probeRoute;
        fields.probeBenchmark = snapshot.probeBenchmark;
        fields.probeReplies = snapshot.probeSamples;
        fields.probeMissed = snapshot.probeLost;
        fields.probeP50Us = snapshot.probeP50Us;
        fields.probeP95Us = snapshot.probeP95Us;
        fields.probeP99Us = snapshot.probeP99Us;
        fields.probeJitterUs = snapshot.probeJitterUs;
        if (!runtime->lastTraceFields || !(*runtime->lastTraceFields == fields)) {
            const bool recorded = runtime->trace.Record(nlohmann::json{
                {"room", fields.room}, {"match", fields.match},
                {"control", fields.control}, {"recovery", fields.recovery},
                {"router", fields.router}, {"router_error", fields.routerError},
                {"match_phase", fields.matchPhase}, {"match_error", fields.matchError},
                {"native_socket", fields.nativeSocket},
                {"result_pending", fields.resultPending}, {"finish_pending", fields.finishPending},
                {"leave_pending", fields.leavePending}, {"terminal_pending", fields.terminalPending},
                {"authority_writable", fields.authorityWritable}, {"ready_requested", fields.readyRequested}, {"ready_gate", fields.readyGate},
                {"probe", fields.probe}, {"probe_failure", fields.probeFailure},
                {"probe_route", RouteLabel(fields.probeRoute)}, {"probe_benchmark", fields.probeBenchmark},
                {"probe_replies", fields.probeReplies}, {"probe_missed", fields.probeMissed},
                {"probe_p50_us", fields.probeP50Us}, {"probe_p95_us", fields.probeP95Us},
                {"probe_p99_us", fields.probeP99Us}, {"probe_jitter_us", fields.probeJitterUs}
            });
            if (recorded) runtime->lastTraceFields = std::move(fields);
        }
    }
}

// Where the game is for Ember's menu (input::MenuContext): what Publish
// says, and what PublishThrottled schedules by.
input::MenuContext MenuContextNow(bool atMainMenu) {
    // A Practice match is this PC's to end only while it fights in it, under rollback.
    const bool practiceFight = training::MatchPracticeActive() && Game::Battle::System::ggpo && !LocalIsSpectator();
    // A spectator leaves the game it watches only while its stream is live:
    // the game the snapshot names and the room action's dispatch reads, so
    // the menu's Stop watching and the leave it sends agree.
    const bool watchedMatch = WatchedGameNow().Active();
    return atMainMenu ? input::MenuContext::MainMenu :
        training::ControlsAvailable() ? input::MenuContext::OfflineTraining :
        replaystore::GetStatus().exportStage != replay::ExportStage::None ? input::MenuContext::ReplayExport :
        practiceFight ? input::MenuContext::PracticeMatch :
        watchedMatch ? input::MenuContext::WatchedMatch : input::MenuContext::Unavailable;
}

// What a throttled fight republishes on (netplay::PublishCadence): every input
// to the snapshot the match HUD and the input fault check read.
std::uint64_t PublishFingerprint() {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t v) { h ^= v; h *= 1099511628211ULL; };
    const auto mixString = [&](const std::string& s) { for (unsigned char c : s) mix(c); mix(0xffULL); };
    const auto state = runtime->controller.GetSnapshot();
    mix(static_cast<std::uint64_t>(state.room)); mix(static_cast<std::uint64_t>(state.match));
    mix(static_cast<std::uint64_t>(state.control)); mix(static_cast<std::uint64_t>(state.gameplay));
    mix(static_cast<std::uint64_t>(state.recovery)); mix(state.readyPending); mix(state.coordinated);
    mix(state.authorityWritable); mix(state.authorityTerm); mix(state.authorityRevision); mix(state.authorityStalledMs != 0);
    mix(state.generation.room); mix(state.generation.match); mixString(state.error); mix(static_cast<std::uint64_t>(state.fault));
    if (runtime->attached && UserApp::netplay) {
        const auto& room = UserApp::netplay->client.GetRoomSnapshot();
        mix(room.roomEpoch); mix(room.revision); mix(room.localMember);
        mixString(UserApp::netplay->client.RoomError());
        mix(UserApp::netplay->client._outstandingReadyRequestNumber);
    }
    mixString(runtime->error); mix(runtime->matchInputFault); mix(runtime->matchInput.connected);
    mix(runtime->helper ? static_cast<std::uint64_t>(runtime->helper->State()) : 0);
    mix(runtime->match ? static_cast<std::uint64_t>(runtime->match->GetPhase()) : 0);
    mix(runtime->readyIntent.Parked() != nullptr); mix(runtime->lobbyEditIntent.Parked() != nullptr); mix(runtime->pendingAbort != nullptr);
    mix(runtime->readyIntent.Armed()); mix(runtime->readyFailureSequence);
    mix(runtime->opponentChangeSequence); mix(runtime->roomNoticeSequence); mix(static_cast<std::uint64_t>(runtime->opponentFighterWatch.Pending() + 1));
    mix(runtime->trainingCallSequence); mix(runtime->trainingReadySequence); mix((runtime->trainingCall.Remaining(GetTickCount64()) + 999) / 1000);
    { const auto call = runtime->trainingCall.Identity(); mix(call.roomEpoch); mix(call.opponent); mix(call.generation); mix(call.table); mix(call.serial); }
    mix(runtime->recoveringMatch); mix(OverlayPrefs::PersistencePending()); mixString(OverlayPrefs::PersistenceError());
    mix(runtime->services.Snapshot().pending); mixString(runtime->discordStatusId); mix(runtime->discordInvite.Revision());
    for (const auto& preference : netplay::BoolPreferences) mix(runtime->preferences.*preference.member);
    mix(static_cast<unsigned>(runtime->preferences.buttonPrompts));
    mix(runtime->preferences.matchHudSize); mix(runtime->preferences.matchHudAnchor); mix(runtime->preferences.matchHudLayout); mix(runtime->preferences.matchHudNameOffset); mix(runtime->preferences.readySoundVolume);
    mix(static_cast<std::uint64_t>(runtime->input.State())); mix(runtime->input.Ready());
    mixString(runtime->padHint);
    mix(AtMainMenu());
    return h;
}
}

namespace internal {
// This PC is one of the watchers of a game starting or not yet over: a
// locked-in spectator whose table holds its start for them, a spectator in the
// roster frozen at the start (the room says Watching), or a member queued at
// its table whom that roster took in. The controller moves to Preparing only on
// the runtime's next tick, so the room says it first. A watcher left out of the
// game (a receipt of theirs was still open when it began) waits for the next.
// `retired` is the game this PC already left and tore down (RetiredMatchGeneration):
// the room may still list it there (a failed stream keeps its place), but it
// watches nothing of that game now. Any other game still counts.
static bool AdmittedToWatch(const room::Snapshot& room, std::uint64_t retired) {
    if (!room.localMember) return false;
    // A game still being played, or paused over conflicting results (which still
    // belong to it), and not the one retired here.
    const auto current = [&](const room::Table& table) {
        return !netplay::GenerationEnded(table, table.matchGeneration) && !(retired && table.matchGeneration == retired);
    };
    for (const auto& member : room.members) {
        if (member.id != room.localMember) continue;
        const bool atTable = member.table >= 0 && member.table < static_cast<int>(room.tables.size());
        if (member.status == room::MemberStatus::Watching && (!atTable || current(room.tables[member.table]))) return true;
        // Leaving now would stall the start the table holds for them.
        if (member.spectatorLocked && atTable) {
            const auto& table = room.tables[member.table];
            if (table.phase == room::TablePhase::Ready && table.spectatorHold) return true;
        }
    }
    const auto listed = [&](const std::vector<room::MemberId>& list) {
        return std::find(list.begin(), list.end(), room.localMember) != list.end();
    };
    // The host says which game's roster took this PC in. The roster outlives a
    // spectator who stopped watching (it still owes them the game's end), so it
    // counts only while they are still among that same table's spectators. A
    // queued member the game took in stays listed there; one who queues after
    // they stopped watching is not taken in again.
    if (room.localMatchGenerationsSent) {
        for (std::size_t i = 0; i < room.tables.size(); ++i) {
            const auto& table = room.tables[i];
            if (current(table) && table.matchGeneration && room.localMatchGenerations[i] == table.matchGeneration &&
                listed(table.spectators)) return true;
        }
        return false;
    }
    // An older host does not, and its snapshot cannot tell a watcher the game took
    // in from one it left out (an open receipt), or one who left the queue or
    // watched again while still streaming (no longer Watching once paused). Any
    // place among the spectators of a game not over refuses, erring on its side.
    for (const auto& table : room.tables)
        if (current(table) && listed(table.spectators)) return true;
    return false;
}
std::uint64_t RetiredMatchGeneration() {
    return runtime ? NetplayFacade::RetiredMatchGeneration(runtime->match.get(), Game::Battle::System::ggpo != nullptr, runtime->recoveringMatch) : 0;
}
bool CanTrain(const netplay::Snapshot& session, bool atMainMenu, const room::Snapshot& room, std::uint64_t retired) {
    return atMainMenu && session.room == netplay::RoomState::Joined &&
        (session.match == netplay::MatchState::None || session.match == netplay::MatchState::PostMatch) &&
        session.control == netplay::Health::Healthy &&
        !session.readyPending && !runtime->readyIntent.Parked() && !runtime->pendingAbort &&
        // A closed room keeps its members until it is left; nobody trains out of it.
        !room.closed && !AdmittedToWatch(room, retired) && room::TrainingCall::MayTrain(room);
}
RuntimeGates ComputeGates(const netplay::Snapshot& state, bool atMainMenu, bool controllerReady, bool helperReady) {
	RuntimeGates gates;
	const bool ggpo = Game::Battle::System::ggpo != nullptr;
	const bool between = state.match == netplay::MatchState::None || state.match == netplay::MatchState::PostMatch;
	gates.editionSelect = runtime->preferences.lobby.editionSelect;
	gates.canEditSelection = atMainMenu && state.room == netplay::RoomState::Idle;
	gates.canOpenRoom = controllerReady && helperReady && atMainMenu && !UserApp::netplay && !UserApp::server && !ggpo;
	gates.canEditPreferences = atMainMenu && state.room == netplay::RoomState::Idle &&
		!UserApp::netplay && !UserApp::server && !ggpo;
	gates.canEditLobby = CanEditLobby();
	static const room::Snapshot noRoom;
	const bool attached = runtime->attached && UserApp::netplay;
	const room::Snapshot& room = attached ? UserApp::netplay->client.GetRoomSnapshot() : noRoom;
	if (attached) {
		const auto& client = UserApp::netplay->client;
		gates.editionSelect = client._lobbyData.editionSelect;
		for (std::size_t i = 0; i < client._lobbyData.members.size(); ++i)
			if (client._lobbyData.members[i].connId == client._cid) gates.localSlot = static_cast<int>(i);
		const room::Member* localMember = nullptr;
		if (room.roomEpoch) {
			const auto local = std::find_if(room.members.begin(), room.members.end(),
				[&](const room::Member& member) { return member.id == room.localMember; });
			if (local != room.members.end()) localMember = &*local;
			gates.localSlot = localMember ? localMember->seat : -1;
		}
		const bool seatSlot = gates.localSlot >= 0 && gates.localSlot < 2;
		const bool matchCanReady = runtime->match && (runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle ||
			(runtime->match->GetPhase() == session::IrohMatchSession::Phase::Ending && state.match == netplay::MatchState::PostMatch));
		gates.canEditSelection = atMainMenu && state.room == netplay::RoomState::Joined && seatSlot && between &&
			!state.readyPending && !runtime->readyIntent.Parked() && client._outstandingReadyRequestNumber == -1 &&
			!client.LocalSelectionLocked(gates.localSlot);
		// readyGate is the environment; canReady additionally requires no Ready
		// in flight. A parked press drains against the gate alone.
		gates.readyGate = atMainMenu && matchCanReady && !runtime->pendingLobbySettings && !runtime->lobbyEditIntent.Parked() &&
			state.room == netplay::RoomState::Joined && seatSlot && client._lobbyData.members.size() >= 2;
		const bool readyInFlight = runtime->readyIntent.Parked() || client._outstandingReadyRequestNumber != -1 ||
			client.LocalSelectionLocked(gates.localSlot) || state.readyPending;
		gates.canReady = gates.readyGate && !readyInFlight;
		if (room.roomEpoch) {
			// Terminal eligibility is part of the committed room snapshot. Keep
			// the player menu gated before any action is attempted; an unrelated
			// chat or projection response must not clear this durable backlog.
			gates.canEditLobby = gates.canEditLobby && !room.localTerminalPending;
			const bool seated = localMember && localMember->table >= 0 && localMember->table < room::TableCount &&
				localMember->seat >= 0 && localMember->seat < 2;
			const auto* table = seated ? &room.tables[localMember->table] : nullptr;
			const bool tableWaiting = table && table->phase == room::TablePhase::Waiting;
			const bool tableReady = tableWaiting && table->p1 && table->p2 && !table->ready[localMember->seat];
			const bool healthyControl = state.control == netplay::Health::Healthy &&
				(!state.coordinated || state.authorityWritable);
			if (table) {
				gates.editionSelect = table->rules.editionSelect;
				gates.selectedDelay = localMember->delayLocked ? localMember->frozenDelay : ReadyDelay();
				gates.delayLocked = localMember->delayLocked || !room::SeatEditable(*table, localMember->seat) ||
					!healthyControl || state.readyPending;
				gates.canProbe = !gates.delayLocked && table->p1 && table->p2;
				if (runtime->room && UserApp::server) {
					const auto opponent = localMember->seat ? table->p1 : table->p2;
					const auto peer = UserApp::server->roomPeerIdentities.find(opponent);
					const auto& probe = runtime->room->Probe();
					if (peer != UserApp::server->roomPeerIdentities.end() && probe.peer == peer->second &&
						probe.pairRevision == table->revision) {
						gates.canApplyDelay = !gates.delayLocked && probe.recommended >= 0;
						if (probe.status == "checking") gates.canProbe = false;
					}
				}
			}
			if (!seated && atMainMenu && between) gates.canEditSelection = healthyControl;
			const bool tableTerminalPending = seated && room.terminalPending[localMember->table];
			gates.readyGate = gates.readyGate && healthyControl && tableReady && !runtime->recoveringMatch &&
				!room.localTerminalPending && !tableTerminalPending && !AutoDelayMeasuring();
			gates.canReady = gates.canReady && gates.readyGate;
			// A seated fighter's table state is already in LocalSelectionLocked.
			gates.canEditSelection = gates.canEditSelection && healthyControl &&
				!runtime->recoveringMatch && !room.localTerminalPending;
		}
		gates.readyGate = gates.readyGate && !runtime->pendingAbort;
		gates.canReady = gates.canReady && !runtime->pendingAbort;
		gates.canEditSelection = gates.canEditSelection && !runtime->pendingAbort;
	}
	gates.canChangeController = gates.canEditSelection && !runtime->readyIntent.Parked() &&
		!state.readyPending && !runtime->lobbyEditIntent.Parked() && !runtime->pendingAbort;
	gates.readyGate = gates.readyGate && controllerReady;
	gates.canReady = gates.canReady && gates.readyGate;
	gates.canTrain = CanTrain(state, atMainMenu, room, RetiredMatchGeneration());
	gates.discordCanSwitch = atMainMenu && !ggpo && between;
	return gates;
}
RuntimeGates CurrentGates() {
	return ComputeGates(runtime->controller.GetSnapshot(), AtMainMenu(), runtime->input.Ready(),
		runtime->helper && runtime->helper->State() == platform::HelperState::Connected);
}
PostPublishState Publish() {
	RuntimeSnapshot snapshot;
	snapshot.session = runtime->controller.GetSnapshot();
	snapshot.helperReady = runtime->helper && runtime->helper->State() == platform::HelperState::Connected;
    snapshot.network = snapshot.helperReady ? netplay::NetworkAvailability::Ready :
        runtime->helper && runtime->helper->State() == platform::HelperState::Connecting ? netplay::NetworkAvailability::Starting : netplay::NetworkAvailability::Unavailable;
    platform::DiagnosticsView diagnostic;
    diagnostic.room = static_cast<int>(snapshot.session.room); diagnostic.match = static_cast<int>(snapshot.session.match);
    diagnostic.control = static_cast<int>(snapshot.session.control); diagnostic.gameplay = static_cast<int>(snapshot.session.gameplay);
    diagnostic.helperReady = snapshot.helperReady;
    FillNetworkDiagnostics(diagnostic);
    snapshot.netReport = diagnostic.netReport;
    if (runtime->room) snapshot.identity = runtime->room->Identity();
    snapshot.identityTicket = runtime->identityTicket; snapshot.identityRequest = runtime->identityRequest;
    snapshot.identityRefusal = runtime->identityRefusal;
    snapshot.tournament = TournamentStatus();
    snapshot.publicRooms = runtime->publicRooms;
    snapshot.rankings = runtime->rankings.Published(GetTickCount64());
    runtime->services.Observe(diagnostic);
    snapshot.services = runtime->services.Snapshot();
    snapshot.inputDevice = runtime->input.Selected();
    snapshot.gameplayBrand = Gamepads::BrandForDevice(snapshot.inputDevice.type, snapshot.inputDevice.index);
    snapshot.promptBrand = gamepads::ResolvePrompts(runtime->preferences.buttonPrompts, snapshot.inputDevice.type, snapshot.gameplayBrand);
    snapshot.pads = runtime->pads; snapshot.padLayer = runtime->padLayer;
    snapshot.runningControllerLayer = runtime->runningControllerLayer;
    snapshot.padSetup = Gamepads::ReadFrame().setup;
    snapshot.padLayerPending = gamepads::LayerPending(runtime->preferences.controllerLayer, runtime->runningControllerLayer);
    snapshot.padHintUntil = runtime->padHintUntil;
    const auto& candidate = runtime->input.Candidate();
    for (auto& pad : snapshot.pads) {
        if (pad.state != gamepads::RowState::Absent && candidate.type == input::PadXInput && pad.slot == candidate.index)
            snapshot.captureCandidate = pad.id;
        if ((pad.state == gamepads::RowState::Unmapped || pad.state == gamepads::RowState::GameRead) &&
            candidate.type == input::PadDirectInput && pad.directInputIndex == candidate.index) snapshot.captureCandidate = pad.id;
        pad.customMapping = pad.customMapping && runtime->preferences.controllerMappings.count(pad.identity.guid) != 0;
    }
    snapshot.controller = snapshot.inputDevice.name;
    snapshot.inputCapture = runtime->input.State();
    snapshot.controllerReady = runtime->input.Ready();
    snapshot.controller = ControllerLabel(snapshot.inputDevice);
    if (AtMainMenu() && runtime->controller.GetSnapshot().room == netplay::RoomState::Idle)
        snapshot.padHint = runtime->padHint;

	snapshot.atMainMenu = AtMainMenu();
	snapshot.practiceMatch = training::MatchPracticeActive();
	const replaystore::Status& replays = replaystore::GetStatus();
	snapshot.replays.ready = snapshot.atMainMenu && replays.step == replaystore::Step::Idle && replaystore::Ready();
	snapshot.replays.notice = replays.notice; snapshot.replays.noticeError = replays.noticeError;
	snapshot.replays.logOpens = replays.logOpens; snapshot.replays.returns = replays.returns;
	snapshot.replays.link = runtime->replayLinkAsked;
	snapshot.replays.captionShown = replays.captionShown;
	replayplayback::Tick(snapshot.inputDevice.type, snapshot.inputDevice.index, snapshot.inputDevice.connected);
	snapshot.replays.playback = replayplayback::GetView();
	snapshot.replays.meterShown = snapshot.replays.playback.playback && snapshot.replays.playback.meter;
	if (snapshot.replays.playback.playback) snapshot.replays.lanes = replayplayback::GetLanes();
	snapshot.replays.exportStage = replays.exportStage; snapshot.replays.exportFrames = replays.exportFrames; snapshot.replays.exportTotal = replays.exportTotal;
	if (replays.captionShown) snapshot.replays.caption = replays.caption;
	snapshot.replays.archive = platform::replays::LatestListing();
	snapshot.replays.detail = platform::replays::LatestDetail();
	snapshot.matchWaitsForMenu = runtime->match && !runtime->matchEntered && !snapshot.atMainMenu &&
		runtime->entryDeferredGeneration && runtime->entryDeferredGeneration == runtime->match->Generation() &&
		runtime->match->GetPhase() == session::IrohMatchSession::Phase::Started;
    snapshot.menuContext = MenuContextNow(snapshot.atMainMenu);
    snapshot.watchedGame = WatchedGameNow();
    snapshot.lockInDrops = runtime->lockInDrops;
    if(snapshot.atMainMenu) for(int fighter=0;fighter<selection::FighterCount;++fighter)
        snapshot.fighterAvailability[fighter]=Dimps::Selection::ReadAvailability(fighter);
    snapshot.installedContent = custom::Installed();
    // The explicit gameplay-device assignment stays authoritative, including
    // keyboard selection, capture, disconnects and local P1/P2 handoff.
    if (snapshot.menuContext != input::MenuContext::Unavailable && snapshot.inputCapture == input::Capture::Idle &&
        !input::FightKeepsMenuAway(netplay::Fighting(snapshot.session.match), snapshot.menuContext)) {
        auto& sample = snapshot.menuController;
        sample.deviceType = snapshot.inputDevice.type;
        sample.deviceIndex = snapshot.inputDevice.index;
        unsigned held = 0, physical = 0;
        sample.connected = snapshot.inputDevice.connected &&
            Dimps::Pad::ReadController(sample.deviceType, sample.deviceIndex, held, &physical);
        // Asked whether or not the read worked, so the prompts stay right.
        sample.menu = MenuButtonsFor(sample.deviceType, sample.deviceIndex);
        sample.buttons = sample.connected ? ui::ControllerButtons(held,sample.deviceType,physical,sample.menu) : 0;
    }
    snapshot.trainingButtons = TrainingButtonsFor(snapshot.inputDevice.type, snapshot.inputDevice.index);
	snapshot.canReplaceRoom = CanBeginReplacement();
	snapshot.displayName = runtime->displayName;
	snapshot.preferences = runtime->preferences;
	snapshot.languagePreference = runtime->languagePreference;
	snapshot.checkUpdatesAtLaunch = runtime->checkUpdatesAtLaunch;
	snapshot.automaticCountry = runtime->automaticCountry;
	snapshot.lobbySettings = runtime->preferences.lobby;
	snapshot.settingsPending = OverlayPrefs::PersistencePending() || runtime->pendingLobbySettings != nullptr || runtime->lobbyEditIntent.Parked() != nullptr;
	snapshot.settingsError = OverlayPrefs::PersistenceError();
	snapshot.helperError = runtime->error;
    snapshot.gameplayInputError=Game::Battle::System::ggpo&&runtime->matchInputFault&&runtime->match&&!LocalIsSpectator()?
        loc::Tf(std::any_of(runtime->pads.begin(), runtime->pads.end(), [&](const gamepads::PadView& pad) {
            return pad.state != gamepads::RowState::Absent && !runtime->matchInput.identity.guid.empty() &&
                gamepads::Same(pad.identity, runtime->matchInput.identity) && pad.slot != runtime->matchInput.index;
        }) ? "runtime.match_input_moved" : "runtime.match_input_blocked", DeviceName(runtime->matchInput)):std::string();
	snapshot.offlineRequested = runtime->offlineRequested;
	snapshot.pendingJoinLink = runtime->pendingJoinLink;
	snapshot.pendingJoinSequence = runtime->pendingJoinSequence;
	constexpr std::chrono::seconds JoinLinkDirectWindow{120};
	snapshot.pendingJoinDirect = runtime->pendingJoinFree &&
		std::chrono::steady_clock::now() - runtime->pendingJoinArrived < JoinLinkDirectWindow;
	if (runtime->room) {
		snapshot.invitation = runtime->room->Invitation();
		snapshot.shortInvitation = runtime->room->ShortInvitation();
		snapshot.shortInvitationPending = runtime->room->ShortInvitationPending();
		snapshot.shortInvitationFailures = runtime->room->ShortInvitationFailures();
		snapshot.memberLatency = runtime->room->MemberLatencies();
	}
	FillRoomView(snapshot);
    {
        const auto gates = ComputeGates(snapshot.session, snapshot.atMainMenu, snapshot.controllerReady, snapshot.helperReady);
        snapshot.canEditPreferences = gates.canEditPreferences; snapshot.canEditSelection = gates.canEditSelection;
        snapshot.canEditLobby = gates.canEditLobby; snapshot.canChangeController = gates.canChangeController;
        snapshot.readyGate = gates.readyGate; snapshot.canReady = gates.canReady;
        snapshot.canOpenRoom = gates.canOpenRoom; snapshot.canTrain = gates.canTrain;
        snapshot.discordCanSwitch = gates.discordCanSwitch;
        snapshot.delayLocked = gates.delayLocked; snapshot.canProbe = gates.canProbe; snapshot.canApplyDelay = gates.canApplyDelay;
        snapshot.localSlot = gates.localSlot; snapshot.selectedDelay = gates.selectedDelay;
    }
    snapshot.readyRequested = runtime->readyIntent.Armed();
    snapshot.readyFailure = runtime->readyFailure; snapshot.readyFailureSequence = runtime->readyFailureSequence;
    snapshot.opponentChangedFighter = runtime->opponentFighterWatch.Pending(); snapshot.opponentChangeSequence = runtime->opponentChangeSequence;
    snapshot.trainingCallSequence = runtime->trainingCallSequence; snapshot.trainingReadySequence = runtime->trainingReadySequence;
    snapshot.trainingReadySeconds = static_cast<int>((runtime->trainingCall.Remaining(GetTickCount64()) + 999) / 1000);
    snapshot.trainingCall = runtime->trainingCall.Identity();
	FillLockReasons(snapshot);
    snapshot.discordPending = runtime->discordInvite.Active();
    snapshot.discordConfirm = runtime->discordInvite.NeedsConfirmation();
    snapshot.discordRevision = runtime->discordInvite.Revision();
    snapshot.discordStatus = loc::T(runtime->discordStatusId);
	PublishDiscordPresence(snapshot);
	TraceSnapshot(snapshot);
    PostPublishState result{snapshot.session,snapshot.discordCanSwitch,snapshot.canOpenRoom};
    bridge::PublishRuntime(std::make_shared<const RuntimeSnapshot>(std::move(snapshot)));
    return result;
}

PostPublishState PublishThrottled() {
    static PostPublishState cached;
    static netplay::PublishCadence cadence;
    // Every tick where the pad drives Ember's menu, since the snapshot carries its sample.
    const bool padDrivesMenu = !Game::Battle::System::ggpo || input::ControllerMenuAvailable(MenuContextNow(AtMainMenu()));
    if (cadence.Due(padDrivesMenu, PublishFingerprint)) cached = Publish();
    return cached;
}

void PumpDiscordClient() {
    if (runtime->discordClient) {
        platform::HelperMessage message;
        for (int budget=0; budget<8 && runtime->discordClient->TryReceive(message); ++budget) {
            try {
                const auto event=nlohmann::json::parse(message.payload);
                if (event.at("type")=="status") {
                    runtime->discordStatusId=event.value("available",false) ?
                        (event.value("registered",false)?"discord.connected":"discord.registration_failed") :
                        "discord.open_desktop";
                } else if (event.at("type")=="join") {
                    const auto state=runtime->controller.GetSnapshot();
                    const auto epoch=event.at("epoch").get<std::uint64_t>();
                    if (epoch != state.generation.room) {
                        spdlog::info("Discord: join ignored; companion epoch {} != room generation {}", epoch, state.generation.room);
                        continue;
                    }
                    const auto secret=event.at("secret").get<std::string>();
                    std::string party; std::uint64_t expires=0;
                    if (!discord::TicketMetadata(secret,party,expires)) { runtime->error=loc::T("discord.invitation_invalid"); continue; }
                    const bool busy=state.room!=netplay::RoomState::Idle || runtime->offlineRequested ||
                        (runtime->eventSystemReady && !AtMainMenu());
                    const bool offered=runtime->discordInvite.Offer(secret,party,expires,event.at("sequence").get<std::uint64_t>(),
                        state.generation.room,busy);
                    spdlog::info("Discord: invitation {} sequence={} room_state={} busy={}", offered ? "pending" : "ignored",
                        event.at("sequence").get<std::uint64_t>(), static_cast<int>(state.room), busy);
                }
            } catch (...) { runtime->discordStatusId="discord.invalid_event"; }
        }
        if (runtime->discordClient->State()==platform::HelperState::Failed) {
            runtime->discordStatusId="discord.companion_stopped";
        }
    }
}

static netplay::LatencyBoardPulse latencyBoard;
// The board's kind of pulse, for the fighter selector.
static netplay::LatencyBoardPulse fighterSelector;

bool FighterSelectorShown(std::uint64_t roomGeneration) {
	return fighterSelector.Shown(roomGeneration, GetTickCount64());
}

void TickMemberLatency() {
	if (!runtime->room) return;
	const auto session = runtime->controller.GetSnapshot();
	static const room::Snapshot none;
	const auto& room = runtime->attached && UserApp::netplay ? UserApp::netplay->client.GetRoomSnapshot() : none;
	const bool shown = latencyBoard.Shown(room.roomEpoch, GetTickCount64());
	runtime->room->WatchLatency(netplay::LatencyWanted(shown, session.room, session.match, room, runtime->replacementPending));
}

void PublishAndTickDiscordInvite() {
	const auto view=PublishThrottled();
    std::string currentParty; std::uint64_t expiry=0;
    if (view.session.room==netplay::RoomState::Joined && runtime->room)
        discord::TicketMetadata(runtime->room->DiscordInvitation(),currentParty,expiry);
    const auto next=runtime->discordInvite.Tick(static_cast<std::uint64_t>(std::time(nullptr)),
        view.session.generation.room,currentParty,view.discordCanSwitch,
        view.session.room==netplay::RoomState::Idle,view.canOpenRoom);
    if (next==discord::Next::Expired) { runtime->error=loc::T("discord.invitation_expired"); spdlog::info("Discord: invitation expired before it could be used"); }
    else if (next==discord::Next::Leave || next==discord::Next::Join) {
        spdlog::info("Discord: submitting {} for the pending invitation", next==discord::Next::Leave ? "leave" : "join");
        // Another room replaces the one being watched, as Leave does.
        runtime->watch.Stop();
        RuntimeCommand invite;
        invite.command={next==discord::Next::Leave ? netplay::CommandKind::LeaveRoom : netplay::CommandKind::JoinInvite,
            view.session.generation,next==discord::Next::Join ? runtime->discordInvite.Secret() : std::string()};
        invite.preferences=runtime->preferences;
        invite.discordRevision=runtime->discordInvite.Revision();
        SubmitRuntimeCommand(std::move(invite));
    }
}
} // namespace internal

std::uint64_t RetiredMatchGeneration(const session::IrohMatchSession* match, bool ggpoLive, bool recovering) {
    // Recovery ends (MatchRecovered) only once the session is idle at the main
    // menu; until then, and while native GGPO lives, nothing is retired yet.
    return match && !ggpoLive && !recovering ? match->RetiredGeneration() : 0;
}

void NoteLatencyBoard(std::uint64_t roomGeneration) {
	internal::latencyBoard.Note(roomGeneration, GetTickCount64());
}

void NoteFighterSelector(std::uint64_t roomGeneration) {
	internal::fighterSelector.Note(roomGeneration, GetTickCount64());
}

bool TrainingHolds(TrainingEntry entry, const netplay::Generation& made, const netplay::Snapshot& session,
    bool atMainMenu, const room::Snapshot& room, std::uint64_t retiredGeneration) {
    if (!runtime || !(session.generation == made) || !atMainMenu) return false;
    return entry == TrainingEntry::Room && internal::CanTrain(session, atMainMenu, room, retiredGeneration);
}

bool TrainingRequestHolds(const netplay::Generation& made) {
    if (!runtime) return false;
    // The client's room as it is now, which may be newer than the last published snapshot.
    const bool attached = runtime->attached && UserApp::netplay;
    return TrainingHolds(TrainingEntry::Room, made, runtime->controller.GetSnapshot(),
        internal::AtMainMenu(), attached ? UserApp::netplay->client.GetRoomSnapshot() : room::Snapshot{}, internal::RetiredMatchGeneration());
}

std::shared_ptr<const RuntimeSnapshot> GetRuntimeSnapshotShared() { return bridge::LatestRuntime(); }

RuntimeSnapshot GetRuntimeSnapshot() {
	return *GetRuntimeSnapshotShared();
}

} } // namespace sf4e::NetplayFacade
