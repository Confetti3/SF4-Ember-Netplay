#pragma once
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplayExport.hxx"
#include "../common/ReplayInputs.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/ReplaySets.hxx"
#include "../platform/Utf8.hxx"
#include "FighterSelector.hxx"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>

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
// A fighter's name, or that it is not one Ember knows.
inline std::string FighterName(int id) {
 const auto* fighter=selection::FindFighter(id);
 return fighter?std::string(fighter->name):std::string(loc::T("common.unavailable"));
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
// What the Replays list's Show row lists, in the order Left and Right step
// through them: every replay, the matches this PC played, the ones it only
// watched live, those not played back yet, those with a video beside them,
// and those the player keeps.
enum class ReplayShow { All, Mine, Spectated, Unseen, Video, Kept };
constexpr const char* kReplayShowKeys[]={"replays.show.all","replays.show.mine","replays.show.spectated","replays.show.unseen","replays.show.video","replays.show.kept"};
// A choice stepped by Left and Right, held to its words.
inline int StepWord(int at,int delta,std::size_t words){return (std::max)(0,(std::min)(static_cast<int>(words)-1,at+delta));}
inline ReplayShow Stepped(ReplayShow show,int delta){return static_cast<ReplayShow>(StepWord(static_cast<int>(show),delta,std::size(kReplayShowKeys)));}
// What the Sort row orders the list by. The archive lists newest first.
enum class ReplaySort { Newest, Oldest, Player, Fighter, Longest };
constexpr const char* kReplaySortKeys[]={"replays.sort.newest","replays.sort.oldest","replays.sort.player","replays.sort.fighter","replays.sort.longest"};
inline ReplaySort Stepped(ReplaySort sort,int delta){return static_cast<ReplaySort>(StepWord(static_cast<int>(sort),delta,std::size(kReplaySortKeys)));}
static_assert(selection::FighterCount<=64,"the fighter filter is a bit per native id");
// A replay's side (0 either, 1 player 1, 2 player 2) played one of the fighters whose native
// ids are the bits of `fighters`.
inline bool ReplayHasFighter(const platform::replays::ArchivedReplay& replay,std::uint64_t fighters,int side) {
 for(int s=0;s<2;s++){
  const int f=replay.fighters[s];
  if((side==0||side==s+1)&&f>=0&&f<64&&(fighters>>f&1))return true;
 }
 return false;
}
// The side filter's words (Either side, Player 1, Player 2).
constexpr const char* kReplaySideKeys[]={"replays.side.either","replays.side.one","replays.side.two"};
// A name in lower case, so Search matches it in any case.
inline std::wstring LoweredText(const std::string& text){std::wstring wide=platform::Utf8ToWide(text.c_str());for(auto& c:wide)c=static_cast<wchar_t>(std::towlower(c));return wide;}
// Whether the list shows a replay under that Show and a lowered Search text,
// which a player's or a fighter's name must contain, and fighters (native ids as bits, none for any)
// one side, or the one `side` names, must have played.
inline bool ReplayShown(const platform::replays::ArchivedReplay& replay,ReplayShow show,const std::wstring& search,std::uint64_t fighters=0,int side=0) {
 const bool spectated=replay.provenance==replayfiles::Provenance::Spectated;
 if((show==ReplayShow::Mine&&spectated)||(show==ReplayShow::Spectated&&!spectated)||(show==ReplayShow::Unseen&&replay.watched)||
  (show==ReplayShow::Video&&!replay.video)||(show==ReplayShow::Kept&&!replay.kept))return false;
 if(fighters&&!ReplayHasFighter(replay,fighters,side))return false;
 if(search.empty())return true;
 for(int side=0;side<2;side++){
  if(LoweredText(replay.names[side]).find(search)!=std::wstring::npos)return true;
  if(const auto* fighter=selection::FindFighter(replay.fighters[side]))if(LoweredText(fighter->name).find(search)!=std::wstring::npos)return true;
 }
 return false;
}
// Whether `a` comes before `b` under Sort. Ties keep the archive's order (newest first),
// so sort with std::stable_sort. Player and Fighter order by player 1, then player 2.
inline bool ReplayBefore(const platform::replays::ArchivedReplay& a,const platform::replays::ArchivedReplay& b,ReplaySort sort) {
 const auto fighterName=[](const platform::replays::ArchivedReplay& r,int side){const auto* f=selection::FindFighter(r.fighters[side]);return std::string_view(f?f->name:"");};
 switch(sort){
 case ReplaySort::Oldest: return a.time<b.time;
 case ReplaySort::Player: return std::tie(a.names[0],a.names[1])<std::tie(b.names[0],b.names[1]);
 case ReplaySort::Fighter: return std::make_tuple(fighterName(a,0),fighterName(a,1))<std::make_tuple(fighterName(b,0),fighterName(b,1));
 case ReplaySort::Longest: return (a.summary?a.summary->frames:0)>(b.summary?b.summary->frames:0);
 default: return false;
 }
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
// How a match went, "First to 2 \xC2\xB7 1-0 / 1-1 / 2-1": what it was played to and the score after each
// round. Empty when the score is not known.
inline std::string ScoreChangesText(const replayinputs::Summary& s){
 if(!s.scored)return std::string();
 std::string text=loc::Tf("rules.set_length.first_to",s.firstTo)+" \xC2\xB7 ";
 for(const auto& wins:s.roundScores)text+=std::to_string(wins[0])+"-"+std::to_string(wins[1])+" / ";
 return text+std::to_string(s.score[0])+"-"+std::to_string(s.score[1]);
}
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
// A set's games oldest first, each one's score for the set's last game's player 1 ("2-0 / 1-2 / 2-1"),
// a dash for a game whose score is not known.
inline std::string SetScoresText(const platform::replays::ArchivedSet& set){
 const std::string& player=set.games.back()->names[0];
 std::string text;
 for(const auto* game:set.games){
  const bool known=game->summary&&game->summary->scored;
  const int seat=game->names[0]!=player; // the seat the set's first player sat in
  text+=(text.empty()?"":" / ")+(known?std::to_string(game->summary->score[seat])+"-"+std::to_string(game->summary->score[!seat]):std::string("-"));
 }
 return text;
}
} }
