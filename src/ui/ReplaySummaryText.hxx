#pragma once
#include "../common/GameCatalog.hxx"
#include "../common/SelectionPick.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplayExport.hxx"
#include "../common/ReplayInputs.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include "SelectionLabels.hxx"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iterator>
#include <string>

namespace sf4e { namespace ui {
// What a replay's file says of its match, as text: the one place it is
// worded, for the Replays list and the Inputs and stats screen alike.
inline std::string ReplayPlayerName(const std::string names[2],int side){return names[side].empty()?(side?"P2":"P1"):names[side];}
// A fighter by name, or that the replay's fighter is not one the game's catalog has.
inline std::string ReplayFighterName(int fighter){return ember::FighterNameOr(fighter,loc::T("common.unavailable"));}
inline std::string ReplayLabel(const std::string& date,const std::string names[2],const int fighters[2]) {
 const auto player=[&](int side) {
  const std::string name=ReplayFighterName(fighters[side]);
  return names[side].empty()?name:loc::Tf("replays.player",names[side],name);
 };
 return date+"  "+loc::Tf("replays.fighters",player(0),player(1));
}
// A replay's row on the Replays list, whose portraits show the fighters: the
// date and the players' names, or a fighter's name where no player name was noted.
inline std::string ReplayRowLabel(const std::string& date,const std::string names[2],const int fighters[2]) {
 const auto player=[&](int side)->std::string {
  return names[side].empty()?ReplayFighterName(fighters[side]):names[side];
 };
 return date+"  "+loc::Tf("replays.fighters",player(0),player(1));
}
// What the Replays list's Show row lists, in the order Left and Right step
// through them: every replay, the matches this PC played, the ones it only
// watched live, those not played back yet, those with a video beside them,
// and those the player keeps.
enum class ReplayShow { All, Mine, Spectated, Unseen, Video, Kept };
constexpr const char* kReplayShowKeys[]={"replays.show.all","replays.show.mine","replays.show.spectated","replays.show.unseen","replays.show.video","replays.show.kept"};
inline ReplayShow Stepped(ReplayShow show,int delta){
 const int last=static_cast<int>(std::size(kReplayShowKeys))-1;
 return static_cast<ReplayShow>((std::max)(0,(std::min)(last,static_cast<int>(show)+delta)));
}
// A name in lower case, so Search matches it in any case.
inline std::wstring LoweredText(const std::string& text){std::wstring wide=platform::Utf8ToWide(text.c_str());for(auto& c:wide)c=static_cast<wchar_t>(std::towlower(c));return wide;}
// Whether the list shows a replay under that Show and a lowered Search text,
// which a player's or a fighter's name must contain.
inline bool ReplayShown(const platform::replays::ArchivedReplay& replay,ReplayShow show,const std::wstring& search) {
 const bool spectated=replay.provenance==replayfiles::Provenance::Spectated;
 if((show==ReplayShow::Mine&&spectated)||(show==ReplayShow::Spectated&&!spectated)||(show==ReplayShow::Unseen&&replay.watched)||
  (show==ReplayShow::Video&&!replay.video)||(show==ReplayShow::Kept&&!replay.kept))return false;
 if(search.empty())return true;
 for(int side=0;side<2;side++){
  if(LoweredText(replay.names[side]).find(search)!=std::wstring::npos)return true;
  if(ember::ValidFighter(replay.fighters[side])&&LoweredText(ember::Catalog().fighterName(replay.fighters[side])).find(search)!=std::wstring::npos)return true;
 }
 return false;
}
// What Ember knows of a replay, after its score: Spectated when this PC only
// watched the match live, Seen once its replay was played with Watch now,
// Video once it was exported, Kept while the player keeps it.
inline std::string ReplayTagsText(bool spectated,bool seen,bool video,bool kept) {
 std::string tags=spectated&&seen?loc::T("replays.spectated_seen"):spectated?loc::T("replays.spectated"):seen?loc::T("replays.seen"):"";
 const auto add=[&](const char* key){tags=tags.empty()?std::string(loc::T(key)):tags+", "+loc::T(key);};
 if(video)add("replays.video");
 if(kept)add("replays.kept");
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
 const bool known=ember::ValidFighter(p.fighter)&&p.costume>=0&&p.costume<ember::Catalog().costumeCount(p.fighter)&&p.color>=0&&p.ultra>=0;
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
