#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace ui {
// Copied input and semantic actions: no renderer, platform, or game dependency.
struct MenuInput {
    // Fighter, Options and Chat are the Xbox X, Y and View shortcuts
    // (ControllerSample uses the same values; 64 is its Menu button).
    enum Button : unsigned { Up=1, Down=2, Left=4, Right=8, Select=16, Back=32, Fighter=128, Options=256, Chat=512 };
    unsigned held = 0;
    double time = 0;
    bool acceptText = false;
};
// One option of a choice. The id is what the option means, so a choice whose
// options change meaning under the player closes instead of quietly sending
// something else. label is short enough for a button; detail says it in full.
struct MenuChoice { std::string id, label, detail; };
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
};
// A grid's cards are the entries before its first wide one; the rest are its
// footer. Rendering and navigation both read this, so what is drawn under the
// cards is also what Down reaches from them.
inline std::size_t MenuGridCells(const std::vector<MenuEntry>& entries) {
    return static_cast<std::size_t>(std::find_if(entries.begin(), entries.end(), [](const MenuEntry& e) { return e.wide; }) - entries.begin());
}
struct MenuAction {
    // Shortcut: a Fighter/Options/Chat press, its button in delta.
    // Chosen: a choice entry's option, its id in text.
    enum Kind { None, Activate, Adjust, TextAccepted, Returned, Close, Back, SubmitText, Shortcut, Chosen } kind = None;
    std::string id, text;
    int delta = 0;
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
        const bool modal = Editing() || Confirming() || Reading();
        dialog_.clear(); editing_.clear(); draft_.clear(); choiceIds_.clear(); reading_.clear();
        if (modal) NeutralGate();
    }
    void NeutralGate() { previous_=~0u; armed_=false; direction_=0; nextRepeat_=0; }
    const std::string& Focus() const { return states_.at(Screen()).id; }
    bool Editing() const { return !editing_.empty(); }
    // The editor's highlighted button: Accept, unless a moving pointer went to
    // Cancel. The controller's Select presses it; Enter always accepts.
    bool EditAccepts() const { return editAccept_; }
    void EditAccepts(bool accept) { if (Editing()) editAccept_ = accept; }
    // A reader is open on this entry; Back closes it.
    bool Reading() const { return !reading_.empty(); }
    const std::string& ReadingId() const { return reading_; }
    bool Confirming() const { return !dialog_.empty(); }
    // A choice is a dialog its owning body draws in place.
    bool Choosing() const { return !choiceIds_.empty(); }
    bool ConfirmSelected() const { return confirmSelected_; }
    // A pointer moving over a confirmation's button highlights it.
    void ConfirmSelected(bool accept) { if (Confirming() && !Choosing()) confirmSelected_ = accept; }
    // The highlighted option of an open choice.
    std::size_t ChoiceIndex() const { return choiceIndex_; }
    void ChoiceIndex(std::size_t index) { if (index < choiceIds_.size()) choiceIndex_ = index; }
    const std::string& EditingId() const { return editing_; }
    const std::string& DialogId() const { return dialog_; }
    const std::string& Draft() const { return draft_; }
    void Draft(std::string text) { draft_=std::move(text); }
    float& Scroll() { return states_[Screen()].scroll; }
    void Reconcile(const std::vector<MenuEntry>& entries) {
        auto& state=states_[Screen()];
        auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==state.id;});
        if (it!=entries.end()) state.index=static_cast<std::size_t>(it-entries.begin());
        else if (!entries.empty()) { state.index=(std::min)(state.index,entries.size()-1); state.id=entries[state.index].id; }
        else { state.id.clear(); state.index=0; }
        const auto valid = [&](const std::string& id, bool text) {
            return std::any_of(entries.begin(), entries.end(), [&](const MenuEntry& e) {
                return e.id == id && (text ? e.text : (e.confirm || !e.choices.empty()) && SameChoices(e)) &&
                    (e.enabled || e.pending);
            });
        };
        // A transient checkpoint must not discard a draft. Removed entries,
        // changed kinds and genuinely unavailable actions still cancel immediately.
        const bool readable = std::any_of(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == reading_ && e.reading; });
        if ((!dialog_.empty() && !valid(dialog_, false)) ||
            (!editing_.empty() && !valid(editing_, true)) || (Reading() && !readable)) Cancel();
        const auto& owner = Editing() ? editing_ : Reading() ? reading_ : dialog_;
        if (!owner.empty()) {
            const auto target = std::find_if(entries.begin(), entries.end(),
                [&](const MenuEntry& e) { return e.id == owner; });
            state.id = owner; state.index = static_cast<std::size_t>(target - entries.begin());
        }
    }
    void Focus(const std::string& id,const std::vector<MenuEntry>& entries) {
        if ((Editing() && id != editing_) || (Confirming() && id != dialog_) || (Reading() && id != reading_)) return;
        auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==id;});
        if(it!=entries.end()) { auto& s=states_[Screen()]; s.id=id; s.index=it-entries.begin(); }
    }
    MenuAction Return() {
        if (Editing()||Confirming()||Reading()) { Cancel(); return {}; }
        if (stack_.size()>1) { stack_.pop_back(); NeutralGate(); return {MenuAction::Returned}; }
        return {MenuAction::Close};
    }
    MenuAction Choose(const std::vector<MenuEntry>& entries) {
        Reconcile(entries);
        if (entries.empty() || Editing() || Confirming() || Reading()) return {};
        const auto& e=entries[states_[Screen()].index];
        if(!e.enabled || e.info) return {};
        if(e.reading) { reading_=e.id; return {}; }
        if(e.adjustable && e.choices.empty()) return {};
        if(e.text) { editing_=e.id; draft_=e.value; editAccept_=true; return {}; }
        if(e.confirm||!e.choices.empty()) {
            dialog_=e.id; confirmSelected_=false; choiceIndex_=0;
            for (const auto& c : e.choices) {
                if (c.id == e.chosen) choiceIndex_ = choiceIds_.size();
                choiceIds_.push_back(c.id);
            }
            return {};
        }
        return {MenuAction::Activate,e.id};
    }
    MenuAction Confirm(bool accept,const std::vector<MenuEntry>& entries) {
        Reconcile(entries);
        if (!Confirming() || Choosing()) return {};
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) {
            return e.id == dialog_ && e.enabled && e.confirm;
        });
        if (accept && entry == entries.end()) return {};
        const auto id=dialog_; Cancel();
        return accept ? MenuAction{MenuAction::Activate,id} : MenuAction{};
    }
    // Option `index` of the open choice, as it was when the choice opened;
    // Reconcile has already closed a changed one.
    MenuAction Pick(std::size_t index,const std::vector<MenuEntry>& entries) {
        Reconcile(entries);
        if (!Choosing() || index >= choiceIds_.size()) return {};
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const MenuEntry& e) { return e.id == dialog_; });
        if (entry == entries.end() || !entry->enabled) return {};
        MenuAction a{MenuAction::Chosen, dialog_, choiceIds_[index]}; Cancel(); return a;
    }
    MenuAction AcceptText(const std::vector<MenuEntry>& entries) {
        Reconcile(entries); if(!Editing()) return {};
        auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==editing_;});
        if(it==entries.end()||!it->enabled||draft_.size()>it->textLimit) return {};
        MenuAction a{MenuAction::TextAccepted,editing_,draft_}; Cancel(); return a;
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
        if(pressed&MenuInput::Back) return deferBack&&!Editing()&&!Confirming()&&!Reading()?MenuAction{MenuAction::Back}:Return();
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
        const unsigned shortcut=pressed&(MenuInput::Fighter|MenuInput::Options|MenuInput::Chat);
        if(shortcut&&!Confirming()) return {MenuAction::Shortcut,{},{},static_cast<int>(shortcut&(~shortcut+1))};
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
        if(delta&&e.adjustable&&columns==1) return e.enabled ? MenuAction{MenuAction::Adjust,e.id,{},delta} : MenuAction{};
        int index=static_cast<int>(s.index), count=static_cast<int>(entries.size());
        // A grid's footer rows sit under its cards, one per line.
        const int cells=columns>1?static_cast<int>(MenuGridCells(entries)):count;
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
    bool SameChoices(const MenuEntry& e) const {
        return e.choices.size() == choiceIds_.size() &&
            std::equal(choiceIds_.begin(), choiceIds_.end(), e.choices.begin(),
                [](const std::string& id, const MenuChoice& c) { return id == c.id; });
    }
    static constexpr double RepeatDelay=.35, RepeatInterval=.085;
    struct State { std::string id; std::size_t index=0; float scroll=0; };
    std::vector<std::string> stack_;
    std::map<std::string,State> states_;
    std::string dialog_,editing_,draft_,reading_;
    std::vector<std::string> choiceIds_;
    bool confirmSelected_=false, armed_=true, editAccept_=true;
    std::size_t choiceIndex_=0;
    unsigned previous_=0,direction_=0;
    double nextRepeat_=0;
};
} }
