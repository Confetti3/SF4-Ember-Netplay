#include "shell_journey_support.hxx"
#include "shell_replay_journey.hxx"
#include "../ui/ReplaySummaryText.hxx"
#include <algorithm>
#include <chrono>
#include <fstream>
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
 save(path,a);save(files::NamesPath(folder,a),files::BindNames(a,{{"Ann","Bob"},false}));
 in::DetailCache cache;
 const auto file=path.u8string();const auto first=cache.Read(file,1,folder);
 Check(first.value!=nullptr,"Original detail unreadable");
 auto row=Listed(file,first.value->label);row.names[0]="Ann";row.names[1]="Bob";
 for(int side=0;side<2;++side)row.fighters[side]=first.value->fighters[side];
 const auto rowLabel=ReplayLabel(row.label,row.names,row.fighters);
 const auto written=fs::last_write_time(path);
 save(path,b);fs::last_write_time(path,written);save(files::NamesPath(folder,b),files::BindNames(b,{{"Carol","Dave"},true}));
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
}
void ReplayJourneys() {
 using namespace sf4e;
 Harness h;h.Frame();
 // A replay's row is its file: Watch now asks for that path. A link's question opens the screen and is answered with Watch or a dismissal.
 {
  auto shown=Listed("C:\\r\\a.emberreplay","2026-10-06 21:32");
  shown.watched=shown.spectated=true;shown.summary=replayinputs::Summary{};shown.summary->scored=true;shown.summary->score[0]=2;shown.summary->score[1]=1;
  List(h,{shown});h.view.replays.ready=true;
  // Only the Replays screen asks the lister for a listing; Home asks for nothing.
  Check(!h.shell.ReplayWants().listing&&!h.shell.ReplayWants().detail,"Home asked the replay workers for something");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shownRows){rows=shownRows;});h.Screen("replays");
  Check(h.shell.ReplayWants().listing&&!h.shell.ReplayWants().detail,"The Replays screen did not ask for a listing");
  Check(rows.size()==3&&rows[0].id=="replay-log"&&rows[1].id=="replay-folder","Replays retained settings above the list");
  Check(rows[2].value=="2-1 · "+std::string(loc::T("replays.watched_spectated")),"Replay score and status separator wrong");
  Check(rows[2].id=="replay:C:\\r\\a.emberreplay"&&rows[2].label==ReplayLabel(shown.label,shown.names,shown.fighters),"Replay row lost its file or its label");
  Check(rows[2].detail.find("LP ")==std::string::npos,"Replay list still duplicates input totals");
  Check(rows[2].choices.size()==5&&rows[2].choices[0].id=="watch"&&rows[2].choices[1].id=="watch-meter"&&rows[2].choices[2].id=="add"&&rows[2].choices[3].id=="export"&&rows[2].choices[4].id=="inputs","Replay choice order wrong");
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
  h.Screen("settings");h.shell.ShowReplays();h.Frame();
  Check(h.shell.Navigation().Screen()=="replays"&&h.shell.Navigation().Parent()=="home","Returning from the battle log did not show Replays over Home");
  h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="home","Back from Replays did not reach Home");h.Screen("replays");
  // A notice from the last request shows on Replays only, as an error when it failed.
  {std::string status;Tone tone=Tone::Neutral;SetMenuStatusProbe([&](const char* text,Tone value){status=text;tone=value;});
   h.view.replays.notice="Not added.";h.view.replays.noticeError=true;h.Frame();
   Check(status=="Not added."&&tone==Tone::Error,"Replays did not show its failed request");
   h.Screen("home");Check(status!="Not added.","The replay notice showed outside Replays");
   SetMenuStatusProbe({});h.view.replays.notice.clear();h.view.replays.noticeError=false;h.Screen("replays");}
  // Export video opens the caption's screen; the set so far is counted from the two matches before, the line is the date and score, and Generate sends all of it.
  {auto third=Listed("C:\\r\\c.emberreplay","2026-10-06 21:40"),second=third,first=third;third.names[0]="A";third.names[1]="B";third.summary=sf4e::replayinputs::Summary{};third.summary->scored=true;third.summary->score[0]=2;third.summary->score[1]=1;third.time=3000;
   second=third;second.path=std::filesystem::u8path("C:\\r\\b.emberreplay");second.time=2500;second.summary->score[0]=0;second.summary->score[1]=2;
   first=third;first.path=std::filesystem::u8path("C:\\r\\a2.emberreplay");first.time=2000;first.names[0]="B";first.names[1]="A";first.summary->score[0]=1;first.summary->score[1]=2;
   h.view.replays.link.clear();List(h,{third,second,first});h.Screen("replays");h.Choose("replay:C:\\r\\c.emberreplay");for(int i=0;i<3;++i)h.Press(MenuInput::Right);h.Press(MenuInput::Select);
   Check(h.shell.Navigation().Screen()=="replay-export","Export video did not open the caption's screen");
   h.Choose("cap-mark");h.Press(MenuInput::Left);h.Frame();h.Choose("cap-generate");
   const auto& sent=h.actions.back().replay;
   Check(sent.mode==sf4e::replay::Mode::Export&&sent.path=="C:\\r\\c.emberreplay"&&sent.caption.names&&sent.caption.line&&!sent.caption.mark&&sent.caption.set,"Generate did not send the export with its caption");
   Check(sent.caption.wins[0]==1&&sent.caption.wins[1]==1&&sent.caption.text=="2026-10-06   2-1"&&sent.caption.name[0]=="A","The caption did not start from the replay and the set before it");
   Check(h.shell.Navigation().Screen()=="replays","Generate did not return to the Replays screen");}
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
