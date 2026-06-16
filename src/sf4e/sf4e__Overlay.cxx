#include <algorithm>
#include <memory>
#include <random>
#include <vector>
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <ggponet.h>

#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__Camera.hxx"
#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__Command.hxx"
#include "../Dimps/Dimps__Game__Battle__Effect.hxx"
#include "../Dimps/Dimps__Game__Battle__Hud.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Game__Battle__Training.hxx"
#include "../Dimps/Dimps__Game__Battle__Vfx.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"

#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionProtocol.hxx"

#include "sf4e.hxx"
#include "sf4e__Event.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__Hud.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__Game__Battle__Vfx.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "../common/sf4e__NetUtil.hxx"
#include "../common/sf4e__NetUtil.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__UserApp.hxx"


#define DEFAULT_ALPHA 0.87f

namespace rBattle = Dimps::Game::Battle;
namespace rVfx = Dimps::Game::Battle::Vfx;
namespace rStageSelect = Dimps::GameEvents::StageSelect;
namespace rHud = Dimps::Game::Battle::Hud;
namespace rPad = Dimps::Pad;
namespace fHud = sf4e::Game::Battle::Hud;
namespace Overlay = sf4e::Overlay;

using CameraUnit = Dimps::Game::Battle::Camera::Unit;
using CharaActor = Dimps::Game::Battle::Chara::Actor;
using CharaUnit = Dimps::Game::Battle::Chara::Unit;
using CommandUnit = Dimps::Game::Battle::Command::Unit;
using EffectUnit = Dimps::Game::Battle::Effect::Unit;
using HudUnit = Dimps::Game::Battle::Hud::Unit;
using TrainingManager = Dimps::Game::Battle::Training::Manager;
using SoundUnit = Dimps::Game::Battle::Sound::Unit;
using PadSystem = Dimps::Pad::System;
using VfxUnit = rVfx::Unit;

using Dimps::App;
using Dimps::Eva::Task;
using Dimps::Eva::TaskCore;
using Dimps::Eva::TaskCoreRegistry;
using Dimps::Event::EventBase;
using Dimps::Event::EventBaseWithEC;
using Dimps::Event::EventController;
using Dimps::Game::ProgressData;
using Dimps::Game::Request;
using Dimps::Game::Battle::Command::CommandImpl;
using Dimps::Game::Battle::GameManager;
using Dimps::Game::Battle::System;
using Dimps::Game::Battle::Vfx::ColorFade;
using Dimps::Game::Battle::Vfx::ColorFadeUnit;
using rSoundPlayerManager = Dimps::Game::Battle::Sound::SoundPlayerManager;
using Dimps::Game::GameMementoKey;
using rMainMenu = Dimps::GameEvents::MainMenu;
using Dimps::GameEvents::RootEvent;
using Dimps::GameEvents::VsCharaSelect;
using rVsMode = Dimps::GameEvents::VsMode;
using Dimps::GameEvents::VsStageSelect;
using Dimps::Math::FixedPoint;
using Dimps::Math::FixedToFloat;
using Dimps::Platform::dString;
using Dimps::Platform::GFxApp;
using Dimps::Platform::WithReleaser;

using fEventController = sf4e::Event::EventController;
using fIUnit = sf4e::Game::Battle::IUnit;
using fKey = sf4e::Game::GameMementoKey;
using fSoundPlayerManager = sf4e::Game::Battle::Sound::SoundPlayerManager;
using fSystem = sf4e::Game::Battle::System;
using fColorFade = sf4e::Game::Battle::Vfx::ColorFade;
using fMainMenu = sf4e::GameEvents::MainMenu;
using fVsBattle = sf4e::GameEvents::VsBattle;
using fVsPreBattle = sf4e::GameEvents::VsPreBattle;
using fVsStageSelect = sf4e::GameEvents::VsStageSelect;
using fPadSystem = sf4e::Pad::System;
using fUserApp = sf4e::UserApp;

using ImGui::Begin;
using ImGui::BeginMainMenuBar;
using ImGui::BeginMenu;
using ImGui::BeginTabBar;
using ImGui::BeginTabItem;
using ImGui::BeginTable;
using ImGui::Button;
using ImGui::CheckboxFlags;
using ImGui::Columns;
using ImGui::End;
using ImGui::EndFrame;
using ImGui::EndMainMenuBar;
using ImGui::EndTabBar;
using ImGui::EndTabItem;
using ImGui::EndTable;
using ImGui::MenuItem;
using ImGui::NewFrame;
using ImGui::NextColumn;
using ImGui::Separator;
using ImGui::Spacing;
using ImGui::TableHeadersRow;
using ImGui::TableNextColumn;
using ImGui::TableNextRow;
using ImGui::TableSetColumnIndex;
using ImGui::TableSetupColumn;
using ImGui::Text;
using ImGui::TextWrapped;

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool show_chara_window = false;
static bool show_command_window = false;
static bool show_demo_window = false;
static bool show_event_window = false;
static bool show_gfxapp_window = false;
static bool show_help_window = false;
static bool show_hud_window = false;
static bool show_log_window = false;
static bool show_main_menu_window = false;
static bool show_memento_window = false;
static bool show_network_window = false;
static bool s_playerNetplayUi = false;
static bool s_devNetplayOverlay = false;
static bool show_pad_window = false;
static bool show_sound_window = false;
static bool show_system_window = false;
static bool show_task_window = false;
static bool show_vfx_window = false;
static bool show_vsbattle_window = false;
static bool show_vscharaselect_window = false;
static bool show_vsstageselect_window = false;
static int nExtraFramesToSimulate = 1;

static bool mainMenuShouldJump = false;
static int mainMenuCharaIDs[2] = { 0 };
static Dimps::GameEvents::VsMode::ConfirmedCharaConditions mainMenuJumpCharaConditions[2] = { 0 };
static int mainMenuJumpCharaCount = 2;
static int mainMenuJumpStageID = 0;
static std::deque<std::string> clientAlerts;

// Yes, this is correct- SF4's menu uses the fractional section of the fixed
// point values 
static const std::pair<int, const char* const> roundCountList[4] = {
	{1, "1"},
	{3, "3"},
	{5, "5"},
	{7, "7"}
};

static const std::pair<FixedPoint, const char* const> roundTimeList[3] = {
	{{0, 30}, "30"},
	{{0, 60}, "60"},
	{{0, 99}, "99"},
};

const char* GetRoundCountLabel(void* options, int idx) {
	return ((std::pair<int, const char* const>*)options)[idx].second;
}

const char* GetRoundTimeLabel(void* options, int idx) {
	return ((std::pair<FixedPoint, const char* const>*)options)[idx].second;
}

void Overlay::PushNetplayAlert(const char* msg) {
	if (msg) {
		clientAlerts.push_back(msg);
		show_network_window = true;
	}
}

void Overlay::ConfigureNetplayUi(bool launcherDriven, bool devOverlay) {
	s_playerNetplayUi = launcherDriven;
	s_devNetplayOverlay = devOverlay;
	// Launcher host/join: show status immediately (session may start before Title -> MainMenu).
	show_network_window = devOverlay || launcherDriven;
}

void Overlay::SetNetplayLobbyVisible(bool visible) {
	if (visible && s_playerNetplayUi) {
		show_network_window = true;
	}
}

void Overlay::OnClientError(SessionClient::ErrorType errType, SessionClient* const client, const SessionClient::Callbacks& callbacks) {
	switch (errType) {
	case SessionClient::ErrorType::SCE_JOIN_REJECTED_HASH_INVALID:
		clientAlerts.push_back("Version mismatch - update sf4e to the same build on both PCs.");
		break;
	case SessionClient::ErrorType::SCE_JOIN_REJECTED_LOBBY_FULL:
		clientAlerts.push_back("Could not join lobby: lobby full");
		break;
	case SessionClient::ErrorType::SCE_JOIN_REJECTED_NAME_TAKEN:
		clientAlerts.push_back("Could not join lobby: name taken");
		break;
	case SessionClient::ErrorType::SCE_JOIN_REJECTED_REQUEST_INVALID:
		clientAlerts.push_back("Could not join lobby: join request incorrectly formatted- version mismatch?");
		break;
	default:
		clientAlerts.push_back("An unknown error occurred");
		break;
	}
	show_network_window = true;
}

enum NetworkWindowState {
	NWS_CAPTURE = 0,
	NWS_DECIDE = 1,
	NWS_HOST = 2,
	NWS_JOIN = 3,
};

char* GetEditionLabel(BYTE edition) {
	switch (edition) {
	case rBattle::ED_SF4:
		return "SF4";
	case rBattle::ED_SSF4:
		return "SSF4";
	case rBattle::ED_AE2011:
		return "AE2011";
	case rBattle::ED_AE2012:
		return "AE2012";
	case rBattle::ED_USF4:
		return "USF4";
	case rBattle::ED_OMEGA:
		return "OMEGA";
	default:
		return "UNKNOWN EDITION";
	}
}

void DrawCharaEditionDropdown(const char* label, int charaID, BYTE* selectedEdition) {
	int numItems = 0;
	// Note that selectedItem is initialized to zero even if the selectedEdition
	// is not valid for this character- in that condition, it effectively sets
	// the output edition to the oldest possible version for this character.
	int selectedItem = 0;
	int editions[7] = { -1 };
	char* items[7] = { 0 };
	for (int i = 0; i < NUM_VALID_EDITIONS; i++) {
		int edition = rBattle::orderedEditions[i];
		if (rBattle::validEditionsPerChara[charaID].valid[i]) {
			editions[numItems] = edition;
			items[numItems] = GetEditionLabel(edition);
			if (edition == *selectedEdition) {
				selectedItem = numItems;
			}
			numItems++;
		}
	}

	ImGui::Combo(label, &selectedItem, items, numItems);
	*selectedEdition = editions[selectedItem];
}

void DrawCharaWindow(bool* pOpen) {
	Begin(
		"Chara",
		pOpen,
		ImGuiWindowFlags_None
	);

	FixedPoint tmp, tmp2, tmp3;
	System* system = System::staticMethods.GetSingleton();

	int isFight = (system->*System::publicMethods.IsFight)();
	Text("Is fight: %d", isFight);
	if (isFight) {
		CharaUnit* lpCharaUnit = (system->*System::publicMethods.GetCharaUnit)();

		ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
		if (BeginTabBar("Battle Actor", tab_bar_flags))
		{
			for (int i = 0; i < 2; i++) {
				CharaActor* a = (lpCharaUnit->*CharaUnit::publicMethods.GetActorByIndex)(i);

				if (BeginTabItem(i == 0 ? "Actor 0" : "Actor 1")) {
					Columns(2, NULL, false);
					CharaActor::__publicMethods& methods = CharaActor::publicMethods;

					int actorID = (int)(a->*methods.GetActorID)();
					char* actorCode = actorID > -1 ? Dimps::characterCodes[actorID] : "";
					char* actorName = actorID > -1 ? Dimps::characterNames[actorID] : "";

					Text("Actor ID:"); NextColumn();
					Text("%d", actorID); NextColumn();

					Text("Actor code:"); NextColumn();
					Text(actorCode); NextColumn();

					Text("Actor name:"); NextColumn();
					Text(actorName); NextColumn();

					Text("Status:"); NextColumn();
					Text("%d", (a->*methods.GetStatus)()); NextColumn();

					Text("Root position:"); NextColumn();
					float* position = (a->*methods.GetCurrentRootPosition)();
					Text("%f %f %f %f", position[0], position[1], position[2], position[3]); NextColumn();

					Text("Current side:"); NextColumn();
					Text("%d", (a->*methods.GetCurrentSide)()); NextColumn();

					(a->*methods.GetVitalityAmt_FixedPoint)(&tmp);
					(a->*methods.GetVitalityMax_FixedPoint)(&tmp2);
					(a->*methods.GetVitalityPct_FixedPoint)(&tmp3);
					Text("Vitality:"); NextColumn();
					Text("%f / %f (%fa%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); NextColumn();

					(a->*methods.GetRevengeAmt_FixedPoint)(&tmp);
					(a->*methods.GetRevengeMax_FixedPoint)(&tmp2);
					(a->*methods.GetRevengePct_FixedPoint)(&tmp3);
					Text("Revenge:"); NextColumn();
					Text("%f / %f (%f%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); NextColumn();

					(a->*methods.GetRecoverableVitalityAmt_FixedPoint)(&tmp);
					(a->*methods.GetRecoverableVitalityMax_FixedPoint)(&tmp2);
					(a->*methods.GetRecoverableVitalityPct_FixedPoint)(&tmp3);
					Text("Recoverable Vitality:"); NextColumn();
					Text("%f / %f (%f%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); NextColumn();

					(a->*methods.GetSuperComboAmt_FixedPoint)(&tmp);
					(a->*methods.GetSuperComboMax_FixedPoint)(&tmp2);
					(a->*methods.GetSuperComboPct_FixedPoint)(&tmp3);
					Text("Super:"); NextColumn();
					Text("%f / %f (%f%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); NextColumn();

					(a->*methods.GetSCTimeAmt_FixedPoint)(&tmp);
					(a->*methods.GetSCTimeMax_FixedPoint)(&tmp2);
					(a->*methods.GetSCTimePct_FixedPoint)(&tmp3);
					Text("Super clock:"); NextColumn();
					Text("%f / %f (%f%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); NextColumn();

					(a->*methods.GetUCTimeAmt_FixedPoint)(&tmp);
					(a->*methods.GetUCTimeMax_FixedPoint)(&tmp2);
					(a->*methods.GetUCTimePct_FixedPoint)(&tmp3);
					Text("Ultra clock:"); NextColumn();
					Text("%f / %f (%f%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); NextColumn();

					(a->*methods.GetDamage)(&tmp);
					Text("Damage:"); NextColumn();
					Text("%f", FixedToFloat(&tmp)); NextColumn();

					(a->*methods.GetComboDamage)(&tmp);
					Text("Combo damage:"); NextColumn();
					Text("%f", FixedToFloat(&tmp)); NextColumn();

					(system->*System::publicMethods.GetUnitTimeScale_Fixed)(&tmp, i);
					Text("Timescale (local):"); NextColumn();
					Text("%f", FixedToFloat(&tmp)); NextColumn();

					Columns(1);

					if (Button("Force to origin")) {
						float* position = (a->*methods.GetCurrentRootPosition)();
						position[0] = 0.0;
					}
					EndTabItem();
				}
			}

			for (int i = 0; i < 2; i++) {
				CharaActor* a = (lpCharaUnit->*CharaUnit::publicMethods.GetActorByIndex)(i);
				CharaActor::__publicMethods& methods = CharaActor::publicMethods;

				if (BeginTabItem(i == 0 ? "Actor 0 bones" : "Actor 1 bones")) {
					Columns(2);
					for (int j = 0; j < CharaActor::BP_HEN_2 + 1; j++) {
						float* position = (a->*methods.GetCurrentBonePositionByID)(j);
						Text(CharaActor::staticMethods.GetBoneLabelByID(j)); NextColumn();
						Text("%f %f %f %f", position[0], position[1], position[2], position[3]); NextColumn();
					}
					Columns(1);
					EndTabItem();
				}
			}

			EndTabBar();
		}
	}

	End();
}

// Only 14 bytes are accounted for so far. Training mode memory and
// reload buttons don't seem to be in this specific buffer- which in
// a way makes sense, as this generally seems to be only "action"
// related buttons.
const char* GetButtonLabel(unsigned int bytePosition) {
	switch (bytePosition) {
	case 0: // 0x1
		return "Directions-Off";
	case 1: // 0x2
		return "Up";
	case 2: // 0x4
		return "Down";
	case 3: // 0x8
		return "Away";
	case 4: // 0x10
		return "Towards";
	case 5: // 0x20
		return "Buttons-Off";
	case 6: // 0x40
		return "Jab";
	case 7: // 0x80
		return "Strong";
	case 8: // 0x100
		return "Fierce";
	case 9: // 0x200
		return "Short";
	case 10: // 0x400
		return "Forward";
	case 11: // 0x800
		return "Roundhouse";
	case 12: // 0x1000
		return "Start";
	case 13: // 0x2000
		return "Select/Back";
	default:
		return "Unknown";
	}
}

void DrawCommandWindow(bool* pOpen) {
	Begin(
		"Command",
		pOpen,
		ImGuiWindowFlags_None
	);

	System* system = System::staticMethods.GetSingleton();
	int isFight = (system->*System::publicMethods.IsFight)();
	Text("Is fight: %d", isFight);
	Separator();

	if (isFight) {
		ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
		if (BeginTabBar("Input Type", tab_bar_flags))
		{
			if (BeginTabItem("On")) {
				Columns(3);
				Text("Button"); NextColumn(); Text("P1"); NextColumn(); Text("P2"); NextColumn();
				Separator();

				CommandUnit* u = (CommandUnit*)(system->*System::publicMethods.GetUnitByIndex)(System::U_COMMAND);
				DWORD padData[2];
				for (int i = 0; i < 2; i++) {
					CommandImpl* impl = (u->*CommandUnit::publicMethods.GetCommandImplForEntry)(i);
					padData[i] = (impl->*CommandImpl::publicMethods.GetCurrentOnSwitches)();
				}

				for (unsigned int bytePosition = 0; bytePosition < 14; bytePosition++) {
					Text("%s (B%d)", GetButtonLabel(bytePosition), bytePosition); NextColumn();
					for (int i = 0; i < 2; i++) {
						Text((padData[i] & (1 << bytePosition)) ? "ON" : "OFF"); NextColumn();
					}
					Separator();
				}

				Columns(1);
				EndTabItem();
			}

			if (BeginTabItem("Rising")) {
				Columns(3);
				Text("Button"); NextColumn(); Text("P1"); NextColumn(); Text("P2"); NextColumn();
				Separator();

				CommandUnit* u = (CommandUnit*)(system->*System::publicMethods.GetUnitByIndex)(System::U_COMMAND);
				DWORD padData[2];
				for (int i = 0; i < 2; i++) {
					CommandImpl* impl = (u->*CommandUnit::publicMethods.GetCommandImplForEntry)(i);
					padData[i] = (impl->*CommandImpl::publicMethods.GetCurrentRisingSwitches)();
				}

				for (unsigned int bytePosition = 0; bytePosition < 14; bytePosition++) {
					Text("%s (B%d)", GetButtonLabel(bytePosition), bytePosition); NextColumn();
					for (int i = 0; i < 2; i++) {
						Text((padData[i] & (1 << bytePosition)) ? "ON" : "OFF"); NextColumn();
					}
					Separator();
				}

				Columns(1);
				EndTabItem();
			}

			if (BeginTabItem("Falling")) {
				Columns(3);
				Text("Button"); NextColumn(); Text("P1"); NextColumn(); Text("P2"); NextColumn();
				Separator();

				CommandUnit* u = (CommandUnit*)(system->*System::publicMethods.GetUnitByIndex)(System::U_COMMAND);
				DWORD padData[2];
				for (int i = 0; i < 2; i++) {
					CommandImpl* impl = (u->*CommandUnit::publicMethods.GetCommandImplForEntry)(i);
					padData[i] = (impl->*CommandImpl::publicMethods.GetCurrentFallingSwitches)();
				}

				for (unsigned int bytePosition = 0; bytePosition < 14; bytePosition++) {
					Text("%s (B%d)", GetButtonLabel(bytePosition), bytePosition); NextColumn();
					for (int i = 0; i < 2; i++) {
						Text((padData[i] & (1 << bytePosition)) ? "ON" : "OFF"); NextColumn();
					}
					Separator();
				}

				Columns(1);
				EndTabItem();
			}

			EndTabBar();
		}
	}

	End();
}

void DrawEventWindow(bool* pOpen) {
	Begin(
		"Event",
		pOpen,
		ImGuiWindowFlags_None
	);

	Text("Halt after next: %s", fEventController::bHaltAfterNext ? "true" : "false");
	Text("Update allowed: %s", fEventController::bUpdateAllowed ? "true" : "false");
	if (Button("Pause")) {
		fEventController::bUpdateAllowed = false;
	}
	if (Button("Play")) {
		fEventController::bUpdateAllowed = true;
	}
	if (Button("Halt after next")) {
		fEventController::bHaltAfterNext = true;
	}
	if (Button("Step")) {
		fEventController::bUpdateAllowed = true;
		fEventController::bHaltAfterNext = true;
	}

	End();
}

void _OnPreBattleTasksRegistered() {
	// XXX (adanducci): this is a little fragile- it's technically possible
	// that the pre-battle event is constructed in another context, but
	// practically speaking the VsPreBattle event will always be used in
	// the context of VsMode.
	char* vsModeQuery[] = { "VSMode" };
	rVsMode* mode = (rVsMode*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), vsModeQuery, 1);
	if (!mode) {
		spdlog::error("Overlay: VsPreBattle tasks registered, but the current foreground event isn't VSMode!");
		return;
	}

	Dimps::Platform::dString* stageName = rVsMode::GetStageName(mode);
	rVsMode::ConfirmedPlayerConditions* conditions = rVsMode::GetConfirmedPlayerConditions(mode);
	size_t charaConditionSize = sizeof(rVsMode::ConfirmedCharaConditions);
	for (int i = 0; i < mainMenuJumpCharaCount; i++) {
		*(rVsMode::ConfirmedPlayerConditions::GetCharaID(&conditions[i])) = mainMenuJumpCharaConditions->charaID;
		*(rVsMode::ConfirmedPlayerConditions::GetSideActive(&conditions[i])) = 1;
		rVsMode::ConfirmedCharaConditions* charaConditions = rVsMode::ConfirmedPlayerConditions::GetCharaConditions(&conditions[i]);
		memcpy_s(charaConditions, charaConditionSize, &mainMenuJumpCharaConditions[i], charaConditionSize);
	}

	(stageName->*Dimps::Platform::dString::publicMethods.assign)(Dimps::stageCodes[mainMenuJumpStageID], 4);
	*(rVsMode::GetStageCode(mode)) = mainMenuJumpStageID;

	// Force some default input handling
	PadSystem* padSys = PadSystem::staticMethods.GetSingleton();
	PadSystem::__publicMethods& padSysMethods = Dimps::Pad::System::publicMethods;
	(padSys->*padSysMethods.AssociatePlayerAndGamepad)(0, 0);
	(padSys->*padSysMethods.SetDeviceTypeForPlayer)(0, 1);
	(padSys->*padSysMethods.SetSideHasAssignedController)(0, 1);
	(padSys->*padSysMethods.AssociatePlayerAndGamepad)(1, 1);
	(padSys->*padSysMethods.SetDeviceTypeForPlayer)(1, 1);
	(padSys->*padSysMethods.SetSideHasAssignedController)(1, 1);
	(padSys->*padSysMethods.SetActiveButtonMapping)(PadSystem::BUTTON_MAPPING_FIGHT);
}

void DrawGFxAppWindow(bool* pOpen) {
	static int selectedAction = 0;
	static int selectedNode = 0;

	Begin(
		"GFxApp",
		pOpen,
		ImGuiWindowFlags_None
	);

	GFxApp* app = GFxApp::staticMethods.GetSingleton();

	if (BeginTabBar("GFxApp"))
	{
		if (BeginTabItem("Actions")) {
			ImGui::BeginChild("left pane", ImVec2(150, 0), true);
			char label[24];
			GFxApp::ObjectPool<Dimps::Eva::IEmSpriteAction>* pool = GFxApp::GetActionPool(app);

			for (int i = 0; i < NUM_GFX_ACTIONS; i++)
			{
				sprintf(label, "Action %d (%d)", i, pool->useIndex[i]);
				if (ImGui::Selectable(label, selectedAction == i)) {
					selectedAction = i;
				}
			}
			ImGui::EndChild();
			ImGui::SameLine();

			ImGui::BeginChild("right pane");
			Dimps::Eva::IEmSpriteAction* a = &GFxApp::GetActionPool(app)->raw[selectedAction];
			Text("Action %d", selectedAction);
			Text("Enabled: %d", pool->useIndex[selectedAction]);
			if (pool->useIndex[selectedAction]) {
				for (int j = 0; j < NUM_ACTION_STATES; j++) {
					Dimps::Eva::IEmSpriteAction::ActionState* state = (a->*Dimps::Eva::IEmSpriteAction::publicMethods.GetActionState)(j);
					Text("State %d: current frame %d / %d , active %d", j, state->currentFrame.integral, state->currentFrame.fractional, state->active_0x1c);
				}
			}
			ImGui::EndChild();

			EndTabItem();
		}
		EndTabBar();
	}

	End();
}

void DrawGGPOStatsOverlay(GGPOSession* ggpo, fSystem::PlayerConnectionInfo* players) {
	GGPONetworkStats stats;
	int i;

	ImGuiIO& io = ImGui::GetIO();
	ImVec2 size(500, 50);
	ImVec2 window_pos(
		(io.DisplaySize.x - size.x) / 2,
		io.DisplaySize.y - size.y - 20
	);

	ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.35f);
	Begin(
		"GGPO",
		nullptr,
		ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoInputs
	);
 
	for (i = 0; i < MAX_SF4E_PROTOCOL_USERS; i++) {
		if (players[i].type == GGPO_PLAYERTYPE_REMOTE) {
			break;
		}
	}
	if (i == MAX_SF4E_PROTOCOL_USERS) {
		// No remote player
		Columns(2);
		Text("len(PndSnaps)"); NextColumn();
		Text("len(snapMap)"); NextColumn();
		if (fUserApp::netplay) {

			Text("%d", fUserApp::netplay->client.pendingRemoteSnapshots.size()); NextColumn();
		}
		else {
			Text("N/A"); NextColumn();
		}
		Text("%d", fSystem::snapshotMap.size());  NextColumn();
		Columns(1);
		return;
	}
	else {
		GGPOErrorCode err = ggpo_get_network_stats(ggpo, players[i].handle, &stats);
		if (!GGPO_SUCCEEDED(err)) {
			spdlog::warn("Couldn't get GGPO stats for overlay: {}", (int)err);
			return;
		}

		Columns(8);
		Text("RTT ms"); NextColumn();
		Text("kbps"); NextColumn();
		Text("recv"); NextColumn();
		Text("send"); NextColumn();
		Text("LFB"); NextColumn();
		Text("RFB"); NextColumn();
		Text("len(PndSnaps)"); NextColumn();
		Text("len(snapMap)"); NextColumn();

		Text("%d", stats.network.ping); NextColumn();
		Text("%d", stats.network.kbps_sent);  NextColumn();
		Text("%d", stats.network.recv_queue_len);  NextColumn();
		Text("%d", stats.network.send_queue_len);  NextColumn();
		Text("%d", stats.timesync.local_frames_behind);  NextColumn();
		Text("%d", stats.timesync.remote_frames_behind);  NextColumn();
		if (fUserApp::netplay) {

			Text("%d", fUserApp::netplay->client.pendingRemoteSnapshots.size()); NextColumn();
		}
		else {
			Text("N/A"); NextColumn();
		}
		Text("%d", fSystem::snapshotMap.size());  NextColumn();
		Columns(1);
	}

	End();
}

void DrawHashOverlay() {
	ImGuiIO& io = ImGui::GetIO();
	ImGuiStyle& style = ImGui::GetStyle();
	ImVec2 textWidth = ImGui::CalcTextSize(sf4e::sidecarHash.c_str());
	ImVec2 window_pos(
		(io.DisplaySize.x - textWidth.x) / 2 - style.WindowPadding.x,
		io.DisplaySize.y - (textWidth.y + style.WindowPadding.y * 2)
	);
	ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.35f);
	Begin(
		"Sidecar Hash",
		nullptr,
		ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoInputs |
		ImGuiWindowFlags_AlwaysAutoResize
	);
	Text("%s", sf4e::sidecarHash.c_str());
	End();
}

void DrawMainMenuWindow(bool* pOpen) {
	static BYTE skipStep = 1;
	static bool bEditionSelect = true;
	static int roundCountIdx = 1;
	static int roundTimeIdx = 2;

	Begin(
		"MainMenu",
		pOpen,
		ImGuiWindowFlags_None
	);

	char* mainMenuQuery[1] = { "MainMenu" };
	rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(
		App::GetRootEvent(),
		mainMenuQuery,
		1
	);

	if (!mainMenu) {
		Text("Main menu not running");
		End();
		return;
	}

	Text("Instance: %p", mainMenu);
	Text("Name: %s", EventBase::GetName(mainMenu));
	ImGui::Checkbox("VS mode: Skip chara/stage select?", &mainMenuShouldJump);
	if (mainMenuShouldJump) {
		ImGui::Combo("Round count", &roundCountIdx, GetRoundCountLabel, (void*)roundCountList, 4);
		ImGui::Combo("Round time", &roundTimeIdx, GetRoundTimeLabel, (void*)roundTimeList, 3);
		ImGui::Checkbox("Edition select", &bEditionSelect);
		ImGui::Combo("Stage", &mainMenuJumpStageID, Dimps::stageNames, 30);
		if (BeginTable("Main Menu", 3)) {
			TableSetupColumn("Property");
			TableSetupColumn("P1");
			TableSetupColumn("P2");
			TableHeadersRow();
			TableNextRow();
			TableSetColumnIndex(0);

			Text("Chara"); TableNextColumn();
			ImGui::Combo("##P1 Character", (int*)&mainMenuCharaIDs[0], Dimps::characterNames, 0x2c); TableNextColumn();
			ImGui::Combo("##P2 Character", (int*)&mainMenuCharaIDs[1], Dimps::characterNames, 0x2c); TableNextColumn();
			mainMenuJumpCharaConditions[0].charaID = mainMenuCharaIDs[0];
			mainMenuJumpCharaConditions[1].charaID = mainMenuCharaIDs[1];

			Text("Color"); TableNextColumn();
			ImGui::InputScalar("##P1 Color", ImGuiDataType_U8, &mainMenuJumpCharaConditions[0].color, &skipStep); TableNextColumn();
			ImGui::InputScalar("##P2 Color", ImGuiDataType_U8, &mainMenuJumpCharaConditions[1].color, &skipStep); TableNextColumn();

			Text("Costume"); TableNextColumn();
			ImGui::InputScalar("##P1 Costume", ImGuiDataType_U8, &mainMenuJumpCharaConditions[0].costume, &skipStep); TableNextColumn();
			ImGui::InputScalar("##P2 Costume", ImGuiDataType_U8, &mainMenuJumpCharaConditions[1].costume, &skipStep); TableNextColumn();

			Text("Handicap"); TableNextColumn();
			// 1: 1 hit, 2: 25%, 3: 50%, 4: 75%
			ImGui::InputScalar("##P1 Handicap", ImGuiDataType_U8, &mainMenuJumpCharaConditions[0].handicap, &skipStep); TableNextColumn();
			ImGui::InputScalar("##P2 Handicap", ImGuiDataType_U8, &mainMenuJumpCharaConditions[1].handicap, &skipStep); TableNextColumn();

			Text("Personal action"); TableNextColumn();
			ImGui::InputScalar("##P1 Personal action", ImGuiDataType_U8, &mainMenuJumpCharaConditions[0].personalAction, &skipStep); TableNextColumn();
			ImGui::InputScalar("##P2 Personal action", ImGuiDataType_U8, &mainMenuJumpCharaConditions[1].personalAction, &skipStep); TableNextColumn();

			Text("Ultra Combo"); TableNextColumn();
			ImGui::InputScalar("##P1 Ultra Combo", ImGuiDataType_U8, &mainMenuJumpCharaConditions[0].ultraCombo, &skipStep); TableNextColumn();
			ImGui::InputScalar("##P2 Ultra Combo", ImGuiDataType_U8, &mainMenuJumpCharaConditions[1].ultraCombo, &skipStep); TableNextColumn();

			Text("Edition"); TableNextColumn();
			DrawCharaEditionDropdown("##P1 Edition", mainMenuJumpCharaConditions[0].charaID, &mainMenuJumpCharaConditions[0].unc_edition); TableNextColumn();
			DrawCharaEditionDropdown("##P2 Edition", mainMenuJumpCharaConditions[1].charaID, &mainMenuJumpCharaConditions[1].unc_edition); TableNextColumn();

			Text("Win quote"); TableNextColumn();
			ImGui::InputScalar("##P1 Win quote", ImGuiDataType_U8, &mainMenuJumpCharaConditions[0].winQuote, &skipStep); TableNextColumn();
			ImGui::InputScalar("##P2 Win quote", ImGuiDataType_U8, &mainMenuJumpCharaConditions[1].winQuote, &skipStep); TableNextColumn();

			EndTable();
		}
	}
	if (Button("Go to versus mode")) {
		if (mainMenuShouldJump) {
			RootEvent* root = App::GetRootEvent();
			ProgressData* progressData = *RootEvent::GetProgressData(root);
			ProgressData::BattleTypeSettings* BattleTypeSettings = &(ProgressData::GetBattleTypeSettings(progressData)[ProgressData::NBT_PVP]);
			*ProgressData::GetNextBattleType(progressData) = ProgressData::NBT_PVP;

			BattleTypeSettings->editionSelect = bEditionSelect;
			BattleTypeSettings->rounds = roundCountList[roundCountIdx].first;
			BattleTypeSettings->timeLimit = roundTimeList[roundTimeIdx].first;
			fVsPreBattle::bSkipToVersus = true;
			fVsPreBattle::OnTasksRegistered = _OnPreBattleTasksRegistered;

			// Hack to configure inputs
			PadSystem* padSys = Dimps::Pad::System::staticMethods.GetSingleton();
			PadSystem::__publicMethods& padSysMethods = Dimps::Pad::System::publicMethods;
			(padSys->*padSysMethods.AssociatePlayerAndGamepad)(0, 0);
			(padSys->*padSysMethods.SetDeviceTypeForPlayer)(0, 1);
			(padSys->*padSysMethods.SetSideHasAssignedController)(0, 1);
			(padSys->*padSysMethods.AssociatePlayerAndGamepad)(1, 1);
			(padSys->*padSysMethods.SetDeviceTypeForPlayer)(1, 1);
			(padSys->*padSysMethods.SetSideHasAssignedController)(1, 1);
			(padSys->*padSysMethods.SetActiveButtonMapping)(PadSystem::BUTTON_MAPPING_FIGHT);
		}

		char* mainMenuQuery[1] = { "MainMenu" };
		rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(
			App::GetRootEvent(),
			mainMenuQuery,
			1
		);
		(rMainMenu::ToItemObserver(mainMenu)->*rMainMenu::itemObserverMethods.GoToVersusMode)();
	}

	End();
}

void DrawNetworkCharaConfig(rVsMode::ConfirmedCharaConditions& charaConditions) {
	static int menuCharaID = 0;
	static const int stepSize = 1;
	ImGui::Combo("Chara", &menuCharaID, Dimps::characterNames, 0x2c);
	charaConditions.charaID = menuCharaID;
	ImGui::InputScalar("Color", ImGuiDataType_U8, &charaConditions.color, &stepSize);
	ImGui::InputScalar("Costume", ImGuiDataType_U8, &charaConditions.costume, &stepSize);
	ImGui::InputScalar("Handicap", ImGuiDataType_U8, &charaConditions.handicap, &stepSize);
	ImGui::InputScalar("Personal action", ImGuiDataType_U8, &charaConditions.personalAction, &stepSize);
	ImGui::InputScalar("Ultra Combo", ImGuiDataType_U8, &charaConditions.ultraCombo, &stepSize);
	DrawCharaEditionDropdown("Edition", charaConditions.charaID, &charaConditions.unc_edition);
	ImGui::InputScalar("Win quote", ImGuiDataType_U8, &charaConditions.winQuote, &stepSize);
}

void DrawNetworkJoinPanel(uint8_t deviceIdx, uint8_t deviceType) {
	static uint8_t delay = 1;
	static uint16 port = 23456;
	static char name[32] = { 0 };
	static char joinAddr[64] = { 0 };

	char* mainMenuQuery[1] = { "MainMenu" };
	rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), mainMenuQuery, 1);
	if (!mainMenu) {
		Text("Must be on the main menu to connect");
		return;
	}
	Text("Join a game");
	Separator();
	ImGui::InputText("Session server", joinAddr, 64);
	ImGui::InputText("Name", name, 32);
	ImGui::InputScalar("Local GGPO port", ImGuiDataType_U16, &port);
	ImGui::InputScalar("Delay", ImGuiDataType_U8, &delay);

	if (Button("Join")) {
		fUserApp::StartSession(joinAddr, port, sf4e::sidecarHash, std::string(name), deviceType, deviceIdx, delay);
	}
}

void DrawNetworkHostPanel(uint8_t deviceIdx, uint8_t deviceType) {
	static uint8_t delay = 1;
	static char name[32] = { 0 };
	static uint16 hostPort = 23456;
	static uint16 ggpoPort = 23457;
	static int roundCountIdx = 1;
	static int roundTimeIdx = 2;
	static bool bEditionSelect = true;

	char* mainMenuQuery[1] = { "MainMenu" };
	rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), mainMenuQuery, 1);
	if (!mainMenu) {
		Text("Must be on the main menu to listen");
		return;
	}

	Text("Host a game");
	Separator();
	ImGui::InputScalar("Delay", ImGuiDataType_U8, &delay);
	ImGui::InputScalar("Session host port", ImGuiDataType_U16, &hostPort);
	ImGui::InputScalar("GGPO port", ImGuiDataType_U16, &ggpoPort);
	ImGui::InputText("Name", name, 32);
	Separator();
	ImGui::Combo("Round cound", &roundCountIdx, GetRoundCountLabel, (void*)roundCountList, 4);
	ImGui::Combo("Round time", &roundTimeIdx, GetRoundTimeLabel, (void*)roundTimeList, 3);
	ImGui::Checkbox("Edition select", &bEditionSelect);

	bool valid = true;
	if (hostPort == ggpoPort) {
		Text("Session host port and GGPO port cannot be the same");
		valid = false;
	}

	ImGui::BeginDisabled(!valid);
	if (Button("Start hosting")) {
		char hostAddr[64];
		char lanIp[64] = { 0 };
		sf4e::DetectLanIPv4(lanIp, sizeof(lanIp));
		snprintf(hostAddr, sizeof(hostAddr), "%s:%d", lanIp, hostPort);
		if (!fUserApp::StartServer(hostPort, std::string("localhost"), sf4e::sidecarHash, bEditionSelect, roundCountList[roundCountIdx].first, roundTimeList[roundTimeIdx].first)) {
			Overlay::PushNetplayAlert("Could not start session server (port in use?).");
		}
		else {
			fUserApp::StartSession(hostAddr, ggpoPort, sf4e::sidecarHash, std::string(name), deviceType, deviceIdx, delay);
		}
	}
	ImGui::EndDisabled();
}

void DrawNetworkLobbyPanel() {
	static int stageID = 0;
	static rVsMode::ConfirmedCharaConditions myConditions = { 0, 0, 0, 0, 0, 0, 0, 0, rBattle::ED_USF4};

	if (!fUserApp::netplay) {
		Text("Netplay not active");
		return;
	}

	int isSelfActiveSide = -1;
	sf4e::SessionClient& client = fUserApp::netplay->client;
	if (s_devNetplayOverlay) {
		ImGui::Checkbox("Verbose logging?", &sf4e::SessionClient::bVerboseLogging);
		Separator();
		Text("LID: %s@%s", client._lobbyData.id.host.c_str(), client._lobbyData.id.key.c_str());
		Text("CID: %s@%s", client._cid.user.c_str(), client._cid.host.c_str());
	}

	// List the members
	Text("Round count: %d", client._lobbyData.roundCount);
	Text("Round time: %d", client._lobbyData.roundTime.integral);
	Text("Edition select: %d", client._lobbyData.editionSelect);
	std::vector<sf4e::SessionProtocol::MemberData>& members = client._lobbyData.members;
	for (int i = 0; i < 2 && i < members.size(); i++) {
		const char* label = i == 0 ? "P1" : "P2";
		const char* isMe = client._lobbyData.members[i].connId == client._cid ? " [Me] " : " ";
		Text(
			"%s: %s%s(%s)",
			label,
			members[i].name.c_str(),
			isMe,
			client._matchData.readyMessageNum[i] > -1 ? "Ready!" : "Waiting"
		);
		if (members[i].name == fUserApp::netplay->client._name) {
			isSelfActiveSide = i;
		}
	}
	Separator();
	Text("Queue:");
	if (members.size() > 2) {
		for (int i = 2; i < members.size(); i++) {
			const char* isMe = client._lobbyData.members[i].connId == client._cid ? " [Me] " : " ";
			Text(
				"%s%s",
				members[i].name.c_str(),
				isMe
			);
		}
	}
	else {
		Text("(No one in queue)");
	}
	
	Separator();
	// Draw the config if the player's active
	if (isSelfActiveSide > -1) {
		if (fUserApp::netplay->client._outstandingReadyRequestNumber > -1) {
			Text("Waiting for response...");
		}
		else if (fUserApp::netplay->client._matchData.readyMessageNum[isSelfActiveSide] > -1) {
			Text("Ready!");
		}
		else {
			DrawNetworkCharaConfig(myConditions);
			if (isSelfActiveSide == 0) {
				ImGui::Combo("Stage", &stageID, Dimps::stageNames, 30);
			}
			const char* readyLabel = s_devNetplayOverlay ? "Send chara / Ready" : "Ready";
			if (Button(readyLabel)) {
				if (fUserApp::netplay->client.PreBattle_SetChara(myConditions) != k_EResultOK) {
					Overlay::PushNetplayAlert("Could not send character selection.");
				}
				if (isSelfActiveSide == 0) {
					if (fUserApp::netplay->client.PreBattle_SetEnv(sf4e::localRand()) != k_EResultOK) {
						Overlay::PushNetplayAlert("Could not send match environment.");
					}
					if (fUserApp::netplay->client.PreBattle_SetStage(stageID) != k_EResultOK) {
						Overlay::PushNetplayAlert("Could not send stage.");
					}
				}
				fUserApp::netplay->client.Lobby_Ready();
			}
		}

		if (s_devNetplayOverlay && Button("Report win")) {
			int loser = (isSelfActiveSide == 0) ? 1 : 0;
			if (fUserApp::netplay->client.Lobby_ReportResults(loser) != k_EResultOK) {
				Overlay::PushNetplayAlert("Could not report match results.");
			}
		}
	}
	else {
		Text("Waiting for turn...");
	}
}

static void DrawNetplayPlayerPanel() {
	static DWORD s_connectStartedMs = 0;
	sf4e::NetplayStatus st = sf4e::NetplayFacade::GetStatus();
	Text("Netplay status");
	Spacing();
	Separator();
	Spacing();
	if (clientAlerts.size() > 0) {
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 160, 160, 255));
		TextWrapped("%s", clientAlerts.at(0).c_str());
		ImGui::PopStyleColor();
	}
	if (sf4e::NetplayFacade::IsAutoNetplayPending()) {
		if (sf4e::NetplayFacade::IsPadCapturePending()) {
			TextWrapped("Press Start or LK on your controller to bind it.");
		}
		else if (!sf4e::NetplayFacade::IsPadCapturePhaseReady()) {
			TextWrapped("Waiting for controller system...");
		}
		else {
			TextWrapped("Starting netplay...");
		}
		return;
	}
	if (!st.active) {
		s_connectStartedMs = 0;
		TextWrapped("Not connected.");
		return;
	}
	const sf4e::NetplayConfig& cfg = sf4e::NetplayFacade::GetConfig();
	const bool isJoin = cfg.mode == (int)sf4e::NetplayMode::Join;
	if (!st.connected) {
		if (s_connectStartedMs == 0) {
			s_connectStartedMs = GetTickCount();
		}
		const DWORD connectElapsedMs = GetTickCount() - s_connectStartedMs;
		if (isJoin) {
			if (connectElapsedMs > 10000) {
				if (cfg.useCentralSession == 2) {
					TextWrapped(
						"Still connecting to the game room. Ask the host to Start game first, then try again."
					);
				}
				else {
					TextWrapped(
						"Still connecting... ask host to Start game first and forward TCP+UDP %u on their router.",
						cfg.sessionPort
					);
				}
			}
			else if (cfg.useCentralSession == 2) {
				TextWrapped("Connecting to game room...");
			}
			else {
				TextWrapped("Connecting to host...");
			}
		}
		else if (connectElapsedMs > 10000) {
			if (cfg.useCentralSession == 2) {
				TextWrapped(
					"Still connecting to the game room server. Check your internet, then create a new room and try again."
				);
			}
			else {
				TextWrapped("Still connecting... check RelayHost is running and port forward for joiners.");
			}
		}
		else if (cfg.useCentralSession == 2) {
			TextWrapped("Connecting to game room server...");
		}
		else {
			TextWrapped("Connecting to session server...");
		}
	}
	else {
		s_connectStartedMs = 0;
		TextWrapped("Connected: yes");
	}
	if (isJoin) {
		if (cfg.useCentralSession == 2 && cfg.relayRoomCode[0]) {
			TextWrapped("Join target: %s", cfg.relayRoomCode);
			if (st.connected) {
				TextWrapped("Joined room: %s", cfg.relayRoomCode);
			}
			if (cfg.devOverlay) {
				TextWrapped("Endpoint: %s:%u", cfg.sessionHost, cfg.sessionPort);
			}
		}
		else {
			TextWrapped("Join target: %s:%u", cfg.sessionHost, cfg.sessionPort);
		}
	}
	else if (cfg.mode == (int)sf4e::NetplayMode::Host) {
		if (cfg.useCentralSession == 2) {
			if (cfg.relayRoomCode[0]) {
				TextWrapped("Room code: %s - share with joiner", cfg.relayRoomCode);
			}
			else {
				TextWrapped("Share your relay code from the launcher with joiners.");
			}
			if (cfg.devOverlay) {
				TextWrapped("Endpoint: %s:%u", cfg.sessionHost, cfg.sessionPort);
			}
		}
		else {
			TextWrapped("Session port: %u (share your public IP:port with joiners)", cfg.sessionPort);
		}
	}
	if (st.opponentName[0]) {
		TextWrapped("Opponent: %s", st.opponentName);
	}
	TextWrapped("Input delay: %d frames", st.inputDelay);
	if (st.pingMs >= 0) {
		TextWrapped("Ping: %d ms", st.pingMs);
	}
	if (cfg.useCentralSession == 2) {
		const sf4e::GgpoTransportStatus ggpoPath = sf4e::NetplayFacade::GetGgpoTransportStatus();
		if (ggpoPath.legacyTunnelActive || ggpoPath.effectiveMode != 0 || st.inMatch) {
			if (ggpoPath.legacyTunnelActive) {
				ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "GGPO path: Legacy (session tunnel)");
			}
			else if (ggpoPath.effectiveMode == 1) {
				ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "GGPO path: UDP relay");
				if (ggpoPath.remoteHost[0]) {
					TextWrapped("  via %s:%u", ggpoPath.remoteHost, ggpoPath.remotePort);
				}
			}
			else if (ggpoPath.effectiveMode == 2) {
				ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "GGPO path: P2P direct");
				if (ggpoPath.remoteHost[0]) {
					TextWrapped("  peer %s:%u", ggpoPath.remoteHost, ggpoPath.remotePort);
				}
			}
		}
		else if (cfg.ggpoTransport != 0) {
			TextWrapped(
				"GGPO plan: %s",
				sf4e::NetplayFacade::GgpoTransportModeName(cfg.ggpoTransport)
			);
			if (cfg.ggpoRemoteHost[0] && cfg.ggpoRemotePort > 0) {
				TextWrapped("  target %s:%u", cfg.ggpoRemoteHost, cfg.ggpoRemotePort);
			}
		}
	}
	if (cfg.devOverlay) {
		sf4e::TransportDiagnostics td = sf4e::NetplayFacade::GetTransportDiagnostics();
		const char* transportLabel = "legacy";
		if (td.ggpoTransport == 1) {
			transportLabel = "udp_relay";
		}
		else if (td.ggpoTransport == 2) {
			transportLabel = "p2p";
		}
		TextWrapped("Transport: %s", transportLabel);
		if (cfg.ggpoRemoteHost[0]) {
			TextWrapped("GGPO remote: %s:%u", cfg.ggpoRemoteHost, cfg.ggpoRemotePort);
		}
		if (td.tunnelSendCount || td.relayOutboundFrames) {
			TextWrapped(
				"Tunnel send: %llu (%llu B)  Relay out: %llu (%llu B)",
				(unsigned long long)td.tunnelSendCount,
				(unsigned long long)td.tunnelSendBytes,
				(unsigned long long)td.relayOutboundFrames,
				(unsigned long long)td.relayOutboundBytes
			);
		}
	}
	if (st.inLobby) {
		TextWrapped("Lobby - configure character and press Ready below.");
		DrawNetworkLobbyPanel();
	}
	else if (st.inMatch) {
		const sf4e::GgpoSyncPhase syncPhase = sf4e::NetplayFacade::GetGgpoSyncPhase();
		if (syncPhase == sf4e::GgpoSyncPhase::Running) {
			TextWrapped("In match");
		}
		else if (syncPhase == sf4e::GgpoSyncPhase::Synchronizing) {
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "GGPO: Synchronizing...");
		}
		else if (syncPhase == sf4e::GgpoSyncPhase::Connected) {
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "GGPO: Connected, waiting to run...");
		}
		else {
			ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "GGPO: Waiting for sync...");
		}
	}
	Spacing();
	Separator();
	Spacing();
	if (Button("Disconnect")) {
		fUserApp::ShutdownNetplay(true);
	}
}

void DrawNetworkWindow(bool* pOpen) {
	static bool bDebug = false;
	static NetworkWindowState netState = NWS_CAPTURE;
	static uint8_t deviceIdx = 0xff;
	static uint8_t deviceType = 0xff;

	Begin(
		"Network",
		pOpen,
		ImGuiWindowFlags_None
	);
	Text("Steam multiplayer is disabled while sf4e is running.");
	Separator();

	if (s_playerNetplayUi && !s_devNetplayOverlay) {
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
		DrawNetplayPlayerPanel();
		ImGui::PopStyleVar();
		End();
		return;
	}

	if (s_devNetplayOverlay) {
		ImGui::InputInt("Randomize inputs every X frames", &fSystem::nRandomizeLocalInputsEveryXFramesInGGPO);
		ImGui::Checkbox("Show debug data?", &bDebug);
		Separator();
	}
	if (clientAlerts.size() > 0) {
		ImU32 red = IM_COL32(255, 0, 0, 255);
		ImU32 darkRed = IM_COL32(255, 0, 0, 102);
		ImGui::PushStyleColor(ImGuiCol_Button, darkRed);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, red);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, red);
		ImGui::PushStyleColor(ImGuiCol_Text, red);
		Text(clientAlerts.at(0).c_str());
		if (Button("OK")) {
			clientAlerts.pop_front();
		}
		ImGui::PopStyleColor(4);
	}
	else {
		switch (netState) {
		case NWS_CAPTURE:
			if (deviceIdx == 0xff && deviceType == 0xff) {
				if (sf4e::NetplayFacade::TryCapturePadForSide(0, deviceIdx, deviceType)) {
					netState = NWS_DECIDE;
				}
				else {
					Text("Press start or LK...");
				}
			}
			break;
		case NWS_DECIDE:
			if (Button("Host a game")) {
				netState = NWS_HOST;
			}
			if (Button("Join a game")) {
				netState = NWS_JOIN;
			}
			break;
		case NWS_HOST:
			if (!fUserApp::server) {
				DrawNetworkHostPanel(deviceIdx, deviceType);
				if (Button("Go back")) {
					netState = NWS_DECIDE;
				}
			}
			else {
				if (bDebug) {
					Text("Server initialized, client map:");
					for (auto iter = fUserApp::server->clients.begin(); iter != fUserApp::server->clients.end(); iter++) {
						Text(
							"%x %s@%s %s",
							iter->conn,
							iter->data.connId.user.c_str(),
							iter->data.connId.host.c_str(),
							iter->data.name.c_str()
						);
					}
				}
				if (fUserApp::netplay) {
					Separator();
					Text("Client");
					DrawNetworkLobbyPanel();
				}
			}
			break;
		case NWS_JOIN:
			if (!fUserApp::netplay) {
				DrawNetworkJoinPanel(deviceIdx, deviceType);
				if (Button("Go back")) {
					netState = NWS_DECIDE;
				}
			}
			else {
				DrawNetworkLobbyPanel();
			}
			break;
		default:
			break;
		}
	}

	End();
}

void DrawPadTable(DWORD* padData) {
	Columns(3);
	Text("Button"); NextColumn(); Text("P1"); NextColumn(); Text("P2"); NextColumn();
	Separator();

	for (unsigned int bytePosition = 0; bytePosition < 14; bytePosition++) {
		Text("B%d", bytePosition); NextColumn();
		for (int i = 0; i < 2; i++) {
			Text((padData[i] & (1 << bytePosition)) ? "ON" : "OFF"); NextColumn();
		}
		Separator();
	}
	Columns(1);
}

void DrawPadWindow(bool* pOpen) {
	Begin(
		"Pad",
		pOpen,
		ImGuiWindowFlags_None
	);

	PadSystem* p = PadSystem::staticMethods.GetSingleton();
	PadSystem::__publicMethods& methods = PadSystem::publicMethods;
	DWORD padData[2];
	if (BeginTabBar("Pad views", ImGuiTabBarFlags_None)) {
		if (BeginTabItem("Switch data")) {
			if (BeginTabBar("Pad types", ImGuiTabBarFlags_None)) {
				if (BeginTabItem("Raw On")) {
					for (int i = 0; i < 2; i++) {
						padData[i] = (p->*methods.GetButtons_RawOn)(i);
					}

					DrawPadTable(padData);
					EndTabItem();
				}

				if (BeginTabItem("Raw Rising")) {
					for (int i = 0; i < 2; i++) {
						padData[i] = (p->*methods.GetButtons_RawRising)(i);
					}

					DrawPadTable(padData);
					EndTabItem();
				}

				if (BeginTabItem("Raw Falling")) {
					for (int i = 0; i < 2; i++) {
						padData[i] = (p->*methods.GetButtons_RawFalling)(i);
					}

					DrawPadTable(padData);
					EndTabItem();
				}

				if (BeginTabItem("Raw Repeat")) {
					for (int i = 0; i < 2; i++) {
						padData[i] = (p->*methods.GetButtons_RawRisingWithRepeat)(i);
					}

					DrawPadTable(padData);
					EndTabItem();
				}

				if (BeginTabItem("Mapped On")) {
					for (int i = 0; i < 2; i++) {
						padData[i] = (p->*methods.GetButtons_MappedOn)(i);
					}

					DrawPadTable(padData);
					EndTabItem();
				}

				EndTabBar();
			}

			EndTabItem();
		}

		if (BeginTabItem("Device Data")) {
			int deviceCount = (p->*methods.GetAllDeviceCount)();
			int okCount = (p->*methods.GetOKDeviceCount)();
			Text("Device count: %d", deviceCount);
			Text("Device OK count: %d", okCount);
			for (int i = 0; i < deviceCount; i++) {
				Text("Device %d: %s", i, (p->*methods.GetDeviceName)(i));
			}
			EndTabItem();
		}

		if (BeginTabItem("Player Data")) {
			int deviceCount = (p->*methods.GetAllDeviceCount)();
			int okCount = (p->*methods.GetOKDeviceCount)();
			Text("Device count: %d", deviceCount);
			Text("Device OK count: %d", okCount);
			for (int i = 0; i < 2; i++) {
				Text(
					"Player %d: device index %d, type %d, is assigned %d",
					i,
					(p->*methods.GetDeviceIndexForPlayer)(i),
					(p->*methods.GetDeviceTypeForPlayer)(i),
					(p->*methods.GetAssigmentStatusForPlayer)(i)
				);
			}

			EndTabItem();
		}

		EndTabBar();
	}

	End();
}

bool compareTasks(Task* a, Task* b) {
	return *Task::GetPriority(a) < *Task::GetPriority(b);
}

void DrawSystemTaskPanel(System* s, TaskCore* core) {
	if (core == NULL) {
		Text("Core not yet allocated");
		return;
	}

	Text("Current system frame: %d", ((FixedPoint*)((unsigned int)s + 0xdd4))->integral);
	Columns(6);

	Text("Prio"); NextColumn(); Text("Name"); NextColumn();
	Text("Flags"); NextColumn(); Text("0x8 value"); NextColumn();
	Text("Phase"); NextColumn(); Text("State"); NextColumn();

	Task* t;
	for (
		t = TaskCore::GetTaskHead(core);
		t != nullptr;
		t = *Task::GetNext(t)
	) {
		Text("%x", *Task::GetPriority(t)); NextColumn();
		Text("%s", (core->*TaskCore::publicMethods.GetTaskName)(&t)); NextColumn();
		Text("%x", *Task::GetFlags(t)); NextColumn();
		Text("%x", *Task::Get0x8(t)); NextColumn();
		Text("%x", *Task::GetPhase(t)); NextColumn();
		Text("%x", *Task::GetState(t)); NextColumn();
	}
	Columns(1);
}

void DrawAdapterSummary(rSoundPlayerManager::CriPlayerAdapter* adapter) {
	Text("  Flags: %x", adapter->flags);
	if (adapter->position != NULL) {
		Text("  Position: %p (%f %f %f %f)", adapter->position, adapter->position->x, adapter->position->y, adapter->position->z, adapter->position->w);
	}
	else {
		Text("  Position: Null");
	}
	Text("  Volume: %f", adapter->volume);
	Text("  Fade scale: %f", adapter->fadeScale);
	Text("  Play state: %d", adapter->playState);
	Text("  UNK last field: %x", adapter->field6_0x18);

	if (fSoundPlayerManager::bUsePureSounds) {
		fSoundPlayerManager::DeferredSoundRequest& req = fSoundPlayerManager::adapterToCurrentSound[adapter];
		Text("  Deferred islive: %d", req.bLive);
		Text("  Deferred cueIdx: %d", req.cueIdx);
		Text("  Deferred cueSheetHandle: %x", req.cueSheetHandle);
		Text("  Deferred currentAdapterHandle: %x", req.currentAdapterHandle);
		Text("  Deferred flags: %x", req.flags);
		Text("  Deferred position: %x", req.position);
		if (req.position != NULL) {
			Text("  Deferred position: %p (%f %f %f %f)", req.position, req.position->x, req.position->y, req.position->z, req.position->w);
		}
		else {
			Text("  Deferred position: Null");
		}
		Text("  Deferred type: %d", req.type);
	}
}

void DrawSoundWindow(bool* pOpen) {
	static int currentManagerIdx = -1;
	static bool bShowDetails = false;

	Begin(
		"Sound",
		pOpen,
		ImGuiWindowFlags_None
	);
	ImGui::Checkbox("Track plays?", &fSoundPlayerManager::bTrackRequests);
	ImGui::Checkbox("Show details?", &bShowDetails);

	System* system = System::staticMethods.GetSingleton();
	int isFight = (system->*System::publicMethods.IsFight)();
	if (!isFight) {
		ImGui::Checkbox("Use pure playback?", &fSoundPlayerManager::bUsePureSounds);
		currentManagerIdx = -1;
		Text("Is fight: %d", isFight);
		End();
		return;
	}
	 
	SoundUnit* unit = (SoundUnit*)(system->*System::publicMethods.GetUnitByIndex)(System::U_SOUND);
	Text("Use count: ");
	for (int i = 0; i < 8; i++) {
		int used = 0;
		int free = 0;
		rSoundPlayerManager* m = SoundUnit::GetManagerArray(unit)[i];
		rSoundPlayerManager::AdapterPool* pool = rSoundPlayerManager::GetAdapterPool(m);
		rSoundPlayerManager::AdapterPool::Entry* cursor;
		for (cursor = pool->activeHead; cursor != NULL; cursor = cursor->next) {
			used++;
		}
		for (cursor = pool->inactiveHead; cursor != NULL; cursor = cursor->next) {
			free++;
		}

		ImGui::SameLine();
		Text(" %d (%d),", used, free);
	}

	Text("Deferred playback: %d", fSoundPlayerManager::bUsePureSounds);
	if (bShowDetails) {
		ImGui::BeginChild("left pane", ImVec2(150, 0), true);
		for (int i = 0; i < 8; i++) {
			char label[128];
			sprintf(label, "Manager %d", i);
			if (ImGui::Selectable(label, currentManagerIdx == i)) {
				currentManagerIdx = i;
			}
		}
		ImGui::EndChild();
		ImGui::SameLine();

		ImGui::BeginChild("right pane");
		if (currentManagerIdx != -1) {
			rSoundPlayerManager* currentSelectedManager = SoundUnit::GetManagerArray(unit)[currentManagerIdx];
			rSoundPlayerManager::AdapterPool* frontAdapterPool = rSoundPlayerManager::GetAdapterPool(currentSelectedManager);
			rSoundPlayerManager::AdapterPool::Entry* cursor;

			for (cursor = frontAdapterPool->activeHead; cursor != NULL; cursor = cursor->next) {
				rSoundPlayerManager::CriPlayerAdapter* adapter = *(rSoundPlayerManager::CriPlayerAdapter**)(cursor->data);
				Text("Active front adapter %p:", adapter);
				DrawAdapterSummary(adapter);
			}
			for (cursor = frontAdapterPool->inactiveHead; cursor != NULL; cursor = cursor->next) {
				rSoundPlayerManager::CriPlayerAdapter* adapter = *(rSoundPlayerManager::CriPlayerAdapter**)(cursor->data);
				Text("Inactive front adapter %p:", adapter);
				DrawAdapterSummary(adapter);
			}

			if (fSoundPlayerManager::bUsePureSounds) {
				rSoundPlayerManager* deferredManager = fSoundPlayerManager::shadowManagerMap[currentSelectedManager];
				rSoundPlayerManager::AdapterPool* deferredAdapterPool = rSoundPlayerManager::GetAdapterPool(deferredManager);
				for (cursor = deferredAdapterPool->activeHead; cursor != NULL; cursor = cursor->next) {
					rSoundPlayerManager::CriPlayerAdapter* adapter = *(rSoundPlayerManager::CriPlayerAdapter**)(cursor->data);
					Text("Active deferred adapter %p:", adapter);
					DrawAdapterSummary(adapter);
				}

				for (cursor = deferredAdapterPool->inactiveHead; cursor != NULL; cursor = cursor->next) {
					rSoundPlayerManager::CriPlayerAdapter* adapter = *(rSoundPlayerManager::CriPlayerAdapter**)(cursor->data);
					Text("Inactive deferred adapter %p:", adapter);
					DrawAdapterSummary(adapter);
				}
			}
		}
		ImGui::EndChild();
	}

	End();
}

void DrawSystemWindow(bool* pOpen) {
	static int selectedForwardSimFrame = 0;

	Begin(
		"System",
		pOpen,
		ImGuiWindowFlags_None
	);

	System* system = System::staticMethods.GetSingleton();
	System::__publicMethods& methods = System::publicMethods;
	System::__staticVars& staticVars = System::staticVars;

	if (BeginTabBar("System tabs", ImGuiTabBarFlags_None)) {
		if (BeginTabItem("Global state")) {
			FixedPoint tmp;
			GameManager* manager = (system->*System::publicMethods.GetGameManager)();

			if (Button("Extended save")) {
				fSystem::extendedSaveRequest = true;
			}
			ImGui::SameLine();
			if (Button("Extended load")) {
				fSystem::extendedLoadRequest = true;
			}

			int isFight = (system->*System::publicMethods.IsFight)();
			Text("Is fight: %d", isFight);
			Text("Is leaving battle: %d", (system->*System::publicMethods.IsLeavingBattle)());
			if (manager != NULL) {
				(manager->*GameManager::publicMethods.GetAgglutinateTime)(&tmp);
				Text("Agglutinate time: %f", FixedToFloat(&tmp));
				(manager->*GameManager::publicMethods.GetRoundTime)(&tmp);
				Text("Round time: %f", FixedToFloat(&tmp));
			}
			(system->*System::publicMethods.GetGlobalTimeScale_Fixed)(&tmp);
			ImGui::Text("Time: %f", FixedToFloat(&tmp));
			(system->*System::publicMethods.GetUnitTimeScale_Fixed)(&tmp, -1);
			ImGui::Text("Timescale (stage): %f", FixedToFloat(&tmp));
			(system->*System::publicMethods.GetUnitTimeScale_Fixed)(&tmp, 0);
			ImGui::Text("Timescale (P1): %f", FixedToFloat(&tmp));
			(system->*System::publicMethods.GetUnitTimeScale_Fixed)(&tmp, 1);
			ImGui::Text("Timescale (P2): %f", FixedToFloat(&tmp));

			Text("Game mode: %d", (system->*methods.GetGameMode)());
			Text("Random seed in system: %d", System::GetRandom(system)->seed);
			Request* r = *System::GetRequest(system);
			if (r) {
				Text("Random seed in request: %d", (r->*Request::publicMethods.GetRandomSeed)());
			}
			else {
				Text("Random seed in request: No request!");
			}

			ImGui::InputInt("Destination flow after next battle start", &fSystem::nNextBattleStartFlowTarget);
			ImGui::InputInt("Battle exit type", System::GetBattleExitType(system));
			Text(
				"Current battle flow: %d (previous %d)",
				*staticVars.CurrentBattleFlow,
				*staticVars.PreviousBattleFlow
			);
			Text(
				"Current battle flow substate: %d (previous %d)",
				*staticVars.CurrentBattleFlowSubstate,
				*staticVars.PreviousBattleFlowSubstate
			);
			Text("Current every-frame callable: %p", *staticVars.BattleFlowCallback_CallEveryFrame_aa9254);
			Text("Current substate callable: %p", *staticVars.BattleFlowSubstateCallable_aa9258);
			Text(
				"Battle flow moved from frame %d to frame %d",
				staticVars.PreviousBattleFlowFrame->integral,
				staticVars.CurrentBattleFlowFrame->integral
			);
			Text(
				"Battle flow substate moved from frame %d to frame %d",
				staticVars.PreviousBattleFlowSubstateFrame->integral,
				staticVars.CurrentBattleFlowSubstateFrame->integral
			);
			EndTabItem();
		}
		if (BeginTabItem("Simulation control")) {
			Text("Halt after next: %s", fSystem::bHaltAfterNext ? "true" : "false");
			Text("Update allowed: %s", fSystem::bUpdateAllowed ? "true" : "false");
			if (Button("Pause")) {
				fSystem::bUpdateAllowed = false;
			}
			if (Button("Play")) {
				fSystem::bUpdateAllowed = true;
			}
			if (Button("Halt after next")) {
				fSystem::bHaltAfterNext = true;
			}
			if (Button("Step")) {
				fSystem::bUpdateAllowed = true;
				fSystem::bHaltAfterNext = true;
			}

			EndTabItem();
		}

		if (BeginTabItem("Forward Simulation")) {
			ImGui::InputInt("Num frames to skip", &nExtraFramesToSimulate);
			if (nExtraFramesToSimulate < 1) {
				nExtraFramesToSimulate = 1;
			}
			if (nExtraFramesToSimulate > fPadSystem::PLAYBACK_MAX) {
				nExtraFramesToSimulate = fPadSystem::PLAYBACK_MAX;
			}
			if (Button("Simulate")) {
				fSystem::nExtraFramesToSimulate = nExtraFramesToSimulate;
			}
			for (int p = 0; p < 2; p++) {
				const char* headerLabel = p == 0 ? "P1 inputs during skip" : "P2 inputs during skip";
				if (ImGui::CollapsingHeader(headerLabel)) {
					ImGui::BeginChild("left pane", ImVec2(150, 0), true);
					for (int i = 0; i < nExtraFramesToSimulate; i++)
					{
						char label[128];
						sprintf(label, "Frame %d", i);
						if (ImGui::Selectable(label, selectedForwardSimFrame == i)) {
							selectedForwardSimFrame = i;
						}
					}
					ImGui::EndChild();
					ImGui::SameLine();

					ImGui::BeginChild("right pane");
					unsigned int* padData = &fPadSystem::playbackData[p][selectedForwardSimFrame].rawOn;
					CheckboxFlags("Up", padData, 0x1); NextColumn();
					CheckboxFlags("Down", padData, 0x2); NextColumn();
					CheckboxFlags("Left", padData, 0x4); NextColumn();
					CheckboxFlags("Right", padData, 0x8); NextColumn();
					CheckboxFlags("LP", padData, 0x10); NextColumn();
					CheckboxFlags("MP", padData, 0x20); NextColumn();
					CheckboxFlags("LK", padData, 0x40); NextColumn();
					CheckboxFlags("MK", padData, 0x80); NextColumn();
					CheckboxFlags("HP", padData, 0x400); NextColumn();
					CheckboxFlags("HK", padData, 0x800); NextColumn();
					ImGui::EndChild();
				}
			}
			EndTabItem();
		}

		if (BeginTabItem("Update tasks")) {
			TaskCore* updateCore = (system->*methods.GetTaskCore)(System::TCI_UPDATE);
			DrawSystemTaskPanel(system, updateCore);
			EndTabItem();
		}

		if (BeginTabItem("Render tasks")) {
			TaskCore* renderCore = (system->*methods.GetTaskCore)(System::TCI_RENDER);
			DrawSystemTaskPanel(system, renderCore);
			EndTabItem();
		}
		EndTabBar();
	}

	End();
}

void DrawVfxWindow(bool* pOpen) {
	Begin(
		"Vfx",
		pOpen,
		ImGuiWindowFlags_None
	);

	System* system = System::staticMethods.GetSingleton();
	int isFight = (system->*System::publicMethods.IsFight)();
	if (!isFight) {
		Text("Is fight: %d", isFight);
		End();
		return;
	}

	ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
	VfxUnit* unit = (VfxUnit*)(system->*System::publicMethods.GetUnitByIndex)(System::U_VFX);
	rVfx::IContainer* (VfxUnit:: * GetContainerByType)(DWORD) = VfxUnit::publicMethods.GetContainerByType;
	rVfx::ObjectContainer* cObject = (rVfx::ObjectContainer*)(unit->*GetContainerByType)(VfxUnit::CT_OBJECT);
	rVfx::ParticleContainer* cParticle = (rVfx::ParticleContainer*)(unit->*GetContainerByType)(VfxUnit::CT_PARTICLE);
	rVfx::TraceContainer* cTrace = (rVfx::TraceContainer*)(unit->*GetContainerByType)(VfxUnit::CT_TRACE);
	ColorFadeUnit* colorFadeUnit = ColorFadeUnit::staticMethods.GetSingleton();

	if (isFight) {
		if (BeginTabBar("Container Type", tab_bar_flags))
		{
			if (BeginTabItem("Object")) {

				if (BeginTabBar("Object Type", tab_bar_flags)) {
					if (BeginTabItem("Reserved")) {
						Columns(2, NULL, false);

						for (unsigned int i = 0; i < rVfx::ObjectContainer::RESERVED_OBJECT_COUNT; i++) {
							DWORD handle = rVfx::ObjectContainer::GenerateFakeHandle(i, true);
							rVfx::Object* o = (cObject->*rVfx::ObjectContainer::publicMethods.GetObjectFromHandle)(handle);
							Text("Object %d:", i); NextColumn();
							if (o) {
								Text("%x , Name: %s", (unsigned int)o, rVfx::Object::GetNameTmp(o)->c_str());
								NextColumn();
							}
							else {
								Text("DEAD"); NextColumn();
							}
						}

						Columns(1);
						EndTabItem();
					}
					if (BeginTabItem("Loose")) {
						Columns(2, NULL, false);

						for (unsigned int i = 0; i < rVfx::ObjectContainer::DEFAULT_LOOSE_OBJECT_COUNT; i++) {
							DWORD handle = rVfx::ObjectContainer::GenerateFakeHandle(i, false);
							rVfx::Object* o = (cObject->*rVfx::ObjectContainer::publicMethods.GetObjectFromHandle)(handle);
							Text("Object %d:", i); NextColumn();
							if (o) {
								Text("%x , Name: %s", (unsigned int)o, rVfx::Object::GetNameTmp(o)->c_str());
								NextColumn();
							}
							else {
								Text("DEAD"); NextColumn();
							}
						}

						Columns(1);
						EndTabItem();
					}
					EndTabBar();
				}

				EndTabItem();
			}

			if (BeginTabItem("Particle")) {
				Columns(2, NULL, false);

				for (unsigned int i = 0; i < rVfx::ParticleContainer::DEFAULT_PARTICLE_COUNT; i++) {
					DWORD handle = rVfx::ParticleContainer::GenerateFakeHandle(i);
					rVfx::Particle* p = (cParticle->*rVfx::ParticleContainer::publicMethods.GetParticleFromHandle)(handle);
					Text("Particle %d:", i); NextColumn();
					if (p) {
						Text("%x , Name: %s", (unsigned int)p, rVfx::Particle::GetNameTmp(p)->c_str());
						NextColumn();
					}
					else {
						Text("DEAD"); NextColumn();
					}
				}

				Columns(1);
				EndTabItem();
			}

			if (BeginTabItem("Trace")) {
				Columns(2, NULL, false);

				for (unsigned int i = 0; i < rVfx::TraceContainer::DEFAULT_TRACE_COUNT; i++) {
					DWORD handle = rVfx::TraceContainer::GenerateFakeHandle(i);
					rVfx::Trace* t = (cTrace->*rVfx::TraceContainer::publicMethods.GetTraceFromHandle)(handle);
					Text("Trace %d:", i); NextColumn();
					if (t) {
						Text("%x , Name: %s", (unsigned int)t, rVfx::Trace::GetNameTmp(t)->c_str());
						NextColumn();
					}
					else {
						Text("DEAD"); NextColumn();
					}
				}

				Columns(1);
				EndTabItem();
			}

			if (BeginTabItem("ColorFade")) {
				Text("Highest observed ColorFadeData count: %d", fColorFade::HIGHEST_OBSERVED_FADES);
				ImGui::SameLine();
				if (Button("Clear")) {
					fColorFade::HIGHEST_OBSERVED_FADES = 0;
				}

				Columns(2, NULL, false);

				for (unsigned int i = 0; i < 2; i++) {
					ColorFade* fade = (colorFadeUnit->*ColorFadeUnit::publicMethods.GetFade)(i);
					auto fadeList = ColorFade::GetList(fade);
					Text("Size %d:", fadeList->numUsed); NextColumn(); NextColumn();

					int j = 0;
					for (auto iter = fadeList->root->next; iter != fadeList->root; iter = iter->next) {
						Text("  Data @ index %d:", j); NextColumn(); NextColumn();
						Text("    Resource offset: %p", iter->data.resourcePtr); NextColumn();
						Text("    Flags: %x", iter->data.flags); NextColumn();
						j++;
					}
				}

				Columns(1);
				EndTabItem();
			}
			EndTabBar();
		}
	}

	End();
}

void DrawVsCharaPlayerPanel(VsCharaSelect::PlayerConditions* c) {
	Text("Last selected character: %s", VsCharaSelect::PlayerConditions::GetSelectedCharaAbbrev(c));
	Text("Current hovered character: %s", VsCharaSelect::PlayerConditions::GetHoveredCharaAbbrev(c));
	Text("Color: %d", *VsCharaSelect::PlayerConditions::GetColor(c));
	Text("Costume: %d", *VsCharaSelect::PlayerConditions::GetCostume(c));
	Text("Personal action: %d", *VsCharaSelect::PlayerConditions::GetPersonalAction(c));
	Text("Win quote: %d", *VsCharaSelect::PlayerConditions::GetWinQuote(c));
	Text("Edition: %d", *VsCharaSelect::PlayerConditions::GetEdition(c));
	Text("Ultra combo: %d", *VsCharaSelect::PlayerConditions::GetUltraCombo(c));
	Text("Handicap: %d", *VsCharaSelect::PlayerConditions::GetHandicap(c));
}

void DrawVsBattleWindow(bool* pOpen) {
	Begin(
		"VsBattle",
		pOpen,
		ImGuiWindowFlags_None
	);

	ImGui::Checkbox("Block initialization?", &fVsBattle::bBlockInitialization);
	ImGui::Checkbox("Block termination?", &fVsBattle::bBlockTermination);
	ImGui::Checkbox("Force next battle online?", &fVsBattle::bForceNextMatchOnline);
	ImGui::Checkbox("Skip results menu on next result?", &fVsBattle::bTerminateOnNextLeftBattle);
	ImGui::Checkbox("Override next random seed?", &fVsBattle::bOverrideNextRandomSeed);
	if (fVsBattle::bOverrideNextRandomSeed) {
		ImGui::InputInt("Next match random seed", (int*)&fVsBattle::nextMatchRandomSeed);
	}

	End();
}

void DrawVsCharaSelectWindow(bool* pOpen) {
	// This should be deleted when a more fully-featured character select
	// writing implementation is done, but is useful in the short term to
	// test the dString typings.
	static char charaNameTest[4];

	Begin(
		"VsCharaSelect",
		pOpen,
		ImGuiWindowFlags_None
	);

	char* vsCharaSelectQuery[] = { "VSMode", "PreBattle", "CharaSelect" };
	VsCharaSelect* charaSelect = (VsCharaSelect*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), vsCharaSelectQuery, 3);
	if (!charaSelect) {
		Text("No instance");
		End();
		return;
	}


	VsCharaSelect::CharaSelectState* state = VsCharaSelect::GetState(charaSelect);
	Text("Instance: %p", charaSelect);
	Text("Flags: %x", state->flags);
	if (BeginTabBar("VsCharaSelect tabs", ImGuiTabBarFlags_None)) {
		if (BeginTabItem("Player 1")) {
			DrawVsCharaPlayerPanel(&state->playerConditions[0]);
			ImGui::InputText("Inject hovered character", charaNameTest, 4);
			if (Button("inject")) {
				dString* charaAbbrev = VsCharaSelect::PlayerConditions::GetHoveredCharaAbbrev(&state->playerConditions[0]);
				(charaAbbrev->*dString::publicMethods.assign)(charaNameTest, 3);
			}
			EndTabItem();
		}

		if (BeginTabItem("Player 2")) {
			DrawVsCharaPlayerPanel(&state->playerConditions[1]);
			EndTabItem();
		}
		EndTabBar();
	}

	End();
}

void DrawVsStageSelectWindow(bool* pOpen) {
	static char newStageCode[4];

	Begin(
		"VsStageSelect",
		pOpen,
		ImGuiWindowFlags_None
	);

	ImGui::Checkbox("Force timer on next vs-stage-select?", &fVsStageSelect::forceTimerOnNextStageSelect);
	ImGui::InputText("Stage code to inject", newStageCode, 4);


	char* vsStageSelectQuery[] = { "VSMode", "PreBattle", "StageSelect" };
	VsStageSelect* stageSelect = (VsStageSelect*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), vsStageSelectQuery, 3);
	if (!stageSelect) {
		Text("No instance");
		End();
		return;
	}
	
	rStageSelect::Control* control = VsStageSelect::GetControl(stageSelect);

	if (Button("set stage cursor")) {
		(control->*rStageSelect::Control::publicMethods.SetStageCursor)(newStageCode);
	}
	if (Button("select stage")) {
		(control->*rStageSelect::Control::publicMethods.SelectStage)(newStageCode);
	}

	VsStageSelect::StageSelectState* state = VsStageSelect::GetState(stageSelect);
	Text("Instance: %p", stageSelect);
	Text("Flags: %x", state->flags);
	Text("Phase: %x", (control->*rStageSelect::Control::publicMethods.GetPhase)());
	Text("Stage code 1: %s", &state->stageCode1);
	Text("Stage code 2: %s", &state->stageCode2);
	End();
}

void DrawHelpWindow(bool* pOpen) {
	Begin(
		"ImGui Help",
		pOpen,
		ImGuiWindowFlags_None
	);

	ImGui::ShowUserGuide();

	End();
}

void DrawHudWindow(bool* pOpen) {
	Begin(
		"HUD",
		pOpen,
		ImGuiWindowFlags_None
	);

	if (fHud::bAllowHudUpdate) {
		if (Button("Disable HUD updates")) {
			fHud::bAllowHudUpdate = false;
			fIUnit::bAllowHudUpdate = false;
		}
	}
	else {
		if (Button("Enable HUD updates")) {
			fHud::bAllowHudUpdate = true;
			fIUnit::bAllowHudUpdate = true;
		}
	}

	System* system = System::staticMethods.GetSingleton();

	int isFight = (system->*System::publicMethods.IsFight)();
	Text("Is fight: %d", isFight);
	if (isFight) {
		HudUnit* hud = (HudUnit*)(system->*System::publicMethods.GetUnitByIndex)(System::U_HUD);
		rHud::Announce::Unit* announce = *HudUnit::GetAnnounce(hud);
		rHud::Notice::View* noticeView = *rHud::Notice::Unit::GetView(*HudUnit::GetNotice(hud));
		WithReleaser<rHud::Notice::Player>* noticePlayers = rHud::Notice::View::GetPlayers(noticeView);
		Text("noticeView: %x", noticeView);
		Text("noticePlayers: %x", noticePlayers);
		for (int playerIdx = 0; playerIdx < (system->*System::publicMethods.GetNumCharasToSimulateThisFrame)(); playerIdx++) {
			Text("Player %d: %x", playerIdx, noticePlayers[playerIdx].obj);
			WithReleaser<rHud::Notice::Bonus>* bonuses = rHud::Notice::Player::GetBonuses(noticePlayers[playerIdx].obj);
			Text("bonuses: %x", bonuses);
			Text("bonus 0: %x", bonuses[0].obj);
			Text("bonus 1: %x", bonuses[1].obj);

			WithReleaser<rHud::Notice::Combo>* combo = rHud::Notice::Player::GetCombo(noticePlayers[playerIdx].obj);
			Text("combo: %x", combo);
			Text("real combo: %x", combo->obj);
		}
	}

	End();
}

void DrawMementoWindow(bool* pOpen) {
	static DWORD targetID = 2;
	Begin(
		"Memento: Save/load",
		pOpen,
		ImGuiWindowFlags_None
	);

	if (BeginTabBar("Memento Tabs", ImGuiTabBarFlags_None)) {
		if (BeginTabItem("Auto-Immediate")) {
			if (Button("Record all to memento 1 immediately")) {
				System* system = System::staticMethods.GetSingleton();
				(system->*System::publicMethods.RecordAllToInternalMementoKeys)();
			}

			if (Button("Restore all from memento 1 immediately")) {
				System* system = System::staticMethods.GetSingleton();
				(system->*System::publicMethods.RestoreAllFromInternalMementoKeys)();
			}

			EndTabItem();
		}

		if (BeginTabItem("Manual")) {
			ImGui::InputInt("Target memento ID", (int*)&targetID);

			Text("Record: ");
			ImGui::SameLine();
			if (Button("Request##Record")) {
				GameMementoKey::MementoID mid = { targetID, targetID };
				fSystem::mementoSaveRequest = mid;
			}
			ImGui::SameLine();
			if (Button("Immediate##Record")) {
				GameMementoKey::MementoID mid = { targetID, targetID };
				System* system = System::staticMethods.GetSingleton();
				fSystem::RecordAllToInternalMementos(system, &mid);
			}

			Text("Restore: ");
			ImGui::SameLine();
			if (Button("Request##Restore")) {
				GameMementoKey::MementoID mid = { targetID, targetID };
				fSystem::mementoLoadRequest = mid;
			}
			ImGui::SameLine();
			if (Button("Immediate##Restore")) {
				GameMementoKey::MementoID mid = { targetID, targetID };
				System* system = System::staticMethods.GetSingleton();
				fSystem::RestoreAllFromInternalMementos(system, &mid);
			}

			EndTabItem();
		}


		if (BeginTabItem("Debug")) {
			auto keyEnd = fKey::trackedKeys.end();
			int keyIdx = 0;
			for (auto keyIter = fKey::trackedKeys.begin(); keyIter != keyEnd; keyIter++) {
				Text("Key %d: ", keyIdx);
				for (int i = 0; i < (*keyIter)->numMementos; i++) {
					ImGui::SameLine();
					Text("Memento %d: %d, %d", i, (*keyIter)->metadata[i].id.lo, (*keyIter)->metadata[i].id.hi);
				}
				keyIdx++;
			}
			EndTabItem();
		}

		ImGui::EndTabBar();
	}

	End();
}

void DrawTaskWindow(bool* pOpen) {
	Begin(
		"Tasks",
		pOpen,
		ImGuiWindowFlags_None
	);

	int numActiveCores = TaskCoreRegistry::staticMethods.GetNumActiveCores();
	Text("Number of active task cores: %d", numActiveCores);
	for (int i = 0; i < numActiveCores; i++) {
		TaskCore* core = TaskCoreRegistry::staticMethods.GetCoreByIndex(i);
		char* name = (core->*TaskCore::publicMethods.GetName)();
		Text("Core ID: %d, name: %s", i, name);
	}

	End();
}

int OnMainMenuModeSelected(int mode) {
	if (mode == rMainMenu::MainMenuItemID::MMI_NETWORK) {
		show_network_window = true;
		return 1;
	}

	return 0;
}

void Overlay::InitializeOverlay(HWND hWnd, IDirect3DDevice9* lpDevice) {
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.Alpha = DEFAULT_ALPHA;
	ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
	ImGui_ImplWin32_Init(hWnd);
	ImGui_ImplDX9_Init(lpDevice);
	fMainMenu::OnModeSelectedOverride = OnMainMenuModeSelected;
}

void Overlay::DrawOverlay() {
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	NewFrame();

	DrawHashOverlay();
	if (ImGui::IsMousePosValid() && ImGui::GetIO().MousePos.y < 200) {
		if (BeginMainMenuBar()) {
			if (BeginMenu("Eva")) {
				if (MenuItem("Tasks")) {
					show_task_window = true;
				}
				ImGui::EndMenu();
			}

			if (BeginMenu("Event")) {
				if (MenuItem("Overview")) {
					show_event_window = true;
				}
				ImGui::EndMenu();
			}

			if (BeginMenu("Game")) {
				if (MenuItem("Mementos")) {
					show_memento_window = true;
				}
				ImGui::EndMenu();
			}

			if (BeginMenu("GameEvents")) {
				if (MenuItem("MainMenu")) {
					show_main_menu_window = true;
				}
				if (MenuItem("VsBattle")) {
					show_vsbattle_window = true;
				}
				if (MenuItem("VsCharaSelect")) {
					show_vscharaselect_window = true;
				}
				if (MenuItem("VsStageSelect")) {
					show_vsstageselect_window = true;
				}
				ImGui::EndMenu();
			}

			if (BeginMenu("Battle")) {
				if (MenuItem("Chara")) {
					show_chara_window = true;
				}

				if (MenuItem("Command")) {
					show_command_window = true;
				}

				if (MenuItem("HUD")) {
					show_hud_window = true;
				}

				if (MenuItem("Sound")) {
					show_sound_window = true;
				}

				if (MenuItem("System")) {
					show_system_window = true;
				}

				if (MenuItem("Vfx")) {
					show_vfx_window = true;
				}

				ImGui::EndMenu();
			}

			if (BeginMenu("Pad")) {
				if (MenuItem("System")) {
					show_pad_window = true;
				}

				ImGui::EndMenu();
			}
			
			if (BeginMenu("Platform")) {
				if (MenuItem("GFxApp")) {
					show_gfxapp_window = true;
				}

				ImGui::EndMenu();
			}

			if (BeginMenu("Network")) {
				if (MenuItem("Network test")) {
					show_network_window = true;
				}

				ImGui::EndMenu();
			}

			if (BeginMenu("Imgui")) {
				if (MenuItem("Demo")) {
					show_demo_window = true;
				}

				if (MenuItem("Help")) {
					show_help_window = true;
				}

				ImGui::EndMenu();
			}


			if (MenuItem("Log")) {
				show_log_window = true;
			}

			EndMainMenuBar();
		}
	}

	if (show_chara_window) {
		DrawCharaWindow(&show_chara_window);
	}

	if (show_command_window) {
		DrawCommandWindow(&show_command_window);
	}

	if (show_event_window) {
		DrawEventWindow(&show_event_window);
	}

	if (show_gfxapp_window) {
		DrawGFxAppWindow(&show_gfxapp_window);
	}

	if (show_hud_window) {
		DrawHudWindow(&show_hud_window);
	}

	if (show_main_menu_window) {
		DrawMainMenuWindow(&show_main_menu_window);
	}

	// Imitate the main menu being in a transition to prevent typing in
	// the network window from affecting the main menu.
	fMainMenu::bOverrideItemObserverState = show_network_window ? rMainMenu::MMIOS_TRANSITION : -1;
	if (show_network_window) {
		DrawNetworkWindow(&show_network_window);
	}

	if (show_pad_window) {
		DrawPadWindow(&show_pad_window);
	}

	if (show_sound_window) {
		DrawSoundWindow(&show_sound_window);
	}

	if (show_system_window) {
		DrawSystemWindow(&show_system_window);
	}

	if (show_vfx_window) {
		DrawVfxWindow(&show_vfx_window);
	}

	if (show_vsbattle_window) {
		DrawVsBattleWindow(&show_vsbattle_window);
	}

	if (show_vscharaselect_window) {
		DrawVsCharaSelectWindow(&show_vscharaselect_window);
	}

	if (show_vsstageselect_window) {
		DrawVsStageSelectWindow(&show_vsstageselect_window);
	}

	if (show_demo_window) {
		ImGui::ShowDemoWindow(&show_demo_window);
	}

	if (show_help_window) {
		DrawHelpWindow(&show_help_window);
	}

	if (show_memento_window) {
		DrawMementoWindow(&show_memento_window);
	}

	if (show_task_window) {
		DrawTaskWindow(&show_task_window);
	}

	if (fSystem::ggpo) {
		if (sf4e::NetplayFacade::IsDevOverlayEnabled()) {
			DrawGGPOStatsOverlay(fSystem::ggpo, fSystem::players);
		}
		else {
			sf4e::NetplayStatus st = sf4e::NetplayFacade::GetStatus();
			if (st.pingMs >= 0) {
				DrawGGPOStatsOverlay(fSystem::ggpo, fSystem::players);
			}
		}
	}

	EndFrame();
	ImGui::Render();
	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void Overlay::FreeOverlay() {
	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

LRESULT WINAPI Overlay::OverlayWindowFunc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}
