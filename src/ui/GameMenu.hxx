#pragma once
#include "MenuNavigation.hxx"
#include "MenuFeedback.hxx"
#include "Theme.hxx"
#include <functional>
#include <imgui.h>
namespace sf4e { namespace ui {
class SelectionArt;
void SetMenuArt(SelectionArt* art);
void SetMenuGlyphs(int deviceType,unsigned selectPhysical,unsigned backPhysical);
// The caller scales the window font, so this takes no scale of its own.
void DrawTrainingOpenPrompt();
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
// Surfaces that draw their own text (the room board) report through the same
// probe, so the harness can check them for overflow too.
void ReportMenuText(const char* id,float textHeight,float interiorHeight,float textWidth,float availableWidth);
using MenuCardProbe = std::function<void(const char*,ImVec2,ImVec2)>;
void SetMenuCardProbe(MenuCardProbe probe);
using MenuStatusProbe = std::function<void(const char*,Tone)>;
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
    // The body renders in place of the list, so it is handed the same visual
    // feedback the list uses. Presentation only: dispatch still gates on the
    // live entry, never on the smoothed verdict.
    using Body = std::function<void(const std::vector<MenuEntry>&,MenuNavigation&,MenuAction&,float,const MenuVisualFeedback&)>;
    // Room status updates reserve a fixed area so asynchronous feedback cannot
    // move the controls under a mouse click or controller highlight.
    // statusTone is the status line's severity. The shell's only feedback
    // channel is this string, so a failure must not read like ordinary text.
    MenuAction Draw(const char* title,const std::vector<MenuEntry>& entries,
                    const char* status = "",const Detail& detail = {},int columns=1,const Card& card = {},const Body& body = {},float flyoutScale=0,float cardHeight=100,bool stableStatus=false,
                    Tone statusTone=Tone::Neutral,bool home=false);
    // A modal notice: owns menu input until Select, Back or OK dismisses it.
    // An empty heading is the error heading; advice supplies its own.
    // An alternative adds a second button beside OK; Left and Right choose it,
    // and onAlternative runs when the notice closes through it. The action
    // belongs to the notice that asked for it, so notices cannot claim each
    // other's outcome.
    void ShowNotice(std::string text,std::string heading={},std::string alternative={},std::function<void()> onAlternative={}) {
        notice_=std::move(text); noticeHeading_=std::move(heading); noticeAlternative_=std::move(alternative);
        noticeAlternativeAction_=std::move(onAlternative); noticeAlternativeSelected_=false;
    }
    bool NoticeOpen() const { return !notice_.empty(); }
private:
    // Clears the notice and runs its action exactly once, whichever of the
    // controller and pointer paths dismissed it.
    void DismissNotice(bool alternative) {
        notice_.clear(); noticeAlternativeSelected_=false;
        auto action=std::move(noticeAlternativeAction_); noticeAlternativeAction_=nullptr;
        if(alternative&&action) action();
    }
    std::string lastScreen_,lastFocus_,lastEdit_,notice_,noticeHeading_,noticeAlternative_;
    std::function<void()> noticeAlternativeAction_;
    bool noticeAlternativeSelected_=false;
    unsigned noticePrevious_=~0u;
    int lastFrame_ = -2;
    UiClock clock_;
    MenuVisualFeedback feedback_;
};
} }
