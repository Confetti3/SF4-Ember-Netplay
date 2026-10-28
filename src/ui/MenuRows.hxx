#pragma once
#include "MenuNavigation.hxx"
#include "../common/RoomRules.hxx"
#include "../netplay/BoolPreferences.hxx"
#include "../common/InputDelay.hxx"
#include "../common/Localization.hxx"
#include <cstring>
#include <algorithm>
#include <string>
namespace sf4e { namespace ui {
// Auto's style by name, and the delay row's line for Auto in that style.
inline const char* AutoDelayStyleName(int style) {
    static const char* const names[AutoDelayStyles] = {"settings.auto_delay_style.balanced", "settings.auto_delay_style.responsive", "settings.auto_delay_style.smooth"};
    return loc::T(names[(std::max)(0, (std::min)(AutoDelayStyles - 1, style))]);
}
inline std::string AutoDelayDetail(int style) {
    const auto range = AutoInputDelayRange(static_cast<AutoDelayStyle>((std::max)(0, (std::min)(AutoDelayStyles - 1, style))));
    return loc::Tf("room.input_delay.auto.detail", range.minimum, range.maximum);
}
inline bool SamePreferences(const netplay::PlayerPreferences& a,const netplay::PlayerPreferences& b) {
    for(const auto& p:netplay::BoolPreferences) if(a.*p.member!=b.*p.member) return false;
    return a.displayName==b.displayName&&a.mainFighter==b.mainFighter&&a.inputDelay==b.inputDelay&&a.autoInputDelay==b.autoInputDelay&&a.autoDelayStyle==b.autoDelayStyle&&
        a.buttonPrompts==b.buttonPrompts&&a.controllerMappings==b.controllerMappings&&a.buttonLayouts==b.buttonLayouts&&a.matchHudSize==b.matchHudSize&&a.matchHudAnchor==b.matchHudAnchor&&a.pictureSpot==b.pictureSpot&&a.pictureMode==b.pictureMode&&a.wornTitle==b.wornTitle&&a.wornIcon==b.wornIcon&&a.roomColumn==b.roomColumn&&a.roomSplit==b.roomSplit&&a.speech==b.speech&&a.matchHudLayout==b.matchHudLayout&&a.matchHudNameOffset==b.matchHudNameOffset&&a.readySoundVolume==b.readySoundVolume&&a.interfaceScale==b.interfaceScale&&a.roomSideWidth==b.roomSideWidth&&a.roomMemberShare==b.roomMemberShare&&
        a.roomName==b.roomName&&a.roomCapacity==b.roomCapacity&&a.roomPublic==b.roomPublic&&a.roomDelayPolicy==b.roomDelayPolicy&&a.tableRules==b.tableRules&&a.publicTableRules==b.publicTableRules&&a.roomTemplates==b.roomTemplates&&a.country==b.country;
}
// Only what a room leaves open changed: button layouts and the room board's column.
inline bool OnlyRoomSettingsChanged(const netplay::PlayerPreferences& a, const netplay::PlayerPreferences& b) {
    auto compared = a;
    compared.CopyRoomSettings(b);
    return SamePreferences(compared, b);
}
inline MenuEntry Row(std::string id,std::string label,std::string detail,bool enabled=true) {
    return {std::move(id),std::move(label),std::move(detail),{},enabled};
}
inline MenuEntry Value(std::string id,std::string label,std::string value,std::string detail,bool enabled=true) {
    auto e=Row(std::move(id),std::move(label),std::move(detail),enabled); e.value=std::move(value); e.adjustable=true; return e;
}
// The saved on/off preferences (netplay::BoolPreferences) as menu rows: each
// row's id and the preference it turns. Right turns a row On and Left Off.
// "game-reads-pads" is On while the game reads the pads itself, which is with
// the controller layer off. Edge spacing is a saved bool too, but its row
// reads Raised or Normal, so it is not one of these.
struct OnOffRow { const char* id; bool netplay::PlayerPreferences::* member; bool inverted; };
inline constexpr OnOffRow OnOffRows[] = {
    {"hud",&netplay::PlayerPreferences::showMatchHud,false},{"ready-sound",&netplay::PlayerPreferences::readySound,false},
    {"shortcut-hint",&netplay::PlayerPreferences::showShortcutHint,false},{"class-emblems",&netplay::PlayerPreferences::classEmblems,false},
    {"match-frame-meter",&netplay::PlayerPreferences::matchFrameMeter,false},{"training-auto-ready",&netplay::PlayerPreferences::trainingAutoReady,false},
    {"replay-save-watched",&netplay::PlayerPreferences::recordWatched,false},{"watched-picture",&netplay::PlayerPreferences::watchedPicture,false},
    {"replay-share",&netplay::PlayerPreferences::shareReplays,false},
    {"presence",&netplay::PlayerPreferences::discordPresence,false},{"invites",&netplay::PlayerPreferences::discordInvites,false},
    {"share-steam",&netplay::PlayerPreferences::shareSteamProfile,false},{"send-reports",&netplay::PlayerPreferences::sendProblemReports,false},
    {"background-play",&netplay::PlayerPreferences::backgroundPlay,false},{"lower-input-lag",&netplay::PlayerPreferences::lowerInputLag,false},
    {"lower-keyboard-lag",&netplay::PlayerPreferences::lowerKeyboardLag,false},{"faster-display",&netplay::PlayerPreferences::fasterDisplay,false},
    {"borderless",&netplay::PlayerPreferences::borderless,false},{"game-reads-pads",&netplay::PlayerPreferences::controllerLayer,true},
};
constexpr bool SavedOnOff(bool netplay::PlayerPreferences::* member) {
    for(const auto& p:netplay::BoolPreferences) if(p.member==member) return true;
    return false;
}
constexpr bool OnOffRowsSaved() {
    for(const auto& row:OnOffRows) if(!SavedOnOff(row.member)) return false;
    return true;
}
static_assert(OnOffRowsSaved(),"An on/off row turns a preference that is not saved as one");
inline const OnOffRow* FindOnOffRow(const char* id) {
    for(const auto& row:OnOffRows) if(std::strcmp(row.id,id)==0) return &row;
    return nullptr;
}
// An on/off row; `id` is one of OnOffRows.
inline MenuEntry OnOffValue(const netplay::PlayerPreferences& p,const char* id,std::string label,std::string detail,bool enabled=true) {
    const OnOffRow& row=*FindOnOffRow(id);
    return Value(id,std::move(label),loc::T(p.*row.member!=row.inverted?"common.on":"common.off"),std::move(detail),enabled);
}
// Left or Right on an on/off row; false for any other row.
inline bool AdjustOnOff(netplay::PlayerPreferences& p,const std::string& id,int delta) {
    const OnOffRow* row=FindOnOffRow(id.c_str());
    if(!row) return false;
    p.*row->member=row->inverted?delta<0:delta>0;
    return true;
}
// A value shown for information: it cannot be focused for adjustment.
inline MenuEntry ReadOnlyValue(std::string id,std::string label,std::string value,std::string detail) {
    auto e=Value(std::move(id),std::move(label),std::move(value),std::move(detail),false); e.adjustable=false; return e;
}
// A row that only informs: it can be focused, its value and detail read, and Select does nothing.
inline MenuEntry InfoRow(std::string id,std::string label,std::string value,std::string detail) {
    auto e=Row(std::move(id),std::move(label),std::move(detail)); e.value=std::move(value); e.info=true; return e;
}
// V2: a value row's every value, in the order Left and Right step through
// them, as choices "#<n>", so Select opens them all in the value panel. The
// row's own value text marks where it is; a pick comes back as the steps to
// it (an Adjust, GameMenu::Finish), through the handler the arrows use.
inline void OfferSteps(MenuEntry& e,const std::vector<std::string>& labels) {
    if(!e.adjustable||!e.choices.empty()||labels.size()<2) return;
    const auto at=std::find(labels.begin(),labels.end(),e.value);
    if(at==labels.end()) return;
    for(std::size_t i=0;i<labels.size();++i) e.choices.push_back({"#"+std::to_string(i),labels[i],{},e.enabled});
    e.chosen="#"+std::to_string(at-labels.begin());
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
// A Practice room's games always take the longest round time
// (room::MatchRoundTime), so its rules have no round time row.
inline void RuleRows(std::vector<MenuEntry>& rows,const room::Rules& rules,bool enabled,const char* reason,bool practice) {
    rows.push_back(Value("edition",loc::T("rules.edition_select"),rules.editionSelect?loc::T("common.on"):loc::T("common.off"),reason,enabled));
    rows.push_back(Value("rounds",loc::T("rules.rounds"),std::to_string(rules.roundCount),reason,enabled));
    if(!practice) rows.push_back(Value("time",loc::T("rules.round_time"),std::to_string(rules.roundTime),reason,enabled));
    rows.push_back(Value("set-length",loc::T("rules.set_length"),SetLengthText(rules.format),enabled?loc::T("rules.set_length.detail"):reason,enabled));
    // Only a set that ends can hand a seat over.
    const bool rotates=rules.format!=room::SetFormat::Unlimited;
    rows.push_back(Value("rotation",loc::T("rules.rotation"),RotationText(rules.rotation),
        !rotates?loc::T("rules.rotation.needs_set"):enabled?RotationDetail(rules.rotation):reason,enabled&&rotates));
    rows.push_back(Value("instant-rematch",loc::T("rules.instant_rematch"),loc::T(rules.instantRematch?"common.on":"common.off"),
        enabled?loc::T("rules.instant_rematch.detail"):reason,enabled));
}
// Create's choice of a Practice room, fixed for the room's life (room::Snapshot::practice).
inline MenuEntry PracticeRoomRow(bool on,bool enabled) {
    return Value("practice-room",loc::T("room.practice"),loc::T(on?"common.on":"common.off"),
        loc::Tf("room.practice.detail",room::PracticeRoundTime),enabled);
}
// Create's choice of the room's type, fixed for its life (room::Snapshot::arcade).
inline MenuEntry RoomTypeRow(room::RoomType type,bool enabled) {
    static const char* const names[]={"room.type.classic","room.type.arcade","room.type.king"};
    static const char* const details[]={"room.type.classic_detail","room.type.arcade_detail","room.type.king_detail"};
    return Value("room-type",loc::T("room.type"),loc::T(names[type]),loc::T(details[type]),enabled);
}
// Create's choice of whose input delay the room plays at, fixed for the room's life (room::Snapshot::delayPolicy).
inline MenuEntry DelayPolicyRow(room::DelayPolicy policy,bool enabled) {
    return Value("delay-policy",loc::T("room.delay_policy"),
        loc::T(policy==room::DelayPolicy::Own?"room.delay_policy.own":"room.delay_policy.shared"),loc::T("room.delay_policy.detail"),enabled);
}
inline bool AdjustRule(room::Rules& rules,const MenuAction& a) {
    if(a.kind!=MenuAction::Adjust) return false;
    if(a.id=="edition") rules.editionSelect=a.delta>0;
    else if(a.id=="rounds") Step(rules.roundCount,{1,3,5,7,15,99},a.delta);
    else if(a.id=="time") Step(rules.roundTime,std::vector<int>(std::begin(room::RoundTimes),std::end(room::RoundTimes)),a.delta);
    else if(a.id=="set-length") {
        std::vector<int> lengths;
        for(int n=0;n<=room::MaxSetLength;++n) lengths.push_back(n);
        Step(rules.format,lengths,a.delta);
    }
    else if(a.id=="rotation"&&rules.format!=room::SetFormat::Unlimited) Step(rules.rotation,{0,1,2},a.delta);
    else if(a.id=="instant-rematch") rules.instantRematch=a.delta>0;
    else return false;
    return true;
}
} }
