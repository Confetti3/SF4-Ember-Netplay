#pragma once
#include "MenuNavigation.hxx"
#include "MenuFeedback.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "MenuGlyphs.hxx"
#include "MenuDialogs.hxx"
#include "MenuProbes.hxx"
#include "MenuSpeech.hxx"
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
// The player's speech settings, which every menu reads as it draws.
void SetMenuSpeech(const netplay::SpeechPreferences& speech);
// How many descriptions the accessibility button has read since the last call.
int TakeMenuDescribes();
// Says a notice that shows on its own (a toast) once, after what speech is saying, with speech on.
void SayMenuNotice(const std::string& text);
// The accessibility button's name on the device the player last used ("RT", "R2", "F2").
std::string AccessibilityKeyName();
struct PlayerCardView {
    std::string name;
    // The flag before the name: the country the player shows, or none.
    CountryCode country;
    // The card as the L tag draws it (title, icon, facts).
    v2::TagView profile;
};
void SetMenuPlayerCard(PlayerCardView view);
const PlayerCardView& MenuPlayerCard();
// Home's news: the newest releases, a title and a "date · version" line each.
struct MenuNewsItem { std::string title, meta; };
void SetMenuNews(std::vector<MenuNewsItem> news);
// Value rows (the Settings board): On and Off as a switch Select flips, a
// number between the arrows that step it, anything else its value and a
// chevron, Select opening every choice in a popup. DrawMenuValue draws one
// right-aligned in `box`.
bool MenuValueIsToggle(const MenuEntry& e);
void DrawMenuValue(ImDrawList* draw, const MenuEntry& e, v2::Box box, bool enabled, ImU32 text);
// A value row's words on a board (Create, Settings, Fighter select): the
// label `inset` from the left, and at the right the value as FitMenuRow
// shares the row (DrawMenuValue), or a fixed value and the chevron when Select
// opens it; a value under its label takes the row's last UnderLine.
void DrawMenuRow(ImDrawList* draw, const MenuEntry& e, v2::Box row, bool enabled, v2::Share value, float inset);
// The width a row's value wants: a switch its own, a value its widest word
// (its value or any of its choices) with the arrows or chevron.
float MenuValueWidth(const MenuEntry& e);
// How a value row's label and value share its `room` (v2::ShareLine): the
// value wants MenuValueWidth and gives way down to a floor that does not
// depend on its word now (a control's under half the row, a fixed value's its
// chevron's place), so rows never move as values change. Lay the row
// out with the result: its value to PressEntry and DrawMenuRow, and the row
// v2::UnderLine taller when the value goes under the label.
v2::Share FitMenuRow(const MenuEntry& e, float room, float gap, bool grows = true);
// A row's box on a board as a button: the moving pointer focuses the row,
// and a press focuses it and is Select on it, or steps its value when it lands
// on one end of the `valueWidth` at the row's right (14 px in from its edge).
// Nothing answers while a question or the keyboard is open. With `scroll` the
// focused row scrolls into view. With `choose` false a press is only reported,
// for a box that means something else when pressed (a panel's head).
struct EntryHit { bool pressed, hovered; };
EntryHit PressEntry(MenuNavigation& nav, const std::vector<MenuEntry>& rows, const MenuEntry& e, v2::Box b, MenuAction& action,
    bool scroll = false, float valueWidth = 0, bool choose = true);
const std::vector<MenuNewsItem>& MenuNews();
// The legend's Select and Back glyphs for the device in use (SetMenuGlyphs).
const char* MenuSelectGlyph();
const char* MenuBackGlyph();
// The glyph of a shortcut press (MenuInput::Fighter, Options, Chat, Leave,
// PrevTab, NextTab) on the device the prompts are drawn for, or null where it
// has no such button (ControllerNavigation.hxx, MenuShortcuts).
const char* MenuShortcutGlyph(unsigned press);
// A hint for each of these shortcut presses the device has, each a button
// that presses it; the others are left out.
std::vector<LegendHint> ShortcutHints(const std::vector<std::pair<unsigned, std::string>>& shortcuts);
// The menu art behind a full-window menu, under a tint; false without art.
bool DrawMenuBackdrop();
void DrawMainPortrait(int fighter,bool saved,ImVec2 min,ImVec2 max);
// A card's corner marker (SAVED, MAIN), fitted to the card's width. `at` is its
// top-left corner, or its top-right when rightAligned. Returns the badge's drawn width.
float DrawCardBadge(ImVec2 at,float width,const char* text,const char* probe,bool rightAligned=false);
// The fighter pickers' select grid (RosterDisplayOrder): up to eight across beside
// the preview, fewer on a narrow window. cardHeight is in unscaled pixels, as
// GameMenu::Draw takes it.
struct RosterGrid { int columns; float cardHeight; };
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
enum class NoticeResult { None, Dismissed, Acknowledged, CreatePublicRoom, HideGameSettings, OpenButtonLayout, LeaveButtonLayout, UpdateNow, SkipUpdate };
struct NoticeButton { std::string label; NoticeResult result=NoticeResult::Acknowledged; };
// selected: the button highlighted when the notice opens (the first by default).
// The card shows `eyebrow` beside Ember's mark and `chip` at the top right.
// asked: the player's own action opened it, so speech reads it at once
// instead of announcing it (MenuNarrator).
struct NoticeDialog { std::string text,heading; std::vector<NoticeButton> buttons; int selected=0; std::string eyebrow; v2::HeaderChip chip{}; bool asked=false; };
class GameMenu {
public:
    MenuNavigation navigation;
    // The X/Y/View hints for the screen being drawn, in the hint bar.
    std::vector<LegendHint> shortcutHints;
    // The screen the hint bar's bell and status open (a Shortcut to it); empty for none.
    std::string statusLeads;
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
    // Small print in the hint bar's right end while there is no status: Home's
    // (and the launcher window's) version and channel, in Muted. Empty for none.
    std::string footerNote;
    // The tabs over this screen, each a screen id and its label, and
    // the one shown. Pressing one, or LB/RB (Page Up/Down) to a neighbour, returns a
    // Shortcut whose id names it; the owner switches. Empty for none.
    std::vector<std::pair<std::string,std::string>> tabs;
    // The words over the title (where the screen sits, as "ONLINE PLAY · HOST") and the
    // chips at the header's right end. Empty for none.
    std::string eyebrow;
    std::vector<v2::HeaderChip> headerChips;
    // The cards' width (unscaled) beside the detail pane, each card with the
    // first line of its detail and its value as a chip (Online play, Join); 0 for the usual split.
    float listWidth=0;
    // The detail pane's width (unscaled) beside the cards (Public rooms); 0 for the usual split.
    float paneWidth=0;
    // The detail pane on the cards' left, on the screen's own ground (launch recovery).
    bool paneLeft=false;
    // A body's focused card and the column it sits in, where a value's
    // choices open as they do on the cards. The body sets them each frame.
    struct { v2::Box card, column; } bodyChoices;
    int tab=-1;
    bool listEnabled=true, ownsBack=false, customDetail=false;
    unsigned additionalInput=0;
    // A toast holds focus (the shell's toasts): the list gets no input, and
    // the buttons newly pressed through the list's gate (MenuNavigation::Take)
    // are left in toastPressed for the toast to answer; a right click is Back.
    bool toastFocused=false;
    unsigned toastPressed=0;
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
                    const char* status = "",const Detail& detail = {},int columns=1,const Card& card = {},const Body& body = {},bool flyout=false,float cardHeight=100,
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
    void CloseNotice() { notice_.clear(); noticeSelected_=0; noticeResult_=NoticeResult::None; navigation.Rearm(); }
    bool NoticeOpen() const { return !notice_.empty(); }
    // How the last notice closed, once; None while it is open or nobody closed one.
    NoticeResult TakeNoticeResult() { const auto result=noticeResult_; noticeResult_=NoticeResult::None; return result; }
    // The menu buttons held this frame (a notice that owns the input holds none), for a body
    // that scrolls with them.
    unsigned Held() const { return held_; }
private:
    // The screen's drawing (GameMenuV2.cxx): Draw's entries, input and
    // dialogs laid out as the V2 design's cards, panes and hint bar.
    MenuAction DrawV2(const char* title,const std::vector<MenuEntry>& entries,const char* status,const Detail& detail,int columns,const Card& card,
        const Body& body,float cardHeight,Tone statusTone,bool home,bool flyout,MenuAction action,unsigned held,bool modalAtStart,bool choosingAtStart,bool acceptEditText,bool noticeOpen);
    // The dialogs over the screen, then the frame's bookkeeping and its answer.
    MenuAction Finish(const std::vector<MenuEntry>& entries,MenuAction action,bool backRequested,bool acceptEditText,bool body,bool noticeOpen);
    // The Shortcut to the tab `step` (-1, 1) away from the current one (StepTab).
    MenuAction TabAction(int step) const { return {MenuAction::Shortcut, tabs[StepTab(tab, step, static_cast<int>(tabs.size()))].first}; }
    // Parts of Draw. The dialogs and their helpers (ConfirmationButtons,
    // AnswerConfirmation) live in MenuDialogs.cxx.
    // An open dialog owns the legend and the header: Select names its
    // highlighted button and Back what dismissing it does. False when none is open.
    bool DialogLegend(const std::vector<MenuEntry>& entries,std::string& select,std::string& back) const;
    std::vector<DialogButton> ConfirmationButtons(const std::vector<MenuEntry>& entries) const;
    void AnswerConfirmation(const std::vector<MenuEntry>& entries,int clicked,int selected,MenuAction& action);
    // listCard over the list (listMin to listMax), clear of the focused row (rowMin to rowMax) where it can be.
    void DrawListCard(ImVec2 listMin,ImVec2 listMax,ImVec2 rowMin,ImVec2 rowMax,float unit);
    void DrawConfirmationModal(const std::vector<MenuEntry>& entries,MenuAction& action);
    // A reader in place of the list; held Up/Down scroll it.
    void DrawReader(const std::vector<MenuEntry>& entries,const Detail& detail,float height,unsigned held);
    void DrawChoiceModal(const std::vector<MenuEntry>& entries,MenuAction& action);
    void DrawNoticeModal(bool noticeOpen);
    // The editor's keys. With the on-screen keyboard (the pad used last)
    // the pad types: directions move over the keys, Select presses one, X
    // deletes, Y spaces, LB switches capitals and Start is Done; B still cancels.
    void ReadEditInput(const std::vector<MenuEntry>& entries,MenuInput& input);
    bool OnScreenKeyboard(const MenuEntry* editing) const;
    // Presses `key` (a character, or shift, delete, space, done, recent-N); true for Done.
    bool PressKey(const MenuEntry& entry,const std::string& key);
    // The key the on-screen keyboard is on, as speech says it; empty while it is not shown.
    std::string SpokenKey(const std::vector<MenuEntry>& entries) const;
    // The keyboard in `room` pixels of height: its keys shrink to fit.
    void DrawKeyboard(const MenuEntry& entry,const std::vector<MenuEntry>& entries,MenuAction& action,float room);
    void DrawEditModal(const std::vector<MenuEntry>& entries,bool acceptEditText,MenuAction& action);
    // Says what changed on this frame (MenuNarrator), with speech on. button
    // is an open dialog's highlighted button; primary is what Select does.
    void Narrate(const char* title,const std::vector<MenuEntry>& entries,const MenuEntry* focus,const std::string& button,const char* primary,bool adjustable,double now,bool describe);
    // Closes the notice with the result of the button, or Back, that did it.
    void DismissNotice(NoticeResult result) { notice_.clear(); noticeSelected_=0; noticeResult_=result; }
    // Where an open popout sits (Values), set on the frame it opens; whether
    // a popout or value panel is open, and closes on this frame's outside click.
    ImVec2 popoutAt_{};bool valuesOpen_=false,valuesClose_=false;
std::string lastScreen_,lastFocus_,lastEdit_,notice_,noticeHeading_,noticeEyebrow_;
v2::HeaderChip noticeChip_{};
    std::vector<NoticeButton> noticeButtons_;
    bool noticeAsked_=false;
    int noticeSelected_=0;
    NoticeResult noticeResult_=NoticeResult::None;
    bool noticeError_=false;
    PressGate noticeGate_;
    int lastFrame_ = -2;
    unsigned held_ = 0, describeHeld_ = 0;
    std::size_t lastChoice_ = ~std::size_t(0);
    // The on-screen keyboard: its key (row -1 the Recent names), capitals, the
    // pad's last buttons, whether it changed the draft this frame, and the
    // texts accepted this session by entry (ponytail: not saved across launches).
    int keyRow_=1,keyColumn_=0;bool capitals_=false,keyTyped_=false;unsigned keyHeld_=~0u;
    std::map<std::string,std::vector<std::string>> recentText_;
    UiClock clock_;
    MenuVisualFeedback feedback_;
    MenuNarrator narrator_;
};
} }
