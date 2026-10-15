#pragma once
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplayExport.hxx"
#include "../common/ReplayInputs.hxx"
#include "FighterSelector.hxx"
#include <algorithm>
#include <cstdint>
#include <string>

namespace sf4e { namespace ui {
// What a replay's file says of its match, as text: the one place it is
// worded, for the Replays list and the Inputs and stats screen alike.
inline std::string ReplayPlayerName(const std::string names[2],int side){return names[side].empty()?(side?"P2":"P1"):names[side];}
inline std::string ReplayLabel(const std::string& date,const std::string names[2],const int fighters[2]) {
 const auto player=[&](int side) {
  const auto* fighter=selection::FindFighter(fighters[side]);
  const std::string name=fighter?fighter->name:loc::T("common.unavailable");
  return names[side].empty()?name:loc::Tf("replays.player",names[side],name);
 };
 return date+"  "+loc::Tf("replays.fighters",player(0),player(1));
}
// A replay's row on the Replays list, whose portraits show the fighters: the
// date and the players' names, or a fighter's name where no player name was noted.
inline std::string ReplayRowLabel(const std::string& date,const std::string names[2],const int fighters[2]) {
 const auto player=[&](int side)->std::string {
  if(!names[side].empty())return names[side];
  const auto* fighter=selection::FindFighter(fighters[side]);
  return fighter?fighter->name:loc::T("common.unavailable");
 };
 return date+"  "+loc::Tf("replays.fighters",player(0),player(1));
}
// What Ember knows of a replay, after its score: Spectated when this PC only
// watched the match live, Seen once its replay was played with Watch now,
// Video once it was exported.
inline std::string ReplayTagsText(bool spectated,bool seen,bool video) {
 std::string tags=spectated&&seen?loc::T("replays.spectated_seen"):spectated?loc::T("replays.spectated"):seen?loc::T("replays.seen"):"";
 if(video)tags=tags.empty()?std::string(loc::T("replays.video")):tags+", "+loc::T("replays.video");
 return tags;
}
// A running export against its replay: the fighting recorded so far and the
// replay's length, "1:02 / 2:41". Only the first when the length is not known.
inline std::string ExportProgressText(std::uint32_t frames,std::uint32_t total) {
 if(!total)return replayinputs::Clock(frames);
 return loc::Tf("export.progress",replayinputs::Clock((std::min)(frames,total)),replayinputs::Clock(total));
}
// The running export's row value at each stage; nothing with no export.
inline std::string ExportStageText(replay::ExportStage stage,std::uint32_t frames,std::uint32_t total) {
 switch(stage){
 case replay::ExportStage::Starting: return loc::T("export.starting");
 case replay::ExportStage::Recording: return ExportProgressText(frames,total);
 case replay::ExportStage::Finishing: return loc::T("export.finishing");
 case replay::ExportStage::Cancelling: return loc::T("export.cancelling");
 default: return std::string();
 }
}
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
 for(int button=0;button<6;button++)presses+=std::string(button?", ":"")+replayinputs::ButtonNames[button]+" "+std::to_string(s.presses[button]);
 return presses;
}
inline std::string ActivityText(const replayinputs::Stats& s){const unsigned perMinute=s.PerMinute(),jumps=s.jumps,crouched=s.CrouchedPercent();return loc::Tf("inputs.activity",perMinute,jumps,crouched);}
} }
