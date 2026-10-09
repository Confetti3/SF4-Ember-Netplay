#include "sf4e__NetplayRuntime.hxx"

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
    case StickyError::HelperUnavailable: return loc::Tf("runtime.network_helper_unavailable", runtime->stickyCode);
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
		snapshot.roomReceivedMs = UserApp::netplay->client.RoomSnapshotReceivedMs();
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
		for (std::size_t i = 0; i < client._lobbyData.members.size(); ++i)
			if (client._lobbyData.members[i].connId == client._cid) snapshot.localSlot = static_cast<int>(i);
		const room::Member* localMember = nullptr;
		if (snapshot.room.roomEpoch) {
			const auto local = std::find_if(snapshot.room.members.begin(), snapshot.room.members.end(),
				[&](const room::Member& member) { return member.id == snapshot.room.localMember; });
			if (local != snapshot.room.members.end()) localMember = &*local;
			snapshot.localSlot = localMember ? localMember->seat : -1;
			if (!client.RoomError().empty()) snapshot.helperError = RoomErrorText(client.RoomError());
		}
		const bool matchCanReady = runtime->match && (runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle ||
			(runtime->match->GetPhase() == session::IrohMatchSession::Phase::Ending && snapshot.session.match == netplay::MatchState::PostMatch));
		snapshot.canEditSelection = snapshot.atMainMenu && snapshot.session.room == netplay::RoomState::Joined && snapshot.localSlot >= 0 && snapshot.localSlot < 2 &&
			(snapshot.session.match == netplay::MatchState::None || snapshot.session.match == netplay::MatchState::PostMatch) &&
			!snapshot.session.readyPending && !runtime->readyIntent.Parked() && client._outstandingReadyRequestNumber == -1 &&
			!client.LocalSelectionLocked(snapshot.localSlot);
		// readyGate is the environment; canReady additionally requires no Ready
		// in flight. A parked press drains against the gate alone.
		snapshot.readyGate = snapshot.atMainMenu && matchCanReady && !runtime->pendingLobbySettings && !runtime->lobbyEditIntent.Parked() &&
			snapshot.session.room == netplay::RoomState::Joined && snapshot.localSlot >= 0 && snapshot.localSlot < 2 &&
			client._lobbyData.members.size() >= 2;
		const bool readyInFlight = runtime->readyIntent.Parked() || client._outstandingReadyRequestNumber != -1 ||
			client.LocalSelectionLocked(snapshot.localSlot) || snapshot.session.readyPending;
		snapshot.canReady = snapshot.readyGate && !readyInFlight;
		if (snapshot.room.roomEpoch) {
			// Terminal eligibility is part of the committed room snapshot. Keep
			// the player menu gated before any action is attempted; an unrelated
			// chat or projection response must not clear this durable backlog.
			snapshot.canEditLobby = snapshot.canEditLobby && !snapshot.room.localTerminalPending;
			const bool seated = localMember && localMember->table >= 0 && localMember->table < room::TableCount &&
				localMember->seat >= 0 && localMember->seat < 2;
			const auto* table = seated ? &snapshot.room.tables[localMember->table] : nullptr;
			const bool tableWaiting = table && table->phase == room::TablePhase::Waiting;
			const bool tableReady = tableWaiting && table->p1 && table->p2 && !table->ready[localMember->seat];
			const bool healthyControl = snapshot.session.control == netplay::Health::Healthy &&
                (!snapshot.session.coordinated || snapshot.session.authorityWritable);
            if (table) {
                snapshot.lobbySettings.editionSelect = table->rules.editionSelect;
                snapshot.lobbySettings.roundCount = table->rules.roundCount;
                snapshot.lobbySettings.roundTime = table->rules.roundTime;
                snapshot.selectedDelay=localMember->delayLocked ? localMember->frozenDelay : ReadyDelay();
                snapshot.autoDelayMeasured=runtime->preferences.autoInputDelay && AutoDelayMeasured();
                snapshot.delayLocked=localMember->delayLocked || !room::SeatEditable(*table, localMember->seat) ||
                    !healthyControl || snapshot.session.readyPending;
                snapshot.canProbe=!snapshot.delayLocked && table->p1 && table->p2;
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
                        snapshot.canApplyDelay=!snapshot.delayLocked && probe.recommended>=0;
                        if(probe.status=="checking") snapshot.canProbe=false;
                    }
                }
            }
            if (!seated && snapshot.atMainMenu && (snapshot.session.match == netplay::MatchState::None ||
                snapshot.session.match == netplay::MatchState::PostMatch)) snapshot.canEditSelection = healthyControl;
			const bool tableTerminalPending = seated && snapshot.room.terminalPending[localMember->table];
			snapshot.readyGate = snapshot.readyGate && healthyControl && tableReady && !runtime->recoveringMatch &&
				!snapshot.room.localTerminalPending && !tableTerminalPending && !AutoDelayMeasuring();
			snapshot.canReady = snapshot.canReady && snapshot.readyGate;
			// A seated fighter's table state is already in LocalSelectionLocked.
			snapshot.canEditSelection = snapshot.canEditSelection && healthyControl &&
				!runtime->recoveringMatch && !snapshot.room.localTerminalPending;
		}
		snapshot.readyGate = snapshot.readyGate && !runtime->pendingAbort;
		snapshot.canReady = snapshot.canReady && !runtime->pendingAbort;
		snapshot.canEditSelection = snapshot.canEditSelection && !runtime->pendingAbort;
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
        const std::array<std::uint64_t,8> key{{runtime->eventSystemReady,
            runtime->offlineRequested || (runtime->eventSystemReady && !snapshot.atMainMenu && snapshot.session.room==netplay::RoomState::Idle),
            static_cast<std::uint64_t>(runtime->preferences.discordPresence) | (static_cast<std::uint64_t>(runtime->preferences.discordInvites)<<1),
            snapshot.session.generation.room,snapshot.room.revision,
            static_cast<std::uint64_t>(snapshot.session.room) | (static_cast<std::uint64_t>(snapshot.session.match)<<8) |
                (static_cast<std::uint64_t>(snapshot.session.control)<<16),
            snapshot.room.localMember,static_cast<std::uint64_t>(snapshot.atMainMenu)}};
        const bool publishDue=nowTick-runtime->discordLastPublish>=1000;
        if (!runtime->discordPresenceKeyValid || key!=runtime->discordPresenceKey || publishDue) {
        discord::PresenceInput input;
        input.ready = runtime->eventSystemReady;
        input.offline = runtime->offlineRequested || (input.ready && !snapshot.atMainMenu &&
            snapshot.session.room == netplay::RoomState::Idle);
        input.show = runtime->preferences.discordPresence; input.invites = runtime->preferences.discordInvites;
        input.session = snapshot.session; input.room = snapshot.room;
        input.now = static_cast<std::uint64_t>(std::time(nullptr));
        // A public room admits by ticket, so its invitation alone would admit nobody.
        if (runtime->room && !snapshot.room.serverOwned) {
            input.secret = runtime->room->DiscordInvitation();
            if (!discord::TicketMetadata(input.secret,input.party,input.expires)) input.secret.clear();
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
        }
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

// Publish() deep-copies the room snapshot, preferences and member list. In a
// menu it must run every tick (it samples the menu controller), but during a
// fight nothing reads the snapshot except the match HUD and input fault
// check, so republish only when an input to it changed, or every sixth tick
// as a backstop for anything the fingerprint does not cover.
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
    mix(runtime->opponentChangeSequence); mix(static_cast<std::uint64_t>(runtime->opponentFighterWatch.Pending() + 1));
    mix(runtime->recoveringMatch); mix(OverlayPrefs::PersistencePending()); mixString(OverlayPrefs::PersistenceError());
    mix(runtime->services.Snapshot().pending); mixString(runtime->discordStatusId); mix(runtime->discordInvite.Revision());
    mix(runtime->preferences.showMatchHud); mix(runtime->preferences.matchHudSize); mix(runtime->preferences.matchHudRaised); mix(runtime->preferences.matchHudAnchor); mix(runtime->preferences.matchHudLayout); mix(runtime->preferences.matchHudNameOffset); mix(runtime->preferences.readySound); mix(runtime->preferences.readySoundVolume); mix(runtime->preferences.backgroundPlay);
    mix(static_cast<std::uint64_t>(runtime->input.State())); mix(runtime->input.Ready());
    mix(AtMainMenu());
    return h;
}
}

namespace internal {
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
    runtime->services.Observe(diagnostic);
    snapshot.services = runtime->services.Snapshot();
    snapshot.inputDevice = runtime->input.Selected();
    snapshot.controller = snapshot.inputDevice.name;
    snapshot.inputCapture = runtime->input.State();
    snapshot.controllerReady = runtime->input.Ready();
    snapshot.controller = ControllerLabel(snapshot.inputDevice);

	snapshot.atMainMenu = AtMainMenu();
	snapshot.matchWaitsForMenu = runtime->match && !runtime->matchEntered && !snapshot.atMainMenu &&
		runtime->entryDeferredGeneration && runtime->entryDeferredGeneration == runtime->match->Generation() &&
		runtime->match->GetPhase() == session::IrohMatchSession::Phase::Started;
    const bool spectatorControls = runtime->attached && Game::Battle::System::ggpo && LocalIsSpectator() &&
        snapshot.session.match == netplay::MatchState::Playing;
    snapshot.menuContext = spectatorControls ? input::MenuContext::Spectating : snapshot.atMainMenu ? input::MenuContext::MainMenu :
        training::ControlsAvailable() ? input::MenuContext::OfflineTraining : input::MenuContext::Unavailable;
    if(snapshot.atMainMenu) for(int fighter=0;fighter<selection::FighterCount;++fighter)
        snapshot.fighterAvailability[fighter]=Dimps::Selection::ReadAvailability(fighter);
    // The explicit gameplay-device assignment stays authoritative, including
    // keyboard selection, capture, disconnects and local P1/P2 handoff.
    if (snapshot.menuContext != input::MenuContext::Unavailable && snapshot.inputCapture == input::Capture::Idle &&
        snapshot.session.match != netplay::MatchState::Preparing &&
        (snapshot.session.match != netplay::MatchState::Playing || spectatorControls)) {
        auto& sample = snapshot.menuController;
        sample.deviceType = snapshot.inputDevice.type;
        sample.deviceIndex = snapshot.inputDevice.index;
        unsigned held = 0, physical = 0;
        sample.connected = snapshot.inputDevice.connected &&
            Dimps::Pad::ReadController(sample.deviceType, sample.deviceIndex, held,&physical,&sample.selectPhysical,&sample.backPhysical);
        if(sample.deviceType==input::PadXInput){sample.selectPhysical=input::xinput::A;sample.backPhysical=input::xinput::B;}
        sample.buttons = sample.connected ? ui::ControllerButtons(held,sample.deviceType,physical) : 0;
    }
    snapshot.canEditSelection = snapshot.atMainMenu && snapshot.session.room == netplay::RoomState::Idle;
	snapshot.canOpenRoom = snapshot.controllerReady && snapshot.helperReady && snapshot.atMainMenu && !UserApp::netplay && !UserApp::server && !Game::Battle::System::ggpo;
	snapshot.canReplaceRoom = CanBeginReplacement();
	snapshot.displayName = runtime->displayName;
	snapshot.preferences = runtime->preferences;
	snapshot.languagePreference = runtime->languagePreference;
	snapshot.lobbySettings = runtime->preferences.lobby;
	snapshot.canEditPreferences = snapshot.atMainMenu && snapshot.session.room == netplay::RoomState::Idle &&
		!UserApp::netplay && !UserApp::server && !Game::Battle::System::ggpo;
	snapshot.canEditLobby = CanEditLobby();
	snapshot.settingsPending = OverlayPrefs::PersistencePending() || runtime->pendingLobbySettings != nullptr || runtime->lobbyEditIntent.Parked() != nullptr;
	snapshot.settingsError = OverlayPrefs::PersistenceError();
	snapshot.helperError = runtime->error;
    snapshot.gameplayInputError=Game::Battle::System::ggpo&&runtime->matchInputFault&&runtime->match&&!LocalIsSpectator()?
        loc::Tf("runtime.match_input_blocked",DeviceName(runtime->matchInput)):std::string();
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
	}
	FillRoomView(snapshot);
    snapshot.canChangeController = snapshot.canEditSelection && !runtime->readyIntent.Parked() &&
        !snapshot.session.readyPending && !runtime->lobbyEditIntent.Parked() && !runtime->pendingAbort;
    snapshot.readyGate = snapshot.readyGate && snapshot.controllerReady;
    snapshot.canReady = snapshot.canReady && snapshot.readyGate;
    snapshot.readyRequested = runtime->readyIntent.Armed();
    snapshot.readyFailure = runtime->readyFailure; snapshot.readyFailureSequence = runtime->readyFailureSequence;
    snapshot.opponentChangedFighter = runtime->opponentFighterWatch.Pending(); snapshot.opponentChangeSequence = runtime->opponentChangeSequence;
	FillLockReasons(snapshot);
    snapshot.discordPending = runtime->discordInvite.Active();
    snapshot.discordConfirm = runtime->discordInvite.NeedsConfirmation();
    snapshot.discordRevision = runtime->discordInvite.Revision();
    snapshot.discordCanSwitch = snapshot.atMainMenu && !Game::Battle::System::ggpo &&
        (snapshot.session.match == netplay::MatchState::None || snapshot.session.match == netplay::MatchState::PostMatch);
    snapshot.discordStatus = loc::T(runtime->discordStatusId);
	PublishDiscordPresence(snapshot);
	TraceSnapshot(snapshot);
    PostPublishState result{snapshot.session,snapshot.discordCanSwitch,snapshot.canOpenRoom};
    bridge::PublishRuntime(std::make_shared<const RuntimeSnapshot>(std::move(snapshot)));
    return result;
}

PostPublishState PublishThrottled() {
    static PostPublishState cached;
    static std::uint64_t fingerprint = 0;
    static unsigned ticksSincePublish = 0;
    if (!Game::Battle::System::ggpo) {
        fingerprint = 0; ticksSincePublish = 0;
        cached = Publish();
        return cached;
    }
    const auto now = PublishFingerprint();
    if (now != fingerprint || ++ticksSincePublish >= 6) {
        fingerprint = now; ticksSincePublish = 0;
        cached = Publish();
    }
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
        RuntimeCommand invite;
        invite.command={next==discord::Next::Leave ? netplay::CommandKind::LeaveRoom : netplay::CommandKind::JoinInvite,
            view.session.generation,next==discord::Next::Join ? runtime->discordInvite.Secret() : std::string()};
        invite.preferences=runtime->preferences;
        invite.discordRevision=runtime->discordInvite.Revision();
        SubmitRuntimeCommand(std::move(invite));
    }
}
} // namespace internal

std::shared_ptr<const RuntimeSnapshot> GetRuntimeSnapshotShared() { return bridge::LatestRuntime(); }

RuntimeSnapshot GetRuntimeSnapshot() {
	return *GetRuntimeSnapshotShared();
}

} } // namespace sf4e::NetplayFacade
