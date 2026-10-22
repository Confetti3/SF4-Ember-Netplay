#pragma once
#include "MenuNavigation.hxx"
#include "MenuFeedback.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "MenuGlyphs.hxx"
#include "MenuDialogs.hxx"
#include "MenuProbes.hxx"
#include "../common/Country.hxx"
#include <functional>
#include <imgui.h>
namespace sf4e { namespace ui {
class SelectionArt;
void SetMenuArt(SelectionArt* art);
// The art SetMenuArt gave, or null; the passive HUD draws its prompts from it.
SelectionArt* MenuArt();
// The fallbacks name a button with no known glyph: the game's menu functions
// LK and MK on a DirectInput stick, a raw input's name in the pad setup
// summary, or the recovery window's stick buttons "1"/"2". They are copied.
void SetMenuGlyphs(int deviceType,unsigned selectPhysical,unsigned backPhysical,const char* selectFallback="LK",const char* backFallback="MK",gamepads::PromptBrand brand={});
// The device's menu buttons as Pad::MenuButtons answers them, glyphs included.
void SetMenuGlyphs(int deviceType,const input::MenuButtons& buttons,gamepads::PromptBrand brand={});
struct PlayerCardView {
    std::string name, fighterName;
    int fighter=0, inputDelay=0, members=0, activeTables=0;
    bool controllerReady=false, connected=false;
    std::uint64_t wins=0,losses=0;
    bool recordAvailable=true;
    // The flag before the name: the country the player shows, or none.
    CountryCode country;
    // V2: the card as the L tag draws it (title, icon, facts).
    v2::TagView profile;
};
void SetMenuPlayerCard(PlayerCardView view);
const PlayerCardView& MenuPlayerCard();
// V2 Home's news: the newest releases, a title and a "date · version" line each.
struct MenuNewsItem { std::string title, meta; };
void SetMenuNews(std::vector<MenuNewsItem> news);
const std::vector<MenuNewsItem>& MenuNews();
// The legend's Select and Back glyphs for the device in use (SetMenuGlyphs).
const char* MenuSelectGlyph();
const char* MenuBackGlyph();
// The menu art behind a full-window menu, under a tint; false without art.
bool DrawMenuBackdrop(bool v2);
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
// Editors own keyboard Select and acceptance; pad Select still reaches them.
MenuInput ReadMenuInput(bool textEditing=false,bool typesText=false,unsigned additionalPad=0);
// The MenuInput bits of the menu keys held now; ReadMenuInput adds them to
// the pad's, and surfaces that only need to know whether the keyboard was
// used read them alone. While a text field has the keyboard only the arrows,
// Enter and Escape count, so typing never presses a menu key; typesText
// says a field holds the keyboard even before ImGui has activated it.
unsigned KeyboardMenuBits(bool typesText=false);
// True while the legend shows keyboard keys: the gameplay device is the
// keyboard, or a key moved the menu after the pad last did.
bool KeyboardPrompts();
// While no menu reads input (the training HUD over a fight), the prompts
// still follow the device last used: a press of the pad's (padHeld, its
// ControllerSample buttons), or a menu key or F-key.
void NoteMenuDevice(unsigned padHeld);
// The device the prompts are drawn for: PadKeyboard, or the pad's type.
int MenuPromptDevice();
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
// What a notice's button does when it closes the notice. The shell reads it
// from GameMenu::TakeNoticeResult and acts on it. Dismissed is Back or the
// header's Close; Acknowledged is a button that only closes it.
enum class NoticeResult { None, Dismissed, Acknowledged, CreatePublicRoom, HideGameSettings, OpenButtonLayout, LeaveButtonLayout, UpdateNow, SkipUpdate,
    // A BluMint match's notices (BluMintMoments.hxx): ready for the next game, change fighter first, play the next match, leave the room.
    BluMintReady, BluMintFighter, BluMintNext, BluMintLeave };
struct NoticeButton { std::string label; NoticeResult result=NoticeResult::Acknowledged; };
// selected: the button highlighted when the notice opens (the first by default).
// V2 shows `eyebrow` beside the brand's mark, `chip` at the top right and
// `facts` (a score line) boxed over the text; Classic shows facts as a line.
struct NoticeDialog { std::string text,heading; std::vector<NoticeButton> buttons; int selected=0; std::string eyebrow,facts; v2::HeaderChip chip{}; };
class GameMenu {
public:
    MenuNavigation navigation;
    // The Xbox X/Y/View hints for the screen being drawn; other devices have
    // no such buttons, so they are shown only for an Xbox pad (or the keyboard),
    // less any button the player made Select or Back.
    std::vector<LegendHint> shortcutHints;
    // A text box on this screen holds the keyboard whether or not ImGui has
    // activated it this frame, so letters, Space, Backspace and Delete are
    // text and never menu keys.
    bool typesText = false;
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
    // Back does nothing on this screen (Home inside a room), so the legend
    // does not offer it.
    bool backInert = false;
    // The menu art and its shade behind a full-window menu; left out where
    // what is under the window shows through it.
    bool backdrop = true;
    // Small print in the bottom right corner, beside the legend or on a line
    // of its own under it when the two would crowd each other: Home's (and the
    // launcher window's) version and channel, in Muted. Empty for none.
    std::string footerNote;
    // A stable status is normally one or two lines beside or under the header.
    // With this set it is drawn whole, wrapped, in a block that grows to its
    // text (up to most of the window, then it scrolls), so a long launcher
    // message keeps the paths and steps it names.
    bool fitStatus=false;
    // V2 only: the tabs over this screen, each a screen id and its label, and
    // the one shown. Pressing one, or LB/RB (Page Up/Down) to a neighbour, returns a
    // Shortcut whose id names it; the owner switches. Empty for none.
    std::vector<std::pair<std::string,std::string>> tabs;
    // V2 only: the words over the title (where the screen sits, as "BLUMINT · WINNERS ROUND 1") and the
    // chips at the header's right end. Empty for none.
    std::string eyebrow;
    std::vector<v2::HeaderChip> headerChips;
    // V2 only: the cards' width (unscaled) beside the detail pane, each card with the
    // first line of its detail and its value as a chip (Online play, Join); 0 for the usual split.
    float listWidth=0;
    // V2 only: the detail pane's width (unscaled) beside the cards (Public rooms); 0 for the usual split.
    float paneWidth=0;
    // V2 only: a body's focused card and the column it sits in, where a value's
    // choices open as they do on the cards. The body sets them each frame.
    struct { v2::Box card, column; } bodyChoices;
    int tab=-1;
    bool listEnabled=true, ownsBack=false, customDetail=false;
    unsigned additionalInput=0;
    // The corner radius, in unscaled pixels, of the rows and grid cells that a Card
    // draws; 0 keeps them square. A card of rounded corners paints its own focus outline.
    float cardRounding=0;
    // When above 0, a narrow layout's detail pane (above the list) takes at most this
    // many lines and leaves out the row's label, which its card already shows.
    int compactDetailLines=0;
    // The part of a wide window the list takes beside its detail pane.
    float wideListShare=.53f;
    // A custom detail can request more room above a compact list, leaving one row visible.
    float compactDetailHeight=0;
    // A card over the list while the screen waits for something outside the
    // menu (a button capture): measure gives the height its content needs at a
    // content width, draw fills it. It sits centred on the list, or centred in
    // the larger band beside the focused row when it would cover that row, so
    // the row stays readable. Presentation only; empty draws none.
    struct ListCard { std::function<float(float)> measure; std::function<void()> draw; };
    ListCard listCard;
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
    // A modal notice: owns menu input until a button, Back or the header's Close
    // ends it. A notice has one or more buttons, each with its own result;
    // Left and Right choose between them, the first is highlighted when it
    // opens, and Select takes the highlighted one. Back always declines
    // (NoticeResult::Dismissed). The result waits for TakeNoticeResult, which
    // the notice's owner reads after the menu has drawn, so a notice can never
    // claim another's outcome: opening one drops a result nobody took.
    // An empty heading reads "Notice"; only ShowError gives the error heading
    // and colour.
    void ShowNotice(NoticeDialog notice);
    // The usual notice: one button, OK unless okLabel names it.
    void ShowNotice(std::string text,std::string heading={},std::string okLabel={});
    // Something failed: the notice says so in its heading.
    void ShowError(std::string text) { ShowNotice(std::move(text)); noticeError_=true; }
    bool NoticeError() const { return !notice_.empty()&&noticeError_; }
    // Closes the notice without an answer.
    void CloseNotice() { notice_.clear(); noticeSelected_=0; noticeResult_=NoticeResult::None; navigation.NeutralGate(); }
    bool NoticeOpen() const { return !notice_.empty(); }
    // How the last notice closed, once; None while it is open or nobody closed one.
    NoticeResult TakeNoticeResult() { const auto result=noticeResult_; noticeResult_=NoticeResult::None; return result; }
    // The menu buttons held this frame (a notice that owns the input holds none), for a body
    // that scrolls with them.
    unsigned Held() const { return held_; }
private:
    // Home's help and status lines, reserved whether or not they are empty:
    // three when the window is tall enough, so a row's columns can put their
    // labels over their values, else two, where the columns give way to the
    // row's one-line detail. Unscaled height; it reads the current window.
    static constexpr float HomeLineHeight=18;
    static int HomeLines();
    // The V2 look's screen (GameMenuV2.cxx): the same entries, input and
    // dialogs as Draw, laid out as the V2 design's cards, panes and hint bar.
    MenuAction DrawV2(const char* title,const std::vector<MenuEntry>& entries,const char* status,const Detail& detail,int columns,const Card& card,
        const Body& body,float cardHeight,Tone statusTone,bool home,bool flyout,MenuAction action,unsigned held,bool modalAtStart,bool choosingAtStart,bool acceptEditText,bool noticeOpen);
    // The dialogs over the screen, then the frame's bookkeeping and its answer; both looks end here.
    MenuAction Finish(const std::vector<MenuEntry>& entries,MenuAction action,bool backRequested,bool acceptEditText,bool body,bool noticeOpen);
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
    // listCard over the list (listMin to listMax), clear of the focused row (rowMin to rowMax) where it can be.
    void DrawListCard(ImVec2 listMin,ImVec2 listMax,ImVec2 rowMin,ImVec2 rowMax,float unit);
    void DrawConfirmationModal(const std::vector<MenuEntry>& entries,MenuAction& action);
    // A reader in place of the list; held Up/Down scroll it.
    void DrawReader(const std::vector<MenuEntry>& entries,const Detail& detail,float height,unsigned held);
    void DrawChoiceModal(const std::vector<MenuEntry>& entries,MenuAction& action);
    void DrawNoticeModal(bool noticeOpen);
    void ReadEditInput(const std::vector<MenuEntry>& entries,MenuInput& input) const;
    void DrawEditModal(const std::vector<MenuEntry>& entries,bool acceptEditText,MenuAction& action);
    // Closes the notice with the result of the button, or Back, that did it.
    void DismissNotice(NoticeResult result) { notice_.clear(); noticeSelected_=0; noticeResult_=result; }
    // Where an open popout sits (V2 Values); set on the frame it opens.
ImVec2 popoutAt_{};bool popoutOpen_=false;
std::string lastScreen_,lastFocus_,lastEdit_,notice_,noticeHeading_,noticeEyebrow_,noticeFacts_;
v2::HeaderChip noticeChip_{};
    std::vector<NoticeButton> noticeButtons_;
    int noticeSelected_=0;
    NoticeResult noticeResult_=NoticeResult::None;
    bool noticeError_=false;
    unsigned noticePrevious_=~0u;
    int lastFrame_ = -2;
    unsigned held_ = 0;
    std::size_t lastChoice_ = ~std::size_t(0);
    UiClock clock_;
    MenuVisualFeedback feedback_;
};
} }
