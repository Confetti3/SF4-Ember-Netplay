#include "ReplaysPanel.hxx"
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "ReplaySummaryText.hxx"
#include "MenuDialogs.hxx"
#include "Theme.hxx"
#include "../platform/ReplaySets.hxx"
#include "V2.hxx"
#include "../platform/Utf8.hxx"
#include "../common/ReplayLink.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
namespace {
// A listed replay's file, as its row names it and as a request sends it.
std::string PathOf(const platform::replays::ArchivedReplay& replay){return platform::WideToUtf8(replay.path.wstring());}
const platform::replays::ArchivedReplay* Find(const ShellView& v,const std::string& path){
 if(v.replays.archive)for(const auto& shown:*v.replays.archive)if(PathOf(shown)==path)return &shown;
 return nullptr;
}
std::vector<platform::replays::ArchivedSet> SetsOf(const ShellView& v){return v.replays.archive?platform::replays::Sets(*v.replays.archive):std::vector<platform::replays::ArchivedSet>{};}
// A set's row names it by its last game: the date and the players.
std::string SetLabel(const platform::replays::ArchivedSet& set){const auto& last=*set.games.back();return ReplayRowLabel(last.label,last.names,last.fighters);}
using Replay=platform::replays::ArchivedReplay;
// The archive's sets, newest first: two or more matches the same two named
// players played one after another, each within half an hour of the next (as
// an export counts its set). Each set's matches are newest first.
std::vector<std::vector<const Replay*>> Sets(const ShellView& v){
 std::vector<std::vector<const Replay*>> sets;
 if(!v.replays.archive)return sets;
 const Replay* last=nullptr;
 for(const auto& r:*v.replays.archive){
  const bool same=last&&!r.names[0].empty()&&!r.names[1].empty()&&last->time>=r.time&&last->time-r.time<=1800&&
   ((r.names[0]==last->names[0]&&r.names[1]==last->names[1])||(r.names[0]==last->names[1]&&r.names[1]==last->names[0]));
  if(!same)sets.emplace_back();
  sets.back().push_back(&r);last=&r;
 }
 sets.erase(std::remove_if(sets.begin(),sets.end(),[](const std::vector<const Replay*>& set){return set.size()<2;}),sets.end());
 return sets;
}
// A set's wins for its newest match's player 1 and 2, from each match's winner.
std::pair<int,int> SetScore(const std::vector<const Replay*>& set){
 std::pair<int,int> wins;
 for(const auto* r:set){
  if(!r->summary||!r->summary->scored||r->summary->score[0]==r->summary->score[1])continue;
  const std::string& winner=r->names[r->summary->score[0]>r->summary->score[1]?0:1];
  ++(winner==set.front()->names[0]?wins.first:wins.second);
 }
 return wins;
}
// The fighters' names, "Ryu vs Ken".
std::string FightersText(const Replay& r){
 const auto name=[&](int side){const auto* f=selection::FindFighter(r.fighters[side]);return std::string(f?f->name:loc::T("common.unavailable"));};
 return loc::Tf("replays.fighters",name(0),name(1));
}
// The replay a Replays row is of: "replay:<file>", or a set's newest, "set:<file>".
const Replay* RowReplay(const ShellView& v,const std::string& id){
 if(id.compare(0,7,"replay:")==0)return Find(v,id.substr(7));
 return id.compare(0,4,"set:")==0?Find(v,id.substr(4)):nullptr;
}
}
ReplaysPanel::Wants ReplaysPanel::Wanted(const std::string& screen) const {
 Wants wants;wants.listing=screen=="replays"||screen=="replay-sets"||screen=="replay-set";
 // Each entry owns a revision. WantDetail coalesces frames of that entry;
 // the worker rereads the bounded file before considering its content cache.
 if(screen=="replay-inputs"){wants.detail=true;wants.detailFile=inputsFile_;wants.detailRevision=inputsRevision_;}
 return wants;
}
void ReplaysPanel::Update(const ShellView& v,MenuNavigation& nav) {
 // In a room the menu is the room's: the link waits until it is left.
 const auto& link=v.replays.link;
 if(link!=linkSeen_&&(link.empty()||v.session.room==netplay::RoomState::Idle)){linkSeen_=link;if(!link.empty()&&nav.Screen()!="replays")Show(nav);}
}
std::vector<MenuEntry> ReplaysPanel::Rows(const ShellView& v,const std::string& screen,bool idle,std::string& title) const {
 const auto& r=v.replays;std::vector<MenuEntry> rows;
 if(screen=="replay-inputs"){title=loc::T("replays.inputs");InputsRows(v,rows);
 }else if(screen=="replay-sets"){
  // V2's Sets: a row a set, its score by who won each match; Select offers each match to watch.
  title=loc::T("replays.title");
  for(const auto& set:Sets(v)){
   const auto& newest=*set.front();const auto wins=SetScore(set);
   rows.push_back(Row("set:"+PathOf(newest),newest.names[0]+" "+std::to_string(wins.first)+"-"+std::to_string(wins.second)+" "+newest.names[1],loc::Tf("replays.set_matches",set.size())));
   rows.back().userText=true;rows.back().value=newest.label;rows.back().height=72;
   for(std::size_t game=set.size();game-->0;){
    const auto& r=*set[game];const std::string score=r.summary?ScoreText(*r.summary):std::string();
    rows.back().choices.push_back({"game:"+PathOf(r),loc::Tf("replays.game",set.size()-game)+(score.empty()?"":"  "+score),loc::T("replays.watch_detail"),idle&&v.replays.ready});
   }
   rows.back().chosen=rows.back().choices.front().id;
  }
 }else if(screen=="replay-export"){
  // Each part of the caption on or off, with its text; Generate sends it with the export.
  title=loc::T("export.title");
  const auto onOff=[](bool on){return std::string(on?loc::T("common.on"):loc::T("common.off"));};
  const int one=1,two=2;
  for(const auto* text:{&caption_.name[0],&caption_.name[1],&caption_.text})NoteUserText(*text);
  rows={Value("cap-names",loc::T("export.names"),onOff(caption_.names),loc::T("export.detail")),
   TextRow("cap-name1",loc::Tf("export.name",one),caption_.name[0],31,caption_.names),
   TextRow("cap-name2",loc::Tf("export.name",two),caption_.name[1],31,caption_.names),
   Value("cap-line",loc::T("export.line"),onOff(caption_.line),loc::T("export.detail")),
   TextRow("cap-text",loc::T("export.line_text"),caption_.text,60,caption_.line),
   Value("cap-set",loc::T("export.set"),onOff(caption_.set),loc::T("export.set_detail")),
   Value("cap-set1",loc::Tf("export.set_wins",one),std::to_string(caption_.wins[0]),loc::T("export.set_detail"),caption_.set),
   Value("cap-set2",loc::Tf("export.set_wins",two),std::to_string(caption_.wins[1]),loc::T("export.set_detail"),caption_.set),
   Value("cap-mark",loc::T("export.mark"),onOff(caption_.mark),loc::T("export.detail")),
   Value("cap-meter",loc::T("export.meter"),onOff(meter_),loc::T("export.meter_detail")),
   Row("cap-generate",loc::T("export.generate"),loc::T("export.generate_detail"),idle&&r.ready)};
 }else if(screen=="replay-sets"){
  // A row a set: its games and rooms, and Select to watch them in order, pick one, or save its pack.
  title=loc::T("replays.tournaments");
  for(const auto& set:SetsOf(v)){
   const auto& last=*set.games.back();
   rows.push_back(Row("set:"+set.matchId,SetLabel(set),loc::T("replays.set_detail")));
   rows.back().userText=true;for(const auto& name:last.names)NoteUserText(name);
   const auto known=[&](int side){return selection::FindFighter(last.fighters[side])?last.fighters[side]:-1;};
   rows.back().fighter1=known(0);rows.back().fighter2=known(1);
   rows.back().value=loc::Tf("replays.set_value",set.games.size(),set.rooms);
   // A set imported from a pack has no service to ask for its pack.
   rows.back().choices={{"set-watch",loc::T("replays.watch_set"),loc::T("replays.watch_set_detail"),idle&&r.ready},{"set-pick",loc::T("replays.pick_game"),loc::T("replays.pick_game_detail")},
    {"set-export",loc::T("replays.export_set"),loc::T("replays.export_set_detail"),idle},
    {"set-pack",loc::T("replays.save_pack"),loc::T("replays.save_pack_detail"),!set.bridgeId.empty()}};
   rows.back().chosen=idle&&r.ready?"set-watch":"set-pick";
  }
 }else if(screen=="replay-set"){
  // One set's games, in the order they were played.
  title=loc::T("replays.tournaments");
  for(const auto& set:SetsOf(v))if(set.matchId==setShown_){title=SetLabel(set);for(const auto* game:set.games)rows.push_back(ReplayRow(v,*game,idle));}
 }else{
  title=loc::T("replays.title");
  // A running export comes first: how far it has got, and Cancel while
  // cancelling can still keep its video from being made.
  if(r.exportStage!=replay::ExportStage::None){
   rows.push_back(InfoRow("export-progress",loc::T("export.running"),ExportStageText(r.exportStage,r.exportFrames,r.exportTotal),loc::T("export.running_detail")));
   if(replay::ExportCancellable(r.exportStage))rows.push_back(ConfirmRow("export-cancel",loc::T("export.cancel"),loc::T("export.cancel_detail")));
  }
  if(!r.link.empty()){
   // A link names a file and nothing is played on its word: Select asks. A set's pack is imported instead.
   rows.push_back(Row("replay-link",loc::T("replays.link"),loc::Tf("replays.link_detail",r.link)));
   rows.back().detailText=DetailText::Name;NoteUserText(r.link);
   const bool pack=replay_link::IsSetPack(r.link);
   rows.back().choices={pack?MenuChoice{"link-import",loc::T("replays.link_import"),loc::T("replays.link_import_detail")}:MenuChoice{"link-play",loc::T("replays.link_play"),loc::T("replays.watch_detail"),idle&&r.ready},
    {"link-dismiss",loc::T("replays.link_dismiss"),loc::T("replays.link_dismiss_detail")}};
   rows.back().chosen=pack?"link-import":idle&&r.ready?"link-play":"link-dismiss";
  }
  // Watch set plays a game at a time, and Export set videos sets one up at a
  // time: the next one is a press away when the last one ends.
  if(!setQueue_.empty())rows.push_back(Row("set-next",loc::Tf(setExport_?"replays.next_video":"replays.next_game",setGames_-setQueue_.size()+1,setGames_),
   loc::T(setExport_?"replays.next_video_detail":"replays.watch_detail"),setExport_?idle:idle&&r.ready));
  rows.push_back(Row("replay-log",loc::T("replays.open_log"),loc::T(idle?"replays.open_log_detail":"replays.open_log_room"),idle&&r.ready));
  rows.push_back(Row("replay-folder",loc::T("replays.open_folder"),v.services.lastAction==platform::ServiceAction::OpenReplayFolder&&!v.services.message.empty()?v.services.message:loc::T("replays.open_folder_detail"),!v.services.pending));
  const auto sets=SetsOf(v);
  if(!sets.empty()){rows.push_back(Row("replay-sets",loc::T("replays.tournaments"),loc::T("replays.tournaments_detail")));rows.back().value=std::to_string(sets.size());}
  if(!r.archive||r.archive->empty())rows.push_back(InfoRow("replay-none",loc::T("replays.empty"),"",loc::T("replays.empty_detail")));
  // A row is its file, not its place: the list is listed again while a row's choices are open.
  // A row is always open: reading its inputs needs nothing of the game. What does is each choice's own.
  if(r.archive)for(const auto& replay:*r.archive){rows.push_back(ReplayRow(v,replay,idle));rows.back().height=72;}
 }
 return rows;
}
// A replay's row, on Replays and on its set's own screen.
MenuEntry ReplaysPanel::ReplayRow(const ShellView& v,const platform::replays::ArchivedReplay& replay,bool idle) const {
 const auto& r=v.replays;
 MenuEntry row=Row("replay:"+PathOf(replay),ReplayRowLabel(replay.label,replay.names,replay.fighters),loc::T(r.ready?"replays.row_detail":"replays.not_ready"));
 // The label carries the players' own names, the portraits their fighters. The value says what Ember knows about it.
 row.userText=true;for(const auto& name:replay.names)NoteUserText(name);
 const auto known=[&](int side){return selection::FindFighter(replay.fighters[side])?replay.fighters[side]:-1;};
 row.fighter1=known(0);row.fighter2=known(1);
 row.value=ReplayTagsText(replay.spectated,replay.watched,replay.video);
 // The score leads the value; the replay's own account of the match leads the detail.
 if(const auto& summary=replay.summary){
  const std::string score=ScoreText(*summary);
  if(!score.empty())row.value=row.value.empty()?score:score+" · "+row.value;
  std::string said=MatchText(*summary);
  for(int side=0;side<2;side++)said+="\n"+ReplayPlayerName(replay.names,side)+": "+LookText(summary->players[side]);
  row.detail=said+"\n\n"+row.detail;row.detailText=DetailText::Name;
 }
 // Select offers playback, adding to the Battle Log, exporting video, reading inputs, or its file in Explorer.
 row.choices={{"watch",loc::T("replays.watch"),loc::T("replays.watch_detail"),idle&&r.ready},{"watch-meter",loc::T("replays.watch_meter"),loc::T("replays.watch_meter_detail"),idle&&r.ready},{"add",loc::T("replays.add"),loc::T("replays.add_detail"),r.ready},{"export",loc::T("replays.export_gpu"),loc::T("replays.export_gpu_detail"),idle&&r.ready},{"inputs",loc::T("replays.inputs"),loc::T("replays.inputs_detail")},
  {"show-folder",loc::T("replays.show_folder"),loc::T("replays.show_folder_detail"),!v.services.pending}};
 row.chosen=!r.ready?"inputs":idle?"watch":"add";
 return row;
}
// The Inputs and stats screen: the replay's own label and length, for each
// player the look and what was pressed, and a row a round whose Select opens
// the round's inputs in the reader. Until the match for that file is handed
// over the screen says it is being read.
void ReplaysPanel::InputsRows(const ShellView& v,std::vector<MenuEntry>& rows) const {
 namespace in=replayinputs;
 const auto& completion=v.replays.detail;
 try {
 if(completion.revision!=inputsRevision_||completion.state==replayinputs::DetailState::Pending){rows.push_back(InfoRow("inputs-reading",loc::T("inputs.reading"),"",loc::T("replays.inputs_detail")));return;}
 if(completion.state!=replayinputs::DetailState::Ready||!completion.value||completion.value->file!=inputsFile_){rows.push_back(InfoRow("inputs-none",loc::T("inputs.unreadable"),"",loc::T("inputs.unreadable_detail")));return;}
 const auto& detail=*completion.value;
 const in::Match& match=detail.match;
 const in::Summary& summary=detail.summary;
 const std::string score=ScoreText(summary);
 rows.push_back(InfoRow("inputs-replay",ReplayLabel(detail.label,detail.names,detail.fighters),(score.empty()?"":score+"  ")+MatchText(summary),loc::T("replays.inputs_detail")));
 rows.back().userText=true;
 for(int side=0;side<2;side++){
  const auto* fighter=selection::FindFighter(detail.fighters[side]);
  std::uint64_t presses=0;for(const auto count:summary.stats[side].presses)presses+=count;
  rows.push_back(InfoRow("inputs-p"+std::to_string(side+1),loc::Tf("replays.player",ReplayPlayerName(detail.names,side),fighter?fighter->name:loc::T("common.unavailable")),loc::Tf("inputs.presses_value",presses),LookText(summary.players[side])+". "+PressesText(summary.stats[side])+". "+ActivityText(summary.stats[side])));
  rows.back().userText=!detail.names[side].empty();
 }
 for(std::size_t round=0;round<match.rounds.size();round++){
  const std::size_t number=round+1;
  auto row=Row("inputs-round-"+std::to_string(number),loc::Tf("inputs.round",number),std::string(loc::T("inputs.legend"))+"\n\n"+detail.logs[round]);
  row.value=in::Clock(match.rounds[round].frames);row.reading=true;
  rows.push_back(std::move(row));
 }
 }catch(...){
  // Preserve nightly's no-partial-detail boundary if constructing UI text
  // fails after the worker has successfully prepared the replay.
  rows.clear();rows.push_back(InfoRow("inputs-none",loc::T("inputs.unreadable"),"",loc::T("inputs.unreadable_detail")));
 }
}
// The caption an export starts from: the names Ember noted, the date and
// score on one line, Ember's mark, and the set so far. The set is counted
// from the archive: the matches these two played right before this one,
// each within half an hour of the next, by who won each.
void ReplaysPanel::OpenExport(const ShellView& v,const platform::replays::ArchivedReplay& replay) {
 exportPath_=PathOf(replay);caption_=replay::Caption{};
 caption_.name[0]=replay.names[0];caption_.name[1]=replay.names[1];
 caption_.names=!replay.names[0].empty()&&!replay.names[1].empty();
 const std::string score=replay.summary?ScoreText(*replay.summary):std::string();
 caption_.text=ReplayLabel(replay.label,replay.names,replay.fighters).substr(0,10)+(score.empty()?"":"   "+score);
 caption_.line=caption_.mark=true;
 std::uint64_t next=replay.time;
 if(v.replays.archive)for(const auto& earlier:*v.replays.archive){
  if(!caption_.names||earlier.time>=replay.time)continue;
  if(next-earlier.time>1800)break; // newest first: nothing older continues the set
  const bool same=earlier.names[0]==replay.names[0]&&earlier.names[1]==replay.names[1];
  const bool swapped=earlier.names[0]==replay.names[1]&&earlier.names[1]==replay.names[0];
  if((!same&&!swapped)||!earlier.summary||!earlier.summary->scored||earlier.summary->score[0]==earlier.summary->score[1])continue;
  const int winner=earlier.summary->score[0]>earlier.summary->score[1]?0:1;
  ++caption_.wins[same?winner:1-winner];next=earlier.time;
 }
 caption_.set=caption_.wins[0]+caption_.wins[1]>0;
}
// The set's next game's caption screen, its set score counted from the
// games before it (OpenExport); Generate there makes its video.
void ReplaysPanel::NextVideo(const ShellView& v,MenuNavigation& nav) {
 const std::string path=setQueue_.front();setQueue_.erase(setQueue_.begin());
 if(const auto* shown=Find(v,path)){OpenExport(v,*shown);nav.Push("replay-export");}
}
void ReplaysPanel::Watch(const ShellView& v,const std::string& path,bool meter,const Submit& submit,std::string& error) {
 ShellAction r;r.command.generation=v.session.generation;r.replay={replay::Mode::Watch,path};r.replay.meter=meter;
 if(!submit(std::move(r)))error=loc::T("error.queue_failed");
}
bool ReplaysPanel::Activate(const MenuAction& a,const ShellView& v,MenuNavigation& nav,const Submit& submit,std::string& error) {
 // Any other request from these screens takes the status line back from Show in folder.
 if(a.id=="cap-generate"||a.id=="export-cancel"||a.id=="replay-log")folderAsked_=setAsked_=false;
 if(a.id=="replay-sets"){nav.Push("replay-sets");return true;}
 if(a.id=="set-next"){
  if(setExport_&&!setQueue_.empty()){NextVideo(v,nav);return true;}
  if(!setQueue_.empty()&&v.replays.ready){Watch(v,setQueue_.front(),false,submit,error);setQueue_.erase(setQueue_.begin());}
  return true;
 }
 if(a.id=="cap-generate"){
  ShellAction r;r.command.generation=v.session.generation;r.replay={replay::Mode::Export,exportPath_,caption_};r.replay.meter=meter_;
  if(!submit(std::move(r)))error=loc::T("error.queue_failed");else nav.Return();
  return true;
 }
 if(a.id=="export-cancel"){
  // Confirmed on the row. The game keeps no video of it; the notice says so once the encoder has gone.
  ShellAction r;r.command.generation=v.session.generation;r.replay.mode=replay::Mode::CancelExport;
  if(!submit(std::move(r)))error=loc::T("error.queue_failed");
  return true;
 }
 if(a.id=="replay-log"){ShellAction r;r.command.generation=v.session.generation;r.replay.mode=replay::Mode::OpenLog;if(!submit(std::move(r)))error=loc::T("error.queue_failed");return true;}
 return false;
}
void ReplaysPanel::Accept(const MenuAction& a) {
 if(a.id=="cap-names")caption_.names=a.delta>0;else if(a.id=="cap-line")caption_.line=a.delta>0;
 else if(a.id=="cap-set")caption_.set=a.delta>0;else if(a.id=="cap-mark")caption_.mark=a.delta>0;
 else if(a.id=="cap-meter")meter_=a.delta>0;
 else if(a.id=="cap-name1")caption_.name[0]=a.text;else if(a.id=="cap-name2")caption_.name[1]=a.text;
 else if(a.id=="cap-text")caption_.text=a.text;
 else if(a.id=="cap-set1"||a.id=="cap-set2"){int& wins=caption_.wins[a.id=="cap-set2"];wins=(std::max)(0,(std::min)(99,wins+a.delta));}
}
void ReplaysPanel::Choose(const MenuAction& a,const ShellView& v,MenuNavigation& nav,const Submit& submit,std::string& error) {
 if(a.id=="replay-link"){
  folderAsked_=false;setAsked_=a.text=="link-import";setNoticeAsked_=v.replays.setNotices;
  ShellAction r;r.command.generation=v.session.generation;
  if(a.text=="link-play")r.replay={replay::Mode::Watch,v.replays.link};
  else if(a.text=="link-import")r.replay={replay::Mode::ImportPack,v.replays.link};
  else r.replay.mode=replay::Mode::DismissLink;
  if(!submit(std::move(r)))error=loc::T("error.queue_failed");
  return;
 }
 if(a.id.compare(0,4,"set:")==0){
  const std::string match=a.id.substr(4);
  for(const auto& set:SetsOf(v)){
   if(set.matchId!=match)continue;
   if(a.text=="set-pick"){setShown_=match;nav.Push("replay-set");}
   else if(a.text=="set-pack"){
    folderAsked_=false;setAsked_=true;setNoticeAsked_=v.replays.setNotices;
    ShellAction r;r.command.generation=v.session.generation;
    r.tournament.op=netplay::tournament::Command::Op::SavePack;r.tournament.bridgeId=set.bridgeId;r.tournament.matchId=set.matchId;
    if(!submit(std::move(r))){setAsked_=false;error=loc::T("error.queue_failed");}
   }else if(a.text=="set-export"){
    // The first game's caption now; each next one from the Next video row on Replays.
    setQueue_.clear();for(const auto* game:set.games)setQueue_.push_back(PathOf(*game));
    setGames_=setQueue_.size();setExport_=true;
    NextVideo(v,nav);
   }else if(a.text=="set-watch"&&v.replays.ready){
    // The first game now; each next one from the Next game row on Replays, where the log comes back to.
    setQueue_.clear();for(const auto* game:set.games)setQueue_.push_back(PathOf(*game));
    setGames_=setQueue_.size();setExport_=false;
    Watch(v,setQueue_.front(),false,submit,error);setQueue_.erase(setQueue_.begin());
   }
  }
  return;
 }
 if(a.id.compare(0,7,"replay:")!=0)return;
 const std::string path=a.id.substr(7);
 folderAsked_=a.text=="show-folder";setAsked_=false;
 // Anything else asked of a replay ends a set being watched.
 setQueue_.clear();
 if(a.text=="show-folder"){
  // Explorer opens on the replay's own folder: the archive, or a usf4-replay-saver folder in it.
  ShellAction r;r.command.generation=v.session.generation;r.service=platform::ServiceAction::ShowReplayFile;r.servicePath=path;
  if(!submit(std::move(r))){folderAsked_=false;error=loc::T("error.queue_failed");}
 }else if(a.text=="inputs"){
  // Nothing is sent to the game: the screen names the file and is handed its match (InputsFile).
  if(Find(v,path)){inputsFile_=path;++inputsRevision_;nav.Push("replay-inputs");}
 }else if(a.text=="export"){
  // The caption is set up first; Generate on that screen sends the export.
  if(const auto* shown=Find(v,path)){OpenExport(v,*shown);nav.Push("replay-export");}
 }else if(v.replays.ready&&(a.text=="watch"||a.text=="watch-meter")){Watch(v,path,a.text=="watch-meter",submit,error);
 }else if(v.replays.ready){
  ShellAction r;r.command.generation=v.session.generation;r.replay={replay::Mode::Add,path};
  if(!submit(std::move(r)))error=loc::T("error.queue_failed");
 }
}
bool ReplaysPanel::Status(const ShellView& v,const std::string& screen,std::string& status,Tone& tone) const {
 if((screen!="replays"&&screen!="replay-sets")||!status.empty())return false;
 // A set pack answers until the next request from these screens.
 if(setAsked_){
  if(v.replays.setNotices==setNoticeAsked_){status=loc::T("replays.set_pack_working");tone=Tone::Pending;}
  else{status=v.replays.setNotice;tone=v.replays.setNoticeError?Tone::Error:Tone::Success;}
  return true;
 }
 if(screen!="replays")return false;
 // Show in folder answers until the next request from this screen.
 const auto& services=v.services;
 if(folderAsked_&&services.lastAction==platform::ServiceAction::ShowReplayFile&&!services.message.empty()){
  status=services.message;tone=services.pending?Tone::Pending:services.succeeded?Tone::Success:Tone::Error;return true;
 }
 if(v.replays.notice.empty())return false;
 status=v.replays.notice;tone=v.replays.noticeError?Tone::Error:Tone::Success;return true;
}
std::vector<std::pair<std::string,std::string>> ReplaysPanel::Tabs(const ShellView& v) const {
 std::vector<std::pair<std::string,std::string>> tabs{{"replays",loc::T("replays.tab.matches")}};
 if(!Sets(v).empty())tabs.push_back({"replay-sets",loc::T("replays.tab.sets")});
 if(!exportPath_.empty()||(v.replays.archive&&!v.replays.archive->empty()))tabs.push_back({"replay-export",loc::T("export.title")});
 return tabs;
}
void ReplaysPanel::ExportFrom(const ShellView& v,const std::string& focus) {
 const Replay* r=RowReplay(v,focus);
 if(!r&&exportPath_.empty()&&v.replays.archive&&!v.replays.archive->empty())r=&v.replays.archive->front();
 if(r)OpenExport(v,*r);
}
bool ReplaysPanel::DrawCard(const ShellView& v,const MenuEntry& e,ImVec2 min,ImVec2 max) const {
 const Replay* r=RowReplay(v,e.id);
 if(!r)return false;
 auto* d=ImGui::GetWindowDrawList();const float s=Scale();const auto& c=v2::Colors();
 const bool set=e.id[0]=='s';
 // The right end: what Ember knows of it as a chip, and the date.
 float right=max.x-16*s;
 const float dateWidth=ImGui::CalcTextSize(r->label.c_str()).x;
 d->AddText(ImVec2(right-dateWidth,min.y+(max.y-min.y-ImGui::GetFontSize())*.5f),c.muted,r->label.c_str());
 right-=dateWidth+14*s;
 const std::string tags=set?std::string():ReplayTagsText(r->spectated,r->watched,r->video);
 if(!tags.empty()){right-=v2::ChipWidth(tags);v2::Chip(d,ImVec2(right,min.y+(max.y-min.y-ImGui::GetFontSize()*.78f-8*s)*.5f),tags,c.muted);right-=14*s;}
 // The players and the score between them (a set's by match), the fighters or the set's length under them.
 std::string score=r->summary?ScoreText(*r->summary):std::string();
 if(set)for(const auto& one:Sets(v))if(one.front()==r){const auto wins=SetScore(one);score=std::to_string(wins.first)+"-"+std::to_string(wins.second);}
 const float x=min.x+16*s,top=min.y+10*s;
 ImGui::PushFont(HeadingFont());
 const std::string a=ReplayPlayerName(r->names,0),b=ReplayPlayerName(r->names,1);
 const float room=(right-x-ImGui::CalcTextSize(score.empty()?"-":score.c_str()).x-24*s)*.5f;
 const std::string fa=FitLabel(a,room),fb=FitLabel(b,room);
 for(const auto& name:r->names)NoteUserText(name);
 float at=x;
 d->AddText(ImVec2(at,top),c.text,fa.c_str());at+=ImGui::CalcTextSize(fa.c_str()).x+12*s;
 const std::string mid=score.empty()?"-":score;
 d->AddText(ImVec2(at,top),c.accent,mid.c_str());at+=ImGui::CalcTextSize(mid.c_str()).x+12*s;
 d->AddText(ImVec2(at,top),c.text,fb.c_str());
 const float below=top+ImGui::GetFontSize()+4*s;
 ImGui::PopFont();
 const std::string sub=set?e.detail:FightersText(*r)+(r->summary?" \xC2\xB7 "+MatchText(*r->summary):"");
 d->AddText(ImVec2(x,below),c.muted,FitLabel(sub,right-x).c_str());
 return true;
}
void ReplaysPanel::DrawDetail(const ShellView& v,const std::string& screen,const std::string& id,MenuNavigation& nav,const std::vector<MenuEntry>& rows) const {
 auto* d=ImGui::GetWindowDrawList();const float s=Scale();const auto& c=v2::Colors();
 if(screen=="replay-export"){
  // What the video will show, over a small screen: the names, the line, the set's score, Ember's mark and the frame meter.
  const auto p=ImGui::GetCursorScreenPos();const float w=ImGui::GetContentRegionAvail().x,h=w*9/16;
  const ImVec2 q(p.x+w,p.y+h);
  d->AddRectFilledMultiColor(p,q,c.header,c.header,c.panel,c.panel);
  d->AddRect(p,q,c.border,4*s,0,s);
  for(const float side:{.3f,.7f})d->AddRectFilled(ImVec2(p.x+w*side-15*s,q.y-h*.16f-90*s*h/400),ImVec2(p.x+w*side+15*s,q.y-h*.16f),side<.5f?c.hover:c.button,4*s);
  d->AddRectFilled(ImVec2(p.x+w*.04f,p.y+h*.06f),ImVec2(p.x+w*.44f,p.y+h*.06f+10*s),c.accentLight);
  d->AddRectFilled(ImVec2(p.x+w*.56f,p.y+h*.06f),ImVec2(p.x+w*.96f,p.y+h*.06f+10*s),c.accentLight);
  if(caption_.names){
   for(const auto& name:caption_.name)NoteUserText(name);
   d->AddText(ImVec2(p.x+w*.04f,p.y+h*.15f),c.text,caption_.name[0].c_str());
   d->AddText(ImVec2(q.x-w*.04f-ImGui::CalcTextSize(caption_.name[1].c_str()).x,p.y+h*.15f),c.text,caption_.name[1].c_str());
  }
  const auto centred=[&](const std::string& text,float y,ImU32 ground,ImU32 ink){
   const float tw=ImGui::CalcTextSize(text.c_str()).x,cx=p.x+(w-tw)*.5f;
   if(ground)d->AddRectFilled(ImVec2(cx-8*s,y-2*s),ImVec2(cx+tw+8*s,y+ImGui::GetFontSize()+2*s),ground,3*s);
   d->AddText(ImVec2(cx,y),ink,text.c_str());
  };
  if(caption_.line){NoteUserText(caption_.text);centred(caption_.text,p.y+h*.14f,v2::color::Alpha(c.ink,.85f),c.text);}
  if(caption_.set)centred(std::to_string(caption_.wins[0])+" - "+std::to_string(caption_.wins[1]),p.y+h*.24f,0,c.accent);
  if(caption_.mark)v2::BrandMark(d,ImVec2(q.x-36*s,q.y-36*s),24*s);
  if(meter_)d->AddRectFilled(ImVec2(p.x+w*.06f,q.y-h*.04f-12*s),ImVec2(q.x-w*.06f,q.y-h*.04f),c.accentLight);
  ImGui::Dummy(ImVec2(w,h));
  return;
 }
 const Replay* r=RowReplay(v,id);
 const auto row=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
 if(!r||row==rows.end())return;
 const bool set=id[0]=='s';
 ImGui::PushFont(HeadingFont());
 for(const auto& name:r->names)NoteUserText(name);
 ImGui::TextWrapped("%s",set?row->label.c_str():loc::Tf("replays.fighters",ReplayPlayerName(r->names,0),ReplayPlayerName(r->names,1)).c_str());
 ImGui::PopFont();
 ImGui::Dummy(ImVec2(0,6*s));
 if(set)v2::Fact(loc::T("replays.tab.matches"),std::to_string(row->choices.size()));
 else v2::Fact(loc::T("replays.fact.fighters"),FightersText(*r));
 v2::Fact(loc::T("replays.fact.played"),r->label);
 if(!set&&r->summary)v2::Fact(loc::T("replays.fact.length"),MatchText(*r->summary));
 // Watch now opens the row's choices, as Select does.
 ImGui::Dummy(ImVec2(0,6*s));
 ImGui::PushStyleColor(ImGuiCol_Button,ImGui::ColorConvertU32ToFloat4(c.accent));
 ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImGui::ColorConvertU32ToFloat4(c.accentLight));
 ImGui::PushStyleColor(ImGuiCol_Text,ImGui::ColorConvertU32ToFloat4(c.ink));
 ImGui::PushFont(HeadingFont());
 if(ImGui::Button(loc::T("replays.watch"),ImVec2(-1,52*s)))nav.Ask(id,rows);
 ImGui::PopFont();
 ImGui::PopStyleColor(3);
}
} }
