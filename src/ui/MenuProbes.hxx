#pragma once
#include "MenuNavigation.hxx"
#include "Theme.hxx"
#include <functional>
#include <imgui.h>
namespace sf4e { namespace ui {
// Observers the render harness and tests install to read what a menu drew.
// Every menu surface reports through these; none affects drawing.
using MenuTextProbe = std::function<void(const char*,float,float,float,float)>;
void SetMenuTextProbe(MenuTextProbe probe);
// Surfaces that draw their own text (the room board) report through the same
// probe, so the harness can check them for overflow too.
void ReportMenuText(const char* id,float textHeight,float interiorHeight,float textWidth,float availableWidth);
using MenuCardProbe = std::function<void(const char*,ImVec2,ImVec2)>;
void SetMenuCardProbe(MenuCardProbe probe);
// The room board reports its cards and their option strips the same way.
void ReportMenuCard(const char* id,ImVec2 min,ImVec2 max);
using MenuStatusProbe = std::function<void(const char*,Tone)>;
void SetMenuStatusProbe(MenuStatusProbe probe);
using MenuEntriesProbe = std::function<void(const std::vector<MenuEntry>&)>;
void SetMenuEntriesProbe(MenuEntriesProbe probe);
using PortraitProbe = std::function<void(int,ImVec2,ImVec2)>;
void SetPortraitProbe(PortraitProbe probe);
} }
