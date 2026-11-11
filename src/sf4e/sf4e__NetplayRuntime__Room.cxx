#include "sf4e__NetplayRuntime.hxx"
#include "sf4e__Gamepads.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/ReplayPath.hxx"
#include "../platform/Utf8.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__MenuMusic.hxx"
#include "sf4e__GameThemes.hxx"
#include "sf4e__ReplayStore.hxx"
#include "sf4e__Steam.hxx"
#include "sf4e__CustomContent.hxx"
#include "../session/IdentityEvents.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include <cwchar>

namespace sf4e { namespace NetplayFacade {
namespace {
std::string CurrentProbePeer(std::uint64_t& revision, int* tableIndex=nullptr) {
    revision=0;
    if (tableIndex) *tableIndex=-1;
    if (!runtime || !runtime->attached || !UserApp::netplay || !UserApp::server) return {};
    const auto& snapshot=UserApp::netplay->client.GetRoomSnapshot();
    const auto local=std::find_if(snapshot.members.begin(),snapshot.members.end(),[&](const room::Member& member){return member.id==snapshot.localMember;});
    if (local==snapshot.members.end() || local->table<0 || local->table>=room::TableCount || local->seat<0 || local->seat>1) return {};
    const auto& table=snapshot.tables[local->table];
    const auto opponent=local->seat==0 ? table.p2 : table.p1;
    const auto peer=UserApp::server->roomPeerIdentities.find(opponent);
    if (peer==UserApp::server->roomPeerIdentities.end()) return {};
    revision=table.revision;
    if (tableIndex) *tableIndex=local->table;
    return peer->second;
}

// A command is dispatched fresh from the interface, retried from its parked
// intent, or sent by tournament play on the player's behalf.
enum class Attempt { Fresh, Retry, Tournament };
using netplay::DispatchOutcome;
// Chat and table actions (Queue, Watch, Unready and the like) park apart.
static Intent& RoomActionIntent(const commands::RoomAction& command) {
	return command.roomAction.kind == room::ActionKind::Chat ? runtime->chatIntent : runtime->roomActionIntent;
}
// Holds a command the room cannot take yet under its intent's budget: a Ready,
// a room action, a lobby edit, or a leave of the watched game.
template <typename Command>
static DispatchOutcome Defer(Intent& intent, const Command& command, Intent::Budget budget = Intent::Budget::Timed) {
	intent.Defer(RuntimeRequest(command), command.generation, GetTickCount64(), budget);
	return DispatchOutcome::Deferred;
}
// Every command but identity and tournament requests is checked against the
// controller generation it was made for first. Validate ownership before even
// a deferred GGPO teardown side effect.
static bool Current(const netplay::Generation& generation) {
	return generation == runtime->controller.GetSnapshot().generation;
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
	const bool readOnly = netplay::EffectOf(op) == netplay::IdentityEffect::Read;
	const bool changesKey = netplay::EffectOf(op) == netplay::IdentityEffect::KeyChange;
	if (!helperReady || !runtime->room) { runtime->identityRefusal = "identity.refused.helper"; return; }
	if (changesKey && !CurrentGates().canEditPreferences) { runtime->identityRefusal = "identity.refused.leave_room"; return; }
	if (!readOnly && !changesKey && Game::Battle::System::ggpo) { runtime->identityRefusal = "identity.refused.match"; return; }
	if (op == IdentityOp::Export && (request.path = IdentityBackupPath()).empty()) { runtime->identityRefusal = "identity.refused.no_folder"; return; }
	auto text = session::BuildTournamentRequest(request);
	std::uint64_t id = 0;
	const bool sent = !text.empty() && runtime->room->SendTournament(text, &id);
	WipeText(text);
	if (sent) runtime->identityRequest = id;
	else runtime->identityRefusal = "identity.refused.helper";
}

// One handler per command type (RuntimeRequest). Fresh presses come from the
// interface queue; retries come straight from a parked intent, revalidated here.
static DispatchOutcome Handle(commands::Identity& command, bool helperReady, Attempt) {
	DispatchIdentity(command.identity, helperReady);
	return DispatchOutcome::Dropped;
}
static DispatchOutcome Handle(commands::Tournament& command, bool helperReady, Attempt) {
	DispatchTournament(command.tournament, helperReady);
	return DispatchOutcome::Dropped;
}
static DispatchOutcome Handle(commands::Training& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
	// Training from a room or Home sends no room command: the game alone moves, and
	// only if that still holds now, not as it was when the player pressed.
	if (TrainingRequestHolds(command.generation)) sf4e::GameEvents::MainMenu::RequestTraining(command.generation);
	else runtime->error = loc::T(command.training == TrainingEntry::Home ? "home.training_unavailable" : "room.training.unavailable");
	return DispatchOutcome::Dropped;
}
static DispatchOutcome Handle(commands::LeaveWatchedGame& command, bool, Attempt attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
	// Stop watching names the game; what leaving it takes is decided here, so
	// a press never outlives the stream it was about. A retry decides afresh.
	if (attempt == Attempt::Fresh) runtime->roomActionIntent.Clear();
	const auto outcome = LeaveWatchedGame(command.leaveWatchedGame);
	return outcome == DispatchOutcome::Deferred ? Defer(runtime->roomActionIntent, command) : outcome;
}
static DispatchOutcome Handle(commands::DiscordInvite& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
    if (!runtime->discordInvite.Matches(command.discordRevision)) return DispatchOutcome::Dropped;
    if (command.discordAction == discord::InviteAction::Cancel) runtime->discordInvite.Cancel();
    else if (AtMainMenu() && CurrentGates().discordCanSwitch && !Game::Battle::System::ggpo &&
        runtime->discordInvite.Confirm(command.generation.room, command.discordRevision))
        runtime->offlineRequested=false;
    return DispatchOutcome::Dropped;
}
static DispatchOutcome Handle(commands::PreviewSound& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
    PlayChallengerCall((std::min)(command.previewSoundVolume, 100));
    return DispatchOutcome::Dropped;
}
static DispatchOutcome Handle(commands::Replay& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
    RunReplayRequest(command.replay);
    return DispatchOutcome::Dropped;
}
static DispatchOutcome Handle(commands::ShortInvitation& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
    // The answer, or the failure, reaches the interface through the snapshot.
    if (runtime->room) runtime->room->RequestShortInvitation();
    return DispatchOutcome::Dropped;
}
static DispatchOutcome Handle(commands::Pad& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
    HandleControllerRequest(command.padAction); return DispatchOutcome::Dropped;
}
static DispatchOutcome Handle(commands::RoomSettings& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
    SaveRoomSettings(command.preferences); return DispatchOutcome::Dropped;
}
static DispatchOutcome Handle(commands::Input& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
    if (command.inputAction == input::Action::BeginCapture || command.inputAction == input::Action::UseKeyboard) runtime->setupAdoption.reset();
    if (command.inputAction == input::Action::Cancel) { runtime->input.Cancel(); runtime->inputInitialized=true; return DispatchOutcome::Dropped; }
    if (!CanChooseControllerNow()) return DispatchOutcome::Dropped;
    if (command.inputAction == input::Action::BeginCapture) runtime->input.Begin();
    else if (command.inputAction == input::Action::UseKeyboard) {
        runtime->input.Cancel();
        for (const auto& device : runtime->inputDevices) if (device.type == Dimps::Pad::PADTYPE_RAWINPUT) {
            runtime->input.Adopt(device); runtime->inputInitialized = true;
            runtime->padHint.clear();
            runtime->padHints.Selected(device.identity);
            OverlayPrefs::SaveGameplayDevice(device.type, device.index, {});
            LogGameplayController(device, "list");
            input::AssignToSide(device, 0, false); break;
        }
    }
    return DispatchOutcome::Dropped;
}
static DispatchOutcome Handle(commands::Service& command, bool helperReady, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
    if (command.service == platform::ServiceAction::OpenSteamProfile) {
        // Steam's overlay when it is up, otherwise the profile page in the browser.
        SteamProfileOpeners openers;
        openers.overlayAvailable = [] { return steam::OverlayAvailable(); };
        openers.overlay = [](const SteamId& id) { return steam::OpenProfileOverlay(id); };
        openers.browser = [](const SteamId& id) { return runtime->services.OpenSteamProfilePage(id); };
        const auto route = OpenSteamProfile(command.steamProfile, openers);
        spdlog::info("Room: Steam profile opened by {}", route == SteamProfileRoute::Overlay ? "the overlay" : route == SteamProfileRoute::Browser ? "the browser" : "no route");
        if (route == SteamProfileRoute::Refused || route == SteamProfileRoute::Failed) runtime->error = loc::T("runtime.steam_profile_failed");
        return DispatchOutcome::Dropped;
    }
    if (command.service != platform::ServiceAction::None) {
        // In-game install is refused; opening the Launcher needs the game out of a room and a match.
        if (command.service == platform::ServiceAction::InstallUpdate ||
            ((command.service == platform::ServiceAction::OpenRecovery ||
              command.service == platform::ServiceAction::UpdateNow) && !CurrentGates().canEditPreferences)) return DispatchOutcome::Dropped;
        // The launch check and the answer to its offer have their own quiet entry points.
        if (command.service == platform::ServiceAction::LaunchUpdateCheck) { runtime->services.RequestLaunchCheck(); return DispatchOutcome::Dropped; }
        if (command.service == platform::ServiceAction::SwitchUpdateChannel) {
            if (!runtime->services.SwitchUpdateChannel(command.updateChannel)) runtime->error = loc::T("runtime.operation_busy");
            return DispatchOutcome::Dropped;
        }
        if (command.service == platform::ServiceAction::UpdateNow || command.service == platform::ServiceAction::SkipUpdate) {
            const bool accepted = command.service == platform::ServiceAction::UpdateNow ?
                runtime->services.UpdateNow(command.updateOffer) : runtime->services.SkipUpdate(command.updateOffer);
            if (!accepted) runtime->error = loc::T("runtime.operation_busy");
            return DispatchOutcome::Dropped;
        }
        platform::DiagnosticsView diagnostics;
        const auto state = runtime->controller.GetSnapshot();
        diagnostics.room = static_cast<int>(state.room); diagnostics.match = static_cast<int>(state.match);
        diagnostics.control = static_cast<int>(state.control); diagnostics.gameplay = static_cast<int>(state.gameplay);
        diagnostics.helperReady = helperReady; diagnostics.verificationAvailable = state.verificationAvailable;
        diagnostics.pingMs = GetStatus().pingMs;
        FillNetworkDiagnostics(diagnostics);
        diagnostics.selectedDelay=CurrentGates().selectedDelay;
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
        const std::wstring target = command.service == platform::ServiceAction::ShowReplayFile ? platform::Utf8ToWide(command.servicePath.c_str()) : std::wstring();
        const bool accepted = command.service == platform::ServiceAction::SendReport ?
            runtime->services.SendReport(command.reportSubmission) : runtime->services.Request(command.service, diagnostics, target);
        if (!accepted) runtime->error = loc::T("runtime.operation_busy");
        else if (command.service == platform::ServiceAction::SendReport || command.service == platform::ServiceAction::PrepareProblemReport ||
            command.service == platform::ServiceAction::CancelReport) runtime->error.clear();
        return DispatchOutcome::Dropped;
    }
    return DispatchOutcome::Dropped;
}
// The room commands: each is checked here, then decided by the session
// controller (Decide), and its effect is applied here.
static netplay::Decision Decide(netplay::CommandKind kind, const netplay::Generation& generation, std::string invitation = {}, bool benchmark = false) {
	return runtime->controller.Execute({kind, generation, std::move(invitation), benchmark});
}
// The authority checkpoint fence is transient. Dropping a fenced command
// silently discarded a press the interface had already accepted, which
// produced repeated pressing until one attempt landed between updates. Ready
// and room actions park the newest intent and retry it once writable,
// including after a match recovery that ends within the budget (their
// handlers). Every other fenced command is reported (ledger H-006).
static DispatchOutcome Refused(const netplay::Decision& decision) {
	if (decision.refusal == netplay::Refusal::Fenced) runtime->error = loc::T("runtime.room_catchup_timeout");
	return DispatchOutcome::Dropped;
}
// A Discord invitation's leave or join (revision not 0) is checked again as it
// runs, after callbacks and controller changes: a replacement or Cancel also
// ends a queued Join or Leave. False when the command ends here.
static bool InvitationHolds(std::uint64_t revision) {
	if (!revision) return true;
	if (!runtime->discordInvite.Matches(revision) || !AtMainMenu() ||
		!CurrentGates().discordCanSwitch || Game::Battle::System::ggpo) return false;
	if (runtime->discordInvite.Expired(static_cast<std::uint64_t>(std::time(nullptr)))) {
		runtime->discordInvite.Cancel();
		runtime->error = loc::T("discord.invitation_expired");
		return false;
	}
	return true;
}
// A room is opened from the main menu with none open and no game running.
// Playing or watching a match waits for the match's room and acts only on
// that one. A room opened or joined by hand meanwhile is the player's own,
// and is left alone, so the match would wait for its room for good: the
// player stops the match first. The match's own commands pass, and so does
// a Discord invitation to someone watching (which stops the watch before it
// is submitted). A fighter's invitation ends here, or it would be submitted
// again on every tick.
static bool MayOpenRoom(netplay::CommandKind kind, bool helperReady, Attempt attempt, std::uint64_t discordRevision) {
	if (!helperReady || !AtMainMenu() || UserApp::netplay || UserApp::server || Game::Battle::System::ggpo) {
		runtime->error = loc::T("runtime.return_main_menu");
		return false;
	}
	if (attempt != Attempt::Tournament &&
		netplay::tournament::RefusesHandOpenedRoom(kind, runtime->tournament.GetPhase(), runtime->watch.GetPhase())) {
		if (discordRevision) runtime->discordInvite.Cancel();
		runtime->error = loc::T("runtime.in_tournament_match");
		return false;
	}
	return true;
}
// The controller took a host or a join: this PC's room starts afresh.
static void BeginOpening(const room::Charter& charter, JoinKind join) {
	runtime->displayName = runtime->preferences.displayName;
	runtime->error.clear(); runtime->offlineRequested = false;
	runtime->charter = charter;
	runtime->join = join;
}
// Only a room just created carries its tables (`rules`): the join of a public
// one, or a private one hosted from a template. Any other ends the last.
static void ArmCreatedRules(const netplay::TableSetup* rules, const room::Charter& charter) {
	if (!rules) { runtime->createdRules.Cancel("other_room"); return; }
	runtime->createdRules.Start(*rules, GetTickCount64(), charter);
	spdlog::info("Room: created room tables armed format={} rotation={} type={} wired_only={} named={}",
		static_cast<int>(rules->rules[0].format), static_cast<int>(rules->rules[0].rotation), static_cast<int>(charter.type), charter.wiredOnly,
		std::count_if(rules->names.begin(), rules->names.end(), [](const std::string& name) { return !name.empty(); }));
}
static DispatchOutcome Handle(commands::HostRoom& command, bool helperReady, Attempt attempt) {
	if (!Current(command.generation) || !command.preferences.Valid()) return DispatchOutcome::Dropped;
	if (!MayOpenRoom(netplay::CommandKind::HostRoom, helperReady, attempt, 0)) return DispatchOutcome::Dropped;
	const auto decision = Decide(netplay::CommandKind::HostRoom, command.generation);
	if (!decision.accepted) return Refused(decision);
	runtime->EditPreferences() = netplay::AcceptedPreferences(runtime->preferences, command.preferences, static_cast<std::uint64_t>(_time64(nullptr)));
	if (!OverlayPrefs::SavePlayerPreferences(runtime->preferences))
		runtime->error = loc::T("runtime.room_defaults_save_failed");
	BeginOpening(command.charter, JoinKind::Private);
	ArmCreatedRules(command.createdRules ? &*command.createdRules : nullptr, command.charter);
	if (!runtime->room->Host(sf4e::sidecarHash)) Apply(netplay::EventKind::RoomFailed, loc::T("runtime.room_open_failed"));
	return DispatchOutcome::Dispatched;
}
static DispatchOutcome Handle(commands::JoinRoom& command, bool helperReady, Attempt attempt) {
	if (!Current(command.generation) || !InvitationHolds(command.discordRevision)) return DispatchOutcome::Dropped;
	if (command.discordRevision && (!runtime->input.Ready() || !helperReady)) return DispatchOutcome::Dropped;
	if (!MayOpenRoom(netplay::CommandKind::JoinInvite, helperReady, attempt, command.discordRevision)) return DispatchOutcome::Dropped;
	const auto decision = Decide(netplay::CommandKind::JoinInvite, command.generation, command.invitation);
	if (!decision.accepted) return Refused(decision);
	if (command.discordRevision) runtime->discordInvite.Cancel();
	// A public room's admission carries the bridge's signed ticket, which the
	// helper presents to the room host; a ticket that is not an object is no admission.
	const bool admitted = !command.publicTicket.empty();
	BeginOpening(room::Charter(), admitted ? JoinKind::Public : command.join);
	ArmCreatedRules(admitted && command.createdRules ? &*command.createdRules : nullptr, command.charter);
	const auto ticket = admitted ? nlohmann::json::parse(command.publicTicket, nullptr, false) : nlohmann::json();
	const bool started = !admitted ? runtime->room->Join(decision.invitation, sf4e::sidecarHash, command.join == JoinKind::Observer) :
		ticket.is_object() && runtime->room->JoinPublic(decision.invitation, ticket, sf4e::sidecarHash);
	if (!started) Apply(netplay::EventKind::RoomFailed, loc::T("runtime.room_open_failed"));
	return DispatchOutcome::Dispatched;
}
static DispatchOutcome Handle(commands::LeaveRoom& command, bool, Attempt attempt) {
	if (!Current(command.generation) || !InvitationHolds(command.discordRevision)) return DispatchOutcome::Dropped;
	// Leaving the room by hand stops the tournament match too, or it would
	// only open the room again.
	if (attempt != Attempt::Tournament) {
		runtime->tournament.Stop();
		runtime->watch.Stop();
	}
	const auto decision = Decide(netplay::CommandKind::LeaveRoom, command.generation);
	if (!decision.accepted) return Refused(decision);
	if (command.discordRevision) runtime->discordInvite.LeaveQueued();
	if (IsRuntimeRecoveryEnabled() && runtime->attached && UserApp::netplay &&
		UserApp::netplay->client.GetRoomSnapshot().localMember) {
		runtime->leave.leaveRequested = true;
		CancelLocalMatch(GgpoEnd::Retire("leave_room"));
	} else CloseRoom();
	return DispatchOutcome::Dispatched;
}
static DispatchOutcome Handle(commands::ReplaceRoom& command, bool, Attempt) {
	if (!Current(command.generation) || !CanBeginReplacement()) return DispatchOutcome::Dropped;
	const auto decision = Decide(netplay::CommandKind::ReplaceRoom, command.generation);
	if (!decision.accepted) return Refused(decision);
	runtime->replacementPending = true;
	// The replacement is the same kind of room as the one it replaces.
	runtime->charter = UserApp::netplay ? room::CharterOf(UserApp::netplay->client.GetRoomSnapshot()) : room::Charter();
	CancelDeferredGgpoClose();
	Game::Battle::System::RetireGgpoSession("replacement_room");
	if (runtime->match) runtime->match->BeginReplacement();
	return DispatchOutcome::Dispatched;
}
static DispatchOutcome Handle(commands::StartOffline& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
	const auto decision = Decide(netplay::CommandKind::StartOffline, command.generation);
	if (!decision.accepted) return Refused(decision);
	if (UserApp::netplay || UserApp::server) ShutdownNetplay(true);
	runtime->offlineRequested = true;
	return DispatchOutcome::Dispatched;
}
static DispatchOutcome Handle(commands::SavePreferences& command, bool, Attempt) {
	if (!Current(command.generation) || !CurrentGates().canEditPreferences || !command.preferences.Valid()) return DispatchOutcome::Dropped;
	const auto decision = Decide(netplay::CommandKind::SavePreferences, command.generation);
	if (!decision.accepted) return Refused(decision);
	// UI drafts cannot replace the game-thread-owned match record or sharing cutoff.
	command.preferences = netplay::AcceptedPreferences(runtime->preferences, std::move(command.preferences), static_cast<std::uint64_t>(_time64(nullptr)));
	if (OverlayPrefs::SavePlayerPreferences(command.preferences)) {
		runtime->EditPreferences() = command.preferences;
		Gamepads::SetLayouts(runtime->preferences.buttonLayouts);
		MenuMusic::Use(runtime->preferences.endlessLobbyMusic);
		GameThemes::Use(runtime->preferences.gameThemes);
		runtime->displayName = command.preferences.displayName;
		runtime->error.clear();
	} else runtime->error = loc::T("runtime.preferences_queue_failed");
	return DispatchOutcome::Dispatched;
}
static DispatchOutcome Handle(commands::LobbySettings& command, bool, Attempt) {
	if (!Current(command.generation) || !CanEditLobby() || !command.settings.Valid()) return DispatchOutcome::Dropped;
	if (runtime->match && (runtime->match->GetPhase() != session::IrohMatchSession::Phase::Idle || Game::Battle::System::ggpo)) {
		// P1 may edit the next match before pressing Ready. Use the same
		// explicit post-match drain/retirement boundary as rematch readiness.
		Defer(runtime->lobbyEditIntent, command);
		RetireFinishedMatch("iroh_lobby_settings");
		return DispatchOutcome::Deferred;
	}
	const auto decision = Decide(netplay::CommandKind::SetLobbySettings, command.generation);
	if (!decision.accepted) return Refused(decision);
	const auto& settings = command.settings;
	if (UserApp::netplay->client.Lobby_SetSettings(settings.editionSelect, settings.roundCount,
		{0, static_cast<short>(settings.roundTime)}) == session::SendResult::Queued) {
		runtime->pendingLobbySettings.reset(new netplay::LobbySettings(settings));
		runtime->lobbySettingsDeadline = GetTickCount64() + 15000;
	} else runtime->error = loc::T("runtime.lobby_settings_send_failed");
	return DispatchOutcome::Dispatched;
}
// A Ready or Rematch press is never silently dropped. Something the player
// must fix fails immediately and visibly; everything the room is still
// finishing (drain, fence, receipt, result) parks the press under one budget
// and resubmits it here once the gate reopens. Judged by the room and the
// seat as they are now (CurrentGates).
static DispatchOutcome Handle(commands::Ready& command, bool, Attempt attempt) {
	WithRandomFighter(command);
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
	const auto gates = CurrentGates();
	const auto roomState = runtime->controller.GetSnapshot().room;
	// A set that ended while the press waited can rotate this player into the
	// queue. The seat is gone, so the press quietly ends with it.
	if (attempt == Attempt::Retry && roomState == netplay::RoomState::Joined &&
		(gates.localSlot < 0 || gates.localSlot > 1)) {
		runtime->readyIntent.Clear();
		return DispatchOutcome::Dropped;
	}
	// Over the WIN of a game that may go on in place, the Ready that started
	// it still holds the seat: the Rematch waits there as the choice the
	// inputs carry, and takes the checks below after the battle if it closes.
	if (command.rematch && Game::Battle::System::ResultHeld() && internal::PostMatchPromptDue())
		return Defer(runtime->readyIntent, command);
	auto* client = UserApp::netplay ? &UserApp::netplay->client : nullptr;
	const bool inFlight = runtime->readyIntent.Parked() || runtime->controller.GetSnapshot().readyPending ||
		(client && (client->_outstandingReadyRequestNumber != -1 ||
			(gates.localSlot >= 0 && gates.localSlot < 2 && client->LocalSelectionLocked(gates.localSlot))));
	if (inFlight) return DispatchOutcome::Dropped;
	const char* refusal = nullptr;
	const int fighter = command.pick.fighter;
	const auto availability = fighter < 44 ? Dimps::Selection::ReadAvailability(fighter) : selection::Availability{};
	const auto installed = custom::Installed();
	if (!runtime->input.Ready()) refusal = loc::T("runtime.ready.assign_controller");
	else if (!selection::IsRandomStage(command.stage) && !selection::ValidStage(command.stage)) refusal = loc::T("runtime.ready.stage_unavailable");
	else if (fighter >= 44 || selection::FighterLocked(fighter, availability)) refusal = loc::T("runtime.ready.fighter_unavailable");
	else if (!selection::Available(command.pick, gates.editionSelect, availability, installed.get()))
		refusal = loc::T("runtime.ready.selection_unavailable");
	else if (roomState != netplay::RoomState::Joined || gates.localSlot < 0 || gates.localSlot > 1)
		refusal = loc::T("runtime.ready.take_seat");
	if (refusal) { FailReady(refusal); return DispatchOutcome::Dropped; }
	// A parked Rematch may drain after the session left PostMatch; the
	// controller accepts Ready in either state.
	if (command.rematch && runtime->controller.GetSnapshot().match != netplay::MatchState::PostMatch) command.rematch = false;
	runtime->readyIntent.Arm(GetTickCount64(), command.generation);
	const bool draining = runtime->match &&
		(runtime->match->GetPhase() != session::IrohMatchSession::Phase::Idle || Game::Battle::System::ggpo);
	if (draining || !gates.readyGate) {
		Defer(runtime->readyIntent, command);
		// Ready is the explicit handoff from post-match spectator draining.
		// Over the WIN screen the press waits for it to play out: both games
		// leave it on the same frame, and one leaving early would stall the
		// other's battle without its inputs.
		if (draining && !PostMatchPromptDue()) RetireFinishedMatch("iroh_rematch");
		return DispatchOutcome::Deferred;
	}
	const auto decision = Decide(command.rematch ? netplay::CommandKind::Rematch : netplay::CommandKind::Ready, command.generation);
	// The gate closed between publish and execute: park the press under its
	// existing budget rather than losing it.
	if (!decision.accepted) return runtime->readyIntent.Parked() ? DispatchOutcome::Dropped : Defer(runtime->readyIntent, command);
	auto& sender = UserApp::netplay->client;
	sender.SetSelectedDelay(ReadyDelay());
	sender.SetSelectedLowerLag(runtime->preferences.lowerInputLag);
	bool sent = sender.PreBattle_SetChara(NativePick(command.pick)) == session::SendResult::Queued;
	if (!sender._lobbyData.members.empty() && sender._lobbyData.members[0].connId == sender._cid) {
		sent = sender.PreBattle_SetEnv(sf4e::localRand()) == session::SendResult::Queued && sent;
		sent = sender.PreBattle_SetStage(selection::ResolveStage(command.stage, sf4e::localRand(), command.randomPool, custom::Installed()->stages)) == session::SendResult::Queued && sent;
	}
	if (!sent || sender.Lobby_Ready() != session::SendResult::Queued) {
		FailReady(loc::T("runtime.match_settings_send_failed"));
		Apply(netplay::EventKind::ControlLost, loc::T("runtime.match_settings_send_failed"));
	}
	return DispatchOutcome::Dispatched;
}
static DispatchOutcome Handle(commands::RoomAction& command, bool, Attempt attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
	if (!runtime->attached || !UserApp::netplay || !runtime->match) return DispatchOutcome::Dropped;
	// A lock-in the player asks for now is newer than a stream failure, so a
	// release still waiting for it must not undo it.
	runtime->spectatorLockRelease.Observe(command.roomAction);
	// The host's own rules edit: whatever the created rules had left is theirs.
	runtime->createdRules.ObservePlayer(command.roomAction);
	// One parked action of each kind, and the newest press wins: an older
	// one retried after this press would undo it (Queue, then Unqueue).
	if (attempt == Attempt::Fresh) RoomActionIntent(command).Clear();
	const auto action = command.roomAction.kind;
	// Unready takes back the player's Ready; one still parked here, which
	// the room never had, needs no message.
	if (action == room::ActionKind::Unready) {
		const bool parked = runtime->readyIntent.Parked();
		runtime->readyIntent.Clear();
		if (parked) return DispatchOutcome::Dispatched;
	}
	const bool changingTable = action == room::ActionKind::Queue || action == room::ActionKind::Unqueue ||
		action == room::ActionKind::Watch || action == room::ActionKind::Unwatch;
	if (changingTable && Game::Battle::System::ggpo) {
		const bool localSpectator = LocalIsSpectator();
		const bool immediateSpectatorAction = localSpectator &&
			(action == room::ActionKind::Unqueue || action == room::ActionKind::Unwatch);
		if (immediateSpectatorAction) {
			// Removing a queued spectator preserves the current spectator
			// stream. An Unwatch here leaves a table whose watched game the
			// room has already finished (the live game's leave is
			// commands::LeaveWatchedGame); it is sent before local GGPO retirement so
			// P1 receives game_peer_end while the generation is still current.
			if (UserApp::netplay->client.SendRoomAction(command.roomAction) != session::SendResult::Queued) {
				runtime->error = loc::T("runtime.spectator_action_retrying");
				return Defer(runtime->roomActionIntent, command);
			}
			// The player chose to leave: a passing note, not an error, and no
			// stream failure, whose lock release would race the Unwatch that
			// already ends the lock-in.
			if (action == room::ActionKind::Unwatch) AbortLocalMatch(GgpoEnd::Leave(loc::T("runtime.leaving_spectator")));
			return DispatchOutcome::Dispatched;
		}
		if (runtime->controller.GetSnapshot().match == netplay::MatchState::Playing && !localSpectator) return DispatchOutcome::Dropped;
		// The match teardown this starts ends by itself, so the wait has no budget.
		Defer(runtime->roomActionIntent, command, Intent::Budget::Untimed);
		if (DrainingSpectators()) RetireFinishedMatch("iroh_room_action");
		// The player chose this table change, so a spectator's view ends as a
		// departure, not a stream failure that would drop its lock-in.
		else AbortLocalMatch(GgpoEnd::Leave(loc::T("runtime.returning_room")));
		return DispatchOutcome::Deferred;
	}
	const auto decision = Decide(netplay::CommandKind::RoomAction, command.generation);
	if (!decision.accepted)
		return decision.refusal == netplay::Refusal::Fenced ? Defer(RoomActionIntent(command), command) : DispatchOutcome::Dropped;
	const auto sent = UserApp::netplay->client.SendRoomAction(command.roomAction);
	// A full queue or a control reconnect is transient: keep the intent
	// under its budget rather than making the player press again.
	if (sent == session::SendResult::NotConnected || sent == session::SendResult::QueueFull)
		return Defer(RoomActionIntent(command), command);
	if (sent != session::SendResult::Queued) runtime->error = loc::T("runtime.room_action_failed");
	return DispatchOutcome::Dispatched;
}
static DispatchOutcome Handle(commands::CheckConnection& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
	const auto decision = Decide(netplay::CommandKind::CheckConnection, command.generation, {}, command.benchmark);
	if (!decision.accepted) return Refused(decision);
	if (!CurrentGates().canProbe || runtime->room->Probe().status == "checking") return DispatchOutcome::Dispatched;
	std::uint64_t revision = 0; const auto peer = CurrentProbePeer(revision);
	if (peer.empty() || !runtime->room->RequestProbe(peer, runtime->nextProbeRequest++, revision, command.benchmark))
		runtime->error = loc::T("runtime.connection_check_unavailable");
	else if (runtime->error == loc::T("runtime.connection_check_unavailable"))
		runtime->error.clear();
	return DispatchOutcome::Dispatched;
}
static DispatchOutcome Handle(commands::ApplyDelay& command, bool, Attempt) {
	if (!Current(command.generation)) return DispatchOutcome::Dropped;
	const auto decision = Decide(netplay::CommandKind::ApplyDelay, command.generation);
	if (!decision.accepted) return Refused(decision);
	if (CurrentGates().delayLocked) return DispatchOutcome::Dispatched;
	int selected = command.selectedDelay;
	if (selected == -1) {
		std::uint64_t revision = 0; const auto peer = CurrentProbePeer(revision);
		const auto& probe = runtime->room->Probe();
		if (peer.empty() || probe.peer != peer || probe.pairRevision != revision) return DispatchOutcome::Dispatched;
		selected = probe.recommended;
	}
	const bool automatic = selected == AutoInputDelayChoice;
	if (!automatic) {
		if (selected < MinimumInputDelay || selected > MaximumInputDelay) return DispatchOutcome::Dispatched;
		runtime->selectedDelay = selected;
		runtime->EditPreferences().inputDelay = selected;
	}
	// A chosen number turns Auto off; choosing Auto keeps the number saved.
	runtime->EditPreferences().autoInputDelay = automatic;
	if (automatic) runtime->autoDelayCheck.Want(GetTickCount64());
	UserApp::netplay->client.SetSelectedDelay(ReadyDelay());
	if (!OverlayPrefs::SavePlayerPreferences(runtime->preferences)) runtime->error = loc::T("runtime.delay_save_failed");
	return DispatchOutcome::Dispatched;
}

// Validates and applies one command, by its type.
static DispatchOutcome Dispatch(RuntimeRequest command, bool helperReady, Attempt attempt) {
	return std::visit([&](auto& typed) { return Handle(typed, helperReady, attempt); }, command);
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
std::optional<RuntimeRequest> Classify(RuntimeCommand command) {
	const auto generation = command.command.generation;
	if (command.identity.op != netplay::IdentityOp::None) return RuntimeRequest(commands::Identity{std::move(command.identity)});
	if (command.tournament.op != netplay::tournament::Command::Op::None) return RuntimeRequest(commands::Tournament{std::move(command.tournament)});
	if (command.training != TrainingEntry::None) return RuntimeRequest(commands::Training{generation, command.training});
	if (command.leaveWatchedGame.Active()) return RuntimeRequest(commands::LeaveWatchedGame{generation, command.leaveWatchedGame});
	if (command.discordAction != discord::InviteAction::None)
		return RuntimeRequest(commands::DiscordInvite{generation, command.discordAction, command.discordRevision});
	if (command.previewSoundVolume >= 0) return RuntimeRequest(commands::PreviewSound{generation, command.previewSoundVolume});
	if (command.replay.mode != replay::Mode::None) return RuntimeRequest(commands::Replay{generation, std::move(command.replay)});
	if (command.shortInvitation) return RuntimeRequest(commands::ShortInvitation{generation});
	if (command.padAction.kind != gamepads::ControllerRequest::Kind::None) return RuntimeRequest(commands::Pad{generation, std::move(command.padAction)});
	if (command.roomSettings) return RuntimeRequest(commands::RoomSettings{generation, std::move(command.preferences)});
	if (command.inputAction != input::Action::None) return RuntimeRequest(commands::Input{generation, command.inputAction});
	if (command.service != platform::ServiceAction::None)
		return RuntimeRequest(commands::Service{generation, command.service, std::move(command.servicePath), std::move(command.updateOffer),
			command.updateChannel, command.steamProfile, std::move(command.reportSubmission)});
	room::Charter charter;
	charter.type = command.arcadeRoom; charter.practice = command.practiceRoom; charter.delayPolicy = command.delayPolicy;
	charter.tableCount = command.tableCount; charter.wiredOnly = command.wiredOnly;
	using Kind = netplay::CommandKind;
	switch (command.command.kind) {
	case Kind::HostRoom: return RuntimeRequest(commands::HostRoom{generation, std::move(command.preferences), charter, std::move(command.createdRules)});
	case Kind::JoinInvite:
		return RuntimeRequest(commands::JoinRoom{generation, std::move(command.command.invitation), std::move(command.publicTicket), command.join,
			command.discordRevision, std::move(command.createdRules), charter});
	case Kind::LeaveRoom: return RuntimeRequest(commands::LeaveRoom{generation, command.discordRevision});
	case Kind::ReplaceRoom: return RuntimeRequest(commands::ReplaceRoom{generation});
	case Kind::StartOffline: return RuntimeRequest(commands::StartOffline{generation});
	case Kind::SavePreferences: return RuntimeRequest(commands::SavePreferences{generation, std::move(command.preferences)});
	case Kind::SetLobbySettings: return RuntimeRequest(commands::LobbySettings{generation, command.preferences.lobby});
	case Kind::Ready:
	case Kind::Rematch: {
		commands::Ready ready;
		ready.generation = generation; ready.rematch = command.command.kind == Kind::Rematch;
		ready.pick = selection::FromNative(command.character); ready.stage = command.stage; ready.randomPool = std::move(command.randomPool);
		return RuntimeRequest(std::move(ready));
	}
	case Kind::RoomAction: return RuntimeRequest(commands::RoomAction{generation, std::move(command.roomAction)});
	case Kind::CheckConnection: return RuntimeRequest(commands::CheckConnection{generation, command.command.benchmark});
	case Kind::ApplyDelay: return RuntimeRequest(commands::ApplyDelay{generation, command.selectedDelay});
	// SubmitRuntimeCommand queues no update request.
	case Kind::RequestUpdate: break;
	}
	return std::nullopt;
}

// No room joined and none being joined: where the battle log may be opened
// and where Ember's menu may come back to the Replays screen.
static bool NoRoom() { return runtime->controller.GetSnapshot().room == netplay::RoomState::Idle && !UserApp::netplay; }

// A replay link's file is played only on the player's word, so any request
// for it, or a dismissal, answers the question the link left.
void RunReplayRequest(const replay::Request& request) {
    if (request.mode == replay::Mode::DismissLink || request.path == runtime->replayLinkAsked) runtime->replayLinkAsked.clear();
    if (request.mode == replay::Mode::Import) {
        // ponytail: read and written on the game thread; a pack is at most a couple of MB.
        const auto held = platform::replays::ImportReplayFile(platform::Utf8ToWide(request.path.c_str()));
        SaySet(held ? loc::Tf("replays.set_imported", held) : std::string(loc::T("replays.set_import_failed")), !held);
        return;
    }
    if (request.mode == replay::Mode::Keep) { platform::replays::MarkKept(platform::Utf8ToWide(request.path.c_str()), request.keep); platform::replays::WantListing(); return; }
    replaystore::Start(request, AtMainMenu(), NoRoom());
}

void TickReplays() {
    const std::string link = platform::ResolveReplayFile(runtime->replayLinks.Take());
    if (!link.empty()) {
        runtime->replayLinkAsked = link;
        spdlog::info("Replays: a link asked for a replay; the Replays screen asks the player");
    }
    replaystore::Tick(AtMainMenu(), NoRoom());
}

// A Ready press that cannot be honoured ends its intent and is announced once.
void FailReady(const char* reason) {
	runtime->readyIntent.Clear();
	runtime->error = reason; runtime->readyFailure = reason; ++runtime->readyFailureSequence;
	spdlog::warn("Ready failed: {}", reason);
}

DispatchOutcome DispatchTournamentRoomCommand(RuntimeRequest command) {
	const bool helperReady = runtime->helper && runtime->helper->State() == platform::HelperState::Connected;
	return Dispatch(std::move(command), helperReady, Attempt::Tournament);
}

bool AutoDelayMeasured() {
    std::uint64_t revision=0;
    return runtime->autoDelayCheck.Measured(CurrentProbePeer(revision));
}

int ReadyDelay() {
    if(!runtime->preferences.autoInputDelay) return runtime->selectedDelay;
    std::uint64_t revision=0;
    return runtime->autoDelayCheck.Delay(CurrentProbePeer(revision),static_cast<AutoDelayStyle>(runtime->preferences.autoDelayStyle));
}

// A Ready sent mid-check would start the match's own link beside the
// check's, so it holds until the check ends, and for a check still to come,
// but never past the check's hold (AutoDelayCheck::HoldMs).
bool AutoDelayMeasuring() {
    if(!runtime->preferences.autoInputDelay || !runtime->room) return false;
    return runtime->autoDelayCheck.Holding(GetTickCount64());
}

// Whether a check against `peer` can be reserved now. The helper refuses a
// reservation unless its own copy of the room seats this pair at exactly the
// table revision the check names, and it ends a check whose opponent already
// has a game link to this PC: a queued opponent who watched the last game
// keeps one until that game's spectators are released.
static bool AutoCheckClear(const std::string& peer, int tableIndex) {
    // No mapping reads as Closed, as the copying Game() does.
    const auto* game=runtime->room->FindGame(peer);
    if(game && game->state!=session::IrohRoom::GameState::Closed) return false;
    const auto* committed=UserApp::server->RoomSnapshot();
    if(!committed || tableIndex<0 || tableIndex>=static_cast<int>(room::TableCount)) return true;
    return room::ProbePairCommitted(UserApp::netplay->client.GetRoomSnapshot().tables[tableIndex],committed->tables[tableIndex]);
}

// Asks for Auto's check. It takes the controller's check of a press of Check
// connection, but a refusal stays in the log: the player did not press it.
static bool RequestAutoCheck(const std::string& peer, std::uint64_t revision) {
    const netplay::Command command{netplay::CommandKind::CheckConnection,runtime->controller.GetSnapshot().generation,{}};
    if(!runtime->controller.Execute(command).accepted) return false;
    const auto request=runtime->nextProbeRequest++;
    const bool sent=runtime->room->RequestProbe(peer,request,revision,false);
    spdlog::info("Auto delay: connection check {} request={} revision={}",sent?"requested":"refused",request,revision);
    return true;
}

// The game that just ended, taken once and logged once per game: old -> new
// delay Auto readies with. It stays local to this PC, like the check's: the
// room commits the higher of the two seats' Ready delays. The game names its
// own opponent, so it cannot be credited to whoever sits down after it.
static void TakeFinishedGame() {
    if(!runtime->finishedGame) return;
    const auto game=*runtime->finishedGame;
    runtime->finishedGame.reset();
    auto& check=runtime->autoDelayCheck;
    const int before=check.Delay(game.opponent);
    if(check.Played(runtime->preferences.autoInputDelay,game))
        spdlog::info("Auto delay: game {} ping median={} ms p95={} ms samples={}: delay {} -> {}",
            game.generation,game.ping.medianMs,game.ping.p95Ms,game.ping.samples,before,check.Delay(game.opponent));
    else
        spdlog::info("Auto delay: game {} ping median={} ms p95={} ms samples={}: delay kept",
            game.generation,game.ping.medianMs,game.ping.p95Ms,game.ping.samples);
}

// Follows the seated opponent and their check results. With Auto on, a due
// check is asked for once the table has settled and this PC holds the pair;
// one that ends without a recommendation is asked for once more.
void TickAutoDelay(bool helperReady) {
    std::uint64_t revision=0; int tableIndex=-1;
    // A room without coordination cannot reserve a check, so Auto readies at
    // once with two frames there.
    const bool coordinated=runtime->room && runtime->room->Coordination().active;
    const auto peer=coordinated ? CurrentProbePeer(revision,&tableIndex) : std::string();
    const auto now=GetTickCount64();
    auto& check=runtime->autoDelayCheck;
    TakeFinishedGame();
    check.Seat(peer,revision,runtime->nextProbeRequest,now);
    if(peer.empty()) return;
    const auto& probe=runtime->room->Probe();
    check.Observe(probe.peer,probe.request,probe.status,probe.recommended,now);
    if(!helperReady || !runtime->preferences.autoInputDelay || !check.Due(now) || probe.status=="checking" ||
        !CurrentGates().canProbe || !AutoCheckClear(peer,tableIndex)) return;
    const auto request=runtime->nextProbeRequest;
    if(RequestAutoCheck(peer,revision)) check.Asked(request);
}

// Sets the rules chosen on Create on the new room's tables, one at a time,
// whether or not Ember is on screen (netplay::CreatedRules).
void TickCreatedRules(bool helperReady) {
    auto& rules=runtime->createdRules;
    if(!rules.Active()) return;
    const auto now=GetTickCount64();
    if(!runtime->attached || !UserApp::netplay) {
        // Not in the room (yet): only the wait for it is timed.
        room::Action unused; rules.Next(room::Snapshot(),false,now,unused);
        if(!rules.Active()) spdlog::info("Room: created room rules ended reason={}",rules.Ended());
        return;
    }
    auto& client=UserApp::netplay->client;
    const auto state=runtime->controller.GetSnapshot();
    const bool canSend=helperReady && state.room==netplay::RoomState::Joined && state.control==netplay::Health::Healthy &&
        state.recovery==netplay::Recovery::None && (!state.coordinated || state.authorityWritable) && !client.GetRoomSnapshot().closed;
    room::Action action;
    if(rules.Next(client.GetRoomSnapshot(),canSend,now,action)) {
        std::uint64_t actionId=0;
        if(client.SendRoomAction(action,&actionId)==session::SendResult::Queued) {
            rules.Sent(action,now);
            spdlog::info("Room: created room rules sent table={} revision={} action={}",action.table,action.tableRevision,actionId);
        }
    }
    if(!rules.Active()) spdlog::info("Room: created room rules ended reason={}",rules.Ended());
}

void DrainCommands(bool helperReady) {
	RuntimeCommand command;
	for (int budget = 0; budget < 8 && runtime->commands->TryPop(command); ++budget)
		if (auto typed = Classify(std::move(command))) Dispatch(std::move(*typed), helperReady, Attempt::Fresh);
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
				if(event.matchGeneration==runtime->teardown.matchFinishedGeneration && event.table==runtime->matchFinishedTable) {
					runtime->teardown.matchFinishedPending=false;
					runtime->teardown.finishActionId=runtime->teardown.finishRetryAt=0;
					runtime->teardown.matchFinishedAction.reset();
				}
				// The room's terminal receipt is an authenticated, per-recipient
				// lifecycle record. Receiving this event only starts local
				// consumption; AcknowledgeTerminal is queued below after profile
				// persistence and native/helper retirement.
				if (event.terminalReplay && event.matchGeneration && runtime->room) {
					runtime->teardown.terminalAckPending = true;
					runtime->teardown.terminalOutcomeConsumed = true; // spectators and aborts need no profile write
					runtime->teardown.terminalAckTable = event.table;
					runtime->teardown.terminalAckGeneration = event.matchGeneration;
					const auto* capture = runtime->resultOutbox.Captured();
					const bool countedResult = event.result == room::MatchResult::P1Win ||
						event.result == room::MatchResult::P2Win;
					const bool profileMatch = countedResult && capture &&
						resultTerminal == netplay::MatchResultOutbox::TerminalResult::Confirmed;
					// Profile persistence is completed by the poll below, which
					// waits for the writer to report the revision on disk.
					if (profileMatch) runtime->teardown.terminalOutcomeConsumed = false;
				}
				// The game this PC took up, also one whose setup failed.
				if (runtime->match && event.matchGeneration && event.matchGeneration == runtime->match->AttemptedGeneration())
					runtime->teardown.committedEndGeneration = event.matchGeneration;
				spdlog::log(runtime->teardown.matchEndLog.First(event.table, event.matchGeneration) ? spdlog::level::info : spdlog::level::debug,
					"Room: match ended table={} generation={} result={} replay={}",
					event.table, event.matchGeneration, static_cast<int>(event.result), event.terminalReplay);
            }
			if (event.kind == room::Event::Kind::ReadyTimeout) {
				runtime->roomNotices.push_back({++runtime->roomNoticeSequence, event});
				if (runtime->roomNotices.size() > Runtime::RoomNoticesKept) runtime->roomNotices.pop_front();
			}
			if (event.kind == room::Event::Kind::ResultDisputed)
				spdlog::warn("Match result: disputed table={} generation={} reporter={} result={}",
					event.table, event.matchGeneration, event.member, static_cast<int>(event.result));
			if (!runtime->match || event.kind != room::Event::Kind::MatchEnded ||
				event.matchGeneration != runtime->match->Generation() ||
				(event.result != room::MatchResult::Abort && event.result != room::MatchResult::Cancel)) continue;
			runtime->readyIntent.Withdraw(); runtime->lobbyEditIntent.Clear();
			EndUnscoredMatch(event.result);
		}
	}
}

void PersistTerminalOutcome() {
	// Record the confirmed outcome in the profile and hold the terminal receipt
	// until the settings writer reports that revision on disk. Anything that
	// stops the write from landing releases the receipt instead, since it
	// gates the whole table's next match (MatchResultOutbox::PersistProfile).
	const auto* capturedResult = runtime->resultOutbox.Captured();
	if (runtime->teardown.terminalAckPending && !runtime->teardown.terminalOutcomeConsumed && runtime->room && capturedResult &&
		runtime->teardown.terminalAckGeneration == capturedResult->generation &&
		runtime->teardown.terminalAckTable == capturedResult->table && capturedResult->slot < 2 &&
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
		runtime->teardown.terminalOutcomeConsumed = true;
	}
}

void DrainActionReplies() {
	if (runtime->attached && UserApp::netplay) {
        SessionClient::ActionReply reply;
        while (UserApp::netplay->client.TakeActionReply(reply)) {
            if(reply.actionId==runtime->leave.leaveActionId && reply.accepted) runtime->leave.leaveAcknowledged=true;
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
			if (reply.actionId==runtime->teardown.finishActionId && (reply.accepted || reply.reason==room::RejectReason::WrongGeneration)) {
				runtime->teardown.matchFinishedPending=false;
				runtime->teardown.finishActionId=runtime->teardown.finishRetryAt=0;
				runtime->teardown.matchFinishedAction.reset();
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
			runtime->EditPreferences().lobby = current;
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
		const auto localSlot = CurrentGates().localSlot;
		if (runtime->controller.GetSnapshot().room == netplay::RoomState::Joined &&
			(localSlot < 0 || localSlot > 1)) runtime->readyIntent.Clear();
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
	// player stood up, or was removed) or the other seat empty. Left armed, it ran out its budget and
	// blamed the previous match. Only the committed room decides this: a leave
	// the room refuses keeps the press.
	if (runtime->attached && UserApp::netplay &&
		netplay::DropReadyWithoutSeat(runtime->readyIntent, UserApp::netplay->client.GetRoomSnapshot()))
		spdlog::info("Ready: dropped, its seat or the opponent's is empty");
	// A parked Ready or lobby edit that never gets its turn is reported, not
	// forgotten: the player pressed it and GGPO was already retired for it.
	if (runtime->readyIntent.Expired(now)) FailReady(loc::T("runtime.ready.previous_match_timeout"));
	if (runtime->lobbyEditIntent.Expired(now)) {
		runtime->lobbyEditIntent.Clear();
		runtime->error = loc::T("runtime.previous_match_close_timeout");
	}
	const auto& controlState = runtime->controller.GetSnapshot();
	const bool matchIdle = runtime->match && runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle;
	netplay::RetryGate gate;
	gate.controlHealthy = controlState.control == netplay::Health::Healthy;
	gate.authorityWritable = !controlState.coordinated || controlState.authorityWritable;
	gate.recoveringMatch = runtime->recoveringMatch;
	gate.spectatorInGame = runtime->attached && UserApp::netplay && LocalIsSpectator() && Game::Battle::System::ggpo;
	gate.atMenuIdle = AtMainMenu() && matchIdle;
	const bool healthyRoomControl = gate.controlHealthy && gate.authorityWritable;
	for (auto* intent : {&runtime->roomActionIntent, &runtime->chatIntent}) {
		const auto* parked = intent->Parked();
		if (!parked) continue;
		// Only a room action or a leave of the watched game is ever parked here.
		const auto* action = std::get_if<commands::RoomAction>(parked);
		if (std::holds_alternative<commands::LeaveWatchedGame>(*parked) ? netplay::ParkedWatchedLeaveRetryable(gate) :
			action && netplay::ParkedActionRetryable(action->roomAction, gate)) Retry(*intent, helperReady);
	}
	// A sent Ready keeps its budget; the pump clears it on commit or failure.
	if (runtime->readyIntent.Parked() && healthyRoomControl && CurrentGates().readyGate &&
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
