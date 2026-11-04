// The developer inspector page: the main menu, character select and stage
// select inspectors, and the selector that draws one inspector at a time.
#include "sf4e__DeveloperOverlay__Internal.hxx"
#include "sf4e__GameThemes.hxx"

static sf4e::ui::SelectionArt* s_selectionArt = nullptr;
static sf4e::ui::FighterSelector s_fighterSelectors[3];

static bool mainMenuShouldJump = false;
static int mainMenuCharaIDs[2] = { 0 };
static Dimps::GameEvents::VsMode::ConfirmedCharaConditions mainMenuJumpCharaConditions[2] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, (BYTE)rBattle::ED_USF4 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, (BYTE)rBattle::ED_USF4 },
};
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
	mainMenuJumpStageID = sf4e::selection::NormalizeStage(mainMenuJumpStageID);
	if (!fVsPreBattle::SetSides(mainMenuJumpCharaConditions, mainMenuJumpStageID)) {
		spdlog::error("Overlay: VsPreBattle tasks registered, but the current foreground event isn't VSMode!");
		return;
	}

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
		if (!mainMenuShouldJump) {
			char* mainMenuQuery[1] = { "MainMenu" };
			rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), mainMenuQuery, 1);
			(rMainMenu::ToItemObserver(mainMenu)->*rMainMenu::itemObserverMethods.GoToVersusMode)();
		}
		else {
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
			fVsPreBattle::VersusSettings settings;
			settings.editionSelect = mainMenuEditionSelect;
			settings.rounds = roundCountList[mainMenuRoundCountIdx].first;
			settings.timeLimit = roundTimeList[mainMenuRoundTimeIdx].first;
			fVsPreBattle::GoToVersus(settings, _OnPreBattleTasksRegistered);
		}
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


// In-game themes, one list a part: a pick saves as Settings' row does and shows
// at once, a battle HUD in a Training fight by loading that battle again.
// Each part's tab: its name, where its files show, and whether Versus mode
// shows them (character select, stage select, the versus screen).
struct ThemeTab { const char* name; const char* where; bool versus; };
static const ThemeTab themeTabs[sf4e::netplay::GameThemePartCount] = {
	{"Main menu", "Shows at once on the main menu.", false},
	{"Title", "Shows on the title screen (leave the main menu, or start the game again).", false},
	{"Character select", "Shows on character select.", true},
	{"Stage select", "Shows on stage select.", true},
	{"Versus", "Shows on the versus and loading screen.", true},
	{"Pause menu", "Shows when a fight is paused.", false},
	{"Network", "Shows on the game's own network screens.", false},
	{"Edition marks", "Shows on character select's edition marks.", true},
	{"Buttons", "Shows in button prompts and icons across menus.", false},
	{"Menu sounds", "Plays in menus; may need the game started again.", false},
	{"Battle HUD", "In Training the battle loads again; elsewhere the next battle shows it.", false},
};

// Test all: every theme part with no verdict yet, a screen at a time. Show
// previews one and takes the game to where it shows; a verdict saves it and
// shows the next. The count of files the game opened from ember\theme since
// Show tells a theme that loaded from one the game never read.
static void DrawThemeTester(const sf4e::NetplayFacade::RuntimeSnapshot& snapshot, bool training) {
	using namespace sf4e::netplay;
	struct Item { int part; std::string name; };
	static std::vector<Item> items;
	static size_t at = 0;
	static bool shown = false;
	if (!ImGui::CollapsingHeader("Test all")) return;
	if (ImGui::Button(items.empty() ? "Start: list the themes with no verdict" : "List again")) {
		items.clear(); at = 0; shown = false;
		// Screens the main menu reaches first, Training's last.
		for (int part : { GameThemeMainMenu, GameThemeTitle, GameThemeCharaSelect, GameThemeStageSelect, GameThemeVersus, GameThemeEditionMarks,
			GameThemeButtons, GameThemeMenuSounds, GameThemeNetwork, GameThemePauseMenu, GameThemeBattleHud })
			for (const auto& name : snapshot.gameThemes[part])
				if (sf4e::GameThemes::Verdict(name).empty()) items.push_back({ part, name });
	}
	while (at < items.size() && !sf4e::GameThemes::Verdict(items[at].name).empty()) { ++at; shown = false; }
	if (items.empty()) return;
	if (at >= items.size()) { Text("All %zu judged.", items.size()); return; }
	const Item& item = items[at];
	const ThemeTab& tab = themeTabs[item.part];
	Text("%zu of %zu: %s", at + 1, items.size(), item.name.c_str());
	Text("%s. %s", tab.name, tab.where);
	// Where the game must be, and the main menu row that takes it to the screen.
	const bool inTraining = item.part == GameThemePauseMenu || item.part == GameThemeBattleHud;
	const int row = item.part == GameThemeTitle ? 0 : tab.versus ? 2 : inTraining && !training ? 4 : -1;
	const bool reachable = row < 0 || snapshot.atMainMenu;
	auto show = [&] {
		if (row == 4) {
			sf4e::GameEvents::TrainingSides sides;
			for (auto& side : sides.sides) { side.charaID = 0; side.unc_edition = Dimps::Game::Battle::ED_USF4; }
			sf4e::GameEvents::TrainingMode::Choose(sides);
		}
		sf4e::GameThemes::Preview(static_cast<GameThemePart>(item.part), item.name, row);
		shown = true;
	};
	if (!reachable) Text(training ? "Leave Training for the main menu to show this one." : "Go back to the main menu to show this one.");
	ImGui::BeginDisabled(!reachable);
	if (ImGui::Button(shown ? "Show again" : "Show")) show();
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("skip")) { ++at; shown = false; }
	if (shown) {
		const int opens = sf4e::GameThemes::ThemeFileOpens();
		if (opens) Text("The game opened %d files from the theme (last: %s)", opens, sf4e::GameThemes::LastThemeFile().c_str());
		else Text("The game has not opened a file from the theme yet");
		for (const char* verdict : { "keep", "fix", "remove" }) {
			if (ImGui::Button(verdict)) {
				sf4e::GameThemes::Judge(item.name, verdict);
				++at; shown = false;
				break;
			}
			ImGui::SameLine();
		}
		ImGui::NewLine();
	}
	ImGui::Separator();
}

void sf4e::ui::DrawGameThemesPicker() {
	using sf4e::netplay::GameThemePart;
	const auto frame = sf4e::NetplayFacade::GetPresentationSnapshotShared();
	const auto& snapshot = *frame->runtime;
	// Where settings are locked (Training, Versus) a pick is shown, not saved.
	static std::optional<std::string> previewed[sf4e::netplay::GameThemePartCount];
	if (snapshot.canEditPreferences) for (auto& p : previewed) p.reset();
	else Text("Preview only: picks here are not saved");
	// The tab for where the game is opens by itself: the main menu's there,
	// the battle HUD in Training.
	const bool training = snapshot.menuContext == sf4e::input::MenuContext::OfflineTraining;
	const int here = snapshot.atMainMenu ? sf4e::netplay::GameThemeMainMenu : training ? sf4e::netplay::GameThemeBattleHud : -1;
	static int shownFor = -2;
	const bool select = here != shownFor;
	shownFor = here;
	DrawThemeTester(snapshot, training);
	if (!ImGui::BeginTabBar("parts", ImGuiTabBarFlags_FittingPolicyScroll)) return;
	for (int part = 0; part < sf4e::netplay::GameThemePartCount; ++part) {
		const auto& themes = snapshot.gameThemes[part];
		if (themes.empty()) continue;
		const ThemeTab& tab = themeTabs[part];
		if (!ImGui::BeginTabItem(tab.name, nullptr, select && part == here ? ImGuiTabItemFlags_SetSelected : 0)) continue;
		ImGui::TextWrapped("%s", tab.where);
		const std::string used = previewed[part].value_or(snapshot.preferences.gameThemes[part]);
		std::string picked = used;
		if (ImGui::BeginListBox("##themes", ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing() * 2))) {
			if (ImGui::Selectable("(the game's own)", used.empty())) picked.clear();
			for (const auto& name : themes) {
				const std::string verdict = sf4e::GameThemes::Verdict(name);
				const std::string label = (verdict.empty() ? name : name + "  [" + verdict + "]") + "##" + name;
				if (ImGui::Selectable(label.c_str(), name == used)) picked = name;
			}
			ImGui::EndListBox();
		}
		// The theme in use is judged here: kept, to be fixed, or moved out.
		if (!used.empty() && std::find(themes.begin(), themes.end(), used) != themes.end()) {
			for (const char* verdict : { "keep", "fix", "remove" }) {
				if (ImGui::Button(verdict)) sf4e::GameThemes::Judge(used, verdict);
				ImGui::SameLine();
			}
			Text("%s", used.c_str());
		}
		if (tab.versus && snapshot.atMainMenu && ImGui::Button("Show it: open Versus mode")) {
			char* query[1] = { "MainMenu" };
			if (rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), query, 1))
				(rMainMenu::ToItemObserver(mainMenu)->*rMainMenu::itemObserverMethods.GoToVersusMode)();
		}
		ImGui::EndTabItem();
		if (picked == used) continue;
		if (!snapshot.canEditPreferences) {
			previewed[part] = picked;
			sf4e::GameThemes::Preview(static_cast<GameThemePart>(part), picked);
			continue;
		}
		sf4e::NetplayFacade::RuntimeCommand request;
		request.command.kind = sf4e::netplay::CommandKind::SavePreferences;
		request.command.generation = snapshot.session.generation;
		request.preferences = snapshot.preferences;
		request.preferences.gameThemes[part] = picked;
		sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(request));
	}
	ImGui::EndTabBar();
}

void DrawGameThemesWindow(bool* pOpen) {
	Begin("In-game themes", pOpen, ImGuiWindowFlags_None);
	sf4e::ui::DrawGameThemesPicker();
	End();
}

void sf4e::ui::DrawDeveloperOverlay(SelectionArt* art) {
    s_selectionArt = art;
    static int selected = 0;
    const char* labels[] = {"Characters", "Commands", "Events", "GFx", "HUD", "In-game themes", "Main menu", "Mementos", "Input devices", "Sound", "System", "Tasks", "VFX", "Battle", "Character select", "Stage select"};
    ImGui::Combo("Inspector", &selected, labels, IM_ARRAYSIZE(labels));
    void (*draw[])(bool*) = {DrawCharaWindow, DrawCommandWindow, DrawEventWindow, DrawGFxAppWindow, DrawHudWindow, DrawGameThemesWindow, DrawMainMenuWindow, DrawMementoWindow, DrawPadWindow, DrawSoundWindow, DrawSystemWindow, DrawTaskWindow, DrawVfxWindow, DrawVsBattleWindow, DrawVsCharaSelectWindow, DrawVsStageSelectWindow};
    draw[selected](nullptr);
    s_selectionArt = nullptr;
}
