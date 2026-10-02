#include "sf4e__NetplayRuntime.hxx"
#include "../common/HexText.hxx"
#include "../ui/NetworkFeedback.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
platform::HelperBootstrap pendingBootstrap, pendingDiscord;
uint32_t pendingError = 0;
}

namespace internal {
// Deliberately no static owning destructor: Windows calls DLL destructors
// under the loader lock. Normal Main::Destroy explicitly stops and deletes
// this object. On abnormal process exit Windows reclaims the pipe/threads,
// and the launcher's owned job reaps the helper.
Runtime* runtime = nullptr;

// The battle of the granted match is no longer entered, or never was.
void ResetMatchEntry() {
	runtime->matchEntered = false;
	runtime->enteredGeneration = 0;
	runtime->sessionlessCloseGeneration = 0;
	runtime->spectatorStreamFailedGeneration = 0;
	runtime->entryDeferredGeneration = 0;
	runtime->entryDeferredSinceMs = 0;
}

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
    runtime->leaveActionId=runtime->leaveRetryAt=runtime->leaveDeadline=0;
    runtime->matchInput={};runtime->matchInputSide=-1;runtime->matchInputFault=false;
	runtime->match.reset();
	runtime->readyIntent.Clear();
	runtime->lobbyEditIntent.Clear();
	runtime->pendingLobbySettings.reset();
	runtime->roomActionIntent.Clear();
	runtime->chatIntent.Clear();
	runtime->resultOutbox.Reset();
	runtime->recoveringMatch = false;
	runtime->matchFinishedPending = false;
	runtime->matchFinishedGeneration = 0;
    runtime->finishActionId=runtime->finishRetryAt=0;
	runtime->matchFinishedAction.reset();
	runtime->pendingAbort.reset();
	runtime->pendingAbortDeadline = 0;
	runtime->spectatorLockRelease.Cancel();
	ResetMatchEntry();
	runtime->matchEnded = false;
	runtime->terminalAckPending = false;
	runtime->terminalOutcomeConsumed = false;
	runtime->terminalAckTable = 0;
	runtime->terminalAckGeneration = 0;
	// A new room numbers its generations from the start again.
	runtime->committedEndGeneration = 0;
	runtime->matchEndLog = netplay::MatchEndLog();
	if (runtime->attached) {
		ShutdownNetplay(true);
		runtime->attached = false;
	}
	if (runtime->room) runtime->room->Leave(abandon);
}

void Apply(netplay::EventKind kind, const std::string& error) {
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
		ObserveControlPlane(ControlPlaneCause::Coordination, false, error.c_str());
		if (!UserApp::netplay) {
			runtime->controller.Execute({netplay::CommandKind::LeaveRoom, runtime->controller.GetSnapshot().generation, {}});
			CloseRoom();
		}
	}
}

bool CanEditLobby() {
	if (!runtime->attached || !UserApp::netplay || !AtMainMenu() || runtime->pendingLobbySettings || runtime->readyIntent.Parked() ||
		runtime->lobbyEditIntent.Parked() || !runtime->match) return false;
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
} // namespace internal

namespace {
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
		const std::string identity = "iroh:" + HexLower(id);
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
	std::string linkDetail;
	const auto link = DetectNetworkLink(&linkDetail);
	spdlog::info("Runtime: network link {} ({})", NetworkLinkLabel(link), linkDetail);
	UserApp::netplay->client.SetProfileLink(link);
	// Known by now unless the helper started only moments ago; Unknown reads as checking.
	UserApp::netplay->client.SetProfileNat(runtime->room->Network().nat);
    runtime->selectedDelay=runtime->preferences.inputDelay;
    UserApp::netplay->client.SetSelectedDelay(runtime->selectedDelay);
    runtime->inputInitialized=true;
	runtime->match.reset(new session::IrohMatchSession(UserApp::netplay->client, runtime->room));
	runtime->attached = true;
}
}

void ConfigureHelper(const platform::HelperBootstrap& bootstrap, uint32_t startupError) {
	pendingBootstrap = bootstrap;
	pendingError = startupError;
}

void ConfigureDiscord(const platform::HelperBootstrap& bootstrap) { pendingDiscord=bootstrap; }

void StartHelper() {
	if (runtime) return;
	runtime = new Runtime();
	bridge::OpenCommands(runtime->commands);
    runtime->languagePreference = platform::LoadLanguagePreference();
    wchar_t gamePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, gamePath, MAX_PATH)) platform::SetGameDirectory(std::filesystem::path(gamePath).parent_path());
    loc::SetActive(platform::ResolveUiLocale(runtime->languagePreference));
    spdlog::info("Interface language {} (preference {}, game {})", loc::Tag(loc::Active()), runtime->languagePreference,
        platform::GameLanguage().empty() ? "unknown" : platform::GameLanguage());
    if (!runtime->trace.Open(netplay::SettingsStore::DefaultDirectory()))
        spdlog::warn("Session trace could not be opened (error {}); lifecycle states will count as dropped", GetLastError());
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
            runtime->preferences.readySound = saved.value("readySound", true);
            const int volume = saved.value("readySoundVolume", 100);
            runtime->preferences.readySoundVolume = volume >= 10 && volume <= 100 ? volume / 10 * 10 : 100;
            runtime->preferences.discordPresence = saved.value("discordPresence", true);
            runtime->preferences.discordInvites = saved.value("discordInvites", true);
            const int main=saved.contains("mainFighter")&&saved["mainFighter"].is_number_integer()?saved["mainFighter"].get<int>():0;
            runtime->preferences.mainFighter=main>=0&&main<sf4e::selection::FighterCount?main:0;
            const float scale = saved.value("interfaceScale", 1.f);
            runtime->preferences.interfaceScale = scale >= 1.f && scale <= 1.5f ? scale : 1.f;
        } catch (...) { runtime->error = loc::T("runtime.interface_preferences_failed"); }
    }
    runtime->offlineRequested = EnvFlag("SF4E_START_OFFLINE");
    {
        // Read once and cleared, so the helper never inherits the code.
        char link[64] = {};
        const DWORD length = GetEnvironmentVariableA("SF4E_JOIN_LINK", link, sizeof(link));
        SetEnvironmentVariableA("SF4E_JOIN_LINK", nullptr);
        const auto code = length && length < sizeof(link) ? join_link::ParseCode(link) : std::string();
        if (!code.empty()) {
            runtime->pendingJoinLink = join_link::ShortLink(code);
            ++runtime->pendingJoinSequence;
            runtime->pendingJoinFree = true;
            runtime->pendingJoinArrived = std::chrono::steady_clock::now();
            spdlog::info("Room: started with a room link");
        }
        if (!runtime->joinLinks.Open()) spdlog::warn("Room: room links from the browser cannot reach this game");
    }
    {
        // A match link, read once and cleared like the room link.
        char text[128] = {};
        const DWORD length = GetEnvironmentVariableA("SF4E_TOURNAMENT_HANDOFF", text, sizeof(text));
        SetEnvironmentVariableA("SF4E_TOURNAMENT_HANDOFF", nullptr);
        const std::string value = length && length < sizeof(text) ? std::string(text, length) : std::string();
        WipeText(text, sizeof(text));
        const auto split = value.find(' ');
        if (split != std::string::npos)
            QueueTournamentHandoff(tournament_link::ParsePasted(value.substr(split + 1), value.substr(0, split)));
        if (!runtime->handoffLinks.Open()) spdlog::warn("Tournament: match links from the browser cannot reach this game");
    }
	runtime->preferences.inputDelay = GetConfig().inputDelay;
	runtime->preferences.lobby.editionSelect = GetConfig().editionSelect != 0;
	runtime->preferences.lobby.roundCount = GetConfig().roundCount;
	runtime->preferences.lobby.roundTime = GetConfig().roundTimeIntegral;
	if (!runtime->preferences.lobby.Valid()) runtime->preferences.lobby = {};
	if (runtime->preferences.inputDelay < 0 || runtime->preferences.inputDelay > MaximumInputDelay) runtime->preferences.inputDelay = 2;
	{
		nlohmann::json saved;
		std::string error;
		netplay::SettingsStore settings(netplay::SettingsStore::DefaultDirectory());
		if (!settings.LoadLauncher(saved, error)) {
			// The store's detail is an English diagnostic: it goes to the log, and the player reads the catalog's sentence.
			spdlog::warn("Settings could not be loaded: {}", error);
			runtime->error = loc::T("runtime.settings_unreadable");
		}
		else if (!netplay::ReadRoomPreferences(saved, runtime->preferences))
			runtime->error = loc::T("runtime.room_defaults_invalid");
	}
	if (pendingBootstrap.helperPid) {
		runtime->helper.reset(new platform::HelperClient());
		if (runtime->helper->Start(pendingBootstrap)) {
			runtime->room = std::make_shared<session::IrohRoom>(*runtime->helper);
		} else RaiseStickyError(StickyError::StartFailed);
	} else {
		RaiseStickyError(StickyError::HelperUnavailable, pendingError);
	}
    if (pendingDiscord.helperPid) {
        runtime->discordClient.reset(new platform::HelperClient());
        runtime->discordStatusId = runtime->discordClient->Start(pendingDiscord)?"discord.connecting":"discord.unavailable_gameplay_ok";
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
	bridge::CloseCommands();
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
	bridge::Reset();
}

// A room link a later launcher handed over. The shell joins it when the
// player is free, and otherwise holds it for the Join screen.
static void TakeJoinLink() {
	const auto code = runtime->joinLinks.Take();
	if (code.empty()) return;
	runtime->pendingJoinLink = join_link::ShortLink(code);
	++runtime->pendingJoinSequence;
	const auto session = runtime->controller.GetSnapshot();
	runtime->pendingJoinFree = session.room == netplay::RoomState::Idle && session.match == netplay::MatchState::None;
	runtime->pendingJoinArrived = std::chrono::steady_clock::now();
	spdlog::info("Room: a room link arrived from the browser");
}

bool SubmitRuntimeCommand(RuntimeCommand command) {
	if (command.displayName.size() >= NETPLAY_DISPLAY_NAME_LEN || command.command.invitation.size() > 4096 ||
		command.preferences.displayName.size() >= NETPLAY_DISPLAY_NAME_LEN || command.roomAction.text.size() > room::MaximumChatBytes ||
		command.preferences.roomName.size() > 64 || !command.identity.Valid() || !command.tournament.Valid()) return false;
	if (command.identity.op != netplay::IdentityOp::None) {
		const auto bytes = sizeof(RuntimeCommand) + command.identity.Bytes();
		return bridge::PushCommand(std::move(command), bytes);
	}
	if (command.tournament.op != netplay::tournament::Command::Op::None) {
		const auto bytes = sizeof(RuntimeCommand) + command.tournament.Bytes();
		return bridge::PushCommand(std::move(command), bytes);
	}
	// Gameplay/update commands join this queue when their effect handlers exist.
	const auto kind = command.command.kind;
	if (command.inputAction == input::Action::None && command.service == platform::ServiceAction::None && command.previewSoundVolume < 0 && !command.shortInvitation && kind != netplay::CommandKind::HostRoom && kind != netplay::CommandKind::JoinInvite &&
		kind != netplay::CommandKind::LeaveRoom && kind != netplay::CommandKind::StartOffline &&
		kind != netplay::CommandKind::Ready && kind != netplay::CommandKind::Rematch &&
		kind != netplay::CommandKind::SavePreferences && kind != netplay::CommandKind::SetLobbySettings &&
		kind != netplay::CommandKind::RoomAction && kind != netplay::CommandKind::ReplaceRoom &&
		kind != netplay::CommandKind::CheckConnection && kind != netplay::CommandKind::ApplyDelay) return false;
	const auto bytes = sizeof(RuntimeCommand) + command.displayName.size() + command.command.invitation.size() +
		command.preferences.displayName.size() + command.preferences.roomName.size() + command.roomAction.text.size();
	return bridge::PushCommand(std::move(command), bytes);
}

bool IsRuntimeRoomActive() { return runtime && runtime->attached; }
bool IsRuntimeRecoveryEnabled() { return runtime && runtime->room && runtime->room->Coordination().active; }

// TickRuntime runs these phases in order on the game thread. Each one reads and
// writes `runtime`; the order is part of the behaviour.
static void ObserveCoordination() {
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
                runtime->lobbyEditIntent.Clear();
                if (changed && runtime->readyIntent.Active())
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
            if(connected || (runtime->attached && runtime->controller.ControlPlaneEstablished()))
                ObserveControlPlane(ControlPlaneCause::Coordination,connected,loc::T("runtime.room_control_recovering"));
        }
        runtime->controller.AdvanceRecovery(GetTickCount64());
    }
}

static void SettleRoomState(bool helperReady) {
	const bool helperFailed = !runtime->helper || runtime->helper->State() == platform::HelperState::Failed;
    if(runtime->leaveRequested && runtime->attached && UserApp::netplay && !Game::Battle::System::ggpo &&
        (!runtime->match || runtime->match->GetPhase()==session::IrohMatchSession::Phase::Idle)) {
        const auto& snapshot=UserApp::netplay->client.GetRoomSnapshot();
        // An authority that keeps rejecting the Leave must not hold this
        // client in Closing: after the same bound IrohRoom uses, leave
        // locally and let the committed room time the seat out (ledger H-005).
        // The bound starts once the Leave can be sent, after GGPO and the
        // match session retired, whose own teardown deadlines cover them.
        if(!runtime->leaveDeadline) runtime->leaveDeadline=GetTickCount64()+session::IrohRoom::LeaveTimeoutMs;
        const bool leaveTimedOut=GetTickCount64()>=runtime->leaveDeadline;
        if(leaveTimedOut && !runtime->leaveAcknowledged && snapshot.localMember)
            spdlog::warn("Room: leave not confirmed after {} ms; leaving locally", session::IrohRoom::LeaveTimeoutMs);
        if(runtime->leaveAcknowledged || !snapshot.localMember || !runtime->room->Coordination().writable ||
            leaveTimedOut) CloseRoom();
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
		{
			// The host's refusal (name taken, room full) says what to fix; the generic text does not.
			const auto rejection = UserApp::netplay ? UserApp::netplay->client.JoinRejection() : std::nullopt;
			// Without a rejection, the helper's stage says where it failed; hosting has its own wording.
			Apply(netplay::EventKind::RoomFailed, rejection ? std::string(loc::T(SessionClient::JoinRejectionKey(*rejection))) :
				ui::DescribeOpeningFailure(state.isHost, runtime->room->Stage(), runtime->room->Network().relay));
		}
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
}

// The other fighter at this player's table readied first: the game's own
// announcer says a challenger is here, unless the player turned it off.
static void CallOutOpponentReady() {
	if (!runtime->attached || !UserApp::netplay) { runtime->readyChime = {}; return; }
	if (runtime->readyChime.Update(UserApp::netplay->client.GetRoomSnapshot(), GetTickCount64()) && runtime->preferences.readySound)
		PlayChallengerCall(runtime->preferences.readySoundVolume);
}
// Every challenger call, the ready one and the settings preview, comes
// through here. It drives the game's sound system from outside the game's own
// code, so it only rings at the main menu with no match running, never in a
// battle or its transitions. The log lines let a crash report show whether
// and when it rang.
void internal::PlayChallengerCall(int volumePercent) {
	if (!AtMainMenu() || Game::Battle::System::ggpo) {
		spdlog::info("Room: challenger call skipped outside the main menu");
		return;
	}
	if (!Dimps::Sound::PlaySystemCue(Dimps::Sound::SystemCue::HereComesChallenger, Dimps::Sound::SystemChannel::Voice, volumePercent / 100.f))
		spdlog::info("Room: challenger call not played; the game's sound system is not up");
	else spdlog::info("Room: challenger call played");
}
void TickRuntime() {
	if (!runtime) return;
	{
		const auto now = GetTickCount64();
		// A language chosen since the networking error was raised rewords it.
		if (runtime->stickyError != StickyError::None && runtime->error == runtime->stickyText) {
			const auto reworded = StickyText();
			if (reworded != runtime->stickyText) runtime->error = runtime->stickyText = reworded;
		}
		if (runtime->error != runtime->errorShown) { runtime->errorShown = runtime->error; runtime->errorShownAtMs = now; }
		else if (!runtime->error.empty() && !StickyRuntimeError(runtime->error) && now - runtime->errorShownAtMs >= 30000) {
			runtime->error.clear(); runtime->errorShown.clear();
		}
	}
	ObserveCoordination();
	PumpDiscordClient();
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
	CaptureMenuInput();
	DrainCommands(helperReady);
	DrainRoomEvents();
	TickTournament(helperReady);
	PersistTerminalOutcome();
	DrainActionReplies();
	RetryPendingAbort();
	RetrySpectatorLockRelease();
	RetryMatchFinished();
	PumpResultOutbox();
	ConfirmLobbySettings();
	TickMatch();
	ReleaseFinishedMatch();
	ResolvePendingIntents(helperReady);
	SettleRoomState(helperReady);
	CallOutOpponentReady();
	TakeJoinLink();
	PublishAndTickDiscordInvite();
}

} } // namespace sf4e::NetplayFacade
