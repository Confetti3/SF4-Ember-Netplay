#include "sf4e__Overlay.hxx"
#include "sf4e__OverlayPrefs.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__ReplayPlayback.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e.hxx"
#include "../Dimps/Dimps__Selection.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/OverlayLifecycle.hxx"
#include "../ui/OverlayPresentation.hxx"
#include "../ui/OverlayLayers.hxx"
#include "../ui/Theme.hxx"
#include "../ui/Win32Input.hxx"
#include "../ui/DeveloperOverlay.hxx"
#include "../ui/TrainingPanel.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../training/TrainingRuntime.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../common/MatchFrameMeter.hxx"
#include "../common/NativeMenuLock.hxx"
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
// The watched game's picture (ShellView::watchedPicture): the render target
// copied at its own size, which also resolves a multisampled one, then
// filtered down. Made at the first size seen and let go with the overlay,
// which a Reset (also a change of resolution) or the device going frees.
static IDirect3DTexture9* s_watchedFull = nullptr, * s_watchedShown = nullptr;
static bool s_watchedFailed = false;
static ImTextureID WatchedPicture(unsigned width, unsigned height) {
	IDirect3DDevice9* device = Dimps::Platform::D3D::staticMethods.GetSingleton()->lpD3DDevice;
	IDirect3DSurface9* target = nullptr, * full = nullptr, * shown = nullptr;
	D3DSURFACE_DESC desc{};
	const bool done = !s_watchedFailed && SUCCEEDED(device->GetRenderTarget(0, &target)) && SUCCEEDED(target->GetDesc(&desc)) &&
		(s_watchedFull || SUCCEEDED(device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &s_watchedFull, nullptr))) &&
		(s_watchedShown || SUCCEEDED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &s_watchedShown, nullptr))) &&
		SUCCEEDED(s_watchedFull->GetSurfaceLevel(0, &full)) && SUCCEEDED(s_watchedShown->GetSurfaceLevel(0, &shown)) &&
		SUCCEEDED(device->StretchRect(target, nullptr, full, nullptr, D3DTEXF_NONE)) && SUCCEEDED(device->StretchRect(full, nullptr, shown, nullptr, D3DTEXF_LINEAR));
	for (auto* surface : {target, full, shown}) if (surface) surface->Release();
	// Once: the menu then stays translucent until the overlay is made again.
	if (!done && !s_watchedFailed) { s_watchedFailed = true; spdlog::warn("Overlay: no {}x{} picture of the watched game's {}x{} frame", width, height, desc.Width, desc.Height); }
	return done ? reinterpret_cast<ImTextureID>(s_watchedShown) : ImTextureID{};
}
static sf4e::ui::FighterSelector s_fighterSelectors[3];
static sf4e::ui::ApplicationShell shell;
static sf4e::ui::OverlayPresentation presentation;
static sf4e::OverlayPrefs::Data s_prefs;
// The menu takes the keys and the mouse: shown, with the focus.
static std::atomic<bool> capture{false};
// The menu's opens and its hold on the pads, focus or not (OverlayPresentation.hxx).
static sf4e::ui::MenuPads s_menuPads;
// The pointer is over the training HUD's chip: the mouse (only) is Ember's.
static std::atomic<bool> pointerCapture{false};
static std::atomic<bool> focused{true};
static bool trainingHud = true;
// The training controls: whether they are open, the pad owner and the pad's
// position events, under one controller for both threads (TrainingPad.hxx).
static sf4e::input::TrainingControls s_trainingControls;
static std::atomic<bool> trainingAvailable{false};
// A replay plays with Ember's controls: the window procedure keeps their keys.
static std::atomic<bool> replayPlayback{false};
// Go now's Enter on the Training call, taken where the key arrives (Win32Input.hxx).
static sf4e::ui::GoNowKey s_goNowKey;
static int lobbyStageID = 0, lobbyMenuCharaID = 0;
static sf4e::selection::RandomPool lobbyRandomPool;
// The thread that draws the overlay (NoteMessageThread).
static std::atomic<DWORD> s_drawThread{0};
// SF4 can pump the window's messages on another thread than the one that
// draws (sf4e.log then says "window messages arrive on thread"); with its
// render thread off (Faster display) both are the game thread. The Win32
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

// The window's keys and mouse are Ember's while its menu is shown with the
// focus or the training controls are open (TrainingPad.hxx: CapturesInput).
// The game's pads have two owners and no other: the menu's (MenuPads), focus
// or not, and the training controls'.
static bool CapturesInput() { return sf4e::input::CapturesInput(capture.load(), s_trainingControls); }
bool Overlay::CapturesMenuInput() { return sf4e::input::CapturesInput(s_menuPads.Holds(), s_trainingControls); }
bool Overlay::HasInputFocus() { return focused.load(); }
sf4e::ui::MenuPads& Overlay::MenuPads() { return s_menuPads; }
sf4e::ui::OverlayLifecycle& Overlay::Lifecycle() { return s_lifecycle; }
// Start at the main menu or over a fight (input::ControllerOpensMenu).
// The request holds the pads at once, until the frame that takes it has
// shown the menu (MenuPads), so the press never reaches the native menu or
// the fight. Behind another window it counts only where a pad update has
// placed the game where Start opens the menu.
void Overlay::RequestMainControls() {
    if (focused || s_menuPads.StartOpens()) s_menuPads.Request(sf4e::ui::MenuPads::Kind::Controls);
}
void Overlay::PostTrainingPad(const sf4e::input::TrainingPadEvent& event) {
    if(!focused) return;
    s_trainingControls.Post(event);
}
void Overlay::DropTrainingPad() { s_trainingControls.Invalidate(); }
sf4e::input::TrainingControls& Overlay::TrainingControls() { return s_trainingControls; }
void Overlay::PushNetplayAlert(const char* message) { if (message) sf4e::NetplayFacade::SetLastError(message); }
void Overlay::OnClientError(SessionClient::ErrorType type, SessionClient* const, const SessionClient::Callbacks&) {
    PushNetplayAlert(sf4e::loc::T(sf4e::NetplayFacade::IsRuntimePublicJoin() ? SessionClient::PublicJoinRejectionKey(type) : SessionClient::JoinRejectionKey(type)));
}
// Game thread: the native menu's Network item opens the room shell instead.
// In a room or a match it is the only item that goes through: a button held
// from the end of a game can land on any of them as the menu comes back, and
// Benchmark then reads the battle system the game has just freed. The menu
// is also kept from input there (fMainMenu::GetItemObserverState); this is
// the second layer, for a selection that gets past it.
static_assert(rMainMenu::MainMenuItemID::MMI_NETWORK == sf4e::input::NativeNetworkItem, "the Network item changed");
static int OnMainMenuModeSelected(int mode) {
    const auto snapshot = sf4e::NetplayFacade::GetRuntimeSnapshotShared();
    const auto lock = sf4e::input::NativeMenuLockFor(snapshot->session.room, snapshot->session.match);
    spdlog::info("Main menu: native selection {} (room {}, match {}, Ember menu {})", mode, sf4e::input::RoomStateName(snapshot->session.room),
        sf4e::input::MatchStateName(snapshot->session.match), s_menuPads.Holds() ? "shown" : "hidden");
    if (!sf4e::input::NativeSelectionAllowed(lock, mode)) {
        spdlog::warn("Main menu: selection {} refused while a room or match is open", mode);
        return 1;
    }
    if (mode != rMainMenu::MainMenuItemID::MMI_NETWORK) return 0;
    s_menuPads.Request(sf4e::ui::MenuPads::Kind::Play); return 1;
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
	// No device, no overlay: with no context every other overlay entry point
	// does nothing, and the next Reset that has a device starts it. Reports
	// from 1.2.0 crashed here on every launch reading through a null device.
	if (!lpDevice) {
		RECT client{}; if (hWnd) GetClientRect(hWnd, &client);
		spdlog::error("Overlay: the game has no Direct3D device (window {}, client area {}x{}); the overlay is not started",
			hWnd ? "present" : "missing", client.right - client.left, client.bottom - client.top);
		return;
	}
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
    shell.SetChatLog([](const std::string& line) { spdlog::info("{}", line); });
	fMainMenu::OnModeSelectedOverride = OnMainMenuModeSelected;

	sf4e::OverlayPrefs::Data prefs{};
	if (sf4e::OverlayPrefs::Load(prefs)) {
		s_prefs = prefs;
        sf4e::OverlayPrefs::ToConfirmed(lobbyConditions, prefs.lobby);
        lobbyMenuCharaID = prefs.lobby.charaID; lobbyStageID = prefs.stageID;
        lobbyRandomPool = prefs.randomPool;
	}
}

void DrawNetworkCharaConfig(rVsMode::ConfirmedCharaConditions& charaConditions, int& menuCharaID, int* stageId,
	const sf4e::NetplayFacade::RuntimeSnapshot& snapshot) {
	auto pick = sf4e::selection::FromNative(charaConditions);
	pick.fighter = menuCharaID;
	const bool editionSelect = snapshot.session.room == sf4e::netplay::RoomState::Joined ? snapshot.lobbySettings.editionSelect : true;
    int stagedStage = stageId ? *stageId : 0;
    sf4e::selection::RandomPool stagedPool = lobbyRandomPool;
    const sf4e::selection::InstalledContent* installed = snapshot.installedContent.get();
	// The room screens direct the player here when a selection is unusable, so
	// say what is wrong on this screen too, not only on the table.
	const std::string selectionError = sf4e::selection::Available(pick, editionSelect, snapshot.fighterAvailability[pick.fighter], installed) ? std::string() :
		sf4e::loc::T("runtime.selection_combination_unavailable");
	s_fighterSelectors[0].Draw(pick, editionSelect, s_selectionArt.get(), [&](int fighter) { return snapshot.fighterAvailability[fighter]; }, stageId ? &stagedStage : nullptr, snapshot.canEditSelection, selectionError,
		stageId ? &stagedPool : nullptr, installed);
	if (snapshot.canEditSelection && pick.fighter != menuCharaID && pick.fighter >= 0 && pick.fighter < sf4e::selection::FighterCount) {
		// Customization is per fighter: the selector carried the previous
		// fighter's values over, so restore what this one last used, fitted to
		// what is unlocked and to the room's edition rule.
		pick = sf4e::selection::FromNative(s_prefs.fighters[pick.fighter]);
		sf4e::selection::Normalize(pick, editionSelect, &snapshot.fighterAvailability[pick.fighter], installed);
	}
	if (snapshot.canEditSelection) {
        sf4e::selection::ToNative(pick, charaConditions);
        menuCharaID = pick.fighter;
        if (stageId) { *stageId = stagedStage; lobbyRandomPool = stagedPool; }
    }

}

// Hidden, the shell still takes identity answers, ends a cancelled Discord
// sign-in on its service and keeps the room chat. The room is read where the
// published snapshot holds it, not copied, since this runs every match frame;
// only its few notices (at most RoomNoticesKept) are.
static void ConcealApplicationHome(const sf4e::NetplayFacade::RuntimeSnapshot& snapshot) {
    sf4e::ui::ShellView view;
    view.session = snapshot.session;
    view.roomNotices = snapshot.roomNotices;
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
	view.roomNotices = snapshot.roomNotices;
	// Idle times and start holds were stamped when the snapshot was sent;
	// count on since. A hold that has run out still holds until the room says
	// otherwise, so it keeps its last millisecond.
	if (snapshot.roomReceivedMs) {
		const auto elapsedMs = GetTickCount64() - snapshot.roomReceivedMs;
		const auto since = static_cast<std::uint32_t>((std::min<std::uint64_t>)(elapsedMs / 1000, sf4e::room::MaximumIdleSeconds));
		for (auto& member : view.room.members)
			if (member.status != sf4e::room::MemberStatus::Playing)
				member.idleSeconds = (std::min)(member.idleSeconds + since, sf4e::room::MaximumIdleSeconds);
		for (auto& table : view.room.tables) {
			if (table.holdRemainingMs)
				table.holdRemainingMs = elapsedMs < table.holdRemainingMs ? static_cast<std::uint32_t>(table.holdRemainingMs - elapsedMs) : 1;
			if (table.readyRemainingMs)
				table.readyRemainingMs = elapsedMs < table.readyRemainingMs ? static_cast<std::uint32_t>(table.readyRemainingMs - elapsedMs) : 1;
		}
	}
	view.preferences = snapshot.preferences;
	view.lobbySettings = snapshot.lobbySettings;
	view.helperReady = snapshot.helperReady;
	view.canOpenRoom = snapshot.canOpenRoom;
	view.canReplaceRoom = snapshot.canReplaceRoom;
	view.canReady = snapshot.canReady;
	view.canReady = snapshot.atMainMenu && view.canReady && sf4e::selection::Available(sf4e::selection::FromNative(lobbyConditions),
		snapshot.lobbySettings.editionSelect, snapshot.fighterAvailability[lobbyConditions.charaID], snapshot.installedContent.get());
	view.canEditSelection = snapshot.canEditSelection;
    view.selectionLockReason = snapshot.selectionLockReason;
    view.readyRequested = snapshot.readyRequested; view.readyFailure = snapshot.readyFailure;
    view.readyFailureSequence = snapshot.readyFailureSequence;
    view.opponentChangedFighter = snapshot.opponentChangedFighter; view.opponentChangeSequence = snapshot.opponentChangeSequence;
    view.trainingCallSequence = snapshot.trainingCallSequence; view.trainingReadySequence = snapshot.trainingReadySequence;
    view.trainingReadySeconds = snapshot.trainingReadySeconds; view.canTrain = snapshot.canTrain;
    view.practiceFight = snapshot.menuContext == sf4e::input::MenuContext::PracticeMatch;
    view.watchedGame = snapshot.watchedGame;
    // Taken before Ember draws anything this frame, so it holds the game's picture alone.
    if (view.watchedGame.Active() && snapshot.preferences.watchedPicture) {
        const auto& size = ImGui::GetMainViewport()->Size;
        view.watchedPicture = WatchedPicture(static_cast<unsigned>(size.x * sf4e::ui::WatchedPictureShare), static_cast<unsigned>(size.y * sf4e::ui::WatchedPictureShare));
    }
    view.lockInDrops = snapshot.lockInDrops;
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
    view.memberLatency=snapshot.memberLatency;
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
	view.checkUpdatesAtLaunch = snapshot.checkUpdatesAtLaunch;
	view.automaticCountry = snapshot.automaticCountry;
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
    view.inputDevice = snapshot.inputDevice; view.menuButtons = snapshot.menuController.menu;
    view.gameplayBrand = snapshot.gameplayBrand; view.promptBrand = snapshot.promptBrand;
    view.padHint = snapshot.padHint;
    view.padHintUntil = snapshot.padHintUntil; view.padLayer = snapshot.padLayer;
    view.padSetup = snapshot.padSetup; view.atMainMenu = snapshot.atMainMenu;
    view.padLayerPending = snapshot.padLayerPending; view.runningControllerLayer = snapshot.runningControllerLayer;
    view.pads = snapshot.pads; view.captureCandidate = snapshot.captureCandidate;
    view.inputCapture = snapshot.inputCapture;
    view.controllerReady = snapshot.controllerReady;
    view.canChangeController = snapshot.canChangeController;
    view.identity = snapshot.identity;
    view.identityTicket = snapshot.identityTicket; view.identityRequest = snapshot.identityRequest;
    view.identityRefusal = snapshot.identityRefusal;
    view.tournament = snapshot.tournament;
    view.publicRooms = snapshot.publicRooms;
    // The archive is listed, and an entry's inputs read, off this thread (the
    // runtime's workers). The screen on show says what to ask for; the last
    // listing and detail they made come with the snapshot.
    const auto replayWants = shell.ReplayWants();
    if (replayWants.listing) sf4e::platform::replays::WantListing();
    if (replayWants.detail) sf4e::platform::replays::WantDetail(replayWants.detailFile, replayWants.detailRevision);
    view.replays = snapshot.replays;
    const auto* fighter = sf4e::selection::FindFighter(lobbyMenuCharaID);
    view.selectedFighter=lobbyMenuCharaID;
    auto summaryPick = sf4e::selection::FromNative(lobbyConditions); summaryPick.fighter = lobbyMenuCharaID;
    view.selectionSummary = sf4e::loc::Tf("runtime.selection_summary", fighter ? fighter->name : sf4e::loc::T("card.choose_fighter"),
        sf4e::ui::CostumeLabel(summaryPick, snapshot.installedContent.get()), lobbyConditions.color + 1, sf4e::ui::UltraLabel(lobbyConditions.ultraCombo));
    view.fighterName = fighter ? fighter->name : sf4e::loc::T("card.choose_fighter");
    if (fighter) {
        view.hintFighter = fighter->name; view.hintUltra = sf4e::ui::ShortUltraLabel(summaryPick.ultra);
        view.hintAppearance = sf4e::ui::ShortAppearanceLabel(summaryPick, snapshot.installedContent.get());
    }
    view.ultraName = !fighter ? std::string() : summaryPick.ultra == 2 ? std::string(sf4e::loc::T("selection.ultra_double")) :
        std::string(sf4e::ui::UltraLabel(summaryPick.ultra)) + ": " + fighter->ultras[summaryPick.ultra < 0 || summaryPick.ultra > 1 ? 0 : summaryPick.ultra];
    view.ultraSteps = sf4e::selection::AllowedUltras(summaryPick.fighter, summaryPick.edition).size() > 1;
    view.appearanceName = !fighter ? std::string() :
        sf4e::loc::Tf("selection.appearance_value", sf4e::ui::CostumeLabel(summaryPick, snapshot.installedContent.get()), lobbyConditions.color + 1);
    view.stageName = sf4e::ui::StageLabel(lobbyStageID, snapshot.installedContent.get());
    view.fighterOptionsName = sf4e::loc::Tf("room.fighter_options.value",
        lobbyConditions.personalAction == 255 ? std::string(sf4e::loc::T("common.none")) : std::to_string(lobbyConditions.personalAction + 1),
        lobbyConditions.winQuote == 255 ? std::string(sf4e::loc::T("selection.random")) : std::to_string(lobbyConditions.winQuote + 1));
    view.colorSteps = fighter && sf4e::selection::AllowedColors(summaryPick.fighter, summaryPick.costume,
        snapshot.fighterAvailability[lobbyMenuCharaID], snapshot.installedContent.get()).size() > 1;
    if (snapshot.atMainMenu && !sf4e::selection::Available(sf4e::selection::FromNative(lobbyConditions),
        snapshot.lobbySettings.editionSelect, snapshot.fighterAvailability[lobbyMenuCharaID], snapshot.installedContent.get()))
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
				sf4e::selection::AllowedColors(pick.fighter, pick.costume, snapshot.fighterAvailability[lobbyMenuCharaID], snapshot.installedContent.get()), step.delta);
			sf4e::selection::ToNative(pick, lobbyConditions);
			return true;
		}
		sf4e::NetplayFacade::RuntimeCommand request;
		request.command = std::move(action.command);
        request.service = action.service; request.servicePath = std::move(action.servicePath); request.steamProfile = action.steamProfile;
        request.updateOffer = std::move(action.updateOffer);
        request.replay = std::move(action.replay);
        request.reportSubmission = std::move(action.reportSubmission);
        request.inputAction = action.inputAction; request.discordAction = action.discordAction;
        request.padAction = action.padAction;
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
        request.practiceRoom = action.practiceRoom;
        request.delayPolicy = action.delayPolicy;
		request.character = lobbyConditions;
		request.character.charaID = static_cast<BYTE>(lobbyMenuCharaID);
		request.stage = lobbyStageID;
		request.randomPool = lobbyRandomPool;
		request.training = action.training;
		request.leaveWatchedGame = action.leaveWatchedGame;
		if (!sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(request))) return false;
		// Training from inside a room: the menu closes behind it while the runtime
		// sends the game on.
		if (action.training == sf4e::TrainingEntry::Room) presentation.Close("Training from the room");
		return true;
	}, [&] {
		DrawNetworkCharaConfig(lobbyConditions, lobbyMenuCharaID,
			(snapshot.session.room == sf4e::netplay::RoomState::Idle || snapshot.localSlot == 0) ? &lobbyStageID : nullptr, snapshot);
	}
#ifdef SF4E_DEVELOPER_UI
    , [] { sf4e::ui::DrawDeveloperOverlay(s_selectionArt.get()); }
#endif
    );
    if (shell.BoardDrawn()) sf4e::NetplayFacade::NoteLatencyBoard(view.room.roomEpoch);
    if (shell.SelectionDrawn()) sf4e::NetplayFacade::NoteFighterSelector(view.room.roomEpoch);
    if (!open) presentation.Close("Back on Ember's Home");
}


void Overlay::DrawOverlay(const std::function<void()>& picture) {
    // Once each call, wherever the frame ends: with nothing drawn, before it.
    struct Capture {
        const std::function<void()>& take; bool taken = false;
        void operator()() { if (!taken) { taken = true; if (take) take(); } }
        ~Capture() { (*this)(); }
    } captureOnce{picture};
    s_drawThread.store(GetCurrentThreadId());

    // Skipped while a Reset replaces the context; held until the frame is drawn.
    sf4e::ui::OverlayLifecycle::Frame drawing(s_lifecycle);
    if (!drawing || !ImGui::GetCurrentContext()) return;
    // The focus period this frame draws under, taken before it reads anything:
    // the training controls open only under it, so a frame that began before
    // the focus went never opens them, even once the focus is back.
    const auto focus = s_trainingControls.Sample();
    // The frame reads only this: the game thread's state as its last tick left it.
    const auto frame = sf4e::NetplayFacade::GetPresentationSnapshotShared();
    const auto& snapshot = *frame->runtime;
    const auto& status = frame->netplay;
    if (sf4e::ui::ApplyTheme(ImGui_ImplWin32_GetDpiScaleForHwnd(s_overlayWindow), snapshot.preferences.interfaceScale)) ImGui_ImplDX9_InvalidateDeviceObjects();
    // A replay being exported can be followed or cancelled from Ember's menu,
    // so the menu stays available over the battle log while it runs; a
    // Practice match's fighter ends the game from it, and a spectator leaves
    // the game it watches (input::MenuContext).
    const bool exporting = snapshot.replays.exportStage != sf4e::replay::ExportStage::None;
    presentation.Update(snapshot.menuContext, snapshot.session.match, snapshot.offlineRequested, focused);
    s_menuAvailable = presentation.Available();
    const auto openRequest = s_menuPads.Take();
    if (openRequest.kind != sf4e::ui::MenuPads::Kind::None && presentation.Available()) {
        presentation.Open(openRequest.kind == sf4e::ui::MenuPads::Kind::Play ? "the Network item" : "Start");
        if (openRequest.kind == sf4e::ui::MenuPads::Kind::Play) shell.ShowPlay();
    }
    static bool inviteShown=false;
    if (snapshot.discordPending && !inviteShown && snapshot.atMainMenu) { presentation.Open("a Discord invite"); inviteShown=true; }
    if (!snapshot.discordPending) inviteShown=false;
    // A room link from the browser opens the menu at the main menu, where
    // the shell puts it on the Join screen.
    static std::uint64_t joinLinkShown=0;
    if (snapshot.pendingJoinSequence!=joinLinkShown && snapshot.atMainMenu && presentation.Available()) {
        presentation.Open("a room link"); joinLinkShown=snapshot.pendingJoinSequence;
    }
    // Called out of Training: back at the main menu the menu opens on the
    // player's table, which the shell turns to, so the Ready row is in reach.
    static std::uint64_t trainingCallShown=0;
    if (snapshot.trainingCallSequence!=trainingCallShown && snapshot.atMainMenu && presentation.Available()) {
        presentation.Open("a Training call"); trainingCallShown=snapshot.trainingCallSequence;
    }
    // A Discord connect link likewise, at the main menu only: during play it
    // waits until the player opens Ember, which then asks before going on.
    static std::uint64_t connectLinkShown=0;
    if (snapshot.tournament.connect.sequence!=connectLinkShown && snapshot.atMainMenu && presentation.Available()) {
        presentation.Open("a Discord connect link"); connectLinkShown=snapshot.tournament.connect.sequence;
    }
    // A replay link opens the menu at the main menu with no room, where the
    // shell puts its question on the Replays screen; in a room or in play it
    // waits until then.
    static std::string replayLinkShown;
    if (snapshot.replays.link.empty()) replayLinkShown.clear();
    else if (snapshot.replays.link != replayLinkShown && snapshot.atMainMenu && presentation.Available() &&
        snapshot.session.room == sf4e::netplay::RoomState::Idle) {
        presentation.Open("a replay link"); replayLinkShown = snapshot.replays.link;
    }
    // A public room link the player was free to follow opens the menu, where
    // the shell takes them to Public rooms; during play it waits until the
    // player opens Ember, and the shell says so.
    static std::uint64_t roomLinkShown=0;
    if (snapshot.tournament.roomLink.sequence!=roomLinkShown && snapshot.atMainMenu && presentation.Available()) {
        if (snapshot.tournament.roomLink.free) presentation.Open("a public room link");
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
    // The pad drives the menu behind another window too (ShellHoldsPads).
    const auto& setup = snapshot.padSetup.state;
    const bool targetIsGameplay = std::any_of(snapshot.pads.begin(), snapshot.pads.end(), [&](const gamepads::PadView& pad) {
        return pad.gameplay && gamepads::Same(pad.identity, snapshot.padSetup.identity);
    });
    controllerNavigation.Update(snapshot.menuController, sf4e::input::ControllerMenuAvailable(snapshot.menuContext),
        presentation.Visible() || s_trainingControls.Open(), !assigning && !(setup.active && (setup.mode == gamepads::SetupMode::Capture || targetIsGameplay)));
    // Start opens Ember at the main menu and over a fight (a Practice match
    // or a watched game); in Training and over an export it is the game's pause.
    if (controllerNavigation.OpenRequested() && presentation.Available() && sf4e::input::ControllerOpensMenu(snapshot.menuContext)) presentation.Open("Start");
    ImGui::NewFrame();
    sf4e::ui::SetMenuInput({controllerNavigation.Buttons(), ImGui::GetTime()});
    for (const auto& pad : snapshot.pads) if (pad.gameplay) sf4e::ui::SetMenuPromptBrand(snapshot.promptBrand, gamepads::NamesOf(pad.table));
    sf4e::ui::SetMenuGlyphs(snapshot.menuController.deviceType,snapshot.menuController.menu,snapshot.promptBrand);
    if (presentation.Reopened()) shell.ShowPlay();
    // The game's battle log was opened from the Replays screen: Ember's menu
    // gets out of the way, and comes back on that screen when the replay
    // operation says the main menu is back (sf4e__ReplayStore.hxx).
    static std::uint64_t logOpensSeen = 0, returnsSeen = 0;
    if (snapshot.replays.logOpens != logOpensSeen) { logOpensSeen = snapshot.replays.logOpens; presentation.Close("the battle log opened"); }
    if (snapshot.replays.returns != returnsSeen) { returnsSeen = snapshot.replays.returns; presentation.Open("the battle log closed"); shell.ShowReplays(); }
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) presentation.Toggle("F10");
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
    // The setter keeps only its first call, and the lookup asks the shell each time.
    // Before the HUD, the match meter and the hotkeys, which read the lab's settings.
    static bool trainingDirectorySet=false;
    if(!trainingDirectorySet) { sf4e::ui::SetTrainingDirectory(sf4e::netplay::SettingsStore::DefaultDirectory()); trainingDirectorySet=true; }
    // The meter in a match is the player's choice, and a Practice match's own,
    // in private rooms only; the runtime reads nothing for it otherwise.
    namespace meter = sf4e::match_frame_meter;
    sf4e::training::WatchMatches(meter::Watched(meter::Wanted(snapshot.preferences.matchFrameMeter, snapshot.practiceMatch),
        snapshot.room.serverOwned, snapshot.replays.meterShown));
    // How far back a held meter reaches and whether it starts over after a pause, as the Meter settings page chose.
    const auto meterOptions = sf4e::ui::TrainingMeterOptions();
    sf4e::training::SetMeterHistory(meterOptions.history, meterOptions.continuous);
    const auto training = sf4e::training::ReadView();
    trainingAvailable = training.available;
    const bool nativePaused = sf4e::battlePause.Paused();
    // A replay the game plays, not for a video: its keys are Ember's, read as
    // the training keys are, and training's own keys stay off.
    const auto& playback = snapshot.replays.playback;
    const bool replayKeys = playback.playback && !playback.exporting;
    replayPlayback = replayKeys;
    if (focused && replayKeys && !presentation.Visible() && !nativePaused && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyAlt) {
        using sf4e::replaytransport::Command;
        const auto send = [&playback](Command command) { sf4e::replayplayback::Submit(command, sf4e::replaytransport::Device::Keyboard, playback.session); };
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) send(Command::TogglePause);
        // Held, F2 steps again after 300 ms, ten times a second.
        for (int steps = ImGui::GetKeyPressedAmount(ImGuiKey_F2, .3f, .1f); steps > 0; --steps) send(Command::Step);
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) send(Command::Slower);
        if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) send(Command::Faster);
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) send(Command::Meter);
        if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) send(Command::Inputs);
    }
    if (!training.available || !focused) s_trainingControls.Close();
    // Taken every frame, in the order the pad asked, so a press from before
    // the lab was shown never acts later.
    const auto padEvents = s_trainingControls.Take();
    // Called back from Training (TRAINING_IN_ROOMS.md): the call is the
    // room's call owner's, published as the snapshot's trainingCall, and
    // this battle is called only while that call stands for it. The controls
    // close for the call and cannot open again under it. Go now is not read
    // here: its Enter and View are taken where they arrive and carried out by
    // the call's owner (common/TrainingCallInput.hxx); the banner only shows
    // its prompt while it can be pressed.
    const auto& call = snapshot.trainingCall;
    const bool called = training.available && training.leavingIn > 0 && call.Live() && call.generation == training.generation && training.leavingCall == call.serial;
    // Behind another window only the Xbox pad's View can go now (sf4e__Pad.cxx),
    // so the banner offers that one there; Enter needs the focus.
    const bool padBehind = !focused && snapshot.inputDevice.connected && snapshot.inputDevice.type == sf4e::input::PadXInput;
    const bool offerGoNow = called && (focused || padBehind) && !presentation.Visible() && !nativePaused;
    if (called) s_trainingControls.Close();
    // Training's keys and pad stay off during any replay, an export's too.
    if (focused && training.available && !playback.playback && !presentation.Visible()) {
        // The pad drives the controls while they are open, through the same
        // adapter as the shell's menus; the prompts follow the device last used.
        if (!s_trainingControls.Open()) sf4e::ui::NoteMenuDevice(snapshot.menuController.buttons);
        for (const auto& pad : padEvents) {
            // Each is applied only under the owner it was pressed under, which
            // may have changed since the batch was taken.
            if (pad.generation != training.generation || !s_trainingControls.Current(pad)) continue;
            sf4e::ui::TrainingPadPosition(training, sf4e::training::Submit, pad);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false) && !called) s_trainingControls.Toggle(focus);
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) trainingHud = !trainingHud;
        auto practice = [&](sf4e::training::Action action) {
            sf4e::training::Submit({action, training.generation});
        };
        sf4e::ui::TrainingHotkeys(training, sf4e::training::Submit);
        // Not under the call either: F7 could otherwise open the recordings there.
        if (!s_trainingControls.Open() && !called && !ImGui::GetIO().WantTextInput) {
            // F7: standby, then record, then stop. From rest on a recorded
            // slot it first asks about overwriting it, and its Record sends
            // the same press.
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1 + sf4e::ui::TrainingRecordKey), false)) {
                if (!sf4e::training::PadPlaysDummy(training.mode) && training.lengths[training.selected] > 0) {
                    sf4e::ui::OpenTrainingRecordings(s_trainingControls, focus);
                } else practice(sf4e::training::Action::AdvanceRecording);
            }
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1 + sf4e::ui::TrainingPlayKey), false)) practice(training.mode == sf4e::training::Mode::Playback ? sf4e::training::Action::Stop : sf4e::training::Action::Play);
            // Escape leaves standby, keeps a recording, and stops a playback.
            if (training.mode != sf4e::training::Mode::Idle && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) practice(sf4e::training::Action::Interrupt);
        }
        // The controls opening, by any key, chip or pad, interrupt the same
        // way, once per opening.
        static bool controlsWereOpen = false;
        const bool controlsOpen = s_trainingControls.Open();
        if (controlsOpen && !controlsWereOpen) practice(sf4e::training::Action::Interrupt);
        controlsWereOpen = controlsOpen;
        if (s_trainingControls.Open()) {
            // Only what this frame's flyout forwards is read below.
            sf4e::ui::TakeForwardedMenuAction();
            sf4e::ui::DrawTrainingFlyout(training, sf4e::training::Submit);
            if(sf4e::ui::TakeForwardedMenuAction().kind==sf4e::ui::MenuAction::Close) s_trainingControls.Close();
        }
    }
    sf4e::ui::OverlayLayersView layers;
    layers.shellVisible = presentation.Visible(); layers.shellAvailable = presentation.Available(); layers.exporting = exporting;
    const bool trainingOpen = s_trainingControls.Open();
    layers.trainingControlsOpen = trainingOpen; layers.nativePaused = nativePaused;
    layers.focused = focused; layers.trainingHud = trainingHud;
    layers.matchActive = frame->ggpoSessionActive; layers.matchWaitsForMenu = snapshot.matchWaitsForMenu;
    layers.showMatchHud = snapshot.preferences.showMatchHud;
    // Waiting in Training from a room: where the player stands there, and on
    // the call who sat down. The room is the snapshot's; the unread count is
    // the room screen's own.
    const bool inRoom = snapshot.session.room == sf4e::netplay::RoomState::Joined;
    if (training.available && inRoom) layers.trainingRoom = sf4e::ui::DescribeTrainingRoom(snapshot.room, shell.UnreadChat());
    if (called && inRoom) layers.challenger = sf4e::ui::DescribeChallenger(snapshot.room, call);
    if (offerGoNow) layers.challenger.goNowGlyph = sf4e::ui::GoNowGlyph(focused ? sf4e::ui::MenuPromptDevice() : sf4e::input::PadXInput);
    layers.controllerWarning = snapshot.gameplayInputError; layers.captionShown = snapshot.replays.captionShown;
    layers.padHint = snapshot.padHint;
    if (snapshot.replays.captionShown) {
        const auto& caption = snapshot.replays.caption;
        auto& shown = layers.caption;
        for (int side = 0; side < 2 && caption.names; ++side) {
            const std::string wins = caption.set ? std::to_string(caption.wins[side]) : std::string();
            shown.names[side] = wins.empty() ? caption.name[side] : side ? wins + "   " + caption.name[side] : caption.name[side] + "   " + wins;
        }
        if (caption.line) shown.line = caption.text;
        if (caption.set && !caption.names) shown.line += (shown.line.empty() ? "" : "   ") + std::to_string(caption.wins[0]) + " - " + std::to_string(caption.wins[1]);
        shown.mark = caption.mark; shown.nameOffset = snapshot.preferences.matchHudNameOffset;
    }
    {
        // The strip shows for a while after each control, and the chip
        // after one pressed outside the fight; both timed from the counts.
        static std::uint32_t takenSeen = 0, refusedSeen = 0;
        static double takenAt = -1, refusedAt = -1;
        const double now = ImGui::GetTime();
        if (!replayKeys) { takenAt = refusedAt = -1; }
        else {
            if (playback.taken != takenSeen) takenAt = now;
            if (playback.refused != refusedSeen) refusedAt = now;
        }
        takenSeen = playback.taken; refusedSeen = playback.refused;
        auto& replay = layers.replay;
        replay.shown = replayKeys;
        replay.meter = snapshot.replays.meterShown;
        replay.armed = playback.armed; replay.paused = playback.paused; replay.divisor = playback.divisor;
        replay.round = playback.round; replay.cursor = playback.cursor;
        replay.pad = playback.device == sf4e::replaytransport::Device::Pad;
        replay.inputsKnown = snapshot.replays.lanes != nullptr;
        replay.stripAlpha = sf4e::replaytransport::StripAlpha(playback.paused, playback.divisor, takenAt < 0 ? -1 : now - takenAt);
        replay.unavailable = refusedAt >= 0 && now - refusedAt < sf4e::replaytransport::kChipSeconds;
        replay.lanes = replayKeys && playback.lanes && snapshot.replays.lanes;
        std::uint32_t played = 0;
        if (replay.lanes && sf4e::replaylane::PlayedFrame(playback.cursor, played))
            for (int side = 0; side < 2; ++side)
                replay.rowCount[side] = sf4e::replaylane::Rows(*snapshot.replays.lanes, side, playback.round, played, replay.rows[side]);
    }
    if (layers.matchActive) {
        auto& strip = layers.match;
        for (int side = 0; side < 2; ++side) { strip.names[side] = status.matchSides[side].name; strip.links[side] = status.matchSides[side].link; }
        if (status.hasMatchScore) strip.score = sf4e::ui::SetScoreText(status.matchScore);
        strip.rollbackFrames = status.rollbackFrames;
        strip.pingMs = status.pingMs; strip.appliedDelay = status.appliedDelay;
        strip.spectator = status.spectator; strip.behindFrames = status.spectatorBacklog; strip.ownDelays = status.ownDelays;
        strip.queuePosition = status.queuePosition; strip.queueLength = status.queueLength; strip.unreadChat = shell.UnreadChat();
        strip.size = snapshot.preferences.matchHudSize; strip.raised = snapshot.preferences.matchHudRaised; strip.anchor = snapshot.preferences.matchHudAnchor;
        strip.layout = snapshot.preferences.matchHudLayout; strip.nameOffset = snapshot.preferences.matchHudNameOffset; strip.spectators = status.spectators;
        strip.notice = status.lastError; strip.noticeSeverity = static_cast<int>(status.lastErrorSeverity);
        strip.connectionWarning = status.connectionWarning; strip.predictionStalled = status.predictionStalled;
        strip.disconnectCountdownMs = status.disconnectCountdownMs;
    }
    const auto hud = sf4e::ui::DrawOverlayLayers(layers, training);
    if (hud.open && !called) s_trainingControls.Open(focus);
    // Shown survives alt-tab and so do the pads; the cursor and keys are
    // taken only with the focus.
    const bool shown = presentation.Visible() || s_trainingControls.Open();
    const bool passive = sf4e::ui::PassiveOverlayShown(presentation.Visible(), s_trainingControls.Open(), nativePaused);
    const bool visible = focused && shown;
    pointerCapture = focused && !visible && hud.pointer;
    sf4e::ui::SetOverlayCursorOwnership(visible || pointerCapture);
    // This frame publishes only the menu's part of the capture; the training
    // controls' part is their controller's own (CapturesInput).
    capture = focused && presentation.Visible();
    s_menuPads.Publish(presentation.Visible(), openRequest);
    static bool wasCapturing = false;
    const bool capturing = CapturesInput();
    if (wasCapturing && !capturing) { ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse(); }
    wasCapturing = capturing;
    // Said when it changes, with the room: the shell's visibility decides
    // whether the pad reaches Ember's menu or the game's.
    static bool shellWasVisible = false;
    if (presentation.Visible() != shellWasVisible) {
        shellWasVisible = presentation.Visible();
        spdlog::info("Overlay: Ember menu {} ({}), room {}, match {}", shellWasVisible ? "shown" : "hidden", presentation.Why(),
            sf4e::input::RoomStateName(snapshot.session.room), sf4e::input::MatchStateName(snapshot.session.match));
    }
    // The native menu stays parked under a shown shell, focused or not, and
    // while the pad update hides the pads from it (behind another window, or
    // a press held from the shell), so a pad press while alt-tabbed cannot drive it.
    fMainMenu::bOverrideItemObserverState = (shown || controllerNavigation.MenuGuard() || sf4e::Pad::MenuInputBlocked())
        ? rMainMenu::MMIOS_TRANSITION : -1;
    // A Training table's match has no shared save and reset; the HUD says so.
    // A watched game says how to leave it, for its first seconds.
    // The hint outlives the ImGui context a DX9 reset recreates, whose clock
    // restarts, so it runs on the process clock.
    static sf4e::ui::TimedHint watchingHint(10);
    const bool watchingHintDue = watchingHint.Due(
        snapshot.menuContext == sf4e::input::MenuContext::WatchedMatch, GetTickCount64() / 1000.0);
    if (layers.matchActive && focused && passive) {
        if (!status.spectator && snapshot.practiceMatch)
            sf4e::ui::DrawMatchKeysNotice({"training.match.checkpoint_disabled", "training.match.end_hint"});
        else if (watchingHintDue) sf4e::ui::DrawMatchKeysNotice({"runtime.watching_hint"});
    }
    sf4e::OverlayPrefs::Data prefs = s_prefs;
    sf4e::OverlayPrefs::FromConfirmed(prefs.lobby, lobbyConditions); prefs.stageID = lobbyStageID;
    prefs.randomPool = lobbyRandomPool;
    // Every edit is remembered for the fighter it was made on.
    if (prefs.lobby.charaID < prefs.fighters.size()) prefs.fighters[prefs.lobby.charaID] = prefs.lobby;
    if (prefs != s_prefs && sf4e::OverlayPrefs::Save(prefs)) s_prefs = prefs;
    // An export takes its picture between the layers its video holds and
    // the rest of Ember, so the menu and the HUD are never in it.
    ImGui::Render();
    sf4e::ui::ExportPasses passes;
    sf4e::ui::SplitExportPasses(*ImGui::GetDrawData(), passes);
    if (passes.video.CmdListsCount) ImGui_ImplDX9_RenderDrawData(&passes.video);
    captureOnce();
    if (passes.rest.CmdListsCount) ImGui_ImplDX9_RenderDrawData(&passes.rest);

}
void Overlay::FreeOverlay() {
    NoteLifecycleThread("free");
    // After any frame still drawing has ended, so nothing it publishes
    // outlives the teardown.
    sf4e::ui::OverlayLifecycle::Change change(s_lifecycle);
    capture = false; pointerCapture = false;
    s_menuPads.Reset();
    trainingAvailable = false; replayPlayback = false;
    fMainMenu::bOverrideItemObserverState = -1;
    if (!ImGui::GetCurrentContext()) return;
    controllerNavigation.Reset();
    sf4e::ui::SetMenuArt(nullptr);
    s_selectionArt.reset();
    for (auto** texture : {&s_watchedFull, &s_watchedShown}) if (*texture) { (*texture)->Release(); *texture = nullptr; }
    s_watchedFailed = false;
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
            // Standby is dropped and a recording kept (TrainingCommands.hxx keeps a place for it).
            sf4e::training::Submit({sf4e::training::Action::Interrupt, training.generation});
            capture = false; pointerCapture = false;
            activationClick.Reset();
            sf4e::ui::SetOverlayCursorOwnership(false);
            s_inputBridge.RequestClear();
            // The controls close with the focus, in the same step that ends
            // the focus period, so nothing sampled before can open them again.
            s_trainingControls.LoseFocus(); s_goNowKey.Reset();
        }
        else s_trainingControls.GainFocus();
    }
    if (!ImGui::GetCurrentContext()) return 0;
    if (activationClick.Swallow(message, l)) return 0;
    // Go now's Enter on the Training call: decided here, once, before Ember's
    // menu or the game sees it, for the call that stands now. Taken, it is
    // the call's alone; otherwise it goes on as any key does.
    if (s_goNowKey.Take(message, w, l, sf4e::input::TrainingGoNow(), focused && !CapturesInput() && !sf4e::battlePause.Paused())) return 1;
    const auto handled = sf4e::ui::HandleOverlayMessage(window, message, w, l, CapturesInput(), s_menuAvailable, pointerCapture);
    // The position hotkeys only as plain keys, so Alt+F4 still reaches the game.
    const bool plainKey = message == WM_KEYDOWN || message == WM_KEYUP, systemKey = message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
    if (sf4e::replaytransport::KeepsKey(replayPlayback, message, static_cast<unsigned>(w))) return 1;
    if (trainingAvailable && !replayPlayback && ((w >= VK_F5 && w <= VK_F8 && (plainKey || systemKey)) ||
        (plainKey && w >= VK_F1 && w <= VK_F12 && sf4e::ui::TrainingHotkeyBound(static_cast<int>(w - VK_F1))))) return 1;
    return handled;
}
