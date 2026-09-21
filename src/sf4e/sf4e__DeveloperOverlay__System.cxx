// Developer inspectors for the engine: events, GFx, input devices, sound, the battle system and tasks.
#include "sf4e__DeveloperOverlay__Internal.hxx"

static int nExtraFramesToSimulate = 1;
static bool soundShowDetails = false;

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
