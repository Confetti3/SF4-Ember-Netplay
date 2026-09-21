#include "../ui/DeveloperOverlay.hxx"
#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <string>
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
#include "sf4e__OverlayPrefs.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/Theme.hxx"
#include "../ui/FighterSelector.hxx"
#include "../common/StageCatalog.hxx"
#include "../Dimps/Dimps__Selection.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__UserApp.hxx"


static sf4e::ui::SelectionArt* s_selectionArt = nullptr;
static sf4e::ui::FighterSelector s_fighterSelectors[3];

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

using sf4e::ui::BeginToolWindow;
static bool Begin(const char* name, bool*, ImGuiWindowFlags) {
    sf4e::ui::Section(name);
    ImGui::PushFont(sf4e::ui::DiagnosticFont());
    return ImGui::BeginChild(name, ImVec2(0, 0));
}
using ImGui::BeginMainMenuBar;
using ImGui::BeginMenu;
using ImGui::BeginTabBar;
using ImGui::BeginTabItem;
using ImGui::BeginTable;
using ImGui::Button;
using ImGui::CheckboxFlags;
using sf4e::ui::EndToolWindow;
static void End() { ImGui::EndChild(); ImGui::PopFont(); }
using ImGui::EndFrame;
using ImGui::EndMainMenuBar;
using ImGui::EndTabBar;
using ImGui::EndTabItem;
using ImGui::EndTable;
using ImGui::MenuItem;
using ImGui::NewFrame;
using ImGui::Separator;
using ImGui::Spacing;
using ImGui::TableHeadersRow;
using ImGui::TableNextColumn;
using ImGui::TableNextRow;
using ImGui::TableSetColumnIndex;
using ImGui::TableSetupColumn;
using sf4e::ui::Text;
using ImGui::TextWrapped;

static int nExtraFramesToSimulate = 1;

static bool mainMenuShouldJump = false;
static int mainMenuCharaIDs[2] = { 0 };
static Dimps::GameEvents::VsMode::ConfirmedCharaConditions mainMenuJumpCharaConditions[2] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, (BYTE)rBattle::ED_USF4 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, (BYTE)rBattle::ED_USF4 },
};
static int mainMenuJumpCharaCount = 2;
static int mainMenuJumpStageID = 0;
static bool mainMenuEditionSelect = true;
static int mainMenuRoundCountIdx = 1;
static int mainMenuRoundTimeIdx = 2;

static bool soundShowDetails = false;

// Yes, this is correct- SF4's menu uses the fractional section of the fixed
// point values
//
// The first four entries are what the game's own menu offers. The last two go
// beyond it so a lobby can be set up for a long sparring session without
// needing a separate mode; rounds past 7 behave as "first to N/2+1".
static const std::pair<int, const char* const> roundCountList[sf4e::OverlayPrefs::ROUND_COUNT_OPTIONS] = {
	{1, "1"},
	{3, "3"},
	{5, "5"},
	{7, "7"},
	{15, "15"},
	{99, "99 (endless)"},
};

// As above, the last two entries exceed the menu's range. 9999 is deliberately
// a huge finite value rather than an "infinite" sentinel: live testing showed a
// zero time limit is taken literally (the round starts at 0 and instantly ends
// in time over), and the engine's encoding for the menu's infinite option is
// unknown. ~2.7 hours per round cannot expire in practice; the worst case is
// cosmetic, as the HUD timer only expects two digits.
static const std::pair<FixedPoint, const char* const> roundTimeList[sf4e::OverlayPrefs::ROUND_TIME_OPTIONS] = {
	{{0, 30}, "30"},
	{{0, 60}, "60"},
	{{0, 99}, "99"},
	{{0, 300}, "300"},
	{{0, 9999}, "9999 (endless)"},
};

static const int kRoundCountListLen = sf4e::OverlayPrefs::ROUND_COUNT_OPTIONS;
static const int kRoundTimeListLen = sf4e::OverlayPrefs::ROUND_TIME_OPTIONS;

const char* GetRoundCountLabel(void* options, int idx) {
	return ((std::pair<int, const char* const>*)options)[idx].second;
}

const char* GetRoundTimeLabel(void* options, int idx) {
	return ((std::pair<FixedPoint, const char* const>*)options)[idx].second;
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
					if (ImGui::BeginTable("Actor attributes", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
						ImGui::TableNextColumn();
						CharaActor::__publicMethods& methods = CharaActor::publicMethods;

						int actorID = (int)(a->*methods.GetActorID)();
						char* actorCode = actorID > -1 ? Dimps::characterCodes[actorID] : "";
						char* actorName = actorID > -1 ? Dimps::characterNames[actorID] : "";

						Text("Actor ID:"); ImGui::TableNextColumn();
						Text("%d", actorID); ImGui::TableNextColumn();

						Text("Actor code:"); ImGui::TableNextColumn();
						Text("%s", actorCode); ImGui::TableNextColumn();

						Text("Actor name:"); ImGui::TableNextColumn();
						Text("%s", actorName); ImGui::TableNextColumn();

						Text("Status:"); ImGui::TableNextColumn();
						Text("%d", (a->*methods.GetStatus)()); ImGui::TableNextColumn();

						Text("Root position:"); ImGui::TableNextColumn();
						float* position = (a->*methods.GetCurrentRootPosition)();
						Text("%f %f %f %f", position[0], position[1], position[2], position[3]); ImGui::TableNextColumn();

						Text("Current side:"); ImGui::TableNextColumn();
						Text("%d", (a->*methods.GetCurrentSide)()); ImGui::TableNextColumn();

						(a->*methods.GetVitalityAmt_FixedPoint)(&tmp);
						(a->*methods.GetVitalityMax_FixedPoint)(&tmp2);
						(a->*methods.GetVitalityPct_FixedPoint)(&tmp3);
						Text("Vitality:"); ImGui::TableNextColumn();
						Text("%f / %f (%fa%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); ImGui::TableNextColumn();

						(a->*methods.GetRevengeAmt_FixedPoint)(&tmp);
						(a->*methods.GetRevengeMax_FixedPoint)(&tmp2);
						(a->*methods.GetRevengePct_FixedPoint)(&tmp3);
						Text("Revenge:"); ImGui::TableNextColumn();
						Text("%f / %f (%f%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); ImGui::TableNextColumn();

						(a->*methods.GetRecoverableVitalityAmt_FixedPoint)(&tmp);
						(a->*methods.GetRecoverableVitalityMax_FixedPoint)(&tmp2);
						(a->*methods.GetRecoverableVitalityPct_FixedPoint)(&tmp3);
						Text("Recoverable Vitality:"); ImGui::TableNextColumn();
						Text("%f / %f (%f%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); ImGui::TableNextColumn();

						(a->*methods.GetSuperComboAmt_FixedPoint)(&tmp);
						(a->*methods.GetSuperComboMax_FixedPoint)(&tmp2);
						(a->*methods.GetSuperComboPct_FixedPoint)(&tmp3);
						Text("Super:"); ImGui::TableNextColumn();
						Text("%f / %f (%f%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); ImGui::TableNextColumn();

						(a->*methods.GetSCTimeAmt_FixedPoint)(&tmp);
						(a->*methods.GetSCTimeMax_FixedPoint)(&tmp2);
						(a->*methods.GetSCTimePct_FixedPoint)(&tmp3);
						Text("Super clock:"); ImGui::TableNextColumn();
						Text("%f / %f (%f%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); ImGui::TableNextColumn();

						(a->*methods.GetUCTimeAmt_FixedPoint)(&tmp);
						(a->*methods.GetUCTimeMax_FixedPoint)(&tmp2);
						(a->*methods.GetUCTimePct_FixedPoint)(&tmp3);
						Text("Ultra clock:"); ImGui::TableNextColumn();
						Text("%f / %f (%f%%)", FixedToFloat(&tmp), FixedToFloat(&tmp2), FixedToFloat(&tmp3) * 100); ImGui::TableNextColumn();

						(a->*methods.GetDamage)(&tmp);
						Text("Damage:"); ImGui::TableNextColumn();
						Text("%f", FixedToFloat(&tmp)); ImGui::TableNextColumn();

						(a->*methods.GetComboDamage)(&tmp);
						Text("Combo damage:"); ImGui::TableNextColumn();
						Text("%f", FixedToFloat(&tmp)); ImGui::TableNextColumn();

						(system->*System::publicMethods.GetUnitTimeScale_Fixed)(&tmp, i);
						Text("Timescale (local):"); ImGui::TableNextColumn();
						Text("%f", FixedToFloat(&tmp)); ImGui::TableNextColumn();

						ImGui::EndTable();
					}

					if (Button("Force to origin")) {
						float* position = (a->*CharaActor::publicMethods.GetCurrentRootPosition)();
						position[0] = 0.0;
					}
					EndTabItem();
				}
			}

			for (int i = 0; i < 2; i++) {
				CharaActor* a = (lpCharaUnit->*CharaUnit::publicMethods.GetActorByIndex)(i);
				CharaActor::__publicMethods& methods = CharaActor::publicMethods;

				if (BeginTabItem(i == 0 ? "Actor 0 bones" : "Actor 1 bones")) {
					if (ImGui::BeginTable("Bone positions", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
						ImGui::TableNextColumn();
						for (int j = 0; j < CharaActor::BP_HEN_2 + 1; j++) {
							float* position = (a->*methods.GetCurrentBonePositionByID)(j);
							Text("%s", CharaActor::staticMethods.GetBoneLabelByID(j)); ImGui::TableNextColumn();
							Text("%f %f %f %f", position[0], position[1], position[2], position[3]); ImGui::TableNextColumn();
						}
						ImGui::EndTable();
					}
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
				if (ImGui::BeginTable("Command pool", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
					ImGui::TableNextColumn();
					Text("Button"); ImGui::TableNextColumn(); Text("P1"); ImGui::TableNextColumn(); Text("P2"); ImGui::TableNextColumn();
					Separator();

					CommandUnit* u = (CommandUnit*)(system->*System::publicMethods.GetUnitByIndex)(System::U_COMMAND);
					DWORD padData[2];
					for (int i = 0; i < 2; i++) {
						CommandImpl* impl = (u->*CommandUnit::publicMethods.GetCommandImplForEntry)(i);
						padData[i] = (impl->*CommandImpl::publicMethods.GetCurrentOnSwitches)();
					}

					for (unsigned int bytePosition = 0; bytePosition < 14; bytePosition++) {
						Text("%s (B%d)", GetButtonLabel(bytePosition), bytePosition); ImGui::TableNextColumn();
						for (int i = 0; i < 2; i++) {
							Text((padData[i] & (1 << bytePosition)) ? "ON" : "OFF"); ImGui::TableNextColumn();
						}
						Separator();
					}

					ImGui::EndTable();
				}
				EndTabItem();
			}

			if (BeginTabItem("Rising")) {
				if (ImGui::BeginTable("Command active", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
					ImGui::TableNextColumn();
					Text("Button"); ImGui::TableNextColumn(); Text("P1"); ImGui::TableNextColumn(); Text("P2"); ImGui::TableNextColumn();
					Separator();

					CommandUnit* u = (CommandUnit*)(system->*System::publicMethods.GetUnitByIndex)(System::U_COMMAND);
					DWORD padData[2];
					for (int i = 0; i < 2; i++) {
						CommandImpl* impl = (u->*CommandUnit::publicMethods.GetCommandImplForEntry)(i);
						padData[i] = (impl->*CommandImpl::publicMethods.GetCurrentRisingSwitches)();
					}

					for (unsigned int bytePosition = 0; bytePosition < 14; bytePosition++) {
						Text("%s (B%d)", GetButtonLabel(bytePosition), bytePosition); ImGui::TableNextColumn();
						for (int i = 0; i < 2; i++) {
							Text((padData[i] & (1 << bytePosition)) ? "ON" : "OFF"); ImGui::TableNextColumn();
						}
						Separator();
					}

					ImGui::EndTable();
				}
				EndTabItem();
			}

			if (BeginTabItem("Falling")) {
				if (ImGui::BeginTable("Command pending", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
					ImGui::TableNextColumn();
					Text("Button"); ImGui::TableNextColumn(); Text("P1"); ImGui::TableNextColumn(); Text("P2"); ImGui::TableNextColumn();
					Separator();

					CommandUnit* u = (CommandUnit*)(system->*System::publicMethods.GetUnitByIndex)(System::U_COMMAND);
					DWORD padData[2];
					for (int i = 0; i < 2; i++) {
						CommandImpl* impl = (u->*CommandUnit::publicMethods.GetCommandImplForEntry)(i);
						padData[i] = (impl->*CommandImpl::publicMethods.GetCurrentFallingSwitches)();
					}

					for (unsigned int bytePosition = 0; bytePosition < 14; bytePosition++) {
						Text("%s (B%d)", GetButtonLabel(bytePosition), bytePosition); ImGui::TableNextColumn();
						for (int i = 0; i < 2; i++) {
							Text((padData[i] & (1 << bytePosition)) ? "ON" : "OFF"); ImGui::TableNextColumn();
						}
						Separator();
					}

					ImGui::EndTable();
				}
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
	sf4e::ui::Section("EVENT TREE");

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

	mainMenuJumpStageID = sf4e::selection::NormalizeStage(mainMenuJumpStageID);
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
	sf4e::ui::Section("MOVIE INSTANCES");

	GFxApp* app = GFxApp::staticMethods.GetSingleton();

	if (BeginTabBar("GFxApp"))
	{
		if (BeginTabItem("Actions")) {
			ImGui::BeginChild("left pane", ImVec2((std::max)(90.f * sf4e::ui::Scale(), ImGui::GetContentRegionAvail().x * .28f), 0), ImGuiChildFlags_Borders);
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

void DrawMainMenuWindow(bool* pOpen) {
	Begin(
		"MainMenu",
		pOpen,
		ImGuiWindowFlags_None
	);
	sf4e::ui::Section("VERSUS DEFAULTS");

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
		sf4e::ui::Combo("Round count", &mainMenuRoundCountIdx, GetRoundCountLabel, (void*)roundCountList, kRoundCountListLen);
		sf4e::ui::Combo("Round time", &mainMenuRoundTimeIdx, GetRoundTimeLabel, (void*)roundTimeList, kRoundTimeListLen);
		ImGui::Checkbox("Edition select", &mainMenuEditionSelect);
        if (ImGui::BeginTabBar("Versus players")) {
            for (int player = 0; player < 2; ++player) {
                const char* label = player == 0 ? "PLAYER 1" : "PLAYER 2";
                if (ImGui::BeginTabItem(label)) {
                    ImGui::PushID(player);
                    auto pick = sf4e::selection::FromNative(mainMenuJumpCharaConditions[player]);
                    pick.fighter = mainMenuCharaIDs[player];
                    s_fighterSelectors[player + 1].Draw(pick, mainMenuEditionSelect, s_selectionArt, Dimps::Selection::ReadAvailability);
                    sf4e::selection::ToNative(pick, mainMenuJumpCharaConditions[player]);
                    mainMenuCharaIDs[player] = pick.fighter;

                    ImGui::PopID();
                    ImGui::EndTabItem();
                }
            }
            if (ImGui::BeginTabItem("STAGE")) {
                sf4e::ui::DrawStageSelector(mainMenuJumpStageID, s_selectionArt);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
	}
    bool selectionsAvailable = true;
    if (mainMenuShouldJump) {
        for (int player = 0; player < 2; ++player) {
            auto pick = sf4e::selection::FromNative(mainMenuJumpCharaConditions[player]);
            const auto availability = Dimps::Selection::ReadAvailability(pick.fighter);
            sf4e::selection::Normalize(pick, mainMenuEditionSelect, &availability);
            sf4e::selection::ToNative(pick, mainMenuJumpCharaConditions[player]);
            mainMenuCharaIDs[player] = pick.fighter;
            selectionsAvailable = selectionsAvailable && sf4e::selection::Available(pick, mainMenuEditionSelect, availability);
        }
    }
    ImGui::BeginDisabled(!selectionsAvailable);
	if (Button("Go to versus mode")) {
		if (mainMenuShouldJump) {
			RootEvent* root = App::GetRootEvent();
			ProgressData* progressData = *RootEvent::GetProgressData(root);
			ProgressData::BattleTypeSettings* BattleTypeSettings = &(ProgressData::GetBattleTypeSettings(progressData)[ProgressData::NBT_PVP]);
			*ProgressData::GetNextBattleType(progressData) = ProgressData::NBT_PVP;

			BattleTypeSettings->editionSelect = mainMenuEditionSelect;
			BattleTypeSettings->rounds = roundCountList[mainMenuRoundCountIdx].first;
			BattleTypeSettings->timeLimit = roundTimeList[mainMenuRoundTimeIdx].first;
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

    ImGui::EndDisabled();
	End();
}

void DrawPadTable(DWORD* padData) {
	if (ImGui::BeginTable("Pad state", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
		ImGui::TableNextColumn();
		Text("Button"); ImGui::TableNextColumn(); Text("P1"); ImGui::TableNextColumn(); Text("P2"); ImGui::TableNextColumn();
		Separator();

		for (unsigned int bytePosition = 0; bytePosition < 14; bytePosition++) {
			Text("B%d", bytePosition); ImGui::TableNextColumn();
			for (int i = 0; i < 2; i++) {
				Text((padData[i] & (1 << bytePosition)) ? "ON" : "OFF"); ImGui::TableNextColumn();
			}
			Separator();
		}
		ImGui::EndTable();
	}
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

void DrawSystemTaskPanel(System* s, TaskCore* core) {
	sf4e::ui::Section("TASK SCHEDULE");
	if (core == NULL) {
		Text("Core not yet allocated");
		return;
	}

	Text("Current system frame: %d", ((FixedPoint*)((unsigned int)s + 0xdd4))->integral);
	if (ImGui::BeginTable("Task schedule", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Prio"); ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Flags"); ImGui::TableSetupColumn("0x8 value");
        ImGui::TableSetupColumn("Phase"); ImGui::TableSetupColumn("State");
        ImGui::TableHeadersRow(); ImGui::TableNextColumn();

		Task* t;
		for (
			t = TaskCore::GetTaskHead(core);
			t != nullptr;
			t = *Task::GetNext(t)
		) {
			Text("%x", *Task::GetPriority(t)); ImGui::TableNextColumn();
			Text("%s", (core->*TaskCore::publicMethods.GetTaskName)(&t)); ImGui::TableNextColumn();
			Text("%x", *Task::GetFlags(t)); ImGui::TableNextColumn();
			Text("%x", *Task::Get0x8(t)); ImGui::TableNextColumn();
			Text("%x", *Task::GetPhase(t)); ImGui::TableNextColumn();
			Text("%x", *Task::GetState(t)); ImGui::TableNextColumn();
		}
		ImGui::EndTable();
	}
}

void DrawAdapterSummary(rSoundPlayerManager::CriPlayerAdapter* adapter) {
	sf4e::ui::Section("ADAPTER DETAILS");
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

	Begin(
		"Sound",
		pOpen,
		ImGuiWindowFlags_None
	);
	sf4e::ui::Section("PLAYBACK AND TRACKING");
	ImGui::Checkbox("Track plays?", &fSoundPlayerManager::bTrackRequests);
	ImGui::Checkbox("Show details?", &soundShowDetails);

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
	if (soundShowDetails) {
		ImGui::BeginChild("left pane", ImVec2((std::max)(90.f * sf4e::ui::Scale(), ImGui::GetContentRegionAvail().x * .28f), 0), ImGuiChildFlags_Borders);
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

			sf4e::ui::InputInt("Destination flow after next battle start", &fSystem::nNextBattleStartFlowTarget);
			sf4e::ui::InputInt("Battle exit type", System::GetBattleExitType(system));
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
				fSystem::simGate.SetManualPause(true);
			}
			if (Button("Play")) {
				fSystem::bUpdateAllowed = true;
				fSystem::simGate.SetManualPause(false);
			}
			if (Button("Halt after next")) {
				fSystem::bHaltAfterNext = true;
			}
			if (Button("Step")) {
				fSystem::bUpdateAllowed = true;
				fSystem::simGate.SetManualPause(false);
				fSystem::bHaltAfterNext = true;
			}

			EndTabItem();
		}

		if (BeginTabItem("Forward Simulation")) {
			sf4e::ui::InputInt("Num frames to skip", &nExtraFramesToSimulate);
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
					ImGui::BeginChild("left pane", ImVec2((std::max)(90.f * sf4e::ui::Scale(), ImGui::GetContentRegionAvail().x * .28f), 0), ImGuiChildFlags_Borders);
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
					CheckboxFlags("Up", padData, 0x1);
					CheckboxFlags("Down", padData, 0x2);
					CheckboxFlags("Left", padData, 0x4);
					CheckboxFlags("Right", padData, 0x8);
					CheckboxFlags("LP", padData, 0x10);
					CheckboxFlags("MP", padData, 0x20);
					CheckboxFlags("LK", padData, 0x40);
					CheckboxFlags("MK", padData, 0x80);
					CheckboxFlags("HP", padData, 0x400);
					CheckboxFlags("HK", padData, 0x800);
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
						if (ImGui::BeginTable("Reserved objects", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
							ImGui::TableNextColumn();

							for (unsigned int i = 0; i < rVfx::ObjectContainer::RESERVED_OBJECT_COUNT; i++) {
								DWORD handle = rVfx::ObjectContainer::GenerateFakeHandle(i, true);
								rVfx::Object* o = (cObject->*rVfx::ObjectContainer::publicMethods.GetObjectFromHandle)(handle);
								Text("Object %d:", i); ImGui::TableNextColumn();
								if (o) {
									Text("%x , Name: %s", (unsigned int)o, rVfx::Object::GetNameTmp(o)->c_str());
									ImGui::TableNextColumn();
								}
								else {
									Text("DEAD"); ImGui::TableNextColumn();
								}
							}

							ImGui::EndTable();
						}
						EndTabItem();
					}
					if (BeginTabItem("Loose")) {
						if (ImGui::BeginTable("Loose objects", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
							ImGui::TableNextColumn();

							for (unsigned int i = 0; i < rVfx::ObjectContainer::DEFAULT_LOOSE_OBJECT_COUNT; i++) {
								DWORD handle = rVfx::ObjectContainer::GenerateFakeHandle(i, false);
								rVfx::Object* o = (cObject->*rVfx::ObjectContainer::publicMethods.GetObjectFromHandle)(handle);
								Text("Object %d:", i); ImGui::TableNextColumn();
								if (o) {
									Text("%x , Name: %s", (unsigned int)o, rVfx::Object::GetNameTmp(o)->c_str());
									ImGui::TableNextColumn();
								}
								else {
									Text("DEAD"); ImGui::TableNextColumn();
								}
							}

							ImGui::EndTable();
						}
						EndTabItem();
					}
					EndTabBar();
				}

				EndTabItem();
			}

			if (BeginTabItem("Particle")) {
				if (ImGui::BeginTable("Particles", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
					ImGui::TableNextColumn();

					for (unsigned int i = 0; i < rVfx::ParticleContainer::DEFAULT_PARTICLE_COUNT; i++) {
						DWORD handle = rVfx::ParticleContainer::GenerateFakeHandle(i);
						rVfx::Particle* p = (cParticle->*rVfx::ParticleContainer::publicMethods.GetParticleFromHandle)(handle);
						Text("Particle %d:", i); ImGui::TableNextColumn();
						if (p) {
							Text("%x , Name: %s", (unsigned int)p, rVfx::Particle::GetNameTmp(p)->c_str());
							ImGui::TableNextColumn();
						}
						else {
							Text("DEAD"); ImGui::TableNextColumn();
						}
					}

					ImGui::EndTable();
				}
				EndTabItem();
			}

			if (BeginTabItem("Trace")) {
				if (ImGui::BeginTable("Traces", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
					ImGui::TableNextColumn();

					for (unsigned int i = 0; i < rVfx::TraceContainer::DEFAULT_TRACE_COUNT; i++) {
						DWORD handle = rVfx::TraceContainer::GenerateFakeHandle(i);
						rVfx::Trace* t = (cTrace->*rVfx::TraceContainer::publicMethods.GetTraceFromHandle)(handle);
						Text("Trace %d:", i); ImGui::TableNextColumn();
						if (t) {
							Text("%x , Name: %s", (unsigned int)t, rVfx::Trace::GetNameTmp(t)->c_str());
							ImGui::TableNextColumn();
						}
						else {
							Text("DEAD"); ImGui::TableNextColumn();
						}
					}

					ImGui::EndTable();
				}
				EndTabItem();
			}

			if (BeginTabItem("ColorFade")) {
				Text("Highest observed ColorFadeData count: %d", fColorFade::HIGHEST_OBSERVED_FADES);
				ImGui::SameLine();
				if (Button("Clear")) {
					fColorFade::HIGHEST_OBSERVED_FADES = 0;
				}
				if (ImGui::BeginTable("Color fades", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
					ImGui::TableNextColumn();

					for (unsigned int i = 0; i < 2; i++) {
						ColorFade* fade = (colorFadeUnit->*ColorFadeUnit::publicMethods.GetFade)(i);
						auto fadeList = ColorFade::GetList(fade);
						Text("Size %d:", fadeList->numUsed); ImGui::TableNextColumn(); ImGui::TableNextColumn();

						int j = 0;
						for (auto iter = fadeList->root->next; iter != fadeList->root; iter = iter->next) {
							Text("  Data @ index %d:", j); ImGui::TableNextColumn(); ImGui::TableNextColumn();
							Text("    Resource offset: %p", iter->data.resourcePtr); ImGui::TableNextColumn();
							Text("    Flags: %x", iter->data.flags); ImGui::TableNextColumn();
							j++;
						}
					}

					ImGui::EndTable();
				}
				EndTabItem();
			}
			EndTabBar();
		}
	}

	End();
}

void DrawVsCharaPlayerPanel(VsCharaSelect::PlayerConditions* c) {
	sf4e::ui::Section("PLAYER SELECTION");
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
	sf4e::ui::Section("BATTLE LIFECYCLE");

	ImGui::Checkbox("Block initialization?", &fVsBattle::bBlockInitialization);
	ImGui::Checkbox("Block termination?", &fVsBattle::bBlockTermination);
	ImGui::Checkbox("Force next battle online?", &fVsBattle::bForceNextMatchOnline);
	ImGui::Checkbox("Skip results menu on next result?", &fVsBattle::bTerminateOnNextLeftBattle);
	ImGui::Checkbox("Override next random seed?", &fVsBattle::bOverrideNextRandomSeed);
	if (fVsBattle::bOverrideNextRandomSeed) {
		sf4e::ui::InputInt("Next match random seed", (int*)&fVsBattle::nextMatchRandomSeed);
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
	sf4e::ui::Section("CHARACTER SELECTION");

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
			sf4e::ui::InputText("Inject hovered character", charaNameTest, 4);
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
	sf4e::ui::Section("STAGE SELECTION");

	ImGui::Checkbox("Force timer on next vs-stage-select?", &fVsStageSelect::forceTimerOnNextStageSelect);
	sf4e::ui::InputText("Stage code to inject", newStageCode, 4);


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

void DrawHudWindow(bool* pOpen) {
	Begin(
		"HUD",
		pOpen,
		ImGuiWindowFlags_None
	);
	sf4e::ui::Section("HUD UPDATES");

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
	sf4e::ui::Section("STATE SNAPSHOTS");

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
			sf4e::ui::InputInt("Target memento ID", (int*)&targetID);

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
	sf4e::ui::Section("TASK TREE");

	int numActiveCores = TaskCoreRegistry::staticMethods.GetNumActiveCores();
	Text("Number of active task cores: %d", numActiveCores);
	for (int i = 0; i < numActiveCores; i++) {
		TaskCore* core = TaskCoreRegistry::staticMethods.GetCoreByIndex(i);
		char* name = (core->*TaskCore::publicMethods.GetName)();
		Text("Core ID: %d, name: %s", i, name);
	}

	End();
}


void sf4e::ui::DrawDeveloperOverlay(SelectionArt* art) {
    s_selectionArt = art;
    static int selected = 0;
    const char* labels[] = {"Characters", "Commands", "Events", "GFx", "HUD", "Main menu", "Mementos", "Input devices", "Sound", "System", "Tasks", "VFX", "Battle", "Character select", "Stage select"};
    ImGui::Combo("Inspector", &selected, labels, IM_ARRAYSIZE(labels));
    void (*draw[])(bool*) = {DrawCharaWindow, DrawCommandWindow, DrawEventWindow, DrawGFxAppWindow, DrawHudWindow, DrawMainMenuWindow, DrawMementoWindow, DrawPadWindow, DrawSoundWindow, DrawSystemWindow, DrawTaskWindow, DrawVfxWindow, DrawVsBattleWindow, DrawVsCharaSelectWindow, DrawVsStageSelectWindow};
    draw[selected](nullptr);
    s_selectionArt = nullptr;
}
