// Developer inspectors for battle state: characters, commands, VFX, HUD, the versus battle event and mementos.
#include "sf4e__DeveloperOverlay__Internal.hxx"

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
