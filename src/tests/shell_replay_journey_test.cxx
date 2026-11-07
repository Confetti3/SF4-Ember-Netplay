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
  const auto at=std::find_if(h.actions.rbegin(),h.actions.rend(),[](const ShellAction& a){return a.command.kind==netplay::CommandKind::SavePreferences;});
  Check(at!=h.actions.rend()&&h.actions.size()>sent,"A caption choice was not saved");
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
 const auto& sent=h.actions.back().replay;
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
  Check(std::none_of(h.actions.begin()+before,h.actions.end(),[](const ShellAction& a){return a.command.kind==netplay::CommandKind::SavePreferences;}),
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
 Check(h.actions.size()==sent+1&&h.actions.back().replay.mode==replay::Mode::Watch&&h.actions.back().replay.path=="C:\\r\\s1.emberreplay","Watch set did not play the set's first game");
 h.Screen("replays");sent=h.actions.size();h.Choose("set-next");
 Check(h.actions.size()==sent+1&&h.actions.back().replay.mode==replay::Mode::Watch&&h.actions.back().replay.path=="C:\\r\\s2.emberreplay","Next game did not play the set's second game");
}
}
// A game that offers only watching, keeping and showing a replay (SFV Ember's
// capabilities): no Battle Log row, and each row's choices are those three,
// Watch now first; Watch now still asks for the row's file.
void WatchOnlyJourney() {
 using namespace sf4e;
 Harness h;h.Frame();
 h.view.replayGameList=h.view.replayVideo=h.view.replayMeter=h.view.replayInputs=false;
 auto shown=Listed("C:\\r\\v.sfvreplay","2026-11-07 10:00");shown.names[0]="player1";shown.names[1]="player2";
 List(h,{shown});h.view.replays.ready=true;
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& e){rows=e;});h.Screen("replays");
 Check(rows.size()==4&&rows[0].id=="replay-folder"&&rows[1].id=="replay-show"&&rows[2].id=="replay-search"&&rows[3].id=="replay:C:\\r\\v.sfvreplay","A game with no replay list of its own still had the Battle Log row");
 Check(rows[3].choices.size()==3&&rows[3].choices[0].id=="watch"&&rows[3].choices[1].id=="keep"&&rows[3].choices[2].id=="show-folder","A watch-only game was offered more than watching, keeping and showing");
 Check(rows[3].chosen=="watch","Watch now was not the row's first choice");
 h.view.replays.ready=false;h.Frame();
 Check(rows[3].chosen=="keep","Where nothing plays, the row did not fall back to Keep");
 h.view.replays.ready=true;h.Frame();SetMenuEntriesProbe({});
 h.Choose("replay:C:\\r\\v.sfvreplay");h.Press(MenuInput::Select);
 Check(h.actions.back().replay.mode==replay::Mode::Watch&&h.actions.back().replay.path=="C:\\r\\v.sfvreplay"&&!h.actions.back().replay.meter,"Watch now did not ask for the replay's file");
 h.Choose("replay:C:\\r\\v.sfvreplay");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.back().replay.mode==replay::Mode::Keep&&h.actions.back().replay.keep,"Keep was not the second choice");
}
void ReplayJourneys() {
 using namespace sf4e;
 ExportChoicesJourney();
 SetWatchJourney();
 WatchOnlyJourney();
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
  Check(rows.size()==5&&rows[0].id=="replay-log"&&rows[1].id=="replay-folder"&&rows[2].id=="replay-show"&&rows[3].id=="replay-search","Replays retained settings above the list");
  Check(rows[4].value=="2-1 · "+std::string(loc::T("replays.spectated_seen")),"Replay score and status separator wrong");
  Check(rows[4].id=="replay:C:\\r\\a.emberreplay"&&rows[4].label==ReplayRowLabel(shown.label,shown.names,shown.fighters),"Replay row lost its file or its label");
  // The portraits show the fighters, so the label names the players: a noted name, else the fighter.
  Check(rows[4].label==shown.label+"  "+loc::Tf("replays.fighters",std::string("Ann"),std::string(loc::T("common.unavailable"))),"Replay row label did not name its players");
  Check(rows[4].Match()&&rows[4].fighter1==0&&rows[4].fighter2==-1,"Replay row did not carry its fighters for the portraits, or kept an unknown one");
  Check(rows[0].fighter1==-1&&!rows[0].Match(),"A plain row drew portraits");
  Check(rows[4].detail.find("LP ")==std::string::npos,"Replay list still duplicates input totals");
  Check(rows[4].choices.size()==7&&rows[4].choices[0].id=="watch"&&rows[4].choices[1].id=="watch-meter"&&rows[4].choices[2].id=="add"&&rows[4].choices[3].id=="export"&&rows[4].choices[4].id=="inputs"&&rows[4].choices[5].id=="keep"&&rows[4].choices[6].id=="show-folder","Replay choice order wrong");
  // Show in folder asks the services for the row's own file, and its answer is the status line.
  {const auto sent=h.actions.size();h.Choose("replay:C:\\r\\a.emberreplay");for(int i=0;i<6;++i)h.Press(MenuInput::Right);h.Press(MenuInput::Select);
   Check(h.actions.size()==sent+1&&h.actions.back().service==platform::ServiceAction::ShowReplayFile&&h.actions.back().servicePath=="C:\\r\\a.emberreplay"&&h.actions.back().replay.mode==sf4e::replay::Mode::None,"Show in folder did not ask for the replay's file");
   std::string status;Tone tone=Tone::Neutral;SetMenuStatusProbe([&](const char* text,Tone value){status=text;tone=value;});
   h.view.services.lastAction=platform::ServiceAction::ShowReplayFile;h.view.services.message="Opened C:\\r in Explorer.";h.view.services.succeeded=true;h.Frame();
   Check(status=="Opened C:\\r in Explorer."&&tone==Tone::Success,"Show in folder's answer was not the status");
   h.view.services.succeeded=false;h.view.services.message="Not opened.";h.Frame();
   Check(status=="Not opened."&&tone==Tone::Error,"Show in folder's failure was not an error");
   SetMenuStatusProbe({});h.view.services={};}
  SetMenuEntriesProbe({});h.Choose("replay:C:\\r\\a.emberreplay");h.Press(MenuInput::Select);
  Check(h.actions.back().replay.mode==sf4e::replay::Mode::Watch&&h.actions.back().replay.path=="C:\\r\\a.emberreplay","Watch now did not ask for the replay's file");
  Check(!h.actions.back().replay.meter,"Watch now unexpectedly requested the frame meter");
  h.Choose("replay:C:\\r\\a.emberreplay");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
  Check(h.actions.back().replay.mode==sf4e::replay::Mode::Watch&&h.actions.back().replay.meter,"Watch with frame meter did not request the meter");
  h.Screen("home");h.view.replays.link="D:\\x\\b.usf4replay";h.Frame();
  Check(h.shell.Navigation().Screen()=="replays","A replay link did not open the Replays screen");
  h.Choose("replay-link");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
  Check(h.actions.back().replay.mode==sf4e::replay::Mode::DismissLink&&h.actions.back().replay.path.empty(),"Not now did not dismiss the link");
  h.Choose("replay-link");h.Press(MenuInput::Select);
  Check(h.actions.back().replay.mode==sf4e::replay::Mode::Watch&&h.actions.back().replay.path==h.view.replays.link,"Play it did not ask for the link's file");
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
  // Export video opens the caption's screen; the set so far is counted from the two matches before, the line is the date and score, and Generate sends all of it.
  {auto third=Listed("C:\\r\\c.emberreplay","2026-10-06 21:40"),second=third,first=third;third.names[0]="A";third.names[1]="B";third.summary=sf4e::replayinputs::Summary{};third.summary->scored=true;third.summary->score[0]=2;third.summary->score[1]=1;third.summary->frames=161*60;third.time=3000;
   second=third;second.path=std::filesystem::u8path("C:\\r\\b.emberreplay");second.time=2500;second.summary->score[0]=0;second.summary->score[1]=2;
   first=third;first.path=std::filesystem::u8path("C:\\r\\a2.emberreplay");first.time=2000;first.names[0]="B";first.names[1]="A";first.summary->score[0]=1;first.summary->score[1]=2;
   h.view.replays.link.clear();List(h,{third,second,first});h.Screen("replays");h.Choose("replay:C:\\r\\c.emberreplay");for(int i=0;i<3;++i)h.Press(MenuInput::Right);h.Press(MenuInput::Select);
   Check(h.shell.Navigation().Screen()=="replay-export","Export video did not open the caption's screen");
   h.Choose("cap-mark");h.Press(MenuInput::Left);h.Frame();h.Choose("cap-generate");
   const auto& sent=h.actions.back().replay;
   Check(sent.mode==sf4e::replay::Mode::Export&&sent.path=="C:\\r\\c.emberreplay"&&sent.caption.names&&sent.caption.line&&!sent.caption.mark&&sent.caption.set,"Generate did not send the export with its caption");
   Check(sent.caption.wins[0]==1&&sent.caption.wins[1]==1&&sent.caption.text=="2026-10-06"&&sent.caption.name[0]=="A","The caption did not start from the replay and the set before it, with no score that tells the winner");
   Check(h.shell.Navigation().Screen()=="replays","Generate did not return to the Replays screen");
   Check(!sent.meter,"Generate sent the frame meter that was off");
   // The export screen's Frame meter row goes with Generate, and is kept for the next export.
   h.Choose("replay:C:\\r\\c.emberreplay");for(int i=0;i<3;++i)h.Press(MenuInput::Right);h.Press(MenuInput::Select);
   h.FocusOn("cap-meter");h.Press(MenuInput::Right);h.Choose("cap-generate");
   Check(h.actions.back().replay.mode==sf4e::replay::Mode::Export&&h.actions.back().replay.meter,"Generate did not send the frame meter");
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
    Check(h.actions.size()==sent+1&&h.actions.back().replay.mode==sf4e::replay::Mode::CancelExport&&h.actions.back().replay.path.empty(),"Confirming Cancel export did not cancel it");
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
  // In a room the menu is the room's: a link waits there, and takes the menu once the room is left.
  h.view.replays.link.clear();h.Frame();h.Screen("home");
  h.view.session.room=netplay::RoomState::Joined;h.view.replays.link="D:\\x\\c.usf4replay";h.Frame();
  Check(h.shell.Navigation().Screen()!="replays","A replay link moved the menu in a room");
  h.view.session.room=netplay::RoomState::Idle;h.Frame();
  Check(h.shell.Navigation().Screen()=="replays","A replay link did not open the Replays screen once the room was left");
  InputsJourney(h,false);
  h.view.replays=ReplaysView{};h.Frame();}
}
