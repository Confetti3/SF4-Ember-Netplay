// The developer inspector page: the main menu, character select and stage
// select inspectors, and the selector that draws one inspector at a time.
#include "sf4e__DeveloperOverlay__Internal.hxx"

static sf4e::ui::SelectionArt* s_selectionArt = nullptr;
static sf4e::ui::FighterSelector s_fighterSelectors[3];

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


void sf4e::ui::DrawDeveloperOverlay(SelectionArt* art) {
    s_selectionArt = art;
    static int selected = 0;
    const char* labels[] = {"Characters", "Commands", "Events", "GFx", "HUD", "Main menu", "Mementos", "Input devices", "Sound", "System", "Tasks", "VFX", "Battle", "Character select", "Stage select"};
    ImGui::Combo("Inspector", &selected, labels, IM_ARRAYSIZE(labels));
    void (*draw[])(bool*) = {DrawCharaWindow, DrawCommandWindow, DrawEventWindow, DrawGFxAppWindow, DrawHudWindow, DrawMainMenuWindow, DrawMementoWindow, DrawPadWindow, DrawSoundWindow, DrawSystemWindow, DrawTaskWindow, DrawVfxWindow, DrawVsBattleWindow, DrawVsCharaSelectWindow, DrawVsStageSelectWindow};
    draw[selected](nullptr);
    s_selectionArt = nullptr;
}
