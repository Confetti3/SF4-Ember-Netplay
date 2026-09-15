#pragma once
#include "MenuNavigation.hxx"
#include "../common/RoomRules.hxx"
#include "../netplay/PlayerPreferences.hxx"
namespace sf4e { namespace ui {
inline bool SamePreferences(const netplay::PlayerPreferences& a,const netplay::PlayerPreferences& b) {
    return a.displayName==b.displayName&&a.mainFighter==b.mainFighter&&a.inputDelay==b.inputDelay&&a.showMatchHud==b.showMatchHud&&
        a.matchHudSize==b.matchHudSize&&a.matchHudRaised==b.matchHudRaised&&a.discordPresence==b.discordPresence&&a.discordInvites==b.discordInvites&&a.interfaceScale==b.interfaceScale&&
        a.roomName==b.roomName&&a.roomCapacity==b.roomCapacity&&a.tableRules==b.tableRules;
}
inline MenuEntry Row(std::string id,std::string label,std::string detail,bool enabled=true) {
    return {std::move(id),std::move(label),std::move(detail),{},enabled};
}
inline MenuEntry Value(std::string id,std::string label,std::string value,std::string detail,bool enabled=true) {
    auto e=Row(std::move(id),std::move(label),std::move(detail),enabled); e.value=std::move(value); e.adjustable=true; return e;
}
inline MenuEntry TextRow(std::string id,std::string label,std::string value,std::size_t limit,bool enabled=true) {
    auto e=Value(std::move(id),std::move(label),value,enabled?"Select to edit. Type or paste, then Enter to accept; Back cancels.":"Editing is unavailable in the current state.",enabled);
    e.adjustable=false; e.text=true; e.textLimit=limit; return e;
}
inline MenuEntry ConfirmRow(std::string id,std::string label,std::string detail,bool enabled=true) {
    auto e=Row(std::move(id),std::move(label),std::move(detail),enabled); e.confirm=true; return e;
}
template<typename T> inline void Step(T& value,const std::vector<int>& choices,int delta) {
    if(choices.empty()) return;
    auto it=std::find(choices.begin(),choices.end(),static_cast<int>(value));
    int index=it==choices.end()?0:static_cast<int>(it-choices.begin());
    value=static_cast<T>(choices[(std::max)(0,(std::min)(static_cast<int>(choices.size())-1,index+delta))]);
}
inline void RuleRows(std::vector<MenuEntry>& rows,const room::Rules& rules,bool enabled,const char* reason) {
    rows.push_back(Value("edition","Edition Select",rules.editionSelect?"On":"Off",reason,enabled));
    rows.push_back(Value("rounds","Rounds",std::to_string(rules.roundCount),reason,enabled));
    rows.push_back(Value("time","Round time",std::to_string(rules.roundTime),reason,enabled));
}
inline bool AdjustRule(room::Rules& rules,const MenuAction& a) {
    if(a.kind!=MenuAction::Adjust) return false;
    if(a.id=="edition") rules.editionSelect=a.delta>0;
    else if(a.id=="rounds") Step(rules.roundCount,{1,3,5,7,15,99},a.delta);
    else if(a.id=="time") Step(rules.roundTime,{30,60,99,300,9999},a.delta);
    else return false;
    return true;
}
} }
