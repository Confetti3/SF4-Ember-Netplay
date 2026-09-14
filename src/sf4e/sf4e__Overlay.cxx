#include "sf4e__Overlay.hxx"
#include "sf4e__OverlayPrefs.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Selection.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/OverlayPresentation.hxx"
#include "../ui/Theme.hxx"
#include "../ui/Win32Input.hxx"
#include "../ui/DeveloperOverlay.hxx"
#include "../ui/TrainingPanel.hxx"
#include "../training/TrainingRuntime.hxx"
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <memory>
#include <atomic>

namespace Overlay = sf4e::Overlay;
using fMainMenu = sf4e::GameEvents::MainMenu;
using rMainMenu = Dimps::GameEvents::MainMenu;
using rVsMode = Dimps::GameEvents::VsMode;
using fSystem = sf4e::Game::Battle::System;
static HWND s_overlayWindow = nullptr;
static sf4e::ui::ControllerNavigation controllerNavigation;
static std::unique_ptr<sf4e::ui::SelectionArt> s_selectionArt;
static sf4e::ui::FighterSelector s_fighterSelectors[3];
static sf4e::ui::ApplicationShell shell;
static sf4e::ui::OverlayPresentation presentation;
static sf4e::OverlayPrefs::Data s_prefs;
static std::atomic<bool> capture{false};
static std::atomic<bool> focused{true};
static std::atomic<bool> mainRequested{false};
static bool trainingOpen = false, trainingHud = true;
static std::atomic<bool> trainingAvailable{false};
static int lobbyStageID = 0, lobbyMenuCharaID = 0;
static rVsMode::ConfirmedCharaConditions lobbyConditions = {0,0,0,0,0,0,0,0,14};

bool Overlay::CapturesMenuInput() { return capture.load(); }
bool Overlay::HasInputFocus() { return focused.load(); }
void Overlay::RequestMainControls() { if(focused) { capture=true; mainRequested=true; } }
void Overlay::PushNetplayAlert(const char* message) { if (message) sf4e::NetplayFacade::SetLastError(message); }
void Overlay::OnClientError(SessionClient::ErrorType type, SessionClient* const, const SessionClient::Callbacks&) {
    const char* message = "The room request failed. Leave the room and try again.";
    switch (type) {
    case SessionClient::ErrorType::SCE_JOIN_REJECTED_HASH_INVALID: message = "Build mismatch. Both players must use the same SF4 Ember Netplay package."; break;
    case SessionClient::ErrorType::SCE_JOIN_REJECTED_LOBBY_FULL: message = "This room is full."; break;
    case SessionClient::ErrorType::SCE_JOIN_REJECTED_NAME_TAKEN: message = "That player name is already in the room. Change it in Settings."; break;
    default: break;
    }
    PushNetplayAlert(message);
}
static int OnMainMenuModeSelected(int mode) {
    if (mode != rMainMenu::MainMenuItemID::MMI_NETWORK) return 0;
    presentation.Open(); shell.ShowPlay(); return 1;
}
void Overlay::InitializeOverlay(HWND hWnd, IDirect3DDevice9* lpDevice) {
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
		moduleFile.substr(0, moduleFile.find_last_of(L"\\/")) + L"/assets/selection"));
    sf4e::ui::SetMenuArt(s_selectionArt.get());
	fMainMenu::OnModeSelectedOverride = OnMainMenuModeSelected;

	sf4e::OverlayPrefs::Data prefs{};
	if (sf4e::OverlayPrefs::Load(prefs)) {
		s_prefs = prefs;
        sf4e::OverlayPrefs::ToConfirmed(lobbyConditions, prefs.lobby);
        lobbyMenuCharaID = prefs.lobby.charaID; lobbyStageID = prefs.stageID;
	}
}

void DrawNetworkCharaConfig(rVsMode::ConfirmedCharaConditions& charaConditions, int& menuCharaID, int* stageId) {
	auto pick = sf4e::selection::FromNative(charaConditions);
	pick.fighter = menuCharaID;
	const auto snapshot = sf4e::NetplayFacade::GetRuntimeSnapshot();
	const bool editionSelect = snapshot.session.room == sf4e::netplay::RoomState::Joined ? snapshot.lobbySettings.editionSelect : true;
    int stagedStage = stageId ? *stageId : 0;
	s_fighterSelectors[0].Draw(pick, editionSelect, s_selectionArt.get(), [&](int fighter) { return snapshot.fighterAvailability[fighter]; }, stageId ? &stagedStage : nullptr, snapshot.canEditSelection);
	if (snapshot.canEditSelection) {
        sf4e::selection::ToNative(pick, charaConditions);
        menuCharaID = pick.fighter;
        if (stageId) *stageId = stagedStage;
    }

}

static void DrawApplicationHome() {

	const auto snapshot = sf4e::NetplayFacade::GetRuntimeSnapshot();
	sf4e::ui::ShellView view;
    view.controllerAvailable = controllerNavigation.Available();
    view.controllerUnavailable = controllerNavigation.Unavailable();
    view.controllerFocus = controllerNavigation.FocusRequested();
    view.controllerBack = controllerNavigation.BackRequested();
	view.session = snapshot.session;
	view.room = snapshot.room;
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
    view.readyLockReason = snapshot.readyLockReason;
	view.canEditPreferences = snapshot.canEditPreferences;
	view.canEditLobby = snapshot.canEditLobby;
	view.settingsPending = snapshot.settingsPending;
    view.selectedDelay=snapshot.selectedDelay; view.recommendedDelay=snapshot.recommendedDelay;
    view.delayLocked=snapshot.delayLocked; view.canProbe=snapshot.canProbe; view.canApplyDelay=snapshot.canApplyDelay;
    view.probeRoute=snapshot.probeRoute; view.probeP50Us=snapshot.probeP50Us; view.probeP95Us=snapshot.probeP95Us;
    view.probeP99Us=snapshot.probeP99Us; view.probeJitterUs=snapshot.probeJitterUs; view.probeBenchmark=snapshot.probeBenchmark;
    view.probeStatus=snapshot.probeStatus; view.probeSamples=snapshot.probeSamples; view.probeLost=snapshot.probeLost;
    view.probeSent=snapshot.probeSent; view.probeExpected=snapshot.probeExpected;
	view.localSlot = snapshot.localSlot;
	view.invitation = snapshot.invitation;
	view.error = snapshot.helperError;
	view.settingsError = snapshot.settingsError;
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
    const auto* fighter = sf4e::selection::FindFighter(lobbyMenuCharaID);
    view.selectedFighter=lobbyMenuCharaID;
    view.selectionSummary = std::string(fighter ? fighter->name : "Choose fighter") + " / Costume " +
        std::to_string(lobbyConditions.costume + 1) + " / Color " + std::to_string(lobbyConditions.color + 1) +
        " / " + (lobbyConditions.ultraCombo == 2 ? "Ultra Double" : lobbyConditions.ultraCombo == 1 ? "Ultra II" : "Ultra I");
    if (snapshot.atMainMenu && !sf4e::selection::Available(sf4e::selection::FromNative(lobbyConditions),
        snapshot.lobbySettings.editionSelect, snapshot.fighterAvailability[lobbyMenuCharaID]))
        view.selectionError = "Selection unavailable under these rules. Open Fighter Select to choose an available option.";
    const auto status = sf4e::NetplayFacade::GetStatus();
    if (view.error.empty() && status.lastError[0]) view.error = status.lastError;
	bool open = true;
    shell.Draw(view, &open, [&](sf4e::ui::ShellAction action) {
		sf4e::NetplayFacade::RuntimeCommand request;
		request.command = std::move(action.command);
        request.service = action.service;
        request.inputAction = action.inputAction; request.discordAction = action.discordAction;
        request.discordRevision = action.discordRevision;
		request.displayName = snapshot.preferences.displayName;
		request.preferences = std::move(action.preferences);
		request.roomAction = std::move(action.roomAction);
        request.selectedDelay=action.selectedDelay;
		request.character = lobbyConditions;
		request.character.charaID = static_cast<BYTE>(lobbyMenuCharaID);
		request.stage = lobbyStageID;
		return sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(request));
	}, [&] {
		DrawNetworkCharaConfig(lobbyConditions, lobbyMenuCharaID, (snapshot.session.room == sf4e::netplay::RoomState::Idle || snapshot.localSlot == 0) ? &lobbyStageID : nullptr);
	}
#ifdef SF4E_DEVELOPER_UI
    , [] { sf4e::ui::DrawDeveloperOverlay(s_selectionArt.get()); }
#endif
    );
    if (!open) presentation.Close();
}


void Overlay::DrawOverlay() {

    if (!ImGui::GetCurrentContext()) return;
    if (s_selectionArt) s_selectionArt->Pump();
    if (sf4e::ui::ApplyTheme(ImGui_ImplWin32_GetDpiScaleForHwnd(s_overlayWindow) * sf4e::NetplayFacade::GetRuntimeSnapshot().preferences.interfaceScale)) ImGui_ImplDX9_InvalidateDeviceObjects();
    const auto snapshot = sf4e::NetplayFacade::GetRuntimeSnapshot();
    presentation.Update(snapshot.atMainMenu, snapshot.session.match, snapshot.offlineRequested, focused);
    if(mainRequested.exchange(false) && presentation.Available()) presentation.Open();
    static bool inviteShown=false;
    if (snapshot.discordPending && !inviteShown && snapshot.atMainMenu) { presentation.Open(); inviteShown=true; }
    if (!snapshot.discordPending) inviteShown=false;
    sf4e::ui::SetOverlayCursorOwnership(presentation.Visible());
    const bool assigning = snapshot.inputCapture != sf4e::input::Capture::Idle;
    // Player navigation is semantic, not ImGui spatial scoring. Text input is
    // still provided by the Win32 backend after explicit field activation.
    ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame();
    controllerNavigation.Update(snapshot.menuController, sf4e::input::ControllerMenuAvailable(snapshot.menuContext),
        presentation.Visible() || trainingOpen, focused && !assigning);
    if (controllerNavigation.OpenRequested() && presentation.Available()) presentation.Open();
    ImGui::NewFrame();
    sf4e::ui::SetMenuInput({controllerNavigation.Buttons(), ImGui::GetTime()});
    sf4e::ui::SetMenuGlyphs(snapshot.menuController.deviceType,snapshot.menuController.selectPhysical,snapshot.menuController.backPhysical);
    if (presentation.Reopened()) shell.ShowPlay();
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) presentation.Toggle();
    if (presentation.Visible()) DrawApplicationHome();
    if (!presentation.Visible() && assigning) {
        sf4e::NetplayFacade::RuntimeCommand cancel;
        cancel.command = {sf4e::netplay::CommandKind::HostRoom, snapshot.session.generation, {}};
        cancel.inputAction = sf4e::input::Action::Cancel;
        sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(cancel));
    }
    const auto training = sf4e::training::ReadView();
    trainingAvailable = training.available;
    if (!training.available || !focused) trainingOpen = false;
    if (focused && training.available && !presentation.Visible()) {
        sf4e::ui::SetMenuInput({0, ImGui::GetTime()});
        sf4e::ui::SetMenuGlyphs(1,0,0);
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) trainingOpen = !trainingOpen;
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) trainingHud = !trainingHud;
        auto practice = [&](sf4e::training::Action action) {
            sf4e::training::Submit({action, 0, training.generation});
        };
        if (!trainingOpen && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
                if(training.mode != sf4e::training::Mode::Recording && training.lengths[training.selected]>0) {
                    sf4e::ui::ShowTrainingRecordings(); trainingOpen=true;
                } else practice(training.mode == sf4e::training::Mode::Recording ? sf4e::training::Action::Stop : sf4e::training::Action::Record);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) practice(training.mode == sf4e::training::Mode::Playback ? sf4e::training::Action::Stop : sf4e::training::Action::Play);
        }
        if (trainingOpen) {
            sf4e::ui::DrawTrainingFlyout(training, sf4e::training::Submit);
            if(sf4e::ui::TakeMenuReturn()) trainingOpen=false;
        } else if (trainingHud) sf4e::ui::DrawTrainingHud(training);
    }
    const bool visible = focused && (presentation.Visible() || trainingOpen);
    sf4e::ui::SetOverlayCursorOwnership(visible);
    if (capture.exchange(visible) && !visible) { ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse(); }
    fMainMenu::bOverrideItemObserverState = (visible || controllerNavigation.MenuGuard()) ? rMainMenu::MMIOS_TRANSITION : -1;
    if (!visible && presentation.Available()) {
        const auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * .5f, vp->Pos.y + 12 * sf4e::ui::Scale()), ImGuiCond_Always, ImVec2(.5f, 0));
        ImGui::Begin("Ember shortcut", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextUnformatted("SF4 Ember Netplay  /  F10 or Start"); ImGui::End();
    }
    if(fSystem::ggpo)sf4e::ui::DrawControllerWarning(snapshot.gameplayInputError);
    if (fSystem::ggpo && snapshot.preferences.showMatchHud) {
        const auto status = sf4e::NetplayFacade::GetStatus();
        sf4e::ui::MatchStripView strip;
        for (int side = 0; side < 2; ++side) strip.names[side] = status.matchNames[side];
        strip.rollbackFrames = status.rollbackFrames;
        sf4e::ui::DrawMatchStrip(strip);
    }
    sf4e::OverlayPrefs::Data prefs = s_prefs;
    sf4e::OverlayPrefs::FromConfirmed(prefs.lobby, lobbyConditions); prefs.stageID = lobbyStageID;
    if (memcmp(&prefs, &s_prefs, sizeof(prefs)) != 0 && sf4e::OverlayPrefs::Save(prefs)) s_prefs = prefs;
    ImGui::Render(); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

}
void Overlay::FreeOverlay() {
    capture = false;
    trainingAvailable = false;
    fMainMenu::bOverrideItemObserverState = -1;
    if (!ImGui::GetCurrentContext()) return;
    controllerNavigation.Reset();
    sf4e::ui::SetMenuArt(nullptr);
    s_selectionArt.reset();
    ImGui_ImplDX9_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
}
LRESULT WINAPI Overlay::OverlayWindowFunc(HWND window, UINT message, WPARAM w, LPARAM l) {
    // Native display resets can pump activation messages after FreeOverlay and
    // before InitializeOverlay. Focus belongs to the window, not its ImGui
    // context: dropping reactivation here leaves F10/Start permanently gated.
    if (message == WM_ACTIVATEAPP) {
        focused = w != 0;
        if (!focused) {
            const auto training = sf4e::training::ReadView();
            sf4e::training::Submit({sf4e::training::Action::Stop, 0, training.generation});
            capture = false;
            if (ImGui::GetCurrentContext()) {
                sf4e::ui::SetOverlayCursorOwnership(false);
                ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse();
            }
        }
    }
    if (!ImGui::GetCurrentContext()) return 0;
    const auto handled = sf4e::ui::HandleOverlayMessage(window, message, w, l, capture, presentation.Available());
    if (trainingAvailable && w >= VK_F5 && w <= VK_F8 &&
        (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)) return 1;
    return handled;
}
