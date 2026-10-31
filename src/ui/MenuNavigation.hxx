#pragma once
#include "../common/WipeText.hxx"
#include "PressGate.hxx"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace ui {
// Copied input and semantic actions: no renderer, platform, or game dependency.
struct MenuInput {
    // Fighter, Options and Chat are the Xbox X, Y and View shortcuts and the
    // F, T and C keys. The overlay passes a ControllerSample's buttons
    // straight in, so both enums number them alike (asserted in
    // ControllerNavigation.hxx). Start is the sample's Menu button: it opens
    // Ember, and while Ember is shown only the on-screen keyboard reads it (Done).
    // Leave is the Delete key: it leaves your seat from your own table card,
    // which a pad does with B. PrevTab and NextTab are LB and RB (Page Up and Down,
    // a DirectInput stick's LP and HP: MenuShortcuts): the tabs over a screen
    // (GameMenu::tabs), wrapping at the ends (StepTab). Describe is the accessibility
    // button, RT and F2: speech reads the focused row's description (MenuNarrator);
    // the menu itself does nothing with it.
    enum Button : unsigned { Up=1, Down=2, Left=4, Right=8, Select=16, Back=32, Start=64, Fighter=128, Options=256, Chat=512, Leave=1024, PrevTab=2048, NextTab=4096, Describe=8192 };
    unsigned held = 0;
    double time = 0;
    bool acceptText = false;
    // The bits of `held` that come from keyboard keys. Last, so that
    // {held, time} still initializes the other two.
    unsigned keyboard = 0;
};
// One option of a choice. The id is what the option means, so a choice whose
// options change meaning under the player closes instead of quietly sending
// something else. label is short enough for a button; detail says it in full.
// A choice that is not enabled is shown, with its reason in detail, but cannot
// be picked.
struct MenuChoice { std::string id, label, detail; bool enabled = true; };
// Whose words an entry's detail carries. The renderer asks the atlas for the
// glyphs of player-written detail when it draws it, and only then.
enum class DetailText { Interface, Name, Chat };
struct MenuEntry {
    std::string id, label, detail, value;
    bool enabled = true, adjustable = false, text = false, confirm = false;
    // Options make Select open a choice: Left/Right (or Up/Down) move along
    // them, Select returns Chosen with that option's id in text, Back cancels.
    // It opens on the option whose id is `chosen`, else the first. A value row
    // may carry choices too; Left/Right still adjust it while the choice is closed.
    std::vector<MenuChoice> choices;
    std::string chosen;
    // Focusable information with nothing to do: no Select in the legend.
    bool info = false;
    // A value row whose Select still opens its page: Left/Right change the
    // value in place, Select shows the choices in full.
    bool opens = false;
    // Its choices are actions, opened as a small menu at the row itself
    // (where it was clicked) rather than as a value panel or a page.
    bool popout = false;
    // In a grid, an ordinary action (Retry saving) under the cards: it and
    // every entry after it form the grid's footer, one full-width row each
    // (MenuGridCells).
    bool wide = false;
    // One of the shell's own actions (Retry saving), not the screen's: a
    // screen with a body of its own never sees it, and the menu draws it in a
    // footer under that body (GameMenu::DrawBody); a list draws it as a card.
    bool footer = false;
    // The label is a player's own text (a member's name), which may be
    // elided; an interface label must fit, and the render probe checks it.
    bool userText = false;
    // Select opens the row's full text in a reader that the pad and keys
    // scroll, for text longer than the detail pane (licences, history).
    bool reading = false;
    // What Select does, when the generic word says too little ("Ready up").
    std::string hint;
    std::size_t textLimit = 256;
    // Explicit pane transitions; empty preserves ordinary list/grid movement.
    std::string left, right, up, down;
    // Presentation-only grace during a healthy room checkpoint. Never authorizes an action.
    bool pending = false;
    // A detail that embeds a player's name, or is a chat message.
    DetailText detailText = DetailText::Interface;
    // A text row for a passphrase: the editor masks it and opens empty, and
    // its draft is wiped when the editor closes.
    bool secret = false;
    bool multiline = false;
    std::string cancelLabel;
    // Row height in unscaled pixels for a list or footer row; 0 keeps the standard
    // height. Grid cells always use the grid's card height.
    float height = 0;
    // A disabled row's pane says nothing more than its detail does: no "Unavailable".
    bool quiet = false;
    bool valueHighlighted = false;
    // The heading this row sits under; a new one starts a group of cards.
    std::string group;
    // A match's row (a replay): each side's fighter as a small native
    // portrait at that end of the row, player 1 on the left. -1 for both
    // draws none; -1 for one draws an unknown fighter on that side. Two
    // scalars, not an array: MSVC fails on brace-initialized entries
    // ({{"a"},{"b"}}) whose type has an array member initializer.
    int fighter1 = -1, fighter2 = -1;
    bool Match() const { return fighter1 >= 0 || fighter2 >= 0; }
    // What a keyed row's id names after its prefix (a table's number in
    // "table-", a member's id in "member-", a fighter in "main-" and so on),
    // as its builder set it, so the row's card reads it instead of parsing
    // the id back. The id stays the row's name for focus and probes.
    std::uint64_t key = 0;
};
// What Select does on an entry, decided in one place so navigation and the
// legend agree. A reader wins over text, text over choices, choices over a
// confirmation (a confirmation that also carries choices asks by choosing).
enum class SelectOpens { Nothing, Reader, Edit, Choice, Confirm, Activate };
// A value that is a number ("3", "100%", "-2 f"): a click steps it on either half.
inline bool MenuValueIsNumber(const MenuEntry& e) {
    const char c = e.value.empty() ? 0 : e.value[0];
    return e.adjustable && ((c >= '0' && c <= '9') || ((c == '+' || c == '-') && e.value.size() > 1));
}
inline SelectOpens MenuSelectOpens(const MenuEntry& e) {
    if (!e.enabled || e.info) return SelectOpens::Nothing;
    if (e.reading) return SelectOpens::Reader;
    if (e.adjustable && e.choices.empty() && !e.opens) return SelectOpens::Nothing;
    if (e.text) return SelectOpens::Edit;
    if (!e.choices.empty()) return SelectOpens::Choice;
    return e.confirm ? SelectOpens::Confirm : SelectOpens::Activate;
}
// A grid's cards are the entries before its first wide one; the rest are its
// footer. Rendering and navigation both read this, so what is drawn under the
// cards is also what Down reaches from them.
inline std::size_t MenuGridCells(const std::vector<MenuEntry>& entries) {
    return static_cast<std::size_t>(std::find_if(entries.begin(), entries.end(), [](const MenuEntry& e) { return e.wide; }) - entries.begin());
}
// A row's box where its board drew it, in screen pixels (MenuNavigation::Place).
struct MenuRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool Contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};
// Where a value row's parts lie: the row's whole box, its label's box and its
// value's, the value at the right end of the row's line or, `under`, on the
// row's last line, and the label on `labelLines` lines. One layout feeds
// drawing (DrawMenuRow, a list's cards) and the pointer (PressEntry,
// MenuValueStep), so what is pressed is what is drawn there.
struct MenuRowLayout { MenuRect row, label, value; bool under = false; int labelLines = 1; };
// A row whose one-line box is `line`, with its words from `left` to `right`
// and the value `valueWidth` wide (0 for none): under the label on a line of
// its own, `underLine` tall, at the row's foot, else beside it with `gap`
// between. A label on two lines (wider than its whole line, its value already
// under it) grows the row by one more `underLine`; the label's box is all of
// the row above the value's line.
inline MenuRowLayout LayOutMenuRow(MenuRect line, float left, float right, float valueWidth, bool under, float underLine, float gap, int labelLines = 1) {
    MenuRowLayout at;
    at.under = under;
    at.labelLines = labelLines;
    at.row = line;
    at.row.y1 += underLine * static_cast<float>((under ? 1 : 0) + labelLines - 1);
    const float valueTop = under ? at.row.y1 - underLine : at.row.y0;
    at.value = {right - valueWidth, valueTop, right, at.row.y1};
    at.label = {left, at.row.y0, under || valueWidth <= 0 ? right : right - valueWidth - gap, under ? valueTop : at.row.y1};
    return at;
}
// What a press at (x, y) does to an adjustable row laid out `at`: -1 or 1 to
// step it, 0 for Select. Only the value's own line steps, from the value's
// left edge to the row's right end: a switch (`toggle`, the step that flips
// it, 0 for none) on its last `switchWidth`; a number on either half; a word
// on its arrows, `arrowWidth` at each end (the word itself opens its list).
inline int MenuValueStep(const MenuEntry& e, const MenuRowLayout& at, float x, float y, int toggle, float switchWidth, float arrowWidth) {
    const MenuRect& v = at.value;
    if (!e.adjustable || !e.enabled || v.x1 <= v.x0 || x < v.x0 || y < v.y0 || y > v.y1) return 0;
    if (toggle) return x < v.x1 - switchWidth ? 0 : toggle;
    if (!MenuValueIsNumber(e) && x >= v.x0 + arrowWidth && x <= v.x1 - arrowWidth) return 0;
    return x < (v.x0 + v.x1) * .5f ? -1 : 1;
}
// What a direction does on a board (rows placed) when no placed row lies that
// way: Stay keeps the focus where it is; Flow goes on to the next placed row
// in entry order (Down, Right) or the previous one (Up, Left), for a board
// whose panels stand side by side but read as one column (Create, the welcome).
enum class MenuEdge { Stay, Flow };
// The tab `delta` steps to from `current` of `count`, wrapping at both ends:
// the one rule for LB and RB (Page Up and Down), the tab strip's end marks and
// the hint bar's. With no current tab (-1) the next is the first and the
// previous the last; with no tabs it is -1.
inline int StepTab(int current, int delta, int count) {
    if (count <= 0) return -1;
    if (current < 0 || current >= count) return delta < 0 ? count - 1 : 0;
    return ((current + delta) % count + count) % count;
}
// The steps a held direction gives, the one rule for every reader of the
// directions (a list or board, a choice, a question, a notice, the on-screen
// keyboard): one at once, then again after RepeatDelay and every
// RepeatInterval while it stays held. Up and Down win over Left and Right, so
// a diagonal moves vertically, never adjusting a value by accident. Give it
// the directions that count (through the reader's PressGate) every frame the
// reader reads them; Reset when the reader opens.
class DirectionRepeat {
public:
    static constexpr double RepeatDelay=.35, RepeatInterval=.085;
    // The direction bits (MenuInput::Up, Down, Left, Right) to step on this frame.
    unsigned Step(unsigned held, double time) {
        const unsigned direction=(held&3) ? (held&3) : (held&12);
        bool step=direction && direction!=direction_;
        if(step) next_=time+RepeatDelay;
        else if(direction && time>=next_) { step=true; next_=time+RepeatInterval; }
        direction_=direction;
        return step?direction:0;
    }
    void Reset() { direction_=0; next_=0; }
private:
    unsigned direction_=0;
    double next_=0;
};
struct MenuAction {
    // Shortcut: a Fighter/Options/Chat/Leave press, its button in delta.
    // Chosen: a choice entry's option, its id in text.
    enum Kind { None, Activate, Adjust, TextAccepted, Returned, Close, Back, SubmitText, Shortcut, Chosen } kind = None;
    std::string id, text;
    int delta = 0;
    // A Back that came from Escape or Backspace, which always goes back;
    // only a pad's B may mean "leave my seat".
    bool keyboard = false;
    // Accepted text can be a passphrase, so no copy of an action leaves it
    // behind. (With this destructor, copies stand in for moves.)
    ~MenuAction() { WipeText(text); }
};
// The step an LB or RB press (Page Up or Down) asks of a tab strip: -1 or 1
// for a Shortcut whose button is PrevTab or NextTab, else 0. GameMenu's tabs
// read it, and so does a body with a strip of its own (the room board's
// narrow tabs), then StepTab.
inline int TabDelta(const MenuAction& a) {
    if (a.kind != MenuAction::Shortcut || !a.id.empty()) return 0;
    return a.delta == MenuInput::PrevTab ? -1 : a.delta == MenuInput::NextTab ? 1 : 0;
}
class MenuNavigation {
public:
    explicit MenuNavigation(const std::string& root = "home") : stack_{root} { states_[root]; }
    const std::string& Screen() const { return stack_.back(); }
    const std::string& Root() const { return stack_.front(); }
    const std::string& Parent() const { return stack_.size()>1 ? stack_[stack_.size()-2] : stack_.front(); }
    void Push(const std::string& screen) {
        if (screen.empty() || screen == Screen()) return;
        Cancel(); stack_.push_back(screen); states_[screen]; Rearm();
    }
    // `screen` takes the place of the top `depth` screens, the root never
    // among them: whatever is open on the screen shown (a reader, a choice)
    // closes, those screens leave the stack and `screen` is pushed, so Back
    // from it goes where Back from the first of them went. A tab replacing the
    // tab shown is depth 1 (at the root it is pushed); 0 is Push.
    void Replace(const std::string& screen, std::size_t depth = 1) {
        Cancel();
        stack_.resize(stack_.size() - (std::min)(depth, stack_.size() - 1));
        if (!screen.empty() && screen != Screen()) { stack_.push_back(screen); states_[screen]; }
        Rearm();
    }
    void Home() { Cancel(); stack_.resize(1); Rearm(); }
    void Cancel() {
        const bool modal = mode_ != Modal::None;
        mode_ = Modal::None; modalId_.clear(); WipeText(draft_); choiceIds_.clear();
        if (modal) Rearm();
    }
    // A screen opening (pushed, returned to, a modal closing): what is held
    // now counts once released, see PressGate. The rows placed so far were
    // the old screen's.
    void Rearm() { gate_.Rearm(); repeat_.Reset(); placing_.clear(); placed_.clear(); }
    // This frame's presses through the screen's gate, for a reader that
    // answers them in the screen's place (a focused toast). What it takes
    // stays held for the screen, so the next frame does not see it again:
    // one press is spent once.
    unsigned Take(unsigned held) { unsigned counted=0, pressed=0; return Read(held,counted,pressed) ? pressed : 0; }
    // Where a board drew a row this frame; PressEntry places every box it is
    // given. The next frame's directions move from a placed focus to the
    // nearest placed row that way (Update). A screen that places nothing (a
    // GameMenu list) keeps the list's movement.
    void Place(const std::string& id, MenuRect box) { placing_.push_back({id, box}); }
    // What the board on this screen does at its edges (MenuEdge); Stay unless set.
    void SetEdge(MenuEdge edge) { states_[Screen()].edge = edge; }
    MenuEdge Edge() const { return states_.at(Screen()).edge; }
    const std::string& Focus() const { return states_.at(Screen()).id; }
    // The one open modal, if any; it belongs to the entry named by modalId_.
    // Confirming, Choosing, Editing and Reading each name exactly one mode.
    enum class Modal { None, Confirm, Choice, Edit, Read };
    Modal Mode() const { return mode_; }
    bool Editing() const { return mode_ == Modal::Edit; }
    // The editor's highlighted button: Accept, unless a moving pointer went to
    // Cancel. The controller's Select presses it; the editor's keyboard
    // acceptance gesture always accepts.
    bool EditAccepts() const { return editAccept_; }
    void EditAccepts(bool accept) { if (Editing()) editAccept_ = accept; }
    // A reader is open on this entry; Back closes it.
    bool Reading() const { return mode_ == Modal::Read; }
    const std::string& ReadingId() const { return Reading() ? modalId_ : NoId(); }
    // A yes/no question about the entry, drawn as a dialog.
    bool Confirming() const { return mode_ == Modal::Confirm; }
    // A choice is a dialog its owning body draws in place.
    bool Choosing() const { return mode_ == Modal::Choice; }
    // A confirmation or a choice: a question the player answers with Select.
    bool Asking() const { return Confirming() || Choosing(); }
    bool ConfirmSelected() const { return confirmSelected_; }
    // A pointer moving over a confirmation's button highlights it.
    void ConfirmSelected(bool accept) { if (Confirming()) confirmSelected_ = accept; }
    // The highlighted option of an open choice.
    std::size_t ChoiceIndex() const { return choiceIndex_; }
    // How many options an open choice shows on a row (MenuChoiceColumns); its owner sets it.
    std::size_t choiceColumns = 1;
    void ChoiceIndex(std::size_t index) { if (index < choiceIds_.size()) choiceIndex_ = index; }
    const std::string& EditingId() const { return Editing() ? modalId_ : NoId(); }
    // The entry an open confirmation or choice belongs to.
    const std::string& DialogId() const { return Asking() ? modalId_ : NoId(); }
    const std::string& Draft() const { return draft_; }
    // Swapped in, then the parameter is wiped too: a moved-from short string
    // keeps its characters in its own buffer.
    void Draft(std::string text) { WipeText(draft_); draft_.swap(text); WipeText(text); }
    // The open editor holds a passphrase (MenuEntry::secret).
    bool EditingSecret() const { return Editing() && secret_; }
    float& Scroll() { return states_[Screen()].scroll; }
    void Reconcile(const std::vector<MenuEntry>& entries) {
        auto& state=states_[Screen()];
        auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==state.id;});
        if (it!=entries.end()) state.index=static_cast<std::size_t>(it-entries.begin());
        else if (!entries.empty()) { state.index=(std::min)(state.index,entries.size()-1); state.id=entries[state.index].id; }
        else { state.id.clear(); state.index=0; }
        const auto valid = [&](bool text) {
            return std::any_of(entries.begin(), entries.end(), [&](const MenuEntry& e) {
                return e.id == modalId_ && (text ? e.text : (e.confirm || !e.choices.empty()) && SameChoices(e)) &&
                    (e.enabled || e.pending);
            });
        };
        // A transient checkpoint must not discard a draft. Removed entries,
        // changed kinds and genuinely unavailable actions still cancel immediately.
        const bool readable = std::any_of(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == modalId_ && e.reading; });
        if ((Asking() && !valid(false)) || (Editing() && !valid(true)) || (Reading() && !readable)) Cancel();
        if (mode_ != Modal::None) {
            const auto target = std::find_if(entries.begin(), entries.end(),
                [&](const MenuEntry& e) { return e.id == modalId_; });
            state.id = modalId_; state.index = static_cast<std::size_t>(target - entries.begin());
        }
    }
    // The entry to focus on this screen once its entries exist, for a screen
    // just pushed whose rows are built on its first frame.
    void Prefer(const std::string& id) { states_[Screen()].id = id; }
    void Focus(const std::string& id,const std::vector<MenuEntry>& entries) {
        if (mode_ != Modal::None && id != modalId_) return;
        auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==id;});
        if(it!=entries.end()) { auto& s=states_[Screen()]; s.id=id; s.index=it-entries.begin(); }
    }
    MenuAction Return() {
        if (mode_ != Modal::None) { Cancel(); return {}; }
        if (stack_.size()>1) { stack_.pop_back(); Rearm(); return {MenuAction::Returned}; }
        return {MenuAction::Close};
    }
    MenuAction Choose(const std::vector<MenuEntry>& entries) {
        Reconcile(entries);
        if (entries.empty() || mode_ != Modal::None) return {};
        const auto& e=entries[states_[Screen()].index];
        switch (MenuSelectOpens(e)) {
        case SelectOpens::Reader: mode_=Modal::Read; modalId_=e.id; return {};
        case SelectOpens::Edit: mode_=Modal::Edit; modalId_=e.id; secret_=e.secret; draft_=e.secret?std::string():e.value; editAccept_=true; return {};
        case SelectOpens::Choice: case SelectOpens::Confirm: Open(e); return {};
        case SelectOpens::Activate: return {MenuAction::Activate,e.id};
        case SelectOpens::Nothing: break;
        }
        return {};
    }
    // Opens the entry's choice as if the player had selected it, for a
    // choice that the player asked for some other way (Back on a place).
    bool Ask(const std::string& id,const std::vector<MenuEntry>& entries) {
        Reconcile(entries);
        if (mode_ != Modal::None) return false;
        const auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==id;});
        if (it==entries.end() || !it->enabled || it->choices.empty()) return false;
        Focus(id,entries); Open(*it); return true;
    }
    MenuAction Confirm(bool accept,const std::vector<MenuEntry>& entries) {
        Reconcile(entries);
        if (!Confirming()) return {};
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) {
            return e.id == modalId_ && e.enabled && e.confirm;
        });
        if (accept && entry == entries.end()) return {};
        const auto id=modalId_; Cancel();
        return accept ? MenuAction{MenuAction::Activate,id} : MenuAction{};
    }
    // Option `index` of the open choice, as it was when the choice opened;
    // Reconcile has already closed a changed one.
    MenuAction Pick(std::size_t index,const std::vector<MenuEntry>& entries) {
        Reconcile(entries);
        if (!Choosing() || index >= choiceIds_.size()) return {};
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == modalId_; });
        if (entry == entries.end() || !entry->enabled || !entry->choices[index].enabled) return {};
        MenuAction a{MenuAction::Chosen, modalId_, choiceIds_[index]}; Cancel(); return a;
    }
    MenuAction AcceptText(const std::vector<MenuEntry>& entries) {
        Reconcile(entries); if(!Editing()) return {};
        auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==modalId_;});
        if(it==entries.end()||!it->enabled||draft_.size()>it->textLimit) return {};
        MenuAction a{MenuAction::TextAccepted,modalId_,draft_}; Cancel(); return a;
    }
    MenuAction Update(MenuInput in,const std::vector<MenuEntry>& entries,int columns=1,bool deferBack=false,bool deferText=false) {
        Reconcile(entries);
        columns = (std::max)(1, columns);
        unsigned held=0, pressed=0;
        if(!Read(in.held,held,pressed)) return {};
        // Renderers finish the current screen before committing a return, so
        // the outgoing frame still has its body, focus and scroll state.
        if(pressed&MenuInput::Back) {
            if(!deferBack||mode_!=Modal::None) return Return();
            MenuAction back{MenuAction::Back}; back.keyboard=(in.keyboard&MenuInput::Back)!=0; return back;
        }
        // The reader scrolls with held directions, which its renderer reads.
        if(Reading()) return {};
        if(Editing()) {
            // The editor supplies the keyboard's explicit accept. Pad Select
            // presses the highlighted button, which is Accept unless the
            // pointer moved to Cancel. Directions stay with the text cursor.
            if(!in.acceptText) {
                if(!(pressed&MenuInput::Select)) return {};
                if(!editAccept_) { Cancel(); return {}; }
            }
            return deferText ? MenuAction{MenuAction::SubmitText} : AcceptText(entries);
        }
        const unsigned shortcut=pressed&(MenuInput::Fighter|MenuInput::Options|MenuInput::Chat|MenuInput::Leave|MenuInput::PrevTab|MenuInput::NextTab);
        if(shortcut&&mode_==Modal::None) return {MenuAction::Shortcut,{},{},static_cast<int>(shortcut&(~shortcut+1))};
        // A held direction steps at once, then repeats: in the list, a choice
        // and a question alike (DirectionRepeat).
        const unsigned stepped=repeat_.Step(held,in.time);
        if(Choosing()) {
            // In a grid of options (choiceColumns), Up and Down move a row; Left and Right one option.
            const std::size_t up=stepped&MenuInput::Up?choiceColumns:stepped&MenuInput::Left?1:0;
            const std::size_t down=stepped&MenuInput::Down?choiceColumns:stepped&MenuInput::Right?1:0;
            if(up&&choiceIndex_>=up) choiceIndex_-=up;
            if(down&&choiceIndex_+down<choiceIds_.size()) choiceIndex_+=down;
            return pressed&MenuInput::Select ? Pick(choiceIndex_,entries) : MenuAction{};
        }
        if(Confirming()) {
            if(stepped&(MenuInput::Up|MenuInput::Left)) confirmSelected_=false;
            if(stepped&(MenuInput::Down|MenuInput::Right)) confirmSelected_=true;
            if(pressed&MenuInput::Select) return Confirm(confirmSelected_,entries);
            return {};
        }
        if(pressed&MenuInput::Select) return Choose(entries);
        if(!stepped||entries.empty()) return {};
        const unsigned direction=stepped;
        auto& s=states_[Screen()]; const auto& e=entries[s.index];
        int delta=(direction&MenuInput::Left)?-1:(direction&MenuInput::Right)?1:0;
        const auto& neighbor=delta<0?e.left:delta>0?e.right:direction&MenuInput::Up?e.up:e.down;
        if(!neighbor.empty()){Focus(neighbor,entries);return {};}
        int index=static_cast<int>(s.index), count=static_cast<int>(entries.size());
        // A grid's footer rows sit under its cards, one per line.
        const int cells=columns>1?static_cast<int>(MenuGridCells(entries)):count;
        if(delta&&e.adjustable&&(columns==1||index>=cells)) return e.enabled ? MenuAction{MenuAction::Adjust,e.id,{},delta} : MenuAction{};
        // A board moves by where it drew its rows; a list by their order, and
        // so does a screen that placed this row alone (a footer under a body
        // that places none), where no other row lies any way.
        if(const MenuRect* at=placed_.size()>1?Placed(e.id):nullptr) {
            const int dy=direction&MenuInput::Up?-1:direction&MenuInput::Down?1:0;
            auto to=Nearest(*at,delta,dy,entries);
            if(to==entries.size()&&s.edge==MenuEdge::Flow) to=NextPlaced(s.index,delta+dy,entries);
            if(to<entries.size()) { s.index=to; s.id=entries[to].id; }
            return {};
        }
        if(index>=cells) {
            if(direction&MenuInput::Up) index=index>cells?index-1:cells-1;
            else if((direction&MenuInput::Down)&&index+1<count) ++index;
        }
        else if(direction&MenuInput::Up) { if(index>=columns)index-=columns; }
        else if(direction&MenuInput::Down) {
            if(index/columns<(cells-1)/columns)index=(std::min)(cells-1,index+columns);
            else if(cells<count)index=cells;
        }
        else if(columns>1 && delta && index/columns==(index+delta)/columns && index+delta>=0 && index+delta<cells) index+=delta;
        if(index<0) index=0;
        s.index=index; s.id=entries[index].id;
        return {};
    }
private:
    static const std::string& NoId() { static const std::string none; return none; }
    // The frame's input, read once a frame by Update or Take: the buttons that
    // count (PressGate; Up with Down and Left with Right cancel) and, of
    // those, the ones newly down. False on the frame the screen opens, which
    // counts nothing. The rows placed last frame become the ones moved among.
    bool Read(unsigned raw, unsigned& held, unsigned& pressed) {
        placed_.swap(placing_); placing_.clear();
        if((raw&3)==3) raw&=~3u;
        if((raw&12)==12) raw&=~12u;
        const bool opening=!gate_.Armed();
        held=gate_.Held(raw); pressed=gate_.Pressed(held);
        return !opening;
    }
    const MenuRect* Placed(const std::string& id) const {
        for(const auto& p:placed_) if(p.first==id) return &p.second;
        return nullptr;
    }
    // The entry placed nearest past `from` in the direction (dx, dy). Past:
    // its centre is beyond from's centre that way, and it overlaps from
    // across (a button inside a heading counts) or lies at least as far past
    // from's edge as it lies off to the side, so a heading in the next column
    // is not Up from the top row. Nearest: the gap along that axis plus twice
    // the offset across it, ties to the earlier entry. entries.size() for
    // none: the board's edge, where the focus stays.
    std::size_t Nearest(const MenuRect& from, int dx, int dy, const std::vector<MenuEntry>& entries) const {
        struct Span { float min, max; float Centre() const { return (min+max)*.5f; } };
        const auto along=[&](const MenuRect& r) { return dx ? Span{r.x0,r.x1} : Span{r.y0,r.y1}; };
        const auto across=[&](const MenuRect& r) { return dx ? Span{r.y0,r.y1} : Span{r.x0,r.x1}; };
        const auto apart=[](Span a, Span b) { return (std::max)(0.f, (std::max)(b.min-a.max, a.min-b.max)); };
        const float way=static_cast<float>(dx ? dx : dy);
        const Span f=along(from);
        std::size_t best=entries.size(); float bestScore=0;
        for(std::size_t i=0;i<entries.size();++i) {
            const MenuRect* r=Placed(entries[i].id);
            if(!r || r==&from) continue;
            const float gap=apart(f,along(*r)), offset=apart(across(from),across(*r));
            if((along(*r).Centre()-f.Centre())*way<=0 || gap<offset) continue;
            const float score=gap+2*offset;
            if(best==entries.size() || score<bestScore) { best=i; bestScore=score; }
        }
        return best;
    }
    // The placed entry after `from` in entry order (way 1) or before it (-1);
    // entries.size() for none.
    std::size_t NextPlaced(std::size_t from, int way, const std::vector<MenuEntry>& entries) const {
        for(std::size_t i=from;way>0?i+1<entries.size():i>0;) {
            i=way>0?i+1:i-1;
            if(Placed(entries[i].id)) return i;
        }
        return entries.size();
    }
    void Open(const MenuEntry& e) {
        mode_=e.choices.empty()?Modal::Confirm:Modal::Choice; modalId_=e.id; confirmSelected_=false; choiceIndex_=0;
        for (const auto& c : e.choices) {
            if (c.id == e.chosen) choiceIndex_ = choiceIds_.size();
            choiceIds_.push_back(c.id);
        }
    }
    bool SameChoices(const MenuEntry& e) const {
        return e.choices.size() == choiceIds_.size() &&
            std::equal(choiceIds_.begin(), choiceIds_.end(), e.choices.begin(),
                [](const std::string& id, const MenuChoice& c) { return id == c.id; });
    }
    struct State { std::string id; std::size_t index=0; float scroll=0; MenuEdge edge=MenuEdge::Stay; };
    std::vector<std::string> stack_;
    std::map<std::string,State> states_;
    // The open modal and the entry it belongs to. choiceIds_ holds the option
    // ids as they were when a choice opened; draft_ is an editor's text.
    Modal mode_=Modal::None;
    std::string modalId_,draft_;
    std::vector<std::string> choiceIds_;
    bool confirmSelected_=false, editAccept_=true, secret_=false;
    PressGate gate_{true};
    std::size_t choiceIndex_=0;
    // The rows placed this frame, and those placed on the last, which this
    // frame's directions read.
    std::vector<std::pair<std::string,MenuRect>> placing_, placed_;
    DirectionRepeat repeat_;
};
} }
