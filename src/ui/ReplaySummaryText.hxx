#pragma once
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplayInputs.hxx"
#include "FighterSelector.hxx"
#include <string>

namespace sf4e { namespace ui {
// What a replay's file says of its match, as text: the one place it is
// worded, for the Replays list and the Inputs and stats screen alike.
inline std::string ReplayPlayerName(const std::string names[2],int side){return names[side].empty()?(side?"P2":"P1"):names[side];}
// "2-1", player 1 first, or nothing when the last round's winner is not known.
inline std::string ScoreText(const replayinputs::Summary& s){return s.scored?std::to_string(s.score[0])+"-"+std::to_string(s.score[1]):std::string();}
inline std::string MatchText(const replayinputs::Summary& s){const std::string length=replayinputs::Clock(s.frames);const unsigned rounds=s.rounds;return loc::Tf("inputs.length_value",rounds,length);}
// The costume, color and Ultra in Fighter Select's own words, or that a choice is not one Ember knows.
inline std::string LookText(const replayinputs::Player& p){
 const bool known=selection::FindFighter(p.fighter)&&p.costume>=0&&p.costume<selection::CostumeCount(p.fighter)&&p.color>=0&&p.ultra>=0;
 if(!known)return loc::T("inputs.look_unknown");
 selection::Pick pick;pick.fighter=p.fighter;pick.costume=p.costume;
 return loc::Tf("selection.appearance_value",CostumeLabel(pick),p.color+1)+", "+UltraLabel(p.ultra);
}
inline std::string PressesText(const replayinputs::Stats& s){
 std::string presses;
 for(int button=0;button<6;button++)presses+=std::string(button?"  ":"")+replayinputs::ButtonNames[button]+" "+std::to_string(s.presses[button]);
 return presses;
}
inline std::string ActivityText(const replayinputs::Stats& s){const unsigned perMinute=s.PerMinute(),jumps=s.jumps,crouched=s.CrouchedPercent();return loc::Tf("inputs.activity",perMinute,jumps,crouched);}
} }
