#pragma once
#include "MenuNavigation.hxx"
#include "MenuFeedback.hxx"
#include "Theme.hxx"
#include "MenuGlyphs.hxx"
#include "MenuDialogs.hxx"
#include "MenuProbes.hxx"
#include <functional>
#include <imgui.h>
namespace sf4e { namespace ui {
class SelectionArt;
void SetMenuArt(SelectionArt* art);
// The fallbacks name a button with no known glyph: the game's LP/LK binding,
// or the recovery window's stick buttons "1"/"2". They must outlive the call.
void SetMenuGlyphs(int deviceType,unsigned selectPhysical,unsigned backPhysical,const char* selectFallback="LP",const char* backFallback="LK");
struct PlayerCardView {
    std::string name, fighterName;
    int fighter=0, inputDelay=0, members=0, activeTables=0;
    bool controllerReady=false, connected=false;
    std::uint64_t wins=0,losses=0;
    bool recordAvailable=true;
};
void SetMenuPlayerCard(PlayerCardView view);
void DrawMainPortrait(int fighter,bool saved,ImVec2 min,ImVec2 max);
// A card's corner marker (SAVED, MAIN), fitted to the card's width. `at` is its
// top-left corner, or its top-right when rightAligned. Returns the badge's drawn width.
float DrawCardBadge(ImVec2 at,float width,const char* text,const char* probe,bool rightAligned=false);
// USFIV's select grid for the fighter pickers: 15 across (RosterDisplayOrder) when
// the list is wide enough for readable cards, else the width-based columns in the
// same order. cardHeight is in unscaled pixels, as GameMenu::Draw takes it;
// listShare is the part of a wide window the list takes (GameMenu::wideListShare).
struct RosterGrid { int columns; float cardHeight; float listShare; };
RosterGrid LayOutRosterGrid(float windowWidth);
// `backing` fills the tile behind the art, which is transparent around the fighter.
void DrawCharacterPortrait(int fighter,ImVec2 min,ImVec2 max,ImU32 backing=IM_COL32(38,34,30,255));
// Set once per overlay frame. Only the visible player screen consumes it.
void SetMenuInput(MenuInput input);
MenuInput ReadMenuInput();
// The MenuInput bits of the menu keys held now; ReadMenuInput adds them to
// the pad's, and surfaces that only need to know whether the keyboard was
// used read them alone. While a text field has the keyboard only the arrows,
// Enter and Escape count, so typing never presses a menu key.
unsigned KeyboardMenuBits();
// True while the legend shows keyboard keys: the gameplay device is the
// keyboard, or a key moved the menu after the pad last did.
bool KeyboardPrompts();
// An embedded screen (fighter select, the training flyout) hands its parent
// what it does not handle itself: Close, or a shortcut the parent owns.
void ForwardMenuAction(MenuAction action);
MenuAction TakeForwardedMenuAction();
// What the parent tells an embedded screen before drawing it: where its Back
// from the root goes, and the parent's shortcuts it forwards and so advertises.
// fresh: the parent has just opened this screen, so it starts at its first
// page; TakeEmbeddedFresh reads it once. openOn: the parent opened fighter
// select for one change ("roster", "ultra", "costumes", "stage" or "options"),
// so it starts on that page and hands Close back once the pick is made, or on
// Back from that page.
struct EmbeddedReturn { std::string exitName; std::vector<LegendHint> shortcutHints; bool fresh=false; std::string openOn; };
void SetEmbeddedReturn(EmbeddedReturn context);
const EmbeddedReturn& EmbeddedReturnContext();
bool TakeEmbeddedFresh();
class GameMenu {
public:
    MenuNavigation navigation;
    // The Xbox X/Y/View hints for the screen being drawn; other devices have
    // no such buttons, so they are shown only with the A/B glyphs.
    std::vector<LegendHint> shortcutHints;
    // What Back does on this screen when it is not the usual return; empty
    // for Back.
    std::string backHint;
    // The display name of the navigation's root screen, used by its children's
    // Back button; empty uses MenuScreenLabel. An embedded menu names itself
    // ("Fighter select"), not the shell's "Home".
    std::string rootName;
    // Where Back from the root screen goes ("Room", "SF4"). The root's header
    // button reads "< Back / exitName", or backHint when there is no exitName;
    // Home shows its header button only with one.
    std::string exitName;
    // A stable status is normally one or two lines beside or under the header.
    // With this set it is drawn whole, wrapped, in a block that grows to its
    // text (up to most of the window, then it scrolls), so a long launcher
    // message keeps the paths and steps it names.
    bool fitStatus=false;
    // The corner radius, in unscaled pixels, of the rows and grid cells that a Card
    // draws; 0 keeps them square. A card of rounded corners paints its own focus outline.
    float cardRounding=0;
    // When above 0, a narrow layout's detail pane (above the list) takes at most this
    // many lines and leaves out the row's label, which its card already shows.
    int compactDetailLines=0;
    // The part of a wide window the list takes beside its detail pane.
    float wideListShare=.53f;
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
    // Home's help line and status line, reserved whether or not they are empty.
    static constexpr float HomeStatusHeight=36;
    // Parts of Draw. The dialogs and their helpers (ConfirmationButtons,
    // AnswerConfirmation) live in MenuDialogs.cxx.
    // An open dialog owns the legend and the header: Select names its
    // highlighted button and Back what dismissing it does. False when none is open.
    bool DialogLegend(const std::vector<MenuEntry>& entries,std::string& select,std::string& back) const;
    std::vector<DialogButton> ConfirmationButtons(const std::vector<MenuEntry>& entries) const;
    void AnswerConfirmation(const std::vector<MenuEntry>& entries,int clicked,int selected,MenuAction& action);
    void DrawHomeStatusLine(const std::vector<MenuEntry>& entries,const char* status,Tone statusTone,float homeMargin);
    // Veils the body between top and bottom (screen y), not the header or legend.
    void DrawFlyoutConfirmation(const std::vector<MenuEntry>& entries,float unit,float top,float bottom,MenuAction& action);
    void DrawConfirmationModal(const std::vector<MenuEntry>& entries,MenuAction& action);
    // A reader in place of the list; held Up/Down scroll it.
    void DrawReader(const std::vector<MenuEntry>& entries,const Detail& detail,float height,unsigned held);
    void DrawChoiceModal(const std::vector<MenuEntry>& entries,MenuAction& action);
    void DrawNoticeModal(bool noticeOpen);
    void DrawEditModal(const std::vector<MenuEntry>& entries,bool acceptEditText,MenuAction& action);
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
    std::size_t lastChoice_ = ~std::size_t(0);
    UiClock clock_;
    MenuVisualFeedback feedback_;
};
} }
