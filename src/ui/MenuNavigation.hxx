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
    enum Button : unsigned { Up=1, Down=2, Left=4, Right=8, Select=16, Back=32 };
    unsigned held = 0;
    double time = 0;
    bool acceptText = false;
};
struct MenuEntry {
    std::string id, label, detail, value;
    bool enabled = true, adjustable = false, text = false, confirm = false;
    std::size_t textLimit = 256;
    // Explicit pane transitions; empty preserves ordinary list/grid movement.
    std::string left, right;
    // Presentation-only grace during a healthy room checkpoint. Never authorizes an action.
    bool pending = false;
};
struct MenuAction {
    enum Kind { None, Activate, Adjust, TextAccepted, Returned, Close, Back, SubmitText } kind = None;
    std::string id, text;
    int delta = 0;
};
class MenuNavigation {
public:
    explicit MenuNavigation(const std::string& root = "home") : stack_{root} { states_[root]; }
    const std::string& Screen() const { return stack_.back(); }
    const std::string& Parent() const { return stack_.size()>1 ? stack_[stack_.size()-2] : stack_.front(); }
    void Push(const std::string& screen) {
        if (screen.empty() || screen == Screen()) return;
        Cancel(); stack_.push_back(screen); states_[screen]; NeutralGate();
    }
    void Home() { Cancel(); stack_.resize(1); NeutralGate(); }
    void Cancel() {
        const bool modal = Editing() || Confirming();
        dialog_.clear(); editing_.clear(); draft_.clear();
        if (modal) NeutralGate();
    }
    void NeutralGate() { previous_=~0u; armed_=false; direction_=0; nextRepeat_=0; }
    const std::string& Focus() const { return states_.at(Screen()).id; }
    bool Editing() const { return !editing_.empty(); }
    bool Confirming() const { return !dialog_.empty(); }
    bool ConfirmSelected() const { return confirmSelected_; }
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
                return e.id == id && (text ? e.text : e.confirm) && (e.enabled || e.pending);
            });
        };
        // A transient checkpoint must not discard a draft. Removed entries,
        // changed kinds and genuinely unavailable actions still cancel immediately.
        if ((!dialog_.empty() && !valid(dialog_, false)) ||
            (!editing_.empty() && !valid(editing_, true))) Cancel();
        const auto& owner = Editing() ? editing_ : dialog_;
        if (!owner.empty()) {
            const auto target = std::find_if(entries.begin(), entries.end(),
                [&](const MenuEntry& e) { return e.id == owner; });
            state.id = owner; state.index = static_cast<std::size_t>(target - entries.begin());
        }
    }
    void Focus(const std::string& id,const std::vector<MenuEntry>& entries) {
        if ((Editing() && id != editing_) || (Confirming() && id != dialog_)) return;
        auto it=std::find_if(entries.begin(),entries.end(),[&](const MenuEntry& e){return e.id==id;});
        if(it!=entries.end()) { auto& s=states_[Screen()]; s.id=id; s.index=it-entries.begin(); }
    }
    MenuAction Return() {
        if (Editing()||Confirming()) { Cancel(); return {}; }
        if (stack_.size()>1) { stack_.pop_back(); NeutralGate(); return {MenuAction::Returned}; }
        return {MenuAction::Close};
    }
    MenuAction Choose(const std::vector<MenuEntry>& entries) {
        Reconcile(entries);
        if (entries.empty() || Editing() || Confirming()) return {};
        const auto& e=entries[states_[Screen()].index];
        if(!e.enabled) return {};
        if(e.adjustable) return {};
        if(e.text) { editing_=e.id; draft_=e.value; return {}; }
        if(e.confirm) { dialog_=e.id; confirmSelected_=false; return {}; }
        return {MenuAction::Activate,e.id};
    }
    MenuAction Confirm(bool accept,const std::vector<MenuEntry>& entries) {
        Reconcile(entries);
        if (!Confirming()) return {};
        if (accept && !std::any_of(entries.begin(), entries.end(), [&](const MenuEntry& e) {
            return e.id == dialog_ && e.enabled && e.confirm;
        })) return {};
        const auto id=dialog_; Cancel();
        return accept ? MenuAction{MenuAction::Activate,id} : MenuAction{};
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
        if(pressed&MenuInput::Back) return deferBack&&!Editing()&&!Confirming()?MenuAction{MenuAction::Back}:Return();
        if(Editing()) {
            if(!in.acceptText) return {};
            return deferText ? MenuAction{MenuAction::SubmitText} : AcceptText(entries);
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
        if(direction&MenuInput::Up) { if(index>=columns)index-=columns; }
        else if(direction&MenuInput::Down) { if(index/columns<(count-1)/columns)index=(std::min)(count-1,index+columns); }
        else if(columns>1 && delta && index/columns==(index+delta)/columns && index+delta>=0 && index+delta<count) index+=delta;
        s.index=index; s.id=entries[index].id;
        return {};
    }
private:
    static constexpr double RepeatDelay=.35, RepeatInterval=.085;
    struct State { std::string id; std::size_t index=0; float scroll=0; };
    std::vector<std::string> stack_;
    std::map<std::string,State> states_;
    std::string dialog_,editing_,draft_;
    bool confirmSelected_=false, armed_=true;
    unsigned previous_=0,direction_=0;
    double nextRepeat_=0;
};
} }
