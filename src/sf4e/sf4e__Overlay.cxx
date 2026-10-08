#include "sf4e__Overlay.hxx"
#include "sf4e__OverlayPrefs.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e.hxx"
#include "../Dimps/Dimps__Selection.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/OverlayLifecycle.hxx"
#include "../ui/OverlayPresentation.hxx"
#include "../ui/Theme.hxx"
#include "../ui/Win32Input.hxx"
#include "../ui/DeveloperOverlay.hxx"
#include "../ui/TrainingPanel.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include "../training/TrainingRuntime.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../common/Localization.hxx"
#include "../platform/GameDisplaySettings.hxx"
#include "../platform/UiPreferencesStore.hxx"
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <spdlog/spdlog.h>
#include <ctime>
#include <memory>
#include <atomic>

namespace Overlay = sf4e::Overlay;
using fMainMenu = sf4e::GameEvents::MainMenu;
using rMainMenu = Dimps::GameEvents::MainMenu;
using rVsMode = Dimps::GameEvents::VsMode;
static HWND s_overlayWindow = nullptr;
static sf4e::ui::ControllerNavigation controllerNavigation;
static std::unique_ptr<sf4e::ui::SelectionArt> s_selectionArt;
static sf4e::ui::FighterSelector s_fighterSelectors[3];
static sf4e::ui::ApplicationShell shell;
static sf4e::ui::OverlayPresentation presentation;
static sf4e::OverlayPrefs::Data s_prefs;
static std::atomic<bool> capture{false};
// The pointer is over the training HUD's chip: the mouse (only) is Ember's.
static std::atomic<bool> pointerCapture{false};
static std::atomic<bool> focused{true};
static sf4e::ui::OpenRequests s_openRequests;
static bool trainingOpen = false, trainingHud = true;
static std::atomic<bool> trainingAvailable{false};
static int lobbyStageID = 0, lobbyMenuCharaID = 0;
static sf4e::selection::StageMask lobbyStageExcluded = 0;
// The thread that draws the overlay (NoteMessageThread).
static std::atomic<DWORD> s_drawThread{0};
// SF4 can pump the window's messages on another thread than the one that
// draws (sf4e.log then says "window messages arrive on thread"). The Win32
// backend hands their ImGui input to this bridge, which the drawing thread
// applies, so a message and a frame never wait for each other. Each D3D Reset
// frees and recreates the context on the game thread; s_lifecycle keeps that
// from happening under a frame or a message (OverlayLifecycle.hxx).
static sf4e::ui::Win32InputBridge s_inputBridge;
static sf4e::ui::OverlayLifecycle s_lifecycle;
// Whether the menu can open now, published by the drawing thread for the
// window procedure (F10 is the game's key otherwise).
static std::atomic<bool> s_menuAvailable{false};
static rVsMode::ConfirmedCharaConditions lobbyConditions = {0,0,0,0,0,0,0,0,14};

bool Overlay::CapturesMenuInput() { return capture.load(); }
bool Overlay::HasInputFocus() { return focused.load(); }
void Overlay::RequestMainControls() { if(focused) { capture=true; s_openRequests.Post(sf4e::ui::OpenRequests::Kind::Controls); } }
void Overlay::PushNetplayAlert(const char* message) { if (message) sf4e::NetplayFacade::SetLastError(message); }
void Overlay::OnClientError(SessionClient::ErrorType type, SessionClient* const, const SessionClient::Callbacks&) {
    PushNetplayAlert(sf4e::loc::T(sf4e::NetplayFacade::IsRuntimePublicJoin() ? SessionClient::PublicJoinRejectionKey(type) : SessionClient::JoinRejectionKey(type)));
}
// Game thread: the native menu's Network item opens the room shell instead.
static int OnMainMenuModeSelected(int mode) {
    if (mode != rMainMenu::MainMenuItemID::MMI_NETWORK) return 0;
    s_openRequests.Post(sf4e::ui::OpenRequests::Kind::Play); return 1;
}
// Which thread creates and frees the overlay, against the one that draws it.
// Freeing it on another thread while a frame draws would use a freed context;
// this says whether that can happen before anything guards against it.
static void NoteLifecycleThread(const char* what) {
    const DWORD self = GetCurrentThreadId(), draw = s_drawThread.load();
    if (draw && self != draw) spdlog::warn("Overlay: {} on thread {} while the overlay draws on thread {}", what, self, draw);
    else spdlog::info("Overlay: {} on thread {}", what, self);
}

void Overlay::InitializeOverlay(HWND hWnd, IDirect3DDevice9* lpDevice) {
	NoteLifecycleThread("initialize");
	sf4e::ui::OverlayLifecycle::Change change(s_lifecycle);
	sf4e::OverlayPrefs::StartPersistence();
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    sf4e::ui::SetOverlayCursorOwnership(false);
	s_overlayWindow = hWnd;
	sf4e::ui::ApplyTheme(ImGui_ImplWin32_GetDpiScaleForHwnd(hWnd));
	ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
	ImGui_ImplWin32_Init(hWnd);
	s_inputBridge.RequestClear(); // nothing queued for a context that is gone
	ImGui_ImplWin32_SetInputBridge(&s_inputBridge);
	// The game draws at its resolution and Present stretches that over the window, whose
	// client area can differ (a window the screen clips, a borderless tool, a
	// 16:10 desktop). Laying out in the window's size put the match HUD's names below the
	// PLAYER labels, so lay out in the backbuffer's. Each Reset initializes the overlay again.
	IDirect3DSurface9* backBuffer = nullptr; D3DSURFACE_DESC backBufferDesc{};
	if (SUCCEEDED(lpDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer) {
		if (SUCCEEDED(backBuffer->GetDesc(&backBufferDesc)))
			ImGui_ImplWin32_SetRenderSize(static_cast<float>(backBufferDesc.Width), static_cast<float>(backBufferDesc.Height));
		backBuffer->Release();
	}
	RECT client{}; GetClientRect(hWnd, &client);
	spdlog::info("Overlay: game draws at {}x{}, window client area {}x{}", backBufferDesc.Width, backBufferDesc.Height, client.right - client.left, client.bottom - client.top);
	ImGui_ImplDX9_Init(lpDevice);
	wchar_t gamePath[MAX_PATH] = {}, modulePath[MAX_PATH] = {};
	HMODULE module = nullptr;
	GetModuleFileNameW(nullptr, gamePath, MAX_PATH);
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&Overlay::InitializeOverlay), &module);
	GetModuleFileNameW(module, modulePath, MAX_PATH);
	const std::wstring gameFile(gamePath), moduleFile(modulePath);
	s_selectionArt.reset(new sf4e::ui::SelectionArt(lpDevice,
		gameFile.substr(0, gameFile.find_last_of(L"\\/")),
		moduleFile.substr(0, moduleFile.find_last_of(L"\\/")) + L"/assets/selection",
		[](const std::string& line) { spdlog::warn("{}", line); }));
    sf4e::ui::SetMenuArt(s_selectionArt.get());
    sf4e::ui::SetAtlasBuildLog([](const char* line) { spdlog::info("{}", line); });
	fMainMenu::OnModeSelectedOverride = OnMainMenuModeSelected;

	sf4e::OverlayPrefs::Data prefs{};
	if (sf4e::OverlayPrefs::Load(prefs)) {
		s_prefs = prefs;
        sf4e::OverlayPrefs::ToConfirmed(lobbyConditions, prefs.lobby);
        lobbyMenuCharaID = prefs.lobby.charaID; lobbyStageID = prefs.stageID;
        lobbyStageExcluded = prefs.randomStageExcluded;
	}
}

void DrawNetworkCharaConfig(rVsMode::ConfirmedCharaConditions& charaConditions, int& menuCharaID, int* stageId,
	const sf4e::NetplayFacade::RuntimeSnapshot& snapshot) {
	auto pick = sf4e::selection::FromNative(charaConditions);
	pick.fighter = menuCharaID;
	const bool editionSelect = snapshot.session.room == sf4e::netplay::RoomState::Joined ? snapshot.lobbySettings.editionSelect : true;
    int stagedStage = stageId ? *stageId : 0;
    sf4e::selection::StageMask stagedPool = lobbyStageExcluded;
	// The room screens direct the player here when a selection is unusable, so
	// say what is wrong on this screen too, not only on the table.
	const std::string selectionError = sf4e::selection::Available(pick, editionSelect, snapshot.fighterAvailability[pick.fighter]) ? std::string() :
		sf4e::loc::T("runtime.selection_combination_unavailable");
	s_fighterSelectors[0].Draw(pick, editionSelect, s_selectionArt.get(), [&](int fighter) { return snapshot.fighterAvailability[fighter]; }, stageId ? &stagedStage : nullptr, snapshot.canEditSelection, selectionError,
		stageId ? &stagedPool : nullptr);
	if (snapshot.canEditSelection && pick.fighter != menuCharaID && pick.fighter >= 0 && pick.fighter < sf4e::selection::FighterCount) {
		// Customization is per fighter: the selector carried the previous
		// fighter's values over, so restore what this one last used, fitted to
		// what is unlocked and to the room's edition rule.
		pick = sf4e::selection::FromNative(s_prefs.fighters[pick.fighter]);
		sf4e::selection::Normalize(pick, editionSelect, &snapshot.fighterAvailability[pick.fighter]);
	}
	if (snapshot.canEditSelection) {
        sf4e::selection::ToNative(pick, charaConditions);
        menuCharaID = pick.fighter;
        if (stageId) { *stageId = stagedStage; lobbyStageExcluded = stagedPool; }
    }

}

// Hidden, the shell still takes identity answers, ends a cancelled Discord
// sign-in on its service and keeps the room chat. The room is read where the
// published snapshot holds it, not copied, since this runs every match frame.
static void ConcealApplicationHome(const sf4e::NetplayFacade::RuntimeSnapshot& snapshot) {
    sf4e::ui::ShellView view;
    view.session = snapshot.session;
    view.identity = snapshot.identity;
    view.identityTicket = snapshot.identityTicket; view.identityRequest = snapshot.identityRequest;
    view.identityRefusal = snapshot.identityRefusal;
    shell.Background(view, snapshot.room, [](sf4e::ui::ShellAction action) {
        sf4e::NetplayFacade::RuntimeCommand request;
        request.command = std::move(action.command);
        request.identity = std::move(action.identity);
        request.publicTicket = std::move(action.publicTicket);
        request.createdRules = action.createdRules;
        return sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(request));
    });
}
static void DrawApplicationHome(const sf4e::NetplayFacade::RuntimeSnapshot& snapshot, const sf4e::NetplayStatus& status) {
	sf4e::ui::ShellView view;
    view.controllerAvailable = controllerNavigation.Available();
    view.controllerUnavailable = controllerNavigation.Unavailable();
    view.controllerFocus = controllerNavigation.FocusRequested();
    view.controllerBack = controllerNavigation.BackRequested();
	view.session = snapshot.session;
	view.room = snapshot.room;
	// Idle times and start holds were stamped when the snapshot was sent;
	// count on since. A hold that has run out still holds until the room says
	// otherwise, so it keeps its last millisecond.
	if (snapshot.roomReceivedMs) {
		const auto elapsedMs = GetTickCount64() - snapshot.roomReceivedMs;
		const auto since = static_cast<std::uint32_t>((std::min<std::uint64_t>)(elapsedMs / 1000, sf4e::room::MaximumIdleSeconds));
		for (auto& member : view.room.members)
			if (member.status != sf4e::room::MemberStatus::Playing)
				member.idleSeconds = (std::min)(member.idleSeconds + since, sf4e::room::MaximumIdleSeconds);
		for (auto& table : view.room.tables)
			if (table.holdRemainingMs)
				table.holdRemainingMs = elapsedMs < table.holdRemainingMs ? static_cast<std::uint32_t>(table.holdRemainingMs - elapsedMs) : 1;
	}
	view.preferences = snapshot.preferences;
	view.lobbySettings = snapshot.lobbySettings;
	view.helperReady = snapshot.helperReady;
	view.canOpenRoom = snapshot.canOpenRoom;
	view.canReplaceRoom = snapshot.canReplaceRoom;
	view.canReady = snapshot.canReady;
	view.canReady = snapshot.atMainMenu && view.canReady && sf4e::selection::Available(sf4e::selection::FromNative(lobbyConditions),
		snapshot.lobbySettings.editionSelect, snapshot.fighterAvailability[lobbyConditions.charaID]);
	view.canEditSelection = snapshot.canEditSelection;
    view.selectionLockReason = snapshot.selectionLockReason;
    view.readyRequested = snapshot.readyRequested; view.readyFailure = snapshot.readyFailure;
    view.readyFailureSequence = snapshot.readyFailureSequence;
    view.opponentChangedFighter = snapshot.opponentChangedFighter; view.opponentChangeSequence = snapshot.opponentChangeSequence;
    view.trainingCallSequence = snapshot.trainingCallSequence; view.trainingReadySequence = snapshot.trainingReadySequence;
    view.trainingReadySeconds = snapshot.trainingReadySeconds; view.canTrain = snapshot.canTrain;
	view.canEditPreferences = snapshot.canEditPreferences;
	view.canEditLobby = snapshot.canEditLobby;
	view.settingsPending = snapshot.settingsPending;
    view.selectedDelay=snapshot.selectedDelay; view.recommendedDelay=snapshot.recommendedDelay; view.autoDelayMeasured=snapshot.autoDelayMeasured;
    view.opponentDelay=snapshot.opponentDelay;
    view.delayLocked=snapshot.delayLocked; view.canProbe=snapshot.canProbe; view.canApplyDelay=snapshot.canApplyDelay;
    view.probeRelay=snapshot.probeRelay; view.probeOpponent=snapshot.probeOpponent;
    view.probeOpponentNat=snapshot.probeOpponentNat; view.netReport=snapshot.netReport;
    view.probeRoute=snapshot.probeRoute; view.probeP50Us=snapshot.probeP50Us; view.probeP95Us=snapshot.probeP95Us;
    view.probeP99Us=snapshot.probeP99Us; view.probeJitterUs=snapshot.probeJitterUs; view.probeBenchmark=snapshot.probeBenchmark;
    view.probeStatus=snapshot.probeStatus; view.probeSamples=snapshot.probeSamples; view.probeLost=snapshot.probeLost;
    view.probeSent=snapshot.probeSent; view.probeExpected=snapshot.probeExpected;
	view.localSlot = snapshot.localSlot;
	view.invitation = snapshot.invitation;
	view.shortInvitation = snapshot.shortInvitation;
	view.shortInvitationPending = snapshot.shortInvitationPending;
	view.shortInvitationFailures = snapshot.shortInvitationFailures;
	view.pendingJoinLink = snapshot.pendingJoinLink;
	view.pendingJoinSequence = snapshot.pendingJoinSequence;
	view.pendingJoinDirect = snapshot.pendingJoinDirect;
	view.error = snapshot.helperError;
	view.settingsError = snapshot.settingsError;
	view.languagePreference = snapshot.languagePreference;
	view.unixNow = static_cast<std::uint64_t>(std::time(nullptr));
	// Both read their file once; the card's own outcome decides whether to ask.
	static const bool showGameSettingsCard = !sf4e::platform::GameSettingsCardHidden();
	view.gameSettings = sf4e::platform::GameDisplaySettings();
	view.showGameSettingsCard = showGameSettingsCard;
	view.build = sf4e::sidecarHash;
	view.members = snapshot.members;
    view.network = snapshot.network; view.services = snapshot.services;
    view.controller = snapshot.controller;
    view.discordPending=snapshot.discordPending; view.discordConfirm=snapshot.discordConfirm;
    view.discordRevision=snapshot.discordRevision;
    view.discordCanSwitch=snapshot.discordCanSwitch; view.discordStatus=snapshot.discordStatus;
    view.inputDevice = snapshot.inputDevice;
    view.inputCapture = snapshot.inputCapture;
    view.controllerReady = snapshot.controllerReady;
    view.canChangeController = snapshot.canChangeController;
    view.identity = snapshot.identity;
    view.identityTicket = snapshot.identityTicket; view.identityRequest = snapshot.identityRequest;
    view.identityRefusal = snapshot.identityRefusal;
    view.tournament = snapshot.tournament;
    view.publicRooms = snapshot.publicRooms;
    // The archive is listed off this thread (the runtime's lister); while the
    // Replays screen shows, a new listing is asked for and the last one
    // becomes its rows.
    if (shell.Navigation().Screen() == "replays") {
        sf4e::platform::replays::WantListing();
        if (snapshot.replays.archive) for (const auto& replay : *snapshot.replays.archive) {
            const auto name = [&](int side) {
                const auto* fighter = sf4e::selection::FindFighter(replay.fighters[side]);
                const std::string fighterName = fighter ? fighter->name : sf4e::loc::T("common.unavailable");
                return replay.names[side].empty() ? fighterName : sf4e::loc::Tf("replays.player", replay.names[side], fighterName);
            };
            // What the replay's own file says: the score, the length, and for
            // each player the look, the buttons pressed and how busy they were.
            std::string score, info;
            if (replay.read) {
                namespace in = sf4e::replayinputs;
                if (replay.score[0] >= 0) score = std::to_string(replay.score[0]) + "-" + std::to_string(replay.score[1]);
                const std::string length = in::Clock(replay.frames);
                info = (score.empty() ? "" : score + "  ") + sf4e::loc::Tf("inputs.length_value", replay.rounds, length);
                for (int side = 0; side < 2; side++) {
                    const auto& player = replay.players[side]; const auto& stats = replay.stats[side];
                    const int costume = player.costume + 1, color = player.color + 1;
                    const char* ultra = player.ultra == 0 ? "I" : player.ultra == 1 ? "II" : "W";
                    const unsigned perMinute = stats.PerMinute(), jumps = stats.jumps, crouched = stats.CrouchedPercent();
                    std::string presses;
                    for (int button = 0; button < 6; button++) presses += std::string(button ? " " : "") + in::ButtonNames[button] + " " + std::to_string(stats.presses[button]);
                    info += std::string("\n") + (side ? "P2 " : "P1 ") + name(side) + ": " + sf4e::loc::Tf("replays.info_look", costume, color, ultra) + ". " + presses + ". " +
                        sf4e::loc::Tf("inputs.activity", perMinute, jumps, crouched);
                }
            }
            view.replays.push_back({sf4e::platform::WideToUtf8(replay.path.wstring()),
                replay.label + "  " + sf4e::loc::Tf("replays.fighters", name(0), name(1)), {replay.names[0], replay.names[1]}, replay.spectated, replay.watched, replay.video, score, info, replay.time, {replay.score[0], replay.score[1]}});
        }
    }
    view.replaysReady = snapshot.replays.ready;
    view.replayNotice = snapshot.replays.notice; view.replayNoticeError = snapshot.replays.noticeError;
    view.replayLink = snapshot.replays.link;
    const auto* fighter = sf4e::selection::FindFighter(lobbyMenuCharaID);
    view.selectedFighter=lobbyMenuCharaID;
    auto summaryPick = sf4e::selection::FromNative(lobbyConditions); summaryPick.fighter = lobbyMenuCharaID;
    view.selectionSummary = sf4e::loc::Tf("runtime.selection_summary", fighter ? fighter->name : sf4e::loc::T("card.choose_fighter"),
        sf4e::ui::CostumeLabel(summaryPick), lobbyConditions.color + 1, sf4e::ui::UltraLabel(lobbyConditions.ultraCombo));
    view.fighterName = fighter ? fighter->name : sf4e::loc::T("card.choose_fighter");
    view.ultraName = !fighter ? std::string() : summaryPick.ultra == 2 ? std::string(sf4e::loc::T("selection.ultra_double")) :
        std::string(sf4e::ui::UltraLabel(summaryPick.ultra)) + ": " + fighter->ultras[summaryPick.ultra < 0 || summaryPick.ultra > 1 ? 0 : summaryPick.ultra];
    view.ultraSteps = sf4e::selection::AllowedUltras(summaryPick.fighter, summaryPick.edition).size() > 1;
    view.appearanceName = !fighter ? std::string() :
        sf4e::loc::Tf("selection.appearance_value", sf4e::ui::CostumeLabel(summaryPick), lobbyConditions.color + 1);
    view.stageName = sf4e::ui::StageLabel(lobbyStageID);
    view.fighterOptionsName = sf4e::loc::Tf("room.fighter_options.value",
        lobbyConditions.personalAction == 255 ? std::string(sf4e::loc::T("common.none")) : std::to_string(lobbyConditions.personalAction + 1),
        lobbyConditions.winQuote == 255 ? std::string(sf4e::loc::T("selection.random")) : std::to_string(lobbyConditions.winQuote + 1));
    view.colorSteps = fighter && sf4e::selection::AllowedColors(summaryPick.fighter, summaryPick.costume,
        snapshot.fighterAvailability[lobbyMenuCharaID]).size() > 1;
    if (snapshot.atMainMenu && !sf4e::selection::Available(sf4e::selection::FromNative(lobbyConditions),
        snapshot.lobbySettings.editionSelect, snapshot.fighterAvailability[lobbyMenuCharaID]))
        view.selectionError = sf4e::loc::T("runtime.selection_unavailable");
    // Outside a fight the shell status line carries the notice; transient
    // info ("Connection restored.") belongs to the match HUD only.
    if (view.error.empty() && status.lastError[0] && status.lastErrorSeverity != sf4e::NoticeSeverity::Info) view.error = status.lastError;
	bool open = true;
    shell.Draw(view, &open, [&](sf4e::ui::ShellAction action) {
		// The table page's Ultra and Appearance rows edit the pick here; nothing is sent.
		if (const auto step = action.selectionStep; step.field != sf4e::ui::ShellAction::SelectionStep::Field::None) {
			if (!snapshot.canEditSelection || !sf4e::selection::FindFighter(lobbyMenuCharaID)) return false;
			auto pick = sf4e::selection::FromNative(lobbyConditions); pick.fighter = lobbyMenuCharaID;
			const bool ultra = step.field == sf4e::ui::ShellAction::SelectionStep::Field::Ultra;
			sf4e::ui::Step(ultra ? pick.ultra : pick.color, ultra ? sf4e::selection::AllowedUltras(pick.fighter, pick.edition) :
				sf4e::selection::AllowedColors(pick.fighter, pick.costume, snapshot.fighterAvailability[lobbyMenuCharaID]), step.delta);
			sf4e::selection::ToNative(pick, lobbyConditions);
			return true;
		}
		// Training from inside a room: no room command is sent, the game alone
		// moves, and the menu closes behind it. The runtime's own gate decides.
		if (action.enterTraining && action.command.kind != sf4e::netplay::CommandKind::StartOffline) {
			if (!snapshot.canTrain) return false;
			fMainMenu::RequestTraining();
			presentation.Close();
			return true;
		}
		sf4e::NetplayFacade::RuntimeCommand request;
		request.command = std::move(action.command);
        request.service = action.service;
        request.replay = std::move(action.replay);
        request.inputAction = action.inputAction; request.discordAction = action.discordAction;
        request.discordRevision = action.discordRevision;
		request.displayName = snapshot.preferences.displayName;
		request.preferences = std::move(action.preferences);
		request.roomAction = std::move(action.roomAction);
        request.selectedDelay=action.selectedDelay;
        request.previewSoundVolume=action.previewSoundVolume;
        request.identity = std::move(action.identity);
        request.shortInvitation=action.shortInvitation;
        request.tournament = std::move(action.tournament);
        request.publicTicket = std::move(action.publicTicket);
        request.createdRules = action.createdRules;
		request.character = lobbyConditions;
		request.character.charaID = static_cast<BYTE>(lobbyMenuCharaID);
		request.stage = lobbyStageID;
		request.randomStageExcluded = lobbyStageExcluded;
		// Training rides on StartOffline: the game is sent on only where that
		// command can be accepted, outside any room or match and at the main menu.
		const bool training = action.enterTraining && request.command.kind == sf4e::netplay::CommandKind::StartOffline &&
			snapshot.atMainMenu && snapshot.session.room == sf4e::netplay::RoomState::Idle &&
			snapshot.session.match == sf4e::netplay::MatchState::None;
		if (!sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(request))) return false;
		if (training) fMainMenu::RequestTraining();
		return true;
	}, [&] {
		DrawNetworkCharaConfig(lobbyConditions, lobbyMenuCharaID,
			(snapshot.session.room == sf4e::netplay::RoomState::Idle || snapshot.localSlot == 0) ? &lobbyStageID : nullptr, snapshot);
	}
#ifdef SF4E_DEVELOPER_UI
    , [] { sf4e::ui::DrawDeveloperOverlay(s_selectionArt.get()); }
#endif
    );
    if (!open) presentation.Close();
}


void Overlay::DrawOverlay() {
    s_drawThread.store(GetCurrentThreadId());

    // Skipped while a Reset replaces the context; held until the frame is drawn.
    sf4e::ui::OverlayLifecycle::Frame drawing(s_lifecycle);
    if (!drawing || !ImGui::GetCurrentContext()) return;
    // The frame reads only this: the game thread's state as its last tick left it.
    const auto frame = sf4e::NetplayFacade::GetPresentationSnapshotShared();
    const auto& snapshot = *frame->runtime;
    const auto& status = frame->netplay;
    if (sf4e::ui::ApplyTheme(ImGui_ImplWin32_GetDpiScaleForHwnd(s_overlayWindow) * snapshot.preferences.interfaceScale)) ImGui_ImplDX9_InvalidateDeviceObjects();
    presentation.Update(snapshot.atMainMenu, snapshot.session.match, snapshot.offlineRequested, focused);
    s_menuAvailable = presentation.Available();
    const auto openRequest = s_openRequests.Take();
    if (openRequest != sf4e::ui::OpenRequests::Kind::None && presentation.Available()) {
        presentation.Open();
        if (openRequest == sf4e::ui::OpenRequests::Kind::Play) shell.ShowPlay();
    }
    static bool inviteShown=false;
    if (snapshot.discordPending && !inviteShown && snapshot.atMainMenu) { presentation.Open(); inviteShown=true; }
    if (!snapshot.discordPending) inviteShown=false;
    // A room link from the browser opens the menu at the main menu, where
    // the shell puts it on the Join screen.
    static std::uint64_t joinLinkShown=0;
    if (snapshot.pendingJoinSequence!=joinLinkShown && snapshot.atMainMenu && presentation.Available()) {
        presentation.Open(); joinLinkShown=snapshot.pendingJoinSequence;
    }
    // Called out of Training: back at the main menu the menu opens on the
    // player's table, which the shell turns to, so the Ready row is in reach.
    static std::uint64_t trainingCallShown=0;
    if (snapshot.trainingCallSequence!=trainingCallShown && snapshot.atMainMenu && presentation.Available()) {
        presentation.Open(); trainingCallShown=snapshot.trainingCallSequence;
    }
    // A Discord connect link likewise, at the main menu only: during play it
    // waits until the player opens Ember, which then asks before going on.
    static std::uint64_t connectLinkShown=0;
    if (snapshot.tournament.connect.sequence!=connectLinkShown && snapshot.atMainMenu && presentation.Available()) {
        presentation.Open(); connectLinkShown=snapshot.tournament.connect.sequence;
    }
    // A replay link opens the menu at the main menu with no room, where the
    // shell puts its question on the Replays screen; in a room or in play it
    // waits until then.
    static std::string replayLinkShown;
    if (snapshot.replays.link.empty()) replayLinkShown.clear();
    else if (snapshot.replays.link != replayLinkShown && snapshot.atMainMenu && presentation.Available() &&
        snapshot.session.room == sf4e::netplay::RoomState::Idle) {
        presentation.Open(); replayLinkShown = snapshot.replays.link;
    }
    // A public room link the player was free to follow opens the menu, where
    // the shell takes them to Public rooms; during play it waits until the
    // player opens Ember, and the shell says so.
    static std::uint64_t roomLinkShown=0;
    if (snapshot.tournament.roomLink.sequence!=roomLinkShown && snapshot.atMainMenu && presentation.Available()) {
        if (snapshot.tournament.roomLink.free) presentation.Open();
        roomLinkShown=snapshot.tournament.roomLink.sequence;
    }
    sf4e::ui::SetOverlayCursorOwnership(focused && presentation.Visible());
    const bool assigning = snapshot.inputCapture != sf4e::input::Capture::Idle;
    // Player navigation is semantic, not ImGui spatial scoring. Text input is
    // still provided by the Win32 backend after explicit field activation.
    ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame();
    static std::size_t notedOverflows = 0;
    if (const auto overflows = s_inputBridge.Overflows(); overflows != notedOverflows) {
        if (!notedOverflows) spdlog::warn("Overlay: window input backed up past {} events; dropped it and released every key",
            sf4e::ui::Win32InputBridge::MaxQueuedEvents);
        notedOverflows = overflows;
    }
    controllerNavigation.Update(snapshot.menuController, sf4e::input::ControllerMenuAvailable(snapshot.menuContext),
        presentation.Visible() || trainingOpen, focused && !assigning);
    if (controllerNavigation.OpenRequested() && presentation.Available()) presentation.Open();
    ImGui::NewFrame();
    sf4e::ui::SetMenuInput({controllerNavigation.Buttons(), ImGui::GetTime()});
    sf4e::ui::SetMenuGlyphs(snapshot.menuController.deviceType,snapshot.menuController.selectPhysical,snapshot.menuController.backPhysical);
    if (presentation.Reopened()) shell.ShowPlay();
    // The game's battle log was opened from the Replays screen: Ember's menu
    // gets out of the way, and comes back on that screen when the replay
    // operation says the main menu is back (sf4e__ReplayStore.hxx).
    static std::uint64_t logOpensSeen = 0, returnsSeen = 0;
    if (snapshot.replays.logOpens != logOpensSeen) { logOpensSeen = snapshot.replays.logOpens; presentation.Close(); }
    if (snapshot.replays.returns != returnsSeen) { returnsSeen = snapshot.replays.returns; presentation.Open(); shell.Navigation().Home(); shell.Navigation().Push("replays"); }
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) presentation.Toggle();
    // Every overlay frame, since the training panel draws art with the menu
    // closed. Pump returns at once when nothing drew art since the last pump.
    if (s_selectionArt) s_selectionArt->Pump();
    if (presentation.Visible()) DrawApplicationHome(snapshot, status);
    else ConcealApplicationHome(snapshot);
    if (!presentation.Visible() && assigning) {
        sf4e::NetplayFacade::RuntimeCommand cancel;
        cancel.command = {sf4e::netplay::CommandKind::HostRoom, snapshot.session.generation, {}};
        cancel.inputAction = sf4e::input::Action::Cancel;
        sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(cancel));
    }
    // The meter in a match is the player's choice; the runtime reads nothing for it otherwise.
    sf4e::training::WatchMatches(snapshot.preferences.matchFrameMeter || snapshot.replays.meterShown);
    const auto training = sf4e::training::ReadView();
    trainingAvailable = training.available;
    bool pointer = false;
    if (!training.available || !focused) trainingOpen = false;
    if (focused && training.available && !presentation.Visible()) {
        sf4e::ui::SetMenuInput({0, ImGui::GetTime()});
        sf4e::ui::SetMenuGlyphs(sf4e::input::PadKeyboard,0,0);
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) trainingOpen = !trainingOpen;
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) trainingHud = !trainingHud;
        auto practice = [&](sf4e::training::Action action) {
            sf4e::training::Submit({action, 0, training.generation});
        };
        // The setter keeps only its first call, and the lookup asks the shell each time.
        // Before the HUD and the hotkeys, which read the combo book too.
        static bool comboBookDirectorySet=false;
        if(!comboBookDirectorySet) { sf4e::ui::SetComboBookDirectory(sf4e::netplay::SettingsStore::DefaultDirectory()); comboBookDirectorySet=true; }
        sf4e::ui::TrainingHotkeys(training, sf4e::training::Submit,
            !trainingOpen && (snapshot.menuController.buttons & sf4e::ui::ControllerSample::Chat) != 0);
        if (!trainingOpen && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
                if(training.mode != sf4e::training::Mode::Recording && training.lengths[training.selected]>0) {
                    sf4e::ui::ShowTrainingRecordings(); trainingOpen=true;
                } else practice(training.mode == sf4e::training::Mode::Recording ? sf4e::training::Action::Stop : sf4e::training::Action::Record);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) practice(training.mode == sf4e::training::Mode::Playback ? sf4e::training::Action::Stop : sf4e::training::Action::Play);
        }
        if (trainingOpen) {
            // Only what this frame's flyout forwards is read below.
            sf4e::ui::TakeForwardedMenuAction();
            sf4e::ui::DrawTrainingFlyout(training, sf4e::training::Submit);
            if(sf4e::ui::TakeForwardedMenuAction().kind==sf4e::ui::MenuAction::Close) trainingOpen=false;
        } else if (trainingHud) {
            const auto hud = sf4e::ui::DrawTrainingHud(training);
            if (hud.open) trainingOpen = true;
            if (hud.replay) practice(sf4e::training::Action::Play);
            if (hud.stop) practice(sf4e::training::Action::Stop);
            pointer = hud.pointer;
        }
    }
    // Shown survives alt-tab; taking the cursor and keys needs focus.
    const bool shown = presentation.Visible() || trainingOpen;
    const bool visible = focused && shown;
    pointerCapture = focused && !visible && pointer;
    sf4e::ui::SetOverlayCursorOwnership(visible || pointerCapture);
    if (capture.exchange(visible) && !visible) { ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse(); }
    // The native menu stays parked under a shown shell, focused or not, so a
    // pad press while alt-tabbed cannot drive it.
    fMainMenu::bOverrideItemObserverState = (shown || controllerNavigation.MenuGuard()) ? rMainMenu::MMIOS_TRANSITION : -1;
    if (!shown && presentation.Available()) {
        const auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .5f, vp->Pos.y + 12 * sf4e::ui::Scale()), ImGuiCond_Always, ImVec2(.5f, 0));
        ImGui::Begin("Ember shortcut", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextUnformatted(sf4e::loc::T("runtime.open_shortcut")); ImGui::End();
    }
    // A started match cannot be entered from Training or Options: say what it is waiting for.
    if (snapshot.matchWaitsForMenu) {
        const auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .5f, vp->Pos.y + 44 * sf4e::ui::Scale()), ImGuiCond_Always, ImVec2(.5f, 0));
        ImGui::Begin("Ember match waiting", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextUnformatted(sf4e::loc::T("runtime.return_menu_to_join")); ImGui::End();
    }
    // An export's caption, over the replay while it records: it is in the picture.
    if (snapshot.replays.captionShown) {
        const auto& caption = snapshot.replays.caption;
        sf4e::ui::ExportCaptionView shown;
        for (int side = 0; side < 2 && caption.names; ++side) {
            const std::string wins = caption.set ? std::to_string(caption.wins[side]) : std::string();
            shown.names[side] = wins.empty() ? caption.name[side] : side ? wins + "   " + caption.name[side] : caption.name[side] + "   " + wins;
        }
        if (caption.line) shown.line = caption.text;
        if (caption.set && !caption.names) shown.line += (shown.line.empty() ? "" : "   ") + std::to_string(caption.wins[0]) + " - " + std::to_string(caption.wins[1]);
        shown.mark = caption.mark; shown.nameOffset = snapshot.preferences.matchHudNameOffset;
        sf4e::ui::DrawExportCaption(shown);
    }
    if (frame->ggpoSessionActive) {
        sf4e::ui::DrawControllerWarning(snapshot.gameplayInputError);
        // The frame meter, while the runtime is watching this match for it.
        if (training.watching && !presentation.Visible()) sf4e::ui::DrawMatchMeter(training);
        // A Training table's shared reset and save: asked for here, sent with the player's next input.
        if (focused && !presentation.Visible() && !status.spectator && sf4e::training::MatchPracticeActive()) {
            sf4e::ui::SetComboBookDirectory(sf4e::netplay::SettingsStore::DefaultDirectory());
            sf4e::training::RequestMatchPractice(sf4e::ui::MatchPracticeKeys(
                (snapshot.menuController.buttons & sf4e::ui::ControllerSample::Chat) != 0));
        }
        sf4e::ui::MatchStripView strip;
        for (int side = 0; side < 2; ++side) { strip.names[side] = status.matchSides[side].name; strip.links[side] = status.matchSides[side].link; }
        if (status.hasMatchScore) strip.score = sf4e::ui::SetScoreText(status.matchScore);
        strip.rollbackFrames = status.rollbackFrames;
        strip.pingMs = status.pingMs; strip.appliedDelay = status.appliedDelay;
        strip.spectator = status.spectator;
        strip.size = snapshot.preferences.matchHudSize; strip.raised = snapshot.preferences.matchHudRaised; strip.anchor = snapshot.preferences.matchHudAnchor;
        strip.layout = snapshot.preferences.matchHudLayout; strip.nameOffset = snapshot.preferences.matchHudNameOffset; strip.spectators = status.spectators;
        strip.notice = status.lastError; strip.noticeSeverity = static_cast<int>(status.lastErrorSeverity);
        strip.connectionWarning = status.connectionWarning; strip.predictionStalled = status.predictionStalled;
        strip.disconnectCountdownMs = status.disconnectCountdownMs;
        if (snapshot.preferences.showMatchHud) sf4e::ui::DrawMatchStrip(strip);
        else {
            // The player hid the telemetry, not the reasons a fight stalls or ends.
            const auto line = sf4e::ui::MatchStripStateLine(strip);
            const int severity = strip.noticeSeverity >= 2 ? 2 : (strip.connectionWarning || strip.predictionStalled) ? 1 : strip.noticeSeverity;
            sf4e::ui::DrawMatchNotice(line, severity);
        }
    }
    sf4e::OverlayPrefs::Data prefs = s_prefs;
    sf4e::OverlayPrefs::FromConfirmed(prefs.lobby, lobbyConditions); prefs.stageID = lobbyStageID;
    prefs.randomStageExcluded = lobbyStageExcluded;
    // Every edit is remembered for the fighter it was made on.
    if (prefs.lobby.charaID < prefs.fighters.size()) prefs.fighters[prefs.lobby.charaID] = prefs.lobby;
    if (memcmp(&prefs, &s_prefs, sizeof(prefs)) != 0 && sf4e::OverlayPrefs::Save(prefs)) s_prefs = prefs;
    ImGui::Render(); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

}
void Overlay::FreeOverlay() {
    capture = false; pointerCapture = false;
    trainingAvailable = false;
    fMainMenu::bOverrideItemObserverState = -1;
    NoteLifecycleThread("free");
    sf4e::ui::OverlayLifecycle::Change change(s_lifecycle);
    if (!ImGui::GetCurrentContext()) return;
    controllerNavigation.Reset();
    sf4e::ui::SetMenuArt(nullptr);
    s_selectionArt.reset();
    ImGui_ImplDX9_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
}
// ImGui's input queue is not thread-safe: the window messages that feed it
// must come on the thread that draws the overlay. Say once which threads
// those are, so a crash report shows whether they ever differ.
static void NoteMessageThread() {
    static std::atomic<bool> noted{false};
    const DWORD draw = s_drawThread.load();
    if (!draw || noted.exchange(true)) return;
    const DWORD messages = GetCurrentThreadId();
    if (messages == draw) spdlog::info("Overlay: window messages and drawing share thread {}", messages);
    else spdlog::warn("Overlay: window messages arrive on thread {} but the overlay draws on thread {}", messages, draw);
}
LRESULT WINAPI Overlay::OverlayWindowFunc(HWND window, UINT message, WPARAM w, LPARAM l) {
    NoteMessageThread();
    // The click that brings the game forward again must not press an Ember
    // row that is drawn under the pointer while the game is behind another window.
    static sf4e::ui::ActivationClickFilter activationClick;
    const sf4e::ui::OverlayLifecycle::Message handling(s_lifecycle);
    // Native display resets can pump activation messages after FreeOverlay and
    // before InitializeOverlay. Focus belongs to the window, not its ImGui
    // context: dropping reactivation here leaves F10/Start permanently gated.
    if (message == WM_ACTIVATEAPP) {
        focused = w != 0;
        if (!focused) {
            const auto training = sf4e::training::ReadView();
            sf4e::training::Submit({sf4e::training::Action::Stop, 0, training.generation});
            capture = false; pointerCapture = false;
            activationClick.Reset();
            sf4e::ui::SetOverlayCursorOwnership(false);
            s_inputBridge.RequestClear();
        }
    }
    if (!ImGui::GetCurrentContext()) return 0;
    if (activationClick.Swallow(message, l)) return 0;
    const auto handled = sf4e::ui::HandleOverlayMessage(window, message, w, l, capture, s_menuAvailable, pointerCapture);
    // The combo hotkeys only as plain keys, so Alt+F4 still reaches the game.
    const bool plainKey = message == WM_KEYDOWN || message == WM_KEYUP, systemKey = message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
    if (trainingAvailable && ((w >= VK_F5 && w <= VK_F8 && (plainKey || systemKey)) ||
        (plainKey && w >= VK_F1 && w <= VK_F12 && sf4e::ui::TrainingHotkeyBound(static_cast<int>(w - VK_F1))))) return 1;
    return handled;
}
