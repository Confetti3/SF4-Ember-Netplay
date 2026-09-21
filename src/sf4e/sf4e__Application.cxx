#include "sf4e__NetplayFacade.hxx"
#include "sf4e__InputDevices.hxx"
#include "../Dimps/Dimps__Selection.hxx"
#include "sf4e.hxx"
#include "sf4e__UserApp.hxx"
#include "sf4e__OverlayPrefs.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../platform/HelperClient.hxx"
#include "../session/IrohRoom.hxx"
#include "../session/IrohMatchSession.hxx"
#include "../session/RoomRecoveryRuntime.hxx"
#include "../netplay/BoundedMailbox.hxx"
#include "../netplay/MatchResultOutbox.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../netplay/ProfileRecordJson.hxx"
#include "../netplay/RoomPreferences.hxx"
#include "../common/StageCatalog.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/SessionTrace.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include "../common/Localization.hxx"
#include "../platform/LocaleWindows.hxx"
#include "../platform/UiPreferencesStore.hxx"
#include <algorithm>
#include <mutex>
#include <optional>
#include <ctime>
#include <spdlog/spdlog.h>
#include "../discord/Ticket.hxx"
#include "../training/TrainingRuntime.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
platform::HelperBootstrap pendingBootstrap, pendingDiscord;
uint32_t pendingError = 0;

// The lifecycle trace records only changes. Publish runs every application
// tick, so the traced values are compared here first and the JSON document is
// built only when one of them moved.
struct TraceFields {
    int room = -1, match = -1, control = -1, recovery = -1, router = -1, matchPhase = -1;
    std::string routerError, matchError, probe, probeRoute;
    bool nativeSocket = false, resultPending = false, finishPending = false;
    bool leavePending = false, terminalPending = false, probeBenchmark = false;
    bool authorityWritable = false, readyRequested = false, readyGate = false;
    unsigned probeFailure = 0, probeReplies = 0, probeMissed = 0;
    std::uint64_t probeP50Us = 0, probeP95Us = 0, probeP99Us = 0, probeJitterUs = 0;

    bool operator==(const TraceFields& other) const {
        return room == other.room && match == other.match && control == other.control &&
            recovery == other.recovery && router == other.router && matchPhase == other.matchPhase &&
            routerError == other.routerError && matchError == other.matchError &&
            probe == other.probe && probeRoute == other.probeRoute &&
            nativeSocket == other.nativeSocket && resultPending == other.resultPending &&
            finishPending == other.finishPending && leavePending == other.leavePending &&
            terminalPending == other.terminalPending && probeBenchmark == other.probeBenchmark &&
            authorityWritable == other.authorityWritable && readyRequested == other.readyRequested && readyGate == other.readyGate &&
            probeFailure == other.probeFailure && probeReplies == other.probeReplies &&
            probeMissed == other.probeMissed && probeP50Us == other.probeP50Us &&
            probeP95Us == other.probeP95Us && probeP99Us == other.probeP99Us &&
            probeJitterUs == other.probeJitterUs;
    }
};

struct Runtime {
    SessionTrace trace;
    std::optional<TraceFields> lastTraceFields;
    std::unique_ptr<platform::HelperClient> discordClient;
    discord::PendingInvite discordInvite;
    std::string discordStatus = loc::T("discord.unavailable_build");
    std::string discordPublished;
    ULONGLONG discordLastPublish = 0;
    std::array<std::uint64_t,8> discordPresenceKey{};
    bool discordPresenceKeyValid=false;

    input::Assignment input;
    bool inputInitialized = false;
    input::Device matchInput;
    int matchInputSide=-1;
    bool matchInputFault=false;
    std::vector<input::Device> inputDevices;
    platform::ApplicationServices services;
    bool updateClosing = false;
	std::unique_ptr<platform::HelperClient> helper;
	std::shared_ptr<session::IrohRoom> room;
	std::unique_ptr<session::IrohMatchSession> match;
	bool matchEntered = false;
	bool matchEnded = false;
	bool matchFinishedPending = false;
	bool recoveringMatch = false;
	netplay::MatchResultOutbox resultOutbox;
    std::uint64_t finishActionId=0, finishRetryAt=0;
	std::optional<room::Action> matchFinishedAction;
    bool replacementPending=false;
    bool leaveRequested=false, leaveAcknowledged=false;
    std::uint64_t leaveActionId=0, leaveRetryAt=0;
    int selectedDelay=2;
    std::uint64_t nextProbeRequest=1;
    session::RoomRecoveryRuntime recovery;
    std::uint64_t observedAuthorityTerm=0;
	// A terminal receipt is released only after its local outcome has been
	// persisted and native/helper teardown has reached Idle. MatchEnded itself
	// is deliberately insufficient: spectators have no profile write, while a
	// fighter may still own GGPO's socket when the event is delivered.
	bool terminalAckPending = false;
	bool terminalOutcomeConsumed = false;
	// Settings revision carrying the recorded outcome. The ACK waits until the
	// writer reports this revision on disk, not merely accepted into its queue.
	std::uint64_t terminalPersistRevision = 0;
	std::uint8_t terminalAckTable = 0;
	std::uint64_t terminalAckGeneration = 0;
	std::uint64_t matchFinishedGeneration = 0;
	std::uint8_t matchFinishedTable = 0;
	std::unique_ptr<room::Action> pendingAbort;
	ULONGLONG pendingAbortDeadline = 0;
	std::unique_ptr<RuntimeCommand> pendingRoomAction;
	// Set only when a room action is parked behind the authority catch-up fence.
	// Zero means the existing GGPO teardown deferrals, which must not expire.
	std::uint64_t pendingRoomActionDeadline = 0;
	std::unique_ptr<RuntimeCommand> pendingReady;
	std::unique_ptr<RuntimeCommand> pendingLobbyEdit;
	// A Ready/Rematch press is one intent with one budget: it may be parked
	// behind the previous match's drain, the authority fence or the terminal
	// receipt, then sent, then awaits the committed seat flag. Non-zero from
	// the accepted press until commit or failure, so the interface shows one
	// steady state and a stall is reported instead of the press vanishing.
	ULONGLONG pendingReadyDeadline = 0;
	ULONGLONG pendingLobbyEditDeadline = 0;
	netplay::Generation pendingReadyGeneration;
	std::string readyFailure;
	std::uint64_t readyFailureSequence = 0;
	// Generation for which "a participant left" was already announced.
	std::uint64_t participantLeftGeneration = 0;
	// `error` is cleared by only a handful of successful actions, so every
	// transient message otherwise stayed pinned for the session and hid save
	// feedback. Transient errors expire; the helper-unavailable startup
	// errors stay because the condition they describe persists.
	std::string errorShown;
	ULONGLONG errorShownAtMs = 0;
	std::unique_ptr<netplay::LobbySettings> pendingLobbySettings;
	ULONGLONG lobbySettingsDeadline = 0;
	netplay::PlayerPreferences preferences;
	netplay::SessionController controller;
	netplay::BoundedMailbox<RuntimeCommand> commands{32, 128 * 1024};
	std::mutex snapshotMutex;
	// Replaced whole on each publish, never mutated, so readers share it.
	std::shared_ptr<const RuntimeSnapshot> snapshot = std::make_shared<const RuntimeSnapshot>();
	std::string displayName;
	std::string languagePreference = "auto";
	std::string error;
	bool ready = false;
	bool eventSystemReady = false;
	bool attached = false;
	bool helperLossReported = false;
	bool offlineRequested = false;
};

// Deliberately no static owning destructor: Windows calls DLL destructors
// under the loader lock. Normal Main::Destroy explicitly stops and deletes
// this object. On abnormal process exit Windows reclaims the pipe/threads,
// and the launcher's owned job reaps the helper.
Runtime* runtime = nullptr;

// Slots 0 and 1 are the fighters; every other seat watches. False without a
// match, so a caller that means "the local fighter" still needs its own null
// check rather than the negation of this.
bool LocalIsSpectator() {
	return runtime && runtime->match && runtime->match->LocalSlot() >= 2;
}

bool AtMainMenu() {
	// Platform initialization precedes the native event-system singleton. The
	// getter dereferences that singleton before it can return a root pointer.
	if (!runtime || !runtime->ready || !runtime->eventSystemReady) return false;
	auto* root = Dimps::App::GetRootEvent();
	if (!root) return false;
	char* query[] = { "MainMenu" };
	return Dimps::Event::EventBaseWithEC::FindForegroundEvent(root, query, 1) != nullptr;
}

void CloseRoom() {
	spdlog::info("Room: closing error='{}' match_error='{}' match_phase={} spectator={} ggpo={}", runtime->error,
		runtime->match ? runtime->match->Error() : std::string(), runtime->match ? static_cast<int>(runtime->match->GetPhase()) : -1,
		LocalIsSpectator(), Game::Battle::System::ggpo != nullptr);
    const bool abandon=runtime->replacementPending || (runtime->room && runtime->room->Coordination().active &&
        !runtime->room->Coordination().writable);
    runtime->recovery=session::RoomRecoveryRuntime{};
    runtime->observedAuthorityTerm=0;
    runtime->leaveRequested=runtime->leaveAcknowledged=false;
    runtime->leaveActionId=runtime->leaveRetryAt=0;
    runtime->matchInput={};runtime->matchInputSide=-1;runtime->matchInputFault=false;
	runtime->match.reset();
	runtime->pendingReady.reset();
	runtime->pendingReadyDeadline = 0;
	runtime->pendingLobbyEdit.reset();
	runtime->pendingLobbySettings.reset();
	runtime->pendingRoomAction.reset();
	runtime->pendingRoomActionDeadline = 0;
	runtime->resultOutbox.Reset();
	runtime->recoveringMatch = false;
	runtime->matchFinishedPending = false;
	runtime->matchFinishedGeneration = 0;
    runtime->finishActionId=runtime->finishRetryAt=0;
	runtime->matchFinishedAction.reset();
	runtime->pendingAbort.reset();
	runtime->pendingAbortDeadline = 0;
	runtime->matchEntered = runtime->matchEnded = false;
	runtime->terminalAckPending = false;
	runtime->terminalOutcomeConsumed = false;
	runtime->terminalPersistRevision = 0;
	runtime->terminalAckTable = 0;
	runtime->terminalAckGeneration = 0;
	if (runtime->attached) {
		ShutdownNetplay(true);
		runtime->attached = false;
	}
	if (runtime->room) runtime->room->Leave(abandon);
}

void Apply(netplay::EventKind kind, const std::string& error = {}) {
	netplay::Event event{kind, runtime->controller.GetSnapshot().generation, error};
	const auto decision = runtime->controller.Apply(event);
	const bool closesRoom = decision.effect == netplay::Effect::CloseSession || decision.effect == netplay::Effect::AbortMatch ||
		decision.effect == netplay::Effect::FinishDegradedMatch;
	if (closesRoom) {
		spdlog::warn("Room: event {} closes the room (effect {}): {}", static_cast<int>(kind), static_cast<int>(decision.effect), error);
		CloseRoom();
	}
	else if (decision.accepted && kind == netplay::EventKind::ControlLost &&
		runtime->controller.GetSnapshot().room == netplay::RoomState::Lost) {
		HandleControlPlaneLoss(error.c_str());
		if (!UserApp::netplay) {
			runtime->controller.Execute({netplay::CommandKind::LeaveRoom, runtime->controller.GetSnapshot().generation, {}});
			CloseRoom();
		}
	}
}

bool Joined() {
	if (!runtime->attached || !UserApp::netplay) return false;
	const auto& client = UserApp::netplay->client;
	if (client.GetRoomSnapshot().roomEpoch) {
		const auto& room = client.GetRoomSnapshot();
		return !room.closed && room.localMember && std::any_of(room.members.begin(), room.members.end(),
			[&](const room::Member& member) { return member.id == room.localMember; });
	}
	return std::any_of(client._lobbyData.members.begin(), client._lobbyData.members.end(),
		[&](const SessionProtocol::MemberData& member) { return member.connId == client._cid; });
}

void AttachRoom() {
	const auto& config = GetConfig();
    if (!runtime->input.Ready()) return;
    const auto& device = runtime->input.Selected();
    const auto deviceIndex = static_cast<uint8_t>(device.index), deviceType = static_cast<uint8_t>(device.type);
	if (runtime->controller.GetSnapshot().isHost || runtime->room->Coordination().active) {
		const auto id = runtime->room->RoomId();
		std::string identity = "iroh:";
		const char* digits = "0123456789abcdef";
		for (auto byte : id) { identity += digits[byte >> 4]; identity += digits[byte & 15]; }
		UserApp::server.reset(new SessionServer(identity, sf4e::sidecarHash,
			runtime->preferences.lobby.editionSelect, runtime->preferences.lobby.roundCount,
			{0, static_cast<short>(runtime->preferences.lobby.roundTime)}, runtime->room->Server()));
		const auto room = runtime->room;
		UserApp::server->EnableMatchAuthorization(id, [room](session::Connection connection) {
			return connection == 1 ? room->LocalIdentity() : room->PeerIdentity(connection);
		}, [room](session::Connection connection) { return room->PeerIncarnation(connection); });
		auto rules = runtime->preferences.tableRules;
		UserApp::server->EnableCustomRooms(runtime->preferences.roomName,
			static_cast<std::uint8_t>(runtime->preferences.roomCapacity), runtime->room->Epoch(), rules);
        const auto& authority=runtime->room->Coordination();
        if(authority.active) UserApp::server->SetAuthority(authority.term,authority.revision,
            authority.writable && authority.leaderLocal && authority.rebound);
	}
    input::ReleaseFromSide(0); // Preserve native menu -> match ownership handoff.
	UserApp::StartIrohSession(runtime->room->Client(), config.ggpoPort, runtime->displayName,
		deviceType, deviceIndex, static_cast<uint8_t>(runtime->preferences.inputDelay));
	UserApp::netplay->client.RequireCustomRooms();
	UserApp::netplay->client.SetProfileMain(runtime->preferences.mainFighter);
    runtime->selectedDelay=runtime->preferences.inputDelay;
    UserApp::netplay->client.SetSelectedDelay(runtime->selectedDelay);
    runtime->inputInitialized=true;
	runtime->match.reset(new session::IrohMatchSession(UserApp::netplay->client, runtime->room));
	runtime->attached = true;
}

bool CanEditLobby() {
	if (!runtime->attached || !UserApp::netplay || !AtMainMenu() || runtime->pendingLobbySettings || runtime->pendingReady ||
		runtime->pendingLobbyEdit || !runtime->match) return false;
	const auto phase = runtime->match->GetPhase();
	if (phase != session::IrohMatchSession::Phase::Idle && !(phase == session::IrohMatchSession::Phase::Ending &&
		runtime->controller.GetSnapshot().match == netplay::MatchState::PostMatch)) return false;
	const auto& client = UserApp::netplay->client;
	return runtime->controller.GetSnapshot().room == netplay::RoomState::Joined && !client._lobbyData.members.empty() &&
		client._lobbyData.members[0].connId == client._cid && client._outstandingReadyRequestNumber == -1 &&
		client._matchData.readyMessageNum[0] == -1 && client._matchData.readyMessageNum[1] == -1;
}

bool CanBeginReplacement() {
	if (runtime->controller.GetSnapshot().recovery != netplay::Recovery::ReplacementOffered) return false;
	const bool ggpoOwnsSocket = Game::Battle::System::ggpo != nullptr;
	return runtime->match ? runtime->match->CanBeginReplacement(ggpoOwnsSocket) : !ggpoOwnsSocket;
}

void FillNetworkDiagnostics(platform::DiagnosticsView& view) {
    if(!runtime || !runtime->room) return;
    const auto& probe=runtime->room->Probe();
    view.probeState=probe.status.empty()?0:probe.status=="checking"?1:
        probe.status=="ready"||probe.status=="complete"?2:probe.status=="invalidated"?3:probe.status=="timed_out"?5:probe.status=="local_overload"?6:4;
    view.probeRoute=probe.route.rfind("ip:",0)==0?1:probe.route.rfind("relay:",0)==0?2:0;
    view.probeFailure=probe.failureReason;
    view.sent=probe.sent;view.expected=probe.expected;
    view.benchmark=probe.benchmark;view.replies=probe.samples;view.missed=probe.lost;
    view.p50Us=probe.p50RttUs;view.p95Us=probe.p95RttUs;view.p99Us=probe.p99RttUs;view.jitterUs=probe.jitterUs;
    if(runtime->match) for(const auto& entry:runtime->room->Games()) {
        const auto& game=entry.second;
        if(game.generation!=runtime->match->Generation()) continue;
        if(game.route.rfind("ip:",0)==0) ++view.directLinks;
        if(game.route.rfind("relay:",0)==0) ++view.relayedLinks;
        view.routeChanges+=game.routeChanges;view.localDrops+=game.localDrops;view.sendPressure+=game.congestionEvents;
    }
}

struct PostPublishState {
    netplay::Snapshot session;
    bool discordCanSwitch=false, canOpenRoom=false;
};
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
    runtime->services.Observe(diagnostic);
    snapshot.services = runtime->services.Snapshot();
    snapshot.inputDevice = runtime->input.Selected();
    snapshot.controller = snapshot.inputDevice.name;
    snapshot.inputCapture = runtime->input.State();
    snapshot.controllerReady = runtime->input.Ready();
    if (snapshot.controller.empty()) snapshot.controller = "Choose a gameplay device";
    else if (!snapshot.inputDevice.connected) snapshot.controller += " (disconnected)";

	snapshot.atMainMenu = AtMainMenu();
    snapshot.menuContext = snapshot.atMainMenu ? input::MenuContext::MainMenu :
        training::ControlsAvailable() ? input::MenuContext::OfflineTraining : input::MenuContext::Unavailable;
    if(snapshot.atMainMenu) for(int fighter=0;fighter<selection::FighterCount;++fighter)
        snapshot.fighterAvailability[fighter]=Dimps::Selection::ReadAvailability(fighter);
    // The explicit gameplay-device assignment stays authoritative, including
    // keyboard selection, capture, disconnects and local P1/P2 handoff.
    if (snapshot.menuContext != input::MenuContext::Unavailable && snapshot.inputCapture == input::Capture::Idle &&
        snapshot.session.match != netplay::MatchState::Preparing && snapshot.session.match != netplay::MatchState::Playing) {
        auto& sample = snapshot.menuController;
        sample.deviceType = snapshot.inputDevice.type;
        sample.deviceIndex = snapshot.inputDevice.index;
        unsigned held = 0, physical = 0;
        sample.connected = snapshot.inputDevice.connected &&
            Dimps::Pad::ReadController(sample.deviceType, sample.deviceIndex, held,&physical,&sample.selectPhysical,&sample.backPhysical);
        if(sample.deviceType==3){sample.selectPhysical=0x40000;sample.backPhysical=0x20000;}
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
	snapshot.settingsPending = OverlayPrefs::PersistencePending() || runtime->pendingLobbySettings != nullptr || runtime->pendingLobbyEdit != nullptr;
	snapshot.settingsError = OverlayPrefs::PersistenceError();
	snapshot.helperError = runtime->error;
    snapshot.gameplayInputError=Game::Battle::System::ggpo&&runtime->matchInputFault&&runtime->match&&!LocalIsSpectator()?
        "Match input blocked: reconnect "+runtime->matchInput.name+". If its slot changed, return to the room to reassign it.":"";
	snapshot.offlineRequested = runtime->offlineRequested;
	if (runtime->room) snapshot.invitation = runtime->room->Invitation();
	if (runtime->attached && UserApp::netplay) {
		snapshot.room = UserApp::netplay->client.GetRoomSnapshot();
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
			if (!client.RoomError().empty()) snapshot.helperError = loc::T(client.RoomError().c_str());
		}
		const bool matchCanReady = runtime->match && (runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle ||
			(runtime->match->GetPhase() == session::IrohMatchSession::Phase::Ending && snapshot.session.match == netplay::MatchState::PostMatch));
		snapshot.canEditSelection = snapshot.atMainMenu && snapshot.session.room == netplay::RoomState::Joined && snapshot.localSlot >= 0 && snapshot.localSlot < 2 &&
			(snapshot.session.match == netplay::MatchState::None || snapshot.session.match == netplay::MatchState::PostMatch) &&
			!snapshot.session.readyPending && !runtime->pendingReady && client._outstandingReadyRequestNumber == -1 &&
			!client.LocalSelectionLocked(snapshot.localSlot);
		// readyGate is the environment; canReady additionally requires no Ready
		// in flight. A parked press drains against the gate alone.
		snapshot.readyGate = snapshot.atMainMenu && matchCanReady && !runtime->pendingLobbySettings && !runtime->pendingLobbyEdit &&
			snapshot.session.room == netplay::RoomState::Joined && snapshot.localSlot >= 0 && snapshot.localSlot < 2 &&
			client._lobbyData.members.size() >= 2;
		const bool readyInFlight = runtime->pendingReady || client._outstandingReadyRequestNumber != -1 ||
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
                snapshot.selectedDelay=localMember->delayLocked ? localMember->frozenDelay : runtime->selectedDelay;
                snapshot.delayLocked=localMember->delayLocked || !tableWaiting || !healthyControl || snapshot.session.readyPending;
                snapshot.canProbe=!snapshot.delayLocked && table->p1 && table->p2;
                if (runtime->room && UserApp::server) {
                    const auto opponent=localMember->seat ? table->p1 : table->p2;
                    const auto peer=UserApp::server->roomPeerIdentities.find(opponent);
                    const auto& probe=runtime->room->Probe();
                    if (peer!=UserApp::server->roomPeerIdentities.end() && probe.peer==peer->second &&
                        probe.pairRevision==table->revision) {
                        snapshot.probeRoute=probe.route.rfind("ip:",0)==0?"Direct":probe.route.rfind("relay:",0)==0?"Relayed":"Unknown";
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
				!snapshot.room.localTerminalPending && !tableTerminalPending;
			snapshot.canReady = snapshot.canReady && snapshot.readyGate;
			// The intent completes when the committed seat flag arrives, or when
			// the table has already moved on to preparing the match.
			if (runtime->pendingReadyDeadline && !runtime->pendingReady && !snapshot.session.readyPending && table &&
				(table->ready[localMember->seat] || table->phase != room::TablePhase::Waiting)) runtime->pendingReadyDeadline = 0;
			snapshot.canEditSelection = snapshot.canEditSelection && healthyControl && (!seated || (tableWaiting && !table->ready[localMember->seat])) &&
				!runtime->recoveringMatch && !snapshot.room.localTerminalPending;
		}
		snapshot.readyGate = snapshot.readyGate && !runtime->pendingAbort;
		snapshot.canReady = snapshot.canReady && !runtime->pendingAbort;
		snapshot.canEditSelection = snapshot.canEditSelection && !runtime->pendingAbort;
		// Legacy lobbies have no committed seat flag; the acknowledged request is the commit.
		if (!snapshot.room.roomEpoch && runtime->pendingReadyDeadline && !runtime->pendingReady && !snapshot.session.readyPending &&
			client._outstandingReadyRequestNumber == -1) runtime->pendingReadyDeadline = 0;
	}
    snapshot.canChangeController = snapshot.canEditSelection && !runtime->pendingReady &&
        !snapshot.session.readyPending && !runtime->pendingLobbyEdit && !runtime->pendingAbort;
    snapshot.readyGate = snapshot.readyGate && snapshot.controllerReady;
    snapshot.canReady = snapshot.canReady && snapshot.readyGate;
    snapshot.readyRequested = runtime->pendingReadyDeadline != 0;
    snapshot.readyFailure = runtime->readyFailure; snapshot.readyFailureSequence = runtime->readyFailureSequence;
    // Report the actual gate; a pending transition is not the same as Ready.
    if (!snapshot.canEditSelection) {
        snapshot.selectionLockReason = !snapshot.atMainMenu ? loc::T("runtime.lock.return_to_menu_fighter") :
            runtime->pendingAbort ? loc::T("runtime.lock.previous_game_closing") :
			snapshot.room.localTerminalPending ? loc::T("runtime.lock.teardown") :
			runtime->recoveringMatch || snapshot.session.control != netplay::Health::Healthy ? loc::T("runtime.lock.room_recovering") :
            snapshot.session.readyPending || runtime->pendingReady ? loc::T("runtime.lock.ready_pending") :
            loc::T("runtime.lock.selection_update");
    }
	if (!snapshot.canReady) {
		snapshot.readyLockReason = !snapshot.controllerReady ? loc::T("room.controller_required") :
			!snapshot.atMainMenu ? loc::T("runtime.lock.return_to_menu_ready") :
			runtime->pendingLobbySettings || runtime->pendingLobbyEdit ? loc::T("runtime.lock.table_settings") :
			runtime->pendingAbort ? loc::T("runtime.lock.previous_game_closing") :
			snapshot.room.localTerminalPending || (snapshot.room.roomEpoch && snapshot.localSlot >= 0 && snapshot.localSlot < 2 &&
				snapshot.room.members.size() > 0 && std::any_of(snapshot.room.members.begin(), snapshot.room.members.end(), [&](const room::Member& member) {
					return member.id == snapshot.room.localMember && member.table >= 0 && member.table < room::TableCount &&
						snapshot.room.terminalPending[member.table];
				})) ? loc::T("runtime.lock.teardown") :
			(UserApp::netplay && UserApp::netplay->client.RoomError() == "terminal_result_backlog") ? loc::T("runtime.lock.teardown") :
			loc::T("runtime.lock.room_update");
	}
    snapshot.discordPending = runtime->discordInvite.Active();
    snapshot.discordConfirm = runtime->discordInvite.NeedsConfirmation();
    snapshot.discordRevision = runtime->discordInvite.Revision();
    snapshot.discordCanSwitch = snapshot.atMainMenu && !Game::Battle::System::ggpo &&
        (snapshot.session.match == netplay::MatchState::None || snapshot.session.match == netplay::MatchState::PostMatch);
    snapshot.discordStatus = runtime->discordStatus;
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
        if (runtime->room) {
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
        if (runtime->match) fields.matchError = runtime->match->Error();
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
                {"probe_route", fields.probeRoute}, {"probe_benchmark", fields.probeBenchmark},
                {"probe_replies", fields.probeReplies}, {"probe_missed", fields.probeMissed},
                {"probe_p50_us", fields.probeP50Us}, {"probe_p95_us", fields.probeP95Us},
                {"probe_p99_us", fields.probeP99Us}, {"probe_jitter_us", fields.probeJitterUs}
            });
            if (recorded) runtime->lastTraceFields = std::move(fields);
        }
    }
    PostPublishState result{snapshot.session,snapshot.discordCanSwitch,snapshot.canOpenRoom};
    auto published = std::make_shared<const RuntimeSnapshot>(std::move(snapshot));
    std::shared_ptr<const RuntimeSnapshot> previous;
    {
        std::lock_guard<std::mutex> lock(runtime->snapshotMutex);
        previous = std::move(runtime->snapshot);
        runtime->snapshot = std::move(published);
    }
    // `previous` is released here, outside the lock, unless a reader still holds it.
    return result;
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
    mix(state.generation.room); mix(state.generation.match); mixString(state.error);
    if (runtime->attached && UserApp::netplay) {
        const auto& room = UserApp::netplay->client.GetRoomSnapshot();
        mix(room.roomEpoch); mix(room.revision); mix(room.localMember);
        mixString(UserApp::netplay->client.RoomError());
        mix(UserApp::netplay->client._outstandingReadyRequestNumber);
    }
    mixString(runtime->error); mix(runtime->matchInputFault); mix(runtime->matchInput.connected);
    mix(runtime->helper ? static_cast<std::uint64_t>(runtime->helper->State()) : 0);
    mix(runtime->match ? static_cast<std::uint64_t>(runtime->match->GetPhase()) : 0);
    mix(runtime->pendingReady != nullptr); mix(runtime->pendingLobbyEdit != nullptr); mix(runtime->pendingAbort != nullptr);
    mix(runtime->pendingReadyDeadline != 0); mix(runtime->readyFailureSequence);
    mix(runtime->recoveringMatch); mix(OverlayPrefs::PersistencePending()); mixString(OverlayPrefs::PersistenceError());
    mix(runtime->services.Snapshot().pending); mixString(runtime->discordStatus); mix(runtime->discordInvite.Revision());
    mix(runtime->preferences.showMatchHud); mix(runtime->preferences.matchHudSize); mix(runtime->preferences.matchHudRaised);
    mix(static_cast<std::uint64_t>(runtime->input.State())); mix(runtime->input.Ready());
    mix(AtMainMenu());
    return h;
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
} // namespace

void ConfigureHelper(const platform::HelperBootstrap& bootstrap, uint32_t startupError) {
	pendingBootstrap = bootstrap;
	pendingError = startupError;
}

void ConfigureDiscord(const platform::HelperBootstrap& bootstrap) { pendingDiscord=bootstrap; }

void StartHelper() {
	if (runtime) return;
	runtime = new Runtime();
    runtime->languagePreference = platform::LoadLanguagePreference();
    loc::SetActive(loc::ResolveLocale(runtime->languagePreference, platform::WindowsUiLanguages()));
    runtime->trace.Open(netplay::SettingsStore::DefaultDirectory());
	runtime->displayName = GetConfig().displayName;
	if (runtime->displayName.empty()) runtime->displayName = "Player";
	runtime->preferences.displayName = runtime->displayName;
    nlohmann::json saved; std::string settingsError;
    netplay::SettingsStore store(netplay::SettingsStore::DefaultDirectory());
    if (store.LoadLauncher(saved, settingsError)) {
        if(saved.contains("onlineRecord")&&!netplay::ReadProfileRecord(saved["onlineRecord"],runtime->preferences.record))
            runtime->error=loc::T("runtime.record_invalid");
        try {
            runtime->preferences.showMatchHud = saved.value("showMatchHud", true);
            const int hudSize = saved.value("matchHudSize", 1);
            runtime->preferences.matchHudSize = hudSize >= 0 && hudSize <= 2 ? hudSize : 1;
            runtime->preferences.matchHudRaised = saved.value("matchHudRaised", false);
            runtime->preferences.discordPresence = saved.value("discordPresence", true);
            runtime->preferences.discordInvites = saved.value("discordInvites", true);
            const int main=saved.contains("mainFighter")&&saved["mainFighter"].is_number_integer()?saved["mainFighter"].get<int>():0;
            runtime->preferences.mainFighter=main>=0&&main<sf4e::selection::FighterCount?main:0;
            const float scale = saved.value("interfaceScale", 1.f);
            runtime->preferences.interfaceScale = scale >= 1.f && scale <= 1.5f ? scale : 1.f;
        } catch (...) { runtime->error = loc::T("runtime.interface_preferences_failed"); }
    }
    runtime->offlineRequested = EnvFlag("SF4E_START_OFFLINE");
	runtime->preferences.inputDelay = GetConfig().inputDelay;
	runtime->preferences.lobby.editionSelect = GetConfig().editionSelect != 0;
	runtime->preferences.lobby.roundCount = GetConfig().roundCount;
	runtime->preferences.lobby.roundTime = GetConfig().roundTimeIntegral;
	if (!runtime->preferences.lobby.Valid()) runtime->preferences.lobby = {};
	if (runtime->preferences.inputDelay < 0 || runtime->preferences.inputDelay > 10) runtime->preferences.inputDelay = 2;
	{
		nlohmann::json saved;
		std::string error;
		netplay::SettingsStore settings(netplay::SettingsStore::DefaultDirectory());
		if (!settings.LoadLauncher(saved, error)) runtime->error = error;
		else if (!netplay::ReadRoomPreferences(saved, runtime->preferences))
			runtime->error = loc::T("runtime.room_defaults_invalid");
	}
	if (pendingBootstrap.helperPid) {
		runtime->helper.reset(new platform::HelperClient());
		if (runtime->helper->Start(pendingBootstrap)) {
			runtime->room = std::make_shared<session::IrohRoom>(*runtime->helper);
		} else runtime->error = loc::T("runtime.network_start_failed");
	} else {
		runtime->error = loc::Tf("runtime.network_helper_unavailable",pendingError);
	}
    if (pendingDiscord.helperPid) {
        runtime->discordClient.reset(new platform::HelperClient());
        runtime->discordStatus = loc::T(runtime->discordClient->Start(pendingDiscord)?"discord.connecting":"discord.unavailable_gameplay_ok");
    }
    SecureZeroMemory(&pendingDiscord, sizeof(pendingDiscord));
	SecureZeroMemory(pendingBootstrap.nonce, sizeof(pendingBootstrap.nonce));
	Publish();
}

void NotifyRuntimeGameReady() { if (runtime) runtime->ready = true; }
void NotifyRuntimeEventSystemReady() { if (runtime) runtime->eventSystemReady = true; }

void StopHelper() {
    training::StopCapture();
	if (!runtime) return;
    if (runtime->discordClient) {
        runtime->discordClient->Send("{\"type\":\"shutdown\"}");
        runtime->discordClient->Stop();
    }
	runtime->commands.Close();
	CloseRoom();
	if (runtime->room && runtime->helper) {
		// Closing the game must mean the same thing to the room as Leave room.
		// Shutdown cancels an in-flight graceful leave in the helper, so give the
		// departure a bounded window to be sent and acknowledged first; otherwise
		// the roster keeps this member until some later teardown prunes them.
		// Process exit may wait briefly; rendering and simulation never do.
		const auto departure = GetTickCount64() + 1200;
		while (runtime->room->GetState() != session::IrohRoom::State::Idle &&
			runtime->helper->State() == platform::HelperState::Connected &&
			GetTickCount64() < departure) {
			runtime->room->Poll();
			Sleep(1);
		}
	}
	if (runtime->helper) {
		runtime->helper->Send("{\"type\":\"shutdown\"}");
		// Game teardown may wait briefly; rendering/simulation never does.
		const auto deadline = GetTickCount64() + 500;
		while (runtime->helper->State() == platform::HelperState::Connected && GetTickCount64() < deadline) Sleep(1);
		runtime->room.reset();
		runtime->helper->Stop();
	}
	delete runtime;
	runtime = nullptr;
}

std::shared_ptr<const RuntimeSnapshot> GetRuntimeSnapshotShared() {
	static const auto empty = std::make_shared<const RuntimeSnapshot>();
	if (!runtime) return empty;
	std::lock_guard<std::mutex> lock(runtime->snapshotMutex);
	return runtime->snapshot;
}

RuntimeSnapshot GetRuntimeSnapshot() {
	return *GetRuntimeSnapshotShared();
}

bool SubmitRuntimeCommand(RuntimeCommand command) {
	if (!runtime || command.displayName.size() >= NETPLAY_DISPLAY_NAME_LEN || command.command.invitation.size() > 4096 ||
		command.preferences.displayName.size() >= NETPLAY_DISPLAY_NAME_LEN || command.roomAction.text.size() > room::MaximumChatBytes ||
		command.preferences.roomName.size() > 64) return false;
	// Gameplay/update commands join this queue when their effect handlers exist.
	const auto kind = command.command.kind;
	if (command.inputAction == input::Action::None && command.service == platform::ServiceAction::None && kind != netplay::CommandKind::HostRoom && kind != netplay::CommandKind::JoinInvite &&
		kind != netplay::CommandKind::LeaveRoom && kind != netplay::CommandKind::StartOffline &&
		kind != netplay::CommandKind::Ready && kind != netplay::CommandKind::Rematch &&
		kind != netplay::CommandKind::SavePreferences && kind != netplay::CommandKind::SetLobbySettings &&
		kind != netplay::CommandKind::RoomAction && kind != netplay::CommandKind::ReplaceRoom &&
		kind != netplay::CommandKind::CheckConnection && kind != netplay::CommandKind::ApplyDelay) return false;
	const auto bytes = sizeof(RuntimeCommand) + command.displayName.size() + command.command.invitation.size() +
		command.preferences.displayName.size() + command.preferences.roomName.size() + command.roomAction.text.size();
	return runtime->commands.TryPush(std::move(command), bytes);
}

bool BindRuntimeInput(int localSlot) {
    if (!runtime || !UserApp::netplay) return false;
    // Refresh connectivity just before native slot binding; never fall back to keyboard.
    runtime->input.Tick(input::ReadDevices());
    if (!runtime->input.Ready()) { runtime->error = loc::T("runtime.gameplay_device_disconnected"); return false; }
    const auto& device = runtime->input.Selected();
    UserApp::netplay->deviceIdx = static_cast<uint8_t>(device.index);
    UserApp::netplay->deviceType = static_cast<uint8_t>(device.type);
    if(!input::AssignToSide(device, localSlot, true)) {
        runtime->error=loc::T("runtime.controller_assignment_unverified");return false;
    }
    runtime->matchInput=device;runtime->matchInputSide=localSlot;runtime->matchInputFault=false;
    return true;
}
bool ReadRuntimeMatchInput(int side,unsigned& mapped,unsigned& raw) {
    mapped=raw=0;
    if(!runtime||!runtime->attached)return false;
    const bool valid=side==runtime->matchInputSide&&input::ReadAssignedInput(runtime->matchInput,side,mapped,raw);
    runtime->matchInputFault=!valid;
    return valid;
}

bool IsRuntimeRoomActive() { return runtime && runtime->attached; }
bool IsRuntimeRecoveryEnabled() { return runtime && runtime->room && runtime->room->Coordination().active; }

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

// Retire this client from the current game. The room-facing half of leaving a
// match is ReportMatchAbort; this is the local half, and every caller needs
// both deferral state and the session torn down in the same order.
void AbortLocalMatch(const char* reason) {
	CancelDeferredGgpoClose();
	if (Game::Battle::System::ggpo) Game::Battle::System::AbortGgpoMatch(reason);
	runtime->match->Abort(); runtime->recoveringMatch = true;
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
	action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
	action.table = static_cast<std::uint8_t>(member->table);
	action.tableRevision = snapshot.tables[action.table].revision;
	action.matchGeneration = runtime->match->Generation();
	if (client.SendRoomAction(action) != session::SendResult::Queued) {
		if (!runtime->pendingAbort || runtime->pendingAbort->matchGeneration == action.matchGeneration)
			runtime->pendingAbort.reset(new room::Action(action));
		if (!runtime->pendingAbortDeadline) runtime->pendingAbortDeadline = GetTickCount64() + 30000;
	}
}

void NotifyRuntimeMatchResult(room::MatchResult result) {
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

static bool StickyRuntimeError(const std::string& error) {
	return error.compare(0, 10, "Networking") == 0;
}

// A Ready press that cannot be honoured ends its intent and is announced once.
static void FailReady(const char* reason) {
	runtime->pendingReady.reset(); runtime->pendingReadyDeadline = 0;
	runtime->error = reason; runtime->readyFailure = reason; ++runtime->readyFailureSequence;
	spdlog::warn("Ready failed: {}", reason);
}
static constexpr ULONGLONG ReadyIntentTimeoutMs = 20000;

void TickRuntime() {
	if (!runtime) return;
	{
		const auto now = GetTickCount64();
		if (runtime->error != runtime->errorShown) { runtime->errorShown = runtime->error; runtime->errorShownAtMs = now; }
		else if (!runtime->error.empty() && !StickyRuntimeError(runtime->error) && now - runtime->errorShownAtMs >= 30000) {
			runtime->error.clear(); runtime->errorShown.clear();
		}
	}
    // Observe locally applied coordination before accepting any room mutation.
    if (runtime->room) {
        runtime->room->Poll();
        const auto authority=runtime->room->Coordination();
        if (authority.active) {
            const bool changed=runtime->observedAuthorityTerm && runtime->observedAuthorityTerm!=authority.term;
            runtime->observedAuthorityTerm=authority.term;
            if((changed || !authority.writable) && runtime->match) {
                // Freeze requested mutations now. Native mappings retire only
                // after the successor's committed cancellation is delivered.
                // A parked Ready survives an ordinary checkpoint fence (it is
                // only sent once the gate reopens); a new authority term
                // discards it and says so.
                runtime->pendingLobbyEdit.reset();
                if (changed && (runtime->pendingReady || runtime->pendingReadyDeadline))
                    FailReady(loc::T("runtime.ready.control_changed"));
            }
            if (runtime->controller.GetSnapshot().room==netplay::RoomState::Opening &&
                runtime->room->GetState()==session::IrohRoom::State::Ready && !runtime->attached) AttachRoom();
            bool recovered=true;
            if (UserApp::server) { diag::ScopedTimer timer(diag::OP_ROOM_RECOVERY_TICK); recovered=runtime->recovery.Tick(*UserApp::server,*runtime->room); }
            if (!recovered)
                runtime->error=loc::T("runtime.recovery_state_failed");
            const auto appliedAuthority=runtime->room->Coordination();
            const bool connected=appliedAuthority.writable && appliedAuthority.rebound;
            const bool applied=!UserApp::server || runtime->recovery.CaughtUp(appliedAuthority);
            runtime->controller.ObserveCoordination(appliedAuthority.term,appliedAuthority.revision,
                connected,GetTickCount64(),applied);
            if(connected) RestoreControlPlane();
            else if(runtime->attached) HandleControlPlaneLoss(loc::T("runtime.room_control_recovering"));
        }
        runtime->controller.AdvanceRecovery(GetTickCount64());
    }
    if (runtime->discordClient) {
        platform::HelperMessage message;
        for (int budget=0; budget<8 && runtime->discordClient->TryReceive(message); ++budget) {
            try {
                const auto event=nlohmann::json::parse(message.payload);
                if (event.at("type")=="status") {
                    runtime->discordStatus=event.value("available",false) ?
                        loc::T(event.value("registered",false)?"discord.connected":"discord.registration_failed") :
                        loc::T("discord.open_desktop");
                } else if (event.at("type")=="join") {
                    const auto state=runtime->controller.GetSnapshot();
                    if (event.at("epoch").get<std::uint64_t>() != state.generation.room) continue;
                    const auto secret=event.at("secret").get<std::string>();
                    std::string party; std::uint64_t expires=0;
                    if (!discord::TicketMetadata(secret,party,expires)) { runtime->error=loc::T("discord.invitation_invalid"); continue; }
                    runtime->discordInvite.Offer(secret,party,expires,event.at("sequence").get<std::uint64_t>(),
                        state.generation.room,state.room!=netplay::RoomState::Idle || runtime->offlineRequested ||
                            (runtime->eventSystemReady && !AtMainMenu()));
                }
            } catch (...) { runtime->discordStatus=loc::T("discord.invalid_event"); }
        }
        if (runtime->discordClient->State()==platform::HelperState::Failed) {
            runtime->discordStatus=loc::T("discord.companion_stopped");
        }
    }
	// Legacy error/teardown hooks can retire the client between owner ticks.
	// Dispose its coordinator before dereferencing that client again.
	if (runtime->attached && !UserApp::netplay) {
		runtime->controller.Execute({netplay::CommandKind::LeaveRoom, runtime->controller.GetSnapshot().generation, {}});
		CloseRoom();
	}
	// A failed router cannot deliver result, game_end, or Leave acknowledgments.
	// Do not retry match teardown against it forever. Native ownership fences
	// retirement; the helper's room_closed event still fences a subsequent room.
	if (runtime->attached && runtime->room &&
		runtime->room->CloseFailedRoom(Game::Battle::System::ggpo != nullptr)) {
		runtime->error = loc::T("runtime.room_connection_failed");
		runtime->controller.Execute({netplay::CommandKind::LeaveRoom, runtime->controller.GetSnapshot().generation, {}});
		CloseRoom();
	}
	if (runtime->services.Snapshot().closeGame && !runtime->updateClosing) {
		runtime->updateClosing = true;
		auto* main = Dimps::Platform::Main::staticMethods.GetSingleton();
		if (main) PostMessageW((*Dimps::Platform::Main::GetWindowData(main))->hWnd, WM_CLOSE, 0, 0);
	}
	// Advance the host authority before the following SessionServer::Step()
	// consumes room actions. This drives result-dispute and chat-rate deadlines
	// on the same owner tick as the server, while clients retain their control
	// stream and existing gameplay links.
	if (UserApp::server) { diag::ScopedTimer timer(diag::OP_ROOM_ADVANCE); UserApp::server->AdvanceCustomRoom(GetTickCount64()); }
	const bool helperReady = runtime->helper && runtime->helper->State() == platform::HelperState::Connected;
    if (AtMainMenu()) {
        runtime->inputDevices = input::ReadDevices();
        if (!runtime->inputInitialized && runtime->input.State()==input::Capture::Idle &&
            runtime->controller.GetSnapshot().room==netplay::RoomState::Idle) {
            auto device = input::MenuDevice(runtime->inputDevices);
            const auto& config = GetConfig();
            for (const auto& candidate : runtime->inputDevices)
                if (candidate.type == config.deviceType && candidate.index == config.deviceIdx) device = candidate;
            if (device.connected) {
                runtime->input.Adopt(device);
                // Keyboard fallback is provisional until explicitly chosen or
                // used to enter a room. A later connected pad can be adopted.
                runtime->inputInitialized=device.type!=Dimps::Pad::PADTYPE_RAWINPUT||config.deviceType==Dimps::Pad::PADTYPE_RAWINPUT;
                if(device.type!=Dimps::Pad::PADTYPE_RAWINPUT&&!input::AssignToSide(device,0,false))
                    runtime->input.Begin();
            } else if(!runtime->inputDevices.empty())runtime->input.Begin();
        }
        if (runtime->input.Tick(runtime->inputDevices)) {
            runtime->inputInitialized=true;
            if(!input::AssignToSide(runtime->input.Selected(),0,false))runtime->input.Begin();
        }
        if (UserApp::netplay && runtime->input.Ready()) {
            UserApp::netplay->deviceIdx = static_cast<uint8_t>(runtime->input.Selected().index);
            UserApp::netplay->deviceType = static_cast<uint8_t>(runtime->input.Selected().type);
        }
    } else runtime->input.Cancel();
	RuntimeCommand command;
	for (int budget = 0; budget < 8 && runtime->commands.TryPop(command); ++budget) {
		// Validate ownership before even a deferred GGPO teardown side effect.
		if (!(command.command.generation == runtime->controller.GetSnapshot().generation)) continue;
        if (command.discordAction != discord::InviteAction::None) {
            if (!runtime->discordInvite.Matches(command.discordRevision)) continue;
            if (command.discordAction == discord::InviteAction::Cancel) runtime->discordInvite.Cancel();
            else if (AtMainMenu() && GetRuntimeSnapshotShared()->discordCanSwitch && !Game::Battle::System::ggpo &&
                runtime->discordInvite.Confirm(command.command.generation.room, command.discordRevision))
                runtime->offlineRequested=false;
            continue;
        }
        if (command.inputAction != input::Action::None) {
            if (command.inputAction == input::Action::Cancel) { runtime->input.Cancel(); runtime->inputInitialized=true; continue; }
            const auto current = runtime->controller.GetSnapshot();
            if (!AtMainMenu() || !GetRuntimeSnapshotShared()->canChangeController || runtime->pendingReady ||
                current.readyPending || (current.match != netplay::MatchState::None && current.match != netplay::MatchState::PostMatch)) continue;
            if (command.inputAction == input::Action::BeginCapture) runtime->input.Begin();
            else if (command.inputAction == input::Action::UseKeyboard) {
                runtime->input.Cancel();
                for (const auto& device : runtime->inputDevices) if (device.type == Dimps::Pad::PADTYPE_RAWINPUT) {
                    runtime->input.Adopt(device); runtime->inputInitialized = true;
                    input::AssignToSide(device, 0, false); break;
                }
            }
            continue;
        }
        if (command.service != platform::ServiceAction::None) {
            if (command.service == platform::ServiceAction::InstallUpdate ||
                ((command.service == platform::ServiceAction::OpenUpdater || command.service == platform::ServiceAction::OpenRecovery) && !GetRuntimeSnapshotShared()->canEditPreferences)) continue;
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
            continue;
        }
		const auto kind = command.command.kind;
        // Recheck queued invite work after callbacks and controller changes.
        // A replacement or Cancel must also invalidate a queued Join/Leave.
        if (command.discordRevision) {
            if (!runtime->discordInvite.Matches(command.discordRevision) || !AtMainMenu() ||
                !GetRuntimeSnapshotShared()->discordCanSwitch || Game::Battle::System::ggpo) continue;
            if (runtime->discordInvite.Expired(static_cast<std::uint64_t>(std::time(nullptr)))) {
                runtime->discordInvite.Cancel();
                runtime->error=loc::T("discord.invitation_expired");
                continue;
            }
            if (kind == netplay::CommandKind::JoinInvite && (!runtime->input.Ready() || !helperReady)) continue;
        }
		if (kind == netplay::CommandKind::HostRoom && !command.preferences.Valid()) continue;
		if (kind == netplay::CommandKind::RoomAction) {
			if (!runtime->attached || !UserApp::netplay || !runtime->match) continue;
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
						runtime->pendingRoomAction.reset(new RuntimeCommand(command));
						runtime->error = loc::T("runtime.spectator_action_retrying");
					} else if (action == room::ActionKind::Unwatch) {
						AbortLocalMatch(loc::T("runtime.leaving_spectator"));
					}
					continue;
				}
				if (runtime->controller.GetSnapshot().match == netplay::MatchState::Playing && !localSpectator) continue;
				runtime->pendingRoomAction.reset(new RuntimeCommand(command));
				AbortLocalMatch(loc::T("runtime.returning_room"));
				continue;
			}
		}
		if (kind == netplay::CommandKind::SavePreferences &&
			(!GetRuntimeSnapshotShared()->canEditPreferences || !command.preferences.Valid())) continue;
		if (kind == netplay::CommandKind::SetLobbySettings &&
			(!CanEditLobby() || !command.preferences.lobby.Valid())) continue;
		if (kind == netplay::CommandKind::SetLobbySettings && runtime->match &&
			(runtime->match->GetPhase() != session::IrohMatchSession::Phase::Idle || Game::Battle::System::ggpo)) {
			// P1 may edit the next match before pressing Ready. Use the same
			// explicit post-match drain/retirement boundary as rematch readiness.
			runtime->pendingLobbyEdit.reset(new RuntimeCommand(command));
			runtime->pendingLobbyEditDeadline = GetTickCount64() + 15000;
			CancelDeferredGgpoClose();
			Game::Battle::System::RetireGgpoSession("iroh_lobby_settings");
			runtime->match->End();
			continue;
		}
		if (kind == netplay::CommandKind::Ready || kind == netplay::CommandKind::Rematch) {
			// A press is never silently dropped. Something the player must fix
			// fails immediately and visibly; everything the room is still
			// finishing (drain, fence, receipt, result) parks the press under
			// one budget and resubmits it here once the gate reopens.
			const auto publishedShared = GetRuntimeSnapshotShared();
			const auto& published = *publishedShared;
			auto* client = UserApp::netplay ? &UserApp::netplay->client : nullptr;
			const bool inFlight = runtime->pendingReady || runtime->controller.GetSnapshot().readyPending ||
				(client && (client->_outstandingReadyRequestNumber != -1 ||
					(published.localSlot >= 0 && published.localSlot < 2 && client->LocalSelectionLocked(published.localSlot))));
			if (inFlight) continue;
			const char* refusal = nullptr;
			if (!runtime->input.Ready()) refusal = loc::T("runtime.ready.assign_controller");
			else if (!selection::FindStage(command.stage)) refusal = loc::T("runtime.ready.stage_unavailable");
			else if (command.character.charaID >= 44) refusal = loc::T("runtime.ready.fighter_unavailable");
			else if (!selection::Available(selection::FromNative(command.character), published.lobbySettings.editionSelect,
				Dimps::Selection::ReadAvailability(command.character.charaID)))
				refusal = loc::T("runtime.ready.selection_unavailable");
			else if (published.session.room != netplay::RoomState::Joined || published.localSlot < 0 || published.localSlot > 1)
				refusal = loc::T("runtime.ready.take_seat");
			if (refusal) { FailReady(refusal); continue; }
			// A parked Rematch may drain after the session left PostMatch; the
			// controller accepts Ready in either state.
			if (kind == netplay::CommandKind::Rematch && runtime->controller.GetSnapshot().match != netplay::MatchState::PostMatch)
				command.command.kind = netplay::CommandKind::Ready;
			if (!runtime->pendingReadyDeadline) {
				runtime->pendingReadyDeadline = GetTickCount64() + ReadyIntentTimeoutMs;
				runtime->pendingReadyGeneration = command.command.generation;
			}
			const bool draining = runtime->match &&
				(runtime->match->GetPhase() != session::IrohMatchSession::Phase::Idle || Game::Battle::System::ggpo);
			if (draining || !published.readyGate) {
				runtime->pendingReady.reset(new RuntimeCommand(command));
				if (draining) {
					// Ready is the explicit handoff from post-match spectator draining.
					// Retire GGPO first and await helper mapping closure before sending it.
					CancelDeferredGgpoClose();
					Game::Battle::System::RetireGgpoSession("iroh_rematch");
					runtime->match->End();
				}
				continue;
			}
		}
		if ((kind == netplay::CommandKind::HostRoom || kind == netplay::CommandKind::JoinInvite) &&
			(!helperReady || !AtMainMenu() || UserApp::netplay || UserApp::server || Game::Battle::System::ggpo)) {
			runtime->error = loc::T("runtime.return_main_menu"); continue;
		}
		if (kind == netplay::CommandKind::ReplaceRoom && !CanBeginReplacement()) continue;
		{
			// The authority checkpoint fence is transient. Dropping the command here
			// silently discarded a press the interface had already accepted, which is
			// what produced repeated pressing until one attempt landed between
			// updates. Park the newest intent instead; the drain below revalidates
			// the generation and resubmits it through this same pump once writable.
			const auto& fence = runtime->controller.GetSnapshot();
			if (kind == netplay::CommandKind::RoomAction && fence.coordinated && !fence.authorityWritable &&
				!runtime->recoveringMatch) {
				runtime->pendingRoomAction.reset(new RuntimeCommand(command));
				runtime->pendingRoomActionDeadline = GetTickCount64() + 3000;
				continue;
			}
		}
		const auto decision = runtime->controller.Execute(command.command);
		if (!decision.accepted) {
			// The fence closed between publish and execute: park the press
			// under its existing budget rather than losing it.
			if ((kind == netplay::CommandKind::Ready || kind == netplay::CommandKind::Rematch) &&
				command.command.generation == runtime->controller.GetSnapshot().generation && !runtime->pendingReady)
				runtime->pendingReady.reset(new RuntimeCommand(command));
			continue;
		}
		if (kind == netplay::CommandKind::RoomAction && command.roomAction.kind == room::ActionKind::Unready)
			{ runtime->pendingReady.reset(); runtime->pendingReadyDeadline = 0; }
        if (command.discordRevision) {
            if (kind == netplay::CommandKind::JoinInvite) runtime->discordInvite.Cancel();
            else if (kind == netplay::CommandKind::LeaveRoom) runtime->discordInvite.LeaveQueued();
        }
		switch (decision.effect) {
		case netplay::Effect::SendRoomAction:
			if (UserApp::netplay->client.SendRoomAction(command.roomAction) != session::SendResult::Queued)
				runtime->error = loc::T("runtime.room_action_failed");
			break;
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
			const bool started = decision.effect == netplay::Effect::HostRoom ? runtime->room->Host(sf4e::sidecarHash) :
				runtime->room->Join(decision.invitation, sf4e::sidecarHash);
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
            if(selected<0 || selected>10) break;
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
				sent = client.PreBattle_SetStage(command.stage) == session::SendResult::Queued && sent;
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
	}
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
					runtime->terminalPersistRevision = 0;
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
				spdlog::info("Room: match ended table={} generation={} result={} replay={}",
					event.table, event.matchGeneration, static_cast<int>(event.result), event.terminalReplay);
            }
			if (event.kind == room::Event::Kind::ResultDisputed)
				spdlog::warn("Match result: disputed table={} generation={} reporter={} result={}",
					event.table, event.matchGeneration, event.member, static_cast<int>(event.result));
			if (!runtime->match || event.kind != room::Event::Kind::MatchEnded ||
				event.matchGeneration != runtime->match->Generation() ||
				(event.result != room::MatchResult::Abort && event.result != room::MatchResult::Cancel)) continue;
			runtime->pendingReady.reset(); runtime->pendingLobbyEdit.reset();
			AbortLocalMatch(loc::T("runtime.table_game_cancelled"));
		}
	}
	// Record the confirmed outcome in the profile and hold the terminal receipt
	// until the settings writer reports that exact revision on disk. The writer
	// retries failed writes itself, so a snapshot is queued once per receipt and
	// only queued again if the writer refused it. PrepareProfileConsumption is
	// idempotent for a key already in the recent list.
	const auto* capturedResult = runtime->resultOutbox.Captured();
	if (runtime->terminalAckPending && !runtime->terminalOutcomeConsumed && runtime->room && capturedResult &&
		runtime->terminalAckGeneration == capturedResult->generation &&
		runtime->terminalAckTable == capturedResult->table && capturedResult->slot < 2 &&
		(capturedResult->result == room::MatchResult::P1Win || capturedResult->result == room::MatchResult::P2Win)) {
		const char* const persistenceWaiting =
			loc::T("runtime.match_record_pending");
		if (!runtime->terminalPersistRevision) {
			const auto consumption = runtime->resultOutbox.PrepareProfileConsumption(runtime->preferences.record);
			if (consumption == netplay::MatchResultOutbox::ProfileConsumption::NoPersistenceRequired) {
				runtime->terminalOutcomeConsumed = true;
			} else if (consumption == netplay::MatchResultOutbox::ProfileConsumption::PersistenceRequired) {
				runtime->terminalPersistRevision = OverlayPrefs::QueuePlayerPreferences(runtime->preferences);
				if (!runtime->terminalPersistRevision && runtime->error != persistenceWaiting) runtime->error = persistenceWaiting;
			}
		}
		if (runtime->terminalPersistRevision) {
			if (OverlayPrefs::PlayerPreferencesSaved(runtime->terminalPersistRevision)) {
				spdlog::info("Match result: profile record saved revision={}", runtime->terminalPersistRevision);
				runtime->terminalOutcomeConsumed = true;
				runtime->terminalPersistRevision = 0;
				if (runtime->error == persistenceWaiting) runtime->error.clear();
			} else if (!OverlayPrefs::PersistenceError().empty() && runtime->error != persistenceWaiting) {
				spdlog::warn("Match result: profile record write failed: {}", OverlayPrefs::PersistenceError());
				runtime->error = persistenceWaiting;
			}
		}
	}
	if (runtime->attached && UserApp::netplay) {
        SessionClient::ActionReply reply;
        while (UserApp::netplay->client.TakeActionReply(reply)) {
            if(reply.actionId==runtime->leaveActionId && reply.accepted) runtime->leaveAcknowledged=true;
			// A refused Ready fails now, with the room's reason, instead of
			// sitting as "Readying up..." until the intent timeout blames the
			// previous match.
			if (!reply.accepted && reply.kindKnown && reply.kind == room::ActionKind::Ready && runtime->pendingReadyDeadline) {
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
	if (runtime->pendingAbort && runtime->attached && UserApp::netplay) {
		const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
		const auto& action = *runtime->pendingAbort;
		const bool current = runtime->match && runtime->match->Generation() == action.matchGeneration &&
			action.table < room::TableCount && snapshot.roomEpoch == action.roomEpoch &&
			snapshot.tables[action.table].matchGeneration == action.matchGeneration;
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
	if (runtime->matchFinishedPending && runtime->attached && UserApp::netplay) {
		const auto& snapshot = UserApp::netplay->client.GetRoomSnapshot();
		const bool current = runtime->match && runtime->match->Generation() == runtime->matchFinishedGeneration &&
			runtime->matchFinishedTable < room::TableCount && snapshot.roomEpoch &&
		snapshot.tables[runtime->matchFinishedTable].matchGeneration == runtime->matchFinishedGeneration;
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
	if (runtime->match) {
		diag::ScopedTimer lifecycleTimer(diag::OP_MATCH_LIFECYCLE);
		if (runtime->matchEnded) {
			runtime->matchEnded = false; runtime->match->End();
			Apply(netplay::EventKind::MatchEnded);
		}
		if (runtime->match && !runtime->match->Tick(Game::Battle::System::ggpo != nullptr)) {
			runtime->error = loc::T("runtime.match_connection_lost");
			// A spectator has no seat to lose: a control reconnect that briefly
			// empties the projection must not turn its stream failure into a room exit.
			if (UserApp::netplay && (LocalIsSpectator() || UserApp::netplay->client.GetRoomSnapshot().roomEpoch)) {
				const bool teardownTimedOut = runtime->match->Error() == "match_teardown_timeout";
				if (!runtime->recoveringMatch) ReportMatchAbort();
				runtime->pendingReady.reset(); runtime->pendingLobbyEdit.reset();
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
			const auto state = runtime->controller.GetSnapshot();
			if (phase != session::IrohMatchSession::Phase::Idle && phase != session::IrohMatchSession::Phase::Ending &&
				(state.match == netplay::MatchState::None || state.match == netplay::MatchState::PostMatch)) {
				Apply(netplay::EventKind::MatchPreparing); runtime->matchEntered = false;
			}
			if (phase == session::IrohMatchSession::Phase::Started && !runtime->matchEntered) {
				Apply(netplay::EventKind::GameplayReady);
				runtime->matchEntered = UserApp::EnterAuthorizedMatch();
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
				else if (missing && runtime->participantLeftGeneration != runtime->match->Generation()) {
					// Custom rooms: the authority ends the game itself (MatchEnded/
					// Abort). The survivor was never told why, only that GGPO
					// timed out. Say it once per game.
					runtime->participantLeftGeneration = runtime->match->Generation();
					PushAlert(loc::T("runtime.participant_left"), NoticeSeverity::Warning);
				}
			}
		}
	}
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
	} else if (runtime->terminalAckPending && Game::Battle::System::ggpo && LocalIsSpectator()) {
		// The spectator cannot reach the acknowledgement above on its own, so
		// bound the wait from the committed match end. Releasing GGPO also
		// re-arms the helper deadline in IrohMatchSession, which cannot age
		// while native GGPO still owns the socket.
		runtime->match->ArmSpectatorExit();
		if (runtime->match->SpectatorExitTimedOut()) {
			const std::string reason = loc::T("runtime.spectator_close_timeout");
			runtime->error = reason;
			ReportMatchAbort();
			AbortLocalMatch(reason.c_str());
		}
	} else if (runtime->match) runtime->match->ClearSpectatorExit();
	if (runtime->recoveringMatch && runtime->match && runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle && AtMainMenu()) {
		runtime->recoveringMatch = false; runtime->matchEntered = false;
		Apply(netplay::EventKind::MatchRecovered, runtime->error);
	}
	const auto currentGeneration = runtime->controller.GetSnapshot().generation;
	if (runtime->pendingRoomAction && !(runtime->pendingRoomAction->command.generation == currentGeneration))
		{ runtime->pendingRoomAction.reset(); runtime->pendingRoomActionDeadline = 0; }
	if (runtime->pendingRoomAction && runtime->pendingRoomActionDeadline &&
		GetTickCount64() >= runtime->pendingRoomActionDeadline) {
		// Say so rather than applying a stale intent or failing silently.
		runtime->pendingRoomAction.reset(); runtime->pendingRoomActionDeadline = 0;
		runtime->error = loc::T("runtime.room_catchup_timeout");
	}
	if (runtime->pendingReady && !(runtime->pendingReady->command.generation == currentGeneration))
		{ runtime->pendingReady.reset(); runtime->pendingReadyDeadline = 0; }
	if (runtime->pendingReadyDeadline && !(runtime->pendingReadyGeneration == currentGeneration)) runtime->pendingReadyDeadline = 0;
	if (runtime->pendingLobbyEdit && !(runtime->pendingLobbyEdit->command.generation == currentGeneration))
		{ runtime->pendingLobbyEdit.reset(); runtime->pendingLobbyEditDeadline = 0; }
	// A parked Ready or lobby edit that never gets its turn is reported, not
	// forgotten: the player pressed it and GGPO was already retired for it.
	if (runtime->pendingReadyDeadline && GetTickCount64() >= runtime->pendingReadyDeadline)
		FailReady(loc::T("runtime.ready.previous_match_timeout"));
	if (runtime->pendingLobbyEdit && runtime->pendingLobbyEditDeadline && GetTickCount64() >= runtime->pendingLobbyEditDeadline) {
		runtime->pendingLobbyEdit.reset(); runtime->pendingLobbyEditDeadline = 0;
		runtime->error = loc::T("runtime.previous_match_close_timeout");
	}
	const auto& controlState = runtime->controller.GetSnapshot();
	const bool healthyRoomControl = controlState.control == netplay::Health::Healthy &&
        (!controlState.coordinated || controlState.authorityWritable);
	if (runtime->pendingRoomAction && healthyRoomControl && !runtime->recoveringMatch && runtime->attached &&
		UserApp::netplay && LocalIsSpectator() && Game::Battle::System::ggpo &&
		(runtime->pendingRoomAction->roomAction.kind == room::ActionKind::Unqueue ||
		 runtime->pendingRoomAction->roomAction.kind == room::ActionKind::Unwatch)) {
		const auto action = runtime->pendingRoomAction->roomAction.kind;
		if (UserApp::netplay->client.SendRoomAction(runtime->pendingRoomAction->roomAction) == session::SendResult::Queued) {
			runtime->pendingRoomAction.reset(); runtime->pendingRoomActionDeadline = 0;
			if (action == room::ActionKind::Unwatch) {
				AbortLocalMatch(loc::T("runtime.leaving_spectator"));
			}
		}
	} else if (runtime->pendingRoomAction && healthyRoomControl && !runtime->recoveringMatch && AtMainMenu() && runtime->match &&
		runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle) {
		if (SubmitRuntimeCommand(*runtime->pendingRoomAction)) { runtime->pendingRoomAction.reset(); runtime->pendingRoomActionDeadline = 0; }
	}
	if (runtime->pendingReady && healthyRoomControl && GetRuntimeSnapshotShared()->readyGate && !Game::Battle::System::ggpo &&
		runtime->match && runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle) {
		// The budget set at the press stays; the pump clears it on commit or failure.
		if (SubmitRuntimeCommand(*runtime->pendingReady)) runtime->pendingReady.reset();
	}
	if (runtime->pendingLobbyEdit && healthyRoomControl && runtime->match && runtime->match->GetPhase() == session::IrohMatchSession::Phase::Idle) {
		if (SubmitRuntimeCommand(*runtime->pendingLobbyEdit)) { runtime->pendingLobbyEdit.reset(); runtime->pendingLobbyEditDeadline = 0; }
	}
	if (runtime->attached && UserApp::netplay && runtime->controller.GetSnapshot().readyPending &&
		UserApp::netplay->client._outstandingReadyRequestNumber == -1) Apply(netplay::EventKind::ReadyAcknowledged);
	const bool helperFailed = !runtime->helper || runtime->helper->State() == platform::HelperState::Failed;
    if(runtime->leaveRequested && runtime->attached && UserApp::netplay && !Game::Battle::System::ggpo &&
        (!runtime->match || runtime->match->GetPhase()==session::IrohMatchSession::Phase::Idle)) {
        const auto& snapshot=UserApp::netplay->client.GetRoomSnapshot();
        if(runtime->leaveAcknowledged || !snapshot.localMember || !runtime->room->Coordination().writable) CloseRoom();
        else if(GetTickCount64()>=runtime->leaveRetryAt) {
            room::Action leave; leave.kind=room::ActionKind::Leave;
            leave.roomEpoch=snapshot.roomEpoch; leave.revision=snapshot.revision;
            if(UserApp::netplay->client.SendRoomAction(leave,&runtime->leaveActionId)==session::SendResult::Queued)
                runtime->leaveRetryAt=GetTickCount64()+500;
        }
    }
    if(runtime->replacementPending && runtime->attached &&
		(!runtime->match ? !Game::Battle::System::ggpo :
			runtime->match->CanReplace(Game::Battle::System::ggpo != nullptr))) CloseRoom();
	if (helperFailed && !runtime->helperLossReported) {
		runtime->helperLossReported = true;
		Apply(netplay::EventKind::HelperLost, loc::T("runtime.networking_unavailable"));
	}
	auto state = runtime->controller.GetSnapshot();
	if (state.room == netplay::RoomState::Opening && runtime->room) {
		if (runtime->room->GetState() == session::IrohRoom::State::Ready && !runtime->attached) AttachRoom();
		if (Joined()) Apply(netplay::EventKind::RoomJoined);
		else if (runtime->room->GetState() == session::IrohRoom::State::Failed || runtime->room->GetState() == session::IrohRoom::State::Idle)
			Apply(netplay::EventKind::RoomFailed, loc::T("runtime.room_join_failed"));
	} else if (state.room == netplay::RoomState::Joined && runtime->room &&
		(runtime->room->GetState() == session::IrohRoom::State::Failed || runtime->room->GetState() == session::IrohRoom::State::Idle ||
		 runtime->room->GetState() == session::IrohRoom::State::Degraded)) {
		Apply(netplay::EventKind::ControlLost, loc::T("runtime.room_connection_lost"));
	}
	state = runtime->controller.GetSnapshot();
	if (state.room == netplay::RoomState::Joined && runtime->attached && UserApp::netplay &&
		UserApp::netplay->client.GetRoomSnapshot().closed) {
        runtime->error=loc::T("runtime.room_closed");
        runtime->controller.Execute({netplay::CommandKind::LeaveRoom,state.generation,{}});
        CloseRoom();
    }
	state = runtime->controller.GetSnapshot();
	if (state.room == netplay::RoomState::Joined && runtime->attached && UserApp::netplay &&
		UserApp::netplay->client.GetRoomSnapshot().roomEpoch &&
		!UserApp::netplay->client.GetRoomSnapshot().localMember) {
        runtime->error=loc::T("runtime.removed_from_room");
        runtime->controller.Execute({netplay::CommandKind::LeaveRoom,state.generation,{}});
        CloseRoom();
    }
	state = runtime->controller.GetSnapshot();
	if (state.room == netplay::RoomState::Closing && (helperFailed || !runtime->room ||
		runtime->room->GetState() == session::IrohRoom::State::Idle)) Apply(netplay::EventKind::RoomClosed);
    if(runtime->replacementPending && runtime->controller.GetSnapshot().room==netplay::RoomState::Idle &&
        helperReady && AtMainMenu() && !UserApp::netplay && !Game::Battle::System::ggpo) {
        RuntimeCommand replacement;
        replacement.command={netplay::CommandKind::HostRoom,runtime->controller.GetSnapshot().generation,{}};
        replacement.preferences=runtime->preferences;
        if(SubmitRuntimeCommand(std::move(replacement))) runtime->replacementPending=false;
    }
	const auto view=PublishThrottled();
    std::string currentParty; std::uint64_t expiry=0;
    if (view.session.room==netplay::RoomState::Joined && runtime->room)
        discord::TicketMetadata(runtime->room->DiscordInvitation(),currentParty,expiry);
    const auto next=runtime->discordInvite.Tick(static_cast<std::uint64_t>(std::time(nullptr)),
        view.session.generation.room,currentParty,view.discordCanSwitch,
        view.session.room==netplay::RoomState::Idle,view.canOpenRoom);
    if (next==discord::Next::Expired) runtime->error=loc::T("discord.invitation_expired");
    else if (next==discord::Next::Leave || next==discord::Next::Join) {
        RuntimeCommand invite;
        invite.command={next==discord::Next::Leave ? netplay::CommandKind::LeaveRoom : netplay::CommandKind::JoinInvite,
            view.session.generation,next==discord::Next::Join ? runtime->discordInvite.Secret() : std::string()};
        invite.preferences=runtime->preferences;
        invite.discordRevision=runtime->discordInvite.Revision();
        SubmitRuntimeCommand(std::move(invite));
    }
}

} } // namespace sf4e::NetplayFacade
