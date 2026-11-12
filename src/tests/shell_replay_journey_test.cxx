#include "shell_journey_support.hxx"
#include "shell_replay_journey.hxx"
#include "../ui/ReplaySummaryText.hxx"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
namespace {
using Archived=sf4e::platform::replays::ArchivedReplay;
// A listed replay as the runtime's lister makes it: its file and its date.
Archived Listed(const std::string& path,const std::string& date){Archived a;a.path=std::filesystem::u8path(path);a.label=date;return a;}
void List(Harness& h,std::vector<Archived> replays){h.view.replays.archive=std::make_shared<const std::vector<Archived>>(std::move(replays));}
std::shared_ptr<const sf4e::replayinputs::Detail> InputsFixture(const std::string& file) {
 auto detail=std::make_shared<sf4e::replayinputs::Detail>();detail->file=file;
 std::vector<unsigned char> replay(0x320+0x88,0);std::memcpy(replay.data(),"#BRP",4);replay[8]=1;replay[10]=8;replay[0x18]=1;replay[0x320+0x7C]=3;
 const unsigned char press[]={0x10,0,0};replay.insert(replay.end(),press,press+3);
 Check(sf4e::replayinputs::Parse(replay.data(),replay.size(),detail->match),"Inputs fixture did not parse");
 detail->summary=sf4e::replayinputs::Summarize(detail->match);
 detail->logs.push_back(sf4e::replayinputs::Log(detail->match.rounds[0]));
 return detail;
}
void OpenInputs(Harness& h,const Archived& read,bool ready) {
 h.view.replays.link.clear();List(h,{read});h.view.replays.ready=ready;h.Screen("replays");
 const std::string path=read.path.u8string();
 const auto sent=h.actions.size();const std::string row="replay:"+path;h.Choose(row.c_str());
 if(ready)for(int i=0;i<4;++i)h.Press(MenuInput::Right);
 h.Press(MenuInput::Select);
 Check(h.shell.Navigation().Screen()=="replay-inputs"&&h.actions.size()==sent&&h.shell.ReplayInputsFile()==path,"Inputs and stats did not open without sending a game action");
 // Only Inputs and stats asks the worker for a detail, of its file under this entry's revision.
 const auto wants=h.shell.ReplayWants();
 Check(wants.detail&&!wants.listing&&wants.detailFile==path&&wants.detailRevision==h.shell.ReplayInputsRevision(),"Inputs and stats did not ask for its own detail");
}
void InputsJourney(Harness& h,bool ready) {
 using sf4e::replayinputs::DetailState;
 const auto read=Listed("C:\\r\\inputs.usf4replay","2026-10-06 21:32");const std::string file=read.path.u8string();
 h.view.replays.detail={};OpenInputs(h,read,ready);h.FocusOn("inputs-reading");
 const auto first=h.shell.ReplayInputsRevision();
 h.view.replays.detail={first,DetailState::Unreadable,{}};h.Frame();h.FocusOn("inputs-none");
 h.view.replays.detail={first,DetailState::Ready,InputsFixture(file)};h.Frame();
 h.FocusOn("inputs-p2");h.FocusOn("inputs-round-1");
 h.Press(MenuInput::Back);OpenInputs(h,read,ready);
 const auto second=h.shell.ReplayInputsRevision();
 Check(second>first,"Reopening the same file did not issue another revision");
 h.FocusOn("inputs-reading"); // old successful result must not satisfy the new entry
 h.view.replays.detail={second,DetailState::Failed,{}};h.Frame();h.FocusOn("inputs-none");
 h.Press(MenuInput::Back);OpenInputs(h,read,ready);
 const auto third=h.shell.ReplayInputsRevision();
 h.view.replays.detail={third,DetailState::Ready,InputsFixture(file)};h.Frame();
 h.FocusOn("inputs-p2");h.FocusOn("inputs-round-1");
 h.view.replays.detail={};h.view.replays.ready=true;
}
void InputsReplacementJourney(Harness& h) {
 namespace fs=std::filesystem;
 namespace files=sf4e::replayfiles;
 namespace in=sf4e::replayinputs;
 const auto folder=fs::temp_directory_path()/("ember-inputs-screen-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 Check(fs::create_directory(folder)&&fs::create_directory(folder/".names"),"Replacement fixture folder");
 const auto path=folder/"inputs.usf4replay";
 const auto body=[](unsigned fighter,unsigned button,std::uint64_t seconds){
  sf4e::replayslots::Bytes replay(0x320+0x88,0);std::memcpy(replay.data(),"#BRP",4);replay[8]=1;replay[10]=8;
  const auto native=116444736000000000ull+seconds*10000000ull;
  sf4e::replayslots::WriteU32(replay.data()+0x10,static_cast<std::uint32_t>(native));
  sf4e::replayslots::WriteU32(replay.data()+0x14,static_cast<std::uint32_t>(native>>32));
  sf4e::replayslots::WriteU32(replay.data()+0x20,fighter);sf4e::replayslots::WriteU32(replay.data()+0x170,1);
  replay[0x18]=1;replay[0x320+0x7C]=3;replay.push_back(static_cast<unsigned char>(button));replay.push_back(0);replay.push_back(0);return replay;
 };
 const auto save=[](const fs::path& file,const sf4e::replayslots::Bytes& bytes){
  std::ofstream out(file,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));out.close();Check(!out.fail(),"Replacement fixture write");
 };
 const auto a=body(0,in::LP,1800000000),b=body(25,in::MP,1800000060);
 save(path,a);save(files::NamesPath(folder,a),files::BindNames(a,{{"Ann","Bob"},files::Provenance::Played}));
 in::DetailCache cache;
 const auto file=path.u8string();const auto first=cache.Read(file,1,folder);
 Check(first.value!=nullptr,"Original detail unreadable");
 auto row=Listed(file,first.value->label);row.names[0]="Ann";row.names[1]="Bob";
 for(int side=0;side<2;++side)row.fighters[side]=first.value->fighters[side];
 const auto rowLabel=ReplayLabel(row.label,row.names,row.fighters);
 const auto written=fs::last_write_time(path);
 save(path,b);fs::last_write_time(path,written);save(files::NamesPath(folder,b),files::BindNames(b,{{"Carol","Dave"},files::Provenance::Spectated}));
 // The listing still holds A when Inputs requests the current body B.
 OpenInputs(h,row,true);
 h.view.replays.detail=cache.Read(file,h.shell.ReplayInputsRevision(),folder);
 const auto detail=h.view.replays.detail.value;
 Check(detail&&detail->match.rounds[0].runs[0].inputs[0]==in::MP,"Replacement inputs did not read B");
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});h.Frame();
 const auto find=[&](const char* id)->const MenuEntry&{const auto at=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});Check(at!=rows.end(),"Replacement Inputs row missing");return *at;};
 const auto label=ReplayLabel(detail->label,detail->names,detail->fighters);
 Check(find("inputs-replay").label==label&&label!=rowLabel,"Inputs displayed A's date/fighters/names above B's stats");
 Check(find("inputs-p1").label==sf4e::loc::Tf("replays.player","Carol",sf4e::selection::FindFighter(detail->fighters[0])->name)&&find("inputs-p2").label==sf4e::loc::Tf("replays.player","Dave",sf4e::selection::FindFighter(detail->fighters[1])->name),"Inputs displayed stale player names or fighters");
 Check(find("inputs-p1").value==sf4e::loc::Tf("inputs.presses_value",1),"Inputs total is not compact or correct");
 Check(find("inputs-p1").detail.find("LP 0, MP 1, HP 0, LK 0, MK 0, HK 0")!=std::string::npos,"Inputs detail lost per-button counts");
 Check(find("inputs-p1").userText&&detail->fighters[0]==25&&detail->time==1800000060,"Inputs metadata was not body-bound");
 h.view.replays.archive.reset();h.Frame();
 Check(find("inputs-replay").label==label,"Listing refresh changed completed Inputs metadata");
 SetMenuEntriesProbe({});h.view.replays.detail={};h.Press(MenuInput::Back);fs::remove_all(folder);
}
// Export video's on/off rows are the player's saved choices, and a side's set
// wins can be typed as well as stepped.
void ExportChoicesJourney() {
 using namespace sf4e;Harness h;h.Frame();
 std::vector<MenuEntry> page;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& e){page=e;});
 const auto row=[&](const char* id){const auto at=std::find_if(page.begin(),page.end(),[&](const MenuEntry& e){return e.id==id;});Check(at!=page.end(),"An export row is missing");return *at;};
 auto& io=ImGui::GetIO();
 const auto key=[&](ImGuiKey k){h.Frame();io.AddKeyEvent(k,true);h.Frame();io.AddKeyEvent(k,false);h.Frame();};
 // The runtime takes the save the shell sends, as SettingsStore does. Rows
 // are focused directly (Prefer), so every change is made before it is sent.
 const auto saved=[&]{
  const auto sent=h.actions.size();h.Wait(1.f);
  const auto* at=Last<commands::SavePreferences>(h.actions);
  Check(at&&h.actions.size()>sent,"A caption choice was not saved");
  h.view.preferences=at->preferences;h.Frame();return h.view.preferences;
 };
 const auto step=[&](const char* id,unsigned direction){h.shell.Navigation().Prefer(id);h.Frame();h.Press(direction);};
 auto replay=Listed("C:\\r\\d.emberreplay","2026-11-01 18:50");replay.names[0]="A";replay.names[1]="B";replay.time=5000;
 auto earlier=replay;earlier.path=std::filesystem::u8path("C:\\r\\e.emberreplay");earlier.time=4800;
 earlier.summary=replayinputs::Summary{};earlier.summary->scored=true;earlier.summary->score[0]=2;earlier.summary->score[1]=0;
 List(h,{replay,earlier});h.view.replays.ready=true;
 const auto open=[&]{h.Screen("replays");h.Choose("replay:C:\\r\\d.emberreplay");for(int i=0;i<3;++i)h.Press(MenuInput::Right);h.Press(MenuInput::Select);h.Frame();
  Check(h.shell.Navigation().Screen()=="replay-export","Export video did not open the caption's screen");};
 open();
 Check(row("cap-line").value==loc::T("common.on")&&row("cap-mark").value==loc::T("common.on")&&row("cap-meter").value==loc::T("common.off")&&row("cap-set1").value=="1",
  "A first export did not start with the line and mark on and the set counted");
 step("cap-line",MenuInput::Left);step("cap-mark",MenuInput::Left);step("cap-meter",MenuInput::Right);
 const auto first=saved();
 Check(!first.exportLine&&!first.exportMark&&first.exportMeter&&first.exportNames&&first.exportSet,"The caption's on/off rows were not saved as they were left");
 h.Press(MenuInput::Back);open();
 Check(row("cap-line").value==loc::T("common.off")&&row("cap-mark").value==loc::T("common.off")&&row("cap-meter").value==loc::T("common.on")&&row("cap-names").value==loc::T("common.on"),
  "The next export did not start from the saved choices");
 // Typed: Select opens the editor on the number, a number replaces it, anything else keeps it.
 h.Choose("cap-set1");
 Check(h.shell.Navigation().Editing()&&h.shell.Navigation().Draft().empty(),"Select on a side's wins did not open an empty editor");
 const auto type=[&](const char* text){h.Frame();io.AddInputCharactersUTF8(text);h.Frame();key(ImGuiKey_Enter);h.Frame();};
 type("0");
 Check(!h.shell.Navigation().Editing(),"Enter did not close the wins editor");
 Check(row("cap-set1").value=="0","Typed wins did not replace the number");
 h.Choose("cap-set1");type("x");
 Check(row("cap-set1").value=="0","Typed text that is not a number changed the wins");
 h.FocusOn("cap-set2");h.Press(MenuInput::Right);h.Frame();
 Check(row("cap-set2").value=="1","Right no longer steps a side's wins");
 h.Choose("cap-generate");
 const auto* exported=As<commands::Replay>(h.actions.back());
 const auto sent=exported?exported->replay:sf4e::replay::Request{};
 Check(sent.mode==replay::Mode::Export&&sent.caption.wins[0]==0&&sent.caption.wins[1]==1&&!sent.caption.line&&!sent.caption.mark&&sent.caption.set&&sent.meter,
  "Generate did not send the typed wins with the saved choices");
 // Set score turned off stays off, even for a replay with a set before it.
 open();step("cap-set",MenuInput::Left);
 Check(!saved().exportSet,"Turning Set score off was not saved");
 h.Press(MenuInput::Back);open();
 Check(row("cap-set").value==loc::T("common.off"),"Set score did not start off once turned off");
 // A replay whose names Ember does not know still starts with names off.
 auto unnamed=replay;unnamed.names[0].clear();unnamed.names[1].clear();List(h,{unnamed});h.Press(MenuInput::Back);open();
 Check(row("cap-names").value==loc::T("common.off"),"An export of unnamed players started with names on");
 // Left there changes nothing on screen, so it saves nothing for later exports.
 {const auto before=h.actions.size();step("cap-names",MenuInput::Left);h.Wait(1.f);
  Check(std::none_of(h.actions.begin()+before,h.actions.end(),[](const ShellAction& a){return Is<commands::SavePreferences>(a);}),
   "Left on Player names already off saved a choice");}
 SetMenuEntriesProbe({});
}
// Watch set under Sets plays a room set's games in the order they were
// played: the first at once, each next one from the Next game row on Replays.
void SetWatchJourney() {
 using namespace sf4e;Harness h;h.Frame();
 auto last=Listed("C:\\r\\s2.emberreplay","2026-11-03 20:10");last.names[0]="A";last.names[1]="B";last.time=6000;
 auto first=last;first.path=std::filesystem::u8path("C:\\r\\s1.emberreplay");first.time=5800;
 List(h,{last,first});h.view.replays.ready=true;
 h.Screen("replay-sets");
 auto sent=h.actions.size();h.Choose("set:C:\\r\\s2.emberreplay");h.Press(MenuInput::Select);
 Check(h.actions.size()==sent+1&&Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){return r.replay.mode==replay::Mode::Watch&&r.replay.path=="C:\\r\\s1.emberreplay";}),"Watch set did not play the set's first game");
 h.Screen("replays");sent=h.actions.size();h.Choose("set-next");
 Check(h.actions.size()==sent+1&&Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){return r.replay.mode==replay::Mode::Watch&&r.replay.path=="C:\\r\\s2.emberreplay";}),"Next game did not play the set's second game");
}
}
void ReplayJourneys() {
 using namespace sf4e;
 ExportChoicesJourney();
 SetWatchJourney();
 Harness h;h.Frame();
 // A replay's row is its file: Watch now asks for that path. A link's question opens the screen and is answered with Watch or a dismissal.
 {
  auto shown=Listed("C:\\r\\a.emberreplay","2026-10-06 21:32");
  shown.watched=true;shown.provenance=sf4e::replayfiles::Provenance::Spectated;shown.summary=replayinputs::Summary{};shown.summary->scored=true;shown.summary->score[0]=2;shown.summary->score[1]=1;
  shown.fighters[0]=0;shown.fighters[1]=99;shown.names[0]="Ann";
  List(h,{shown});h.view.replays.ready=true;
  // Only the Replays screen asks the lister for a listing; Home asks for nothing.
  Check(!h.shell.ReplayWants().listing&&!h.shell.ReplayWants().detail,"Home asked the replay workers for something");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shownRows){rows=shownRows;});h.Screen("replays");
  Check(h.shell.ReplayWants().listing&&!h.shell.ReplayWants().detail,"The Replays screen did not ask for a listing");
  Check(rows.size()==6&&rows[0].id=="replay-import"&&rows[1].id=="replay-log"&&rows[2].id=="replay-folder"&&rows[3].id=="replay-filter"&&rows[4].id=="replay-search","Replays retained settings above the list");
  Check(rows[5].value=="2-1 · "+std::string(loc::T("replays.spectated_seen")),"Replay score and status separator wrong");
  Check(rows[5].id=="replay:C:\\r\\a.emberreplay"&&rows[5].label==ReplayRowLabel(shown.label,shown.names,shown.fighters),"Replay row lost its file or its label");
  // The portraits show the fighters, so the label names the players: a noted name, else the fighter.
  Check(rows[5].label==shown.label+"  "+loc::Tf("replays.fighters",std::string("Ann"),std::string(loc::T("common.unavailable"))),"Replay row label did not name its players");
  Check(rows[5].Match()&&rows[5].fighter1==0&&rows[5].fighter2==-1,"Replay row did not carry its fighters for the portraits, or kept an unknown one");
  Check(rows[0].fighter1==-1&&!rows[0].Match(),"A plain row drew portraits");
  Check(rows[5].detail.find("LP ")==std::string::npos,"Replay list still duplicates input totals");
  Check(rows[5].choices.size()==7&&rows[5].choices[0].id=="watch"&&rows[5].choices[1].id=="watch-meter"&&rows[5].choices[2].id=="add"&&rows[5].choices[3].id=="export"&&rows[5].choices[4].id=="inputs"&&rows[5].choices[5].id=="keep"&&rows[5].choices[6].id=="show-folder","Replay choice order wrong");
  // Show in folder asks the services for the row's own file, and its answer is the status line.
  {const auto sent=h.actions.size();h.Choose("replay:C:\\r\\a.emberreplay");for(int i=0;i<6;++i)h.Press(MenuInput::Right);h.Press(MenuInput::Select);
   Check(h.actions.size()==sent+1&&Is<commands::Service>(h.actions.back(),[](const commands::Service& s){return s.service==platform::ServiceAction::ShowReplayFile&&s.servicePath=="C:\\r\\a.emberreplay";}),"Show in folder did not ask for the replay's file");
   std::string status;Tone tone=Tone::Neutral;SetMenuStatusProbe([&](const char* text,Tone value){status=text;tone=value;});
   h.view.services.lastAction=platform::ServiceAction::ShowReplayFile;h.view.services.message="Opened C:\\r in Explorer.";h.view.services.succeeded=true;h.Frame();
   Check(status=="Opened C:\\r in Explorer."&&tone==Tone::Success,"Show in folder's answer was not the status");
   h.view.services.succeeded=false;h.view.services.message="Not opened.";h.Frame();
   Check(status=="Not opened."&&tone==Tone::Error,"Show in folder's failure was not an error");
   SetMenuStatusProbe({});h.view.services={};}
  // Import replay is always listed, ahead of the list. With the picker unavailable (full screen) it is off and says why.
  Check(!rows[0].enabled&&rows[0].detail==loc::T("replays.import_fullscreen"),"Import replay was offered where the picker cannot open");
  // Select asks the services for the picker, and nothing more: the files it returns, files dropped on the
  // window and a link's Import all go to one queue in the game, which says what they came to. The screen only
  // shows the picker open, the files waiting for Ember's menus, and the batch's answer.
  {h.view.services.canPickFiles=true;h.Frame();
   Check(rows[0].enabled&&rows[0].detail==loc::T("replays.import_detail"),"Import replay was not offered where the picker can open");
   const auto sent=h.actions.size();h.Choose("replay-import");
   Check(h.actions.size()==sent+1&&Is<commands::Service>(h.actions.back(),[](const commands::Service& s){return s.service==platform::ServiceAction::PickReplays;}),"Import replay did not ask for the picker");
   std::string status;Tone tone=Tone::Neutral;SetMenuStatusProbe([&](const char* text,Tone value){status=text;tone=value;});
   h.view.services.picking=true;h.Frame();
   Check(!rows[0].enabled&&status==loc::T("services.picking_replays")&&tone==Tone::Pending,"An open picker did not say so, or could be opened twice");
   h.view.services.picking=false;h.Frame();
   Check(rows[0].enabled&&h.actions.size()==sent+1,"The panel sent files to import itself");
   // Files wait for Ember's menus (in a room or a match): the screen says how many, and never gives up on them.
   h.view.replays.importWaiting=3;h.Frame();
   Check(status==loc::Tf("replays.import_waiting",3)&&tone==Tone::Pending,"Files waiting for the menus were not said");
   for(int frame=0;frame<2000;++frame)h.Frame();
   Check(status==loc::Tf("replays.import_waiting",3),"Waiting files were given up on after a time");
   // A batch begins (from any of the three ways in) and finishes: the game's notice is the answer, and a
   // notice that was already there does not end it.
   h.view.replays.importWaiting=0;h.view.replays.importBatches=1;h.view.replays.setNotices=7;h.Frame();
   Check(status==loc::T("replays.set_pack_working"),"A batch that began was not shown as working");
   h.view.replays.importFinished=1;h.view.replays.importNotice=loc::Tf("replays.import_done",5,2);h.view.replays.importNoticeError=false;h.Frame();
   Check(status==loc::Tf("replays.import_done",5,2)&&tone==Tone::Success,"A batch that all went in was not reported");
   // A set pack saved meanwhile is not the batch's answer, and does not end its wait.
   h.view.replays.importBatches=2;h.view.replays.setNotices=8;h.Frame();
   Check(status==loc::T("replays.set_pack_working"),"A set pack's notice ended an import batch's wait");
   // The runtime begins and ends a one-file batch in one tick: the one snapshot has both counts moved, and the
   // answer is shown, not waited for.
   h.view.replays.importBatches=3;h.view.replays.importFinished=3;h.view.replays.importNotice=loc::T("replays.set_import_failed");h.view.replays.importNoticeError=true;h.Frame();
   Check(status==loc::T("replays.set_import_failed")&&tone==Tone::Error,"A batch that began and ended in one snapshot was left on Working");
   h.view.replays.importBatches=4;h.view.replays.importFinished=4;h.view.replays.importNotice=loc::Tf("replays.import_done",1,1);h.view.replays.importNoticeError=false;h.Frame();
   Check(status==loc::Tf("replays.import_done",1,1)&&tone==Tone::Success,"A second batch that began and ended in one snapshot was not reported");
   SetMenuStatusProbe({});h.view.services={};h.view.replays.importNoticeError=false;}
  // A batch that begins while the player is elsewhere brings the Replays screen up, though not over a room's menu.
  h.Screen("home");h.view.replays.importBatches=5;h.view.replays.importFinished=5;h.Frame();
  Check(h.shell.Navigation().Screen()=="replays","A batch of dropped files did not open the Replays screen");
  // The next request from the screen (here the picker again) takes the batch's answer off the status line.
  h.view.services.canPickFiles=true;h.Frame();h.Choose("replay-import");h.view.services={};h.Frame();
  SetMenuEntriesProbe({});h.Choose("replay:C:\\r\\a.emberreplay");h.Press(MenuInput::Select);
  Check(Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){return r.replay.mode==sf4e::replay::Mode::Watch&&r.replay.path=="C:\\r\\a.emberreplay";}),"Watch now did not ask for the replay's file");
  Check(Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){return !r.replay.meter;}),"Watch now unexpectedly requested the frame meter");
  h.Choose("replay:C:\\r\\a.emberreplay");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
  Check(Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){return r.replay.mode==sf4e::replay::Mode::Watch&&r.replay.meter;}),"Watch with frame meter did not request the meter");
  // A replay with no class noted draws nothing over the game; one with classes
  // sends its names and classes to be drawn on the plates, while emblems show.
  Check(Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){return !r.replay.caption.Any();}),"Watch drew a caption for a replay with no class");
  {auto classed=shown;classed.names[0]="Player 1";classed.names[1]="Player 2";
   classed.classes[0]=SkillBadge::OfClass(7,true);classed.classes[1]=SkillBadge::OfClass(10,true);List(h,{classed});
   h.Choose("replay:C:\\r\\a.emberreplay");h.Press(MenuInput::Select);
   Check(Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){const auto& c=r.replay.caption;
    return r.replay.mode==sf4e::replay::Mode::Watch&&c.names&&c.name[0]=="Player 1"&&c.name[1]=="Player 2"&&c.band[0]==7&&c.band[1]==10&&!c.line&&!c.set&&!c.mark;}),
    "Watch did not send the replay's names and classes");
   h.view.preferences.classEmblems=false;h.Choose("replay:C:\\r\\a.emberreplay");h.Press(MenuInput::Select);
   Check(Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){return r.replay.mode==sf4e::replay::Mode::Watch&&!r.replay.caption.Any();}),"Watch drew classes the player hides");
   h.view.preferences.classEmblems=true;List(h,{shown});}
  h.Screen("home");h.view.replays.link="D:\\x\\b.usf4replay";h.Frame();
  Check(h.shell.Navigation().Screen()=="replays","A replay link did not open the Replays screen");
  // A replay file may be imported as a set pack is; Not now is last.
  h.Choose("replay-link");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
  Check(Is<commands::Replay>(h.actions.back(),[&](const commands::Replay& r){return r.replay.mode==sf4e::replay::Mode::Import&&r.replay.path==h.view.replays.link;}),"Import did not ask for the link's replay file");
  h.Choose("replay-link");h.Press(MenuInput::Right);h.Press(MenuInput::Right);h.Press(MenuInput::Select);
  Check(Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){return r.replay.mode==sf4e::replay::Mode::DismissLink&&r.replay.path.empty();}),"Not now did not dismiss the link");
  h.Choose("replay-link");h.Press(MenuInput::Select);
  Check(Is<commands::Replay>(h.actions.back(),[&](const commands::Replay& r){return r.replay.mode==sf4e::replay::Mode::Watch&&r.replay.path==h.view.replays.link;}),"Play it did not ask for the link's file");
  // The game's battle log came back to the main menu: Replays again, from Home.
  h.Screen("tab-player");h.shell.ShowReplays();h.Frame();
  Check(h.shell.Navigation().Screen()=="replays"&&h.shell.Navigation().Parent()=="home","Returning from the battle log did not show Replays over Home");
  h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="home","Back from Replays did not reach Home");h.Screen("replays");
  // A notice from the last request shows on Replays only, as an error when it failed.
  {std::string status;Tone tone=Tone::Neutral;SetMenuStatusProbe([&](const char* text,Tone value){status=text;tone=value;});
   h.view.replays.notice="Not added.";h.view.replays.noticeError=true;h.Frame();
   Check(status=="Not added."&&tone==Tone::Error,"Replays did not show its failed request");
   h.Screen("home");Check(status!="Not added.","The replay notice showed outside Replays");
   SetMenuStatusProbe({});h.view.replays.notice.clear();h.view.replays.noticeError=false;h.Screen("replays");}
  // A batch that begins while a room's menu is in front does not take it over; its files wait.
  h.Screen("home");h.view.session.room=netplay::RoomState::Joined;h.view.replays.importBatches=6;h.view.replays.importFinished=6;h.Frame();
  Check(h.shell.Navigation().Screen()!="replays","A batch took the room's menu");
  h.view.session.room=netplay::RoomState::Idle;h.Frame();h.Screen("replays");
  // Export video opens the caption's screen; the set so far is counted from the two matches before, the line is the date and score, and Generate sends all of it.
  {auto third=Listed("C:\\r\\c.emberreplay","2026-10-06 21:40"),second=third,first=third;third.names[0]="A";third.names[1]="B";third.summary=sf4e::replayinputs::Summary{};third.summary->scored=true;third.summary->score[0]=2;third.summary->score[1]=1;third.summary->frames=161*60;third.time=3000;
   second=third;second.path=std::filesystem::u8path("C:\\r\\b.emberreplay");second.time=2500;second.summary->score[0]=0;second.summary->score[1]=2;
   first=third;first.path=std::filesystem::u8path("C:\\r\\a2.emberreplay");first.time=2000;first.names[0]="B";first.names[1]="A";first.summary->score[0]=1;first.summary->score[1]=2;
   h.view.replays.link.clear();List(h,{third,second,first});h.Screen("replays");h.Choose("replay:C:\\r\\c.emberreplay");for(int i=0;i<3;++i)h.Press(MenuInput::Right);h.Press(MenuInput::Select);
   Check(h.shell.Navigation().Screen()=="replay-export","Export video did not open the caption's screen");
   h.Choose("cap-mark");h.Press(MenuInput::Left);h.Frame();h.Choose("cap-generate");
   const auto* exported=As<commands::Replay>(h.actions.back());
   const auto sent=exported?exported->replay:sf4e::replay::Request{};
   Check(sent.mode==sf4e::replay::Mode::Export&&sent.path=="C:\\r\\c.emberreplay"&&sent.caption.names&&sent.caption.line&&!sent.caption.mark&&sent.caption.set,"Generate did not send the export with its caption");
   Check(sent.caption.wins[0]==1&&sent.caption.wins[1]==1&&sent.caption.text=="2026-10-06"&&sent.caption.name[0]=="A","The caption did not start from the replay and the set before it, with no score that tells the winner");
   Check(h.shell.Navigation().Screen()=="replays","Generate did not return to the Replays screen");
   Check(!sent.meter,"Generate sent the frame meter that was off");
   // The export screen's Frame meter row goes with Generate, and is kept for the next export.
   h.Choose("replay:C:\\r\\c.emberreplay");for(int i=0;i<3;++i)h.Press(MenuInput::Right);h.Press(MenuInput::Select);
   h.FocusOn("cap-meter");h.Press(MenuInput::Right);h.Choose("cap-generate");
   Check(Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){return r.replay.mode==sf4e::replay::Mode::Export&&r.replay.meter;}),"Generate did not send the frame meter");
   {std::vector<MenuEntry> page;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& e){page=e;});
    h.Choose("replay:C:\\r\\c.emberreplay");for(int i=0;i<3;++i)h.Press(MenuInput::Right);h.Press(MenuInput::Select);
    const auto meter=std::find_if(page.begin(),page.end(),[](const MenuEntry& e){return e.id=="cap-meter";});
    Check(meter!=page.end()&&meter->value==loc::T("common.on")&&std::prev(page.end())->id=="cap-generate","The Frame meter row was not kept on, before Generate");
    SetMenuEntriesProbe({});h.Press(MenuInput::Back);
    Check(h.shell.Navigation().Screen()=="replays","Back from the export screen did not return to Replays");}
   // While it runs, Replays leads with its progress against the replay's length, and Cancel export asks first.
   {std::vector<MenuEntry> shownRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& e){shownRows=e;});
    const auto row=[&](const char* id)->const MenuEntry*{for(const auto& e:shownRows)if(e.id==id)return &e;return nullptr;};
    h.view.replays.ready=false;h.view.replays.exportStage=sf4e::replay::ExportStage::Starting;h.Frame();
    Check(!shownRows.empty()&&shownRows[0].id=="export-progress"&&shownRows[0].value==loc::T("export.starting")&&row("export-cancel"),"A starting export did not lead the Replays screen with its Cancel");
    h.view.replays.exportStage=sf4e::replay::ExportStage::Recording;h.view.replays.exportFrames=62*60;h.view.replays.exportTotal=161*60;h.Frame();
    Check(shownRows[0].value=="1:02 / 2:41","The export's progress was not shown against the replay's length");
    const auto sent=h.actions.size();
    h.FocusOn("export-cancel");h.Press(MenuInput::Select);
    Check(h.shell.Navigation().Confirming()&&h.actions.size()==sent,"Cancel export did not ask first");
    h.Press(MenuInput::Select);
    Check(h.actions.size()==sent&&!h.shell.Navigation().Confirming(),"Cancel focused first did not keep the export");
    h.FocusOn("export-cancel");h.Press(MenuInput::Select);h.Press(MenuInput::Right);h.Press(MenuInput::Select);
    Check(h.actions.size()==sent+1&&Is<commands::Replay>(h.actions.back(),[](const commands::Replay& r){return r.replay.mode==sf4e::replay::Mode::CancelExport&&r.replay.path.empty();}),"Confirming Cancel export did not cancel it");
    // Once the encoder closes the file, or is ending a cancelled one, there is nothing to cancel.
    h.view.replays.exportStage=sf4e::replay::ExportStage::Cancelling;h.Frame();
    Check(shownRows[0].value==loc::T("export.cancelling")&&!row("export-cancel"),"A cancelling export still offered Cancel");
    h.view.replays.exportStage=sf4e::replay::ExportStage::Finishing;h.Frame();
    Check(shownRows[0].value==loc::T("export.finishing")&&!row("export-cancel"),"A finishing export still offered Cancel");
    h.view.replays.exportStage=sf4e::replay::ExportStage::None;h.view.replays.exportFrames=h.view.replays.exportTotal=0;h.view.replays.ready=true;h.Frame();
    Check(!row("export-progress")&&!row("export-cancel"),"The export's rows outlived it");
    SetMenuEntriesProbe({});}}
  InputsJourney(h,true);
  InputsReplacementJourney(h);
  // A choice that needs the main menu and no room says why it is unavailable, not only greys.
  {std::vector<MenuEntry> shownRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& e){shownRows=e;});
   const auto choice=[&](const char* id){
    for(const auto& e:shownRows)if(e.id=="replay:C:\\r\\c.emberreplay")for(const auto& c:e.choices)if(c.id==id)return c;
    Check(false,"A replay row's choice was not listed");return MenuChoice{};};
   h.view.replays.link.clear();List(h,{Listed("C:\\r\\c.emberreplay","2026-10-06 21:40")});h.Screen("replays");
   Check(choice("watch").enabled&&choice("watch").detail==loc::T("replays.watch_detail"),"Watch now was not offered at the main menu");
   // Joining a room takes the menu to the room, so Replays is opened again over it.
   h.view.session.room=netplay::RoomState::Joined;h.Frame();h.shell.Navigation().Push("replays");h.Frame();
   Check(!choice("watch").enabled&&choice("watch").detail==loc::T("replays.open_log_room"),"Watch now in a room did not say to leave it");
   Check(!choice("export").enabled&&choice("export").detail==loc::T("replays.open_log_room")&&choice("add").enabled&&choice("inputs").enabled,"A room left a choice without its reason, or greyed one that works there");
   h.view.session.room=netplay::RoomState::Idle;h.view.replays.ready=false;h.Frame();
   Check(!choice("watch").enabled&&choice("watch").detail==loc::T("replays.not_ready")&&!choice("add").enabled&&choice("add").detail==loc::T("replays.not_ready"),"Watch now in a match did not say it needs the main menu");
   h.view.replays.ready=true;SetMenuEntriesProbe({});h.Frame();}
  // In a room the menu is the room's: a link waits there, and takes the menu once the room is left.
  h.view.replays.link.clear();h.Frame();h.Screen("home");
  h.view.session.room=netplay::RoomState::Joined;h.view.replays.link="D:\\x\\c.usf4replay";h.Frame();
  Check(h.shell.Navigation().Screen()!="replays","A replay link moved the menu in a room");
  h.view.session.room=netplay::RoomState::Idle;h.Frame();
  Check(h.shell.Navigation().Screen()=="replays","A replay link did not open the Replays screen once the room was left");
  InputsJourney(h,false);
  h.view.replays=ReplaysView{};h.Frame();}
}
