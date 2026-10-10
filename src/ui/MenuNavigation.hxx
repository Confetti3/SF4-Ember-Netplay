#pragma once
#include "../common/WipeText.hxx"
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
    // ControllerNavigation.hxx); 64 is left for the sample's Menu button.
    // Leave is the Delete key: it leaves your seat from your own table card,
    // which a pad does with B.
    enum Button : unsigned { Up=1, Down=2, Left=4, Right=8, Select=16, Back=32, Fighter=128, Options=256, Chat=512, Leave=1024 };
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
    // In a grid, an ordinary action (Retry saving) under the cards: it and
    // every entry after it form the grid's footer, one full-width row each
    // (MenuGridCells).
    bool wide = false;
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
    std::string left, right;
    // Presentation-only grace during a healthy room checkpoint. Never authorizes an action.
    bool pending = false;
    // A detail that embeds a player's name, or is a chat message.
    DetailText detailText = DetailText::Interface;
    // A text row for a passphrase: the editor masks it and opens empty, and
    // its draft is wiped when the editor closes.
    bool secret = false;
    // Row height in unscaled pixels for a list or footer row; 0 keeps the standard
    // height. Grid cells always use the grid's card height.
    float height = 0;
    // A disabled row's pane says nothing more than its detail does: no "Unavailable".
    bool quiet = false;
    // A match's row (a replay): each side's fighter as a small native
    // portrait at that end of the row, player 1 on the left. -1 for both
    // draws none; -1 for one draws an unknown fighter on that side. Two
    // scalars, not an array: MSVC fails on brace-initialized entries
    // ({{"a"},{"b"}}) whose type has an array member initializer.
    int fighter1 = -1, fighter2 = -1;
    bool Match() const { return fighter1 >= 0 || fighter2 >= 0; }
};
// What Select does on an entry, decided in one place so navigation and the
// legend agree. A reader wins over text, text over choices, choices over a
// confirmation (a confirmation that also carries choices asks by choosing).
enum class SelectOpens { Nothing, Reader, Edit, Choice, Confirm, Activate };
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
class MenuNavigation {
public:
    explicit MenuNavigation(const std::string& root = "home") : stack_{root} { states_[root]; }
    const std::string& Screen() const { return stack_.back(); }
    const std::string& Root() const { return stack_.front(); }
    const std::string& Parent() const { return stack_.size()>1 ? stack_[stack_.size()-2] : stack_.front(); }
    void Push(const std::string& screen) {
        if (screen.empty() || screen == Screen()) return;
        Cancel(); stack_.push_back(screen); states_[screen]; NeutralGate();
    }
    void Home() { Cancel(); stack_.resize(1); NeutralGate(); }
    void Cancel() {
        const bool modal = mode_ != Modal::None;
        mode_ = Modal::None; modalId_.clear(); WipeText(draft_); choiceIds_.clear();
        if (modal) NeutralGate();
    }
    void NeutralGate() { previous_=~0u; armed_=false; direction_=0; nextRepeat_=0; }
    const std::string& Focus() const { return states_.at(Screen()).id; }
    // The one open modal, if any; it belongs to the entry named by modalId_.
    // Confirming, Choosing, Editing and Reading each name exactly one mode.
    enum class Modal { None, Confirm, Choice, Edit, Read };
    Modal Mode() const { return mode_; }
    bool Editing() const { return mode_ == Modal::Edit; }
    // The editor's highlighted button: Accept, unless a moving pointer went to
    // Cancel. The controller's Select presses it; Enter always accepts.
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
        if (stack_.size()>1) { stack_.pop_back(); NeutralGate(); return {MenuAction::Returned}; }
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
        unsigned held=in.held;
        if((held&3)==3) held&=~3u;
        if((held&12)==12) held&=~12u;
        if(!armed_) { previous_=held; armed_=held==0; return {}; }
        const auto pressed=held&~previous_; previous_=held;
        // Renderers finish the current screen before committing a return, so
        // the outgoing frame still has its body, focus and scroll state.
        if(pressed&MenuInput::Back) {
            if(!deferBack||mode_!=Modal::None) return Return();
            MenuAction back{MenuAction::Back}; back.keyboard=(in.keyboard&MenuInput::Back)!=0; return back;
        }
        // The reader scrolls with held directions, which its renderer reads.
        if(Reading()) return {};
        if(Editing()) {
            // Enter is the keyboard's explicit accept. The controller's Select
            // presses the highlighted button, which is Accept unless the
            // pointer moved to Cancel. Directions stay with the text cursor.
            if(!in.acceptText) {
                if(!(pressed&MenuInput::Select)) return {};
                if(!editAccept_) { Cancel(); return {}; }
            }
            return deferText ? MenuAction{MenuAction::SubmitText} : AcceptText(entries);
        }
        const unsigned shortcut=pressed&(MenuInput::Fighter|MenuInput::Options|MenuInput::Chat|MenuInput::Leave);
        if(shortcut&&mode_==Modal::None) return {MenuAction::Shortcut,{},{},static_cast<int>(shortcut&(~shortcut+1))};
        if(Choosing()) {
            if((pressed&(MenuInput::Left|MenuInput::Up))&&choiceIndex_>0) --choiceIndex_;
            if((pressed&(MenuInput::Right|MenuInput::Down))&&choiceIndex_+1<choiceIds_.size()) ++choiceIndex_;
            return pressed&MenuInput::Select ? Pick(choiceIndex_,entries) : MenuAction{};
        }
        if(Confirming()) {
            if(pressed&(MenuInput::Up|MenuInput::Left)) confirmSelected_=false;
            if(pressed&(MenuInput::Down|MenuInput::Right)) confirmSelected_=true;
            if(pressed&MenuInput::Select) return Confirm(confirmSelected_,entries);
            return {};
        }
        if(pressed&MenuInput::Select) return Choose(entries);
        // A diagonal should move through the list, not accidentally adjust a value.
        const unsigned direction=(held&3) ? (held&3) : (held&12);
        bool move=direction && direction!=direction_;
        if(move) nextRepeat_=in.time+RepeatDelay;
        else if(direction && in.time>=nextRepeat_) { move=true; nextRepeat_=in.time+RepeatInterval; }
        direction_=direction;
        if(!move||entries.empty()) return {};
        auto& s=states_[Screen()]; const auto& e=entries[s.index];
        int delta=(direction&MenuInput::Left)?-1:(direction&MenuInput::Right)?1:0;
        const auto& neighbor=delta<0?e.left:e.right;
        if(delta&&!neighbor.empty()){Focus(neighbor,entries);return {};}
        int index=static_cast<int>(s.index), count=static_cast<int>(entries.size());
        // A grid's footer rows sit under its cards, one per line.
        const int cells=columns>1?static_cast<int>(MenuGridCells(entries)):count;
        if(delta&&e.adjustable&&(columns==1||index>=cells)) return e.enabled ? MenuAction{MenuAction::Adjust,e.id,{},delta} : MenuAction{};
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
    static constexpr double RepeatDelay=.35, RepeatInterval=.085;
    struct State { std::string id; std::size_t index=0; float scroll=0; };
    std::vector<std::string> stack_;
    std::map<std::string,State> states_;
    // The open modal and the entry it belongs to. choiceIds_ holds the option
    // ids as they were when a choice opened; draft_ is an editor's text.
    Modal mode_=Modal::None;
    std::string modalId_,draft_;
    std::vector<std::string> choiceIds_;
    bool confirmSelected_=false, armed_=true, editAccept_=true, secret_=false;
    std::size_t choiceIndex_=0;
    unsigned previous_=0,direction_=0;
    double nextRepeat_=0;
};
} }
