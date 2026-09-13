#pragma once
#include "MenuNavigation.hxx"
#include <functional>
#include <imgui.h>
namespace sf4e { namespace ui {
class SelectionArt;
void SetMenuArt(SelectionArt* art);
void SetMenuGlyphs(int deviceType,unsigned selectPhysical,unsigned backPhysical);
void DrawTrainingOpenPrompt(float scale);
struct PlayerCardView {
    std::string name, fighterName;
    int fighter=0, inputDelay=0, members=0, activeTables=0;
    bool controllerReady=false, connected=false;
    std::uint64_t wins=0,losses=0;
    bool recordAvailable=true;
};
void SetMenuPlayerCard(PlayerCardView view);
void DrawMainPortrait(int fighter,bool saved,ImVec2 min,ImVec2 max);
void DrawCharacterPortrait(int fighter,ImVec2 min,ImVec2 max);
// Optional geometry observer used by the renderer regression harness.
using MenuTextProbe = std::function<void(const char*,float,float,float,float)>;
void SetMenuTextProbe(MenuTextProbe probe);
using MenuCardProbe = std::function<void(const char*,ImVec2,ImVec2)>;
void SetMenuCardProbe(MenuCardProbe probe);
using MenuStatusProbe = std::function<void(const char*)>;
void SetMenuStatusProbe(MenuStatusProbe probe);
using MenuEntriesProbe = std::function<void(const std::vector<MenuEntry>&)>;
void SetMenuEntriesProbe(MenuEntriesProbe probe);
using PortraitProbe = std::function<void(int,ImVec2,ImVec2)>;
void SetPortraitProbe(PortraitProbe probe);
// Set once per overlay frame. Only the visible player screen consumes it.
void SetMenuInput(MenuInput input);
MenuInput ReadMenuInput();
void RequestMenuReturn();
bool TakeMenuReturn();
class GameMenu {
public:
    MenuNavigation navigation;
    using Detail = std::function<void(const std::string&)>;
    // Return false for ordinary actions embedded in an artwork grid, so their
    // labels still render (for example, Retry saving after a portrait failure).
    using Card = std::function<bool(const MenuEntry&, ImVec2, ImVec2)>;
    using Body = std::function<void(const std::vector<MenuEntry>&,MenuNavigation&,MenuAction&,float)>;
    // Room status updates reserve a fixed area so asynchronous feedback cannot
    // move the controls under a mouse click or controller highlight.
    MenuAction Draw(const char* title,const std::vector<MenuEntry>& entries,
                    const char* status = "",const Detail& detail = {},int columns=1,const Card& card = {},const Body& body = {},float flyoutScale=0,float cardHeight=100,bool stableStatus=false);
private:
    std::string lastScreen_,lastFocus_,lastEdit_;
    int lastFrame_ = -2;
};
} }
