#pragma once

// Shared by the translation units that implement the developer inspector.
// Not a public interface. Begin and End below deliberately shadow
// ImGui::Begin and ImGui::End: every inspector body calls the unqualified
// names and must draw as a section of the fixed page, not as a window.
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

// One inspector per entry of the page selector in sf4e__DeveloperOverlay.cxx.
void DrawCharaWindow(bool* pOpen);
void DrawCommandWindow(bool* pOpen);
void DrawEventWindow(bool* pOpen);
void DrawGFxAppWindow(bool* pOpen);
void DrawMainMenuWindow(bool* pOpen);
void DrawPadWindow(bool* pOpen);
void DrawSoundWindow(bool* pOpen);
void DrawSystemWindow(bool* pOpen);
void DrawVfxWindow(bool* pOpen);
void DrawVsBattleWindow(bool* pOpen);
void DrawVsCharaSelectWindow(bool* pOpen);
void DrawVsStageSelectWindow(bool* pOpen);
void DrawHudWindow(bool* pOpen);
void DrawMementoWindow(bool* pOpen);
void DrawTaskWindow(bool* pOpen);
