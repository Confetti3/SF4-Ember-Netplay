#pragma once
#include "MenuNavigation.hxx"
#include "../common/RoomRules.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../common/Localization.hxx"
namespace sf4e { namespace ui {
inline bool SamePreferences(const netplay::PlayerPreferences& a,const netplay::PlayerPreferences& b) {
    return a.displayName==b.displayName&&a.mainFighter==b.mainFighter&&a.inputDelay==b.inputDelay&&a.showMatchHud==b.showMatchHud&&
        a.matchHudSize==b.matchHudSize&&a.matchHudRaised==b.matchHudRaised&&a.matchHudAnchor==b.matchHudAnchor&&a.matchHudLayout==b.matchHudLayout&&a.matchHudNames==b.matchHudNames&&a.readySound==b.readySound&&a.readySoundVolume==b.readySoundVolume&&a.discordPresence==b.discordPresence&&a.discordInvites==b.discordInvites&&a.interfaceScale==b.interfaceScale&&
        a.roomName==b.roomName&&a.roomCapacity==b.roomCapacity&&a.roomPublic==b.roomPublic&&a.tableRules==b.tableRules;
}
inline MenuEntry Row(std::string id,std::string label,std::string detail,bool enabled=true) {
    return {std::move(id),std::move(label),std::move(detail),{},enabled};
}
inline MenuEntry Value(std::string id,std::string label,std::string value,std::string detail,bool enabled=true) {
    auto e=Row(std::move(id),std::move(label),std::move(detail),enabled); e.value=std::move(value); e.adjustable=true; return e;
}
// A value shown for information: it cannot be focused for adjustment.
inline MenuEntry ReadOnlyValue(std::string id,std::string label,std::string value,std::string detail) {
    auto e=Value(std::move(id),std::move(label),std::move(value),std::move(detail),false); e.adjustable=false; return e;
}
// A row that only informs: it can be focused, its value and detail read, and Select does nothing.
inline MenuEntry InfoRow(std::string id,std::string label,std::string value,std::string detail) {
    auto e=Row(std::move(id),std::move(label),std::move(detail)); e.value=std::move(value); e.info=true; return e;
}
inline MenuEntry TextRow(std::string id,std::string label,std::string value,std::size_t limit,bool enabled=true) {
    auto e=Value(std::move(id),std::move(label),value,enabled?loc::T("menu.edit_detail"):loc::T("menu.edit_unavailable"),enabled);
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
inline std::string SetLengthText(room::SetFormat format) {
    return format==room::SetFormat::Unlimited?loc::T("rules.set_length.none"):loc::Tf("rules.set_length.first_to",static_cast<int>(format));
}
inline const char* RotationText(room::RotationMode rotation) {
    return loc::T(rotation==room::RotationMode::LoserStays?"rules.rotation.loser_stays":
        rotation==room::RotationMode::BothRotate?"rules.rotation.both_rotate":"rules.rotation.winner_stays");
}
inline const char* RotationDetail(room::RotationMode rotation) {
    return loc::T(rotation==room::RotationMode::LoserStays?"rules.rotation.loser_stays.detail":
        rotation==room::RotationMode::BothRotate?"rules.rotation.both_rotate.detail":"rules.rotation.winner_stays.detail");
}
// "First to 2, Winner stays", or "No set length" when nobody rotates.
inline std::string SetSummaryText(const room::Rules& rules) {
    if(rules.format==room::SetFormat::Unlimited) return SetLengthText(rules.format);
    return loc::Tf("rules.set_summary",SetLengthText(rules.format),RotationText(rules.rotation));
}
inline void RuleRows(std::vector<MenuEntry>& rows,const room::Rules& rules,bool enabled,const char* reason) {
    rows.push_back(Value("edition",loc::T("rules.edition_select"),rules.editionSelect?loc::T("common.on"):loc::T("common.off"),reason,enabled));
    rows.push_back(Value("rounds",loc::T("rules.rounds"),std::to_string(rules.roundCount),reason,enabled));
    rows.push_back(Value("time",loc::T("rules.round_time"),std::to_string(rules.roundTime),reason,enabled));
    rows.push_back(Value("set-length",loc::T("rules.set_length"),SetLengthText(rules.format),enabled?loc::T("rules.set_length.detail"):reason,enabled));
    // Only a set that ends can hand a seat over.
    const bool rotates=rules.format!=room::SetFormat::Unlimited;
    rows.push_back(Value("rotation",loc::T("rules.rotation"),RotationText(rules.rotation),
        !rotates?loc::T("rules.rotation.needs_set"):enabled?RotationDetail(rules.rotation):reason,enabled&&rotates));
}
inline bool AdjustRule(room::Rules& rules,const MenuAction& a) {
    if(a.kind!=MenuAction::Adjust) return false;
    if(a.id=="edition") rules.editionSelect=a.delta>0;
    else if(a.id=="rounds") Step(rules.roundCount,{1,3,5,7,15,99},a.delta);
    else if(a.id=="time") Step(rules.roundTime,{30,60,99,300,9999},a.delta);
    else if(a.id=="set-length") Step(rules.format,{0,1,2,3,5},a.delta);
    else if(a.id=="rotation"&&rules.format!=room::SetFormat::Unlimited) Step(rules.rotation,{0,1,2},a.delta);
    else return false;
    return true;
}
} }
