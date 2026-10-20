#include "ReplaysPanel.hxx"
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "ReplaySummaryText.hxx"
#include "Theme.hxx"
#include "../platform/ReplaySets.hxx"
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
  if(r.archive)for(const auto& replay:*r.archive)rows.push_back(ReplayRow(v,replay,idle));
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
} }
