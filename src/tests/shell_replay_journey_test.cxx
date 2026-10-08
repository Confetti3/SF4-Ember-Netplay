#include "shell_journey_support.hxx"
#include "shell_replay_journey.hxx"
namespace {
std::shared_ptr<const sf4e::replayinputs::Detail> InputsFixture(const std::string& file) {
 auto detail=std::make_shared<sf4e::replayinputs::Detail>();detail->file=file;
 std::vector<unsigned char> replay(0x320+0x88,0);std::memcpy(replay.data(),"#BRP",4);replay[8]=1;replay[10]=8;replay[0x18]=1;replay[0x320+0x7C]=3;
 const unsigned char press[]={0x10,0,0};replay.insert(replay.end(),press,press+3);
 Check(sf4e::replayinputs::Parse(replay.data(),replay.size(),detail->match),"Inputs fixture did not parse");
 detail->summary=sf4e::replayinputs::Summarize(detail->match);
 detail->logs.push_back(sf4e::replayinputs::Log(detail->match.rounds[0]));
 return detail;
}
void OpenInputs(Harness& h,const ShellView::Replay& read,bool ready) {
 h.view.replayLink.clear();h.view.replays={read};h.view.replaysReady=ready;h.Screen("replays");
 const auto sent=h.actions.size();const std::string row="replay:"+read.path;h.Choose(row.c_str());
 if(ready)for(int i=0;i<2;++i)h.Press(MenuInput::Right);
 h.Press(MenuInput::Select);
 Check(h.shell.Navigation().Screen()=="replay-inputs"&&h.actions.size()==sent&&h.shell.ReplayInputsFile()==read.path,"Inputs and stats did not open without sending a game action");
}
void InputsJourney(Harness& h,bool ready) {
 using sf4e::replayinputs::DetailState;
 ShellView::Replay read;read.path="C:\\r\\inputs.usf4replay";read.label="2026-10-06 21:32  Ryu vs Ken";
 h.view.replayDetail={};OpenInputs(h,read,ready);h.FocusOn("inputs-reading");
 const auto first=h.shell.ReplayInputsRevision();
 h.view.replayDetail={first,DetailState::Unreadable,{}};h.Frame();h.FocusOn("inputs-none");
 h.view.replayDetail={first,DetailState::Ready,InputsFixture(read.path)};h.Frame();
 h.FocusOn("inputs-p2");h.FocusOn("inputs-round-1");
 h.Press(MenuInput::Back);OpenInputs(h,read,ready);
 const auto second=h.shell.ReplayInputsRevision();
 Check(second>first,"Reopening the same file did not issue another revision");
 h.FocusOn("inputs-reading"); // old successful result must not satisfy the new entry
 h.view.replayDetail={second,DetailState::Failed,{}};h.Frame();h.FocusOn("inputs-none");
 h.Press(MenuInput::Back);OpenInputs(h,read,ready);
 const auto third=h.shell.ReplayInputsRevision();
 h.view.replayDetail={third,DetailState::Ready,InputsFixture(read.path)};h.Frame();
 h.FocusOn("inputs-p2");h.FocusOn("inputs-round-1");
 h.view.replayDetail={};h.view.replaysReady=true;
}
}
void ReplayJourneys() {
 using namespace sf4e;
 Harness h;h.Frame();
 // A replay's row is its file: Watch now asks for that path. A link's question opens the screen and is answered with Watch or a dismissal.
 {
  ShellView::Replay shown;shown.path="C:\\r\\a.emberreplay";shown.label="2026-10-06 21:32  Ryu vs Ken";
  h.view.replays={shown};h.view.replaysReady=true;h.Screen("replays");h.Choose("replay:C:\\r\\a.emberreplay");h.Press(MenuInput::Select);
  Check(h.actions.back().replay.mode==sf4e::replay::Mode::Watch&&h.actions.back().replay.path==shown.path,"Watch now did not ask for the replay's file");
  Check(!h.actions.back().replay.meter,"Watch now asked for the frame meter with the row off");
  h.Choose("replay-meter");h.Press(MenuInput::Right);h.Frame();h.Choose("replay:C:\\r\\a.emberreplay");h.Press(MenuInput::Select);
  Check(h.actions.back().replay.mode==sf4e::replay::Mode::Watch&&h.actions.back().replay.meter,"Watch now did not ask for the frame meter with the row on");
  h.Screen("home");h.view.replayLink="D:\\x\\b.usf4replay";h.Frame();
  Check(h.shell.Navigation().Screen()=="replays","A replay link did not open the Replays screen");
  h.Choose("replay-link");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
  Check(h.actions.back().replay.mode==sf4e::replay::Mode::DismissLink&&h.actions.back().replay.path.empty(),"Not now did not dismiss the link");
  h.Choose("replay-link");h.Press(MenuInput::Select);
  Check(h.actions.back().replay.mode==sf4e::replay::Mode::Watch&&h.actions.back().replay.path==h.view.replayLink,"Play it did not ask for the link's file");
  // Export video opens the caption's screen; the set so far is counted from the two matches before, the line is the date and score, and Generate sends all of it.
  {ShellView::Replay third,second,first;third.path="C:\\r\\c.emberreplay";third.label="2026-10-06 21:40  A (Ryu) vs B (Ken)";third.names[0]="A";third.names[1]="B";third.summary=sf4e::replayinputs::Summary{};third.summary->scored=true;third.summary->score[0]=2;third.summary->score[1]=1;third.time=3000;
   second=third;second.path="C:\\r\\b.emberreplay";second.time=2500;second.summary->score[0]=0;second.summary->score[1]=2;
   first=third;first.path="C:\\r\\a2.emberreplay";first.time=2000;first.names[0]="B";first.names[1]="A";first.summary->score[0]=1;first.summary->score[1]=2;
   h.view.replayLink.clear();h.view.replays={third,second,first};h.Screen("replays");h.Choose("replay:C:\\r\\c.emberreplay");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
   Check(h.shell.Navigation().Screen()=="replay-export","Export video did not open the caption's screen");
   h.Choose("cap-mark");h.Press(MenuInput::Left);h.Frame();h.Choose("cap-generate");
   const auto& sent=h.actions.back().replay;
   Check(sent.mode==sf4e::replay::Mode::Export&&sent.path==third.path&&sent.caption.names&&sent.caption.line&&!sent.caption.mark&&sent.caption.set,"Generate did not send the export with its caption");
   Check(sent.caption.wins[0]==1&&sent.caption.wins[1]==1&&sent.caption.text=="2026-10-06   2-1"&&sent.caption.name[0]=="A","The caption did not start from the replay and the set before it");
   Check(h.shell.Navigation().Screen()=="replays","Generate did not return to the Replays screen");}
  InputsJourney(h,true);
  // In a room the menu is the room's: a link waits there, and takes the menu once the room is left.
  h.view.replayLink.clear();h.Frame();h.Screen("home");
  h.view.session.room=netplay::RoomState::Joined;h.view.replayLink="D:\\x\\c.usf4replay";h.Frame();
  Check(h.shell.Navigation().Screen()!="replays","A replay link moved the menu in a room");
  h.view.session.room=netplay::RoomState::Idle;h.Frame();
  Check(h.shell.Navigation().Screen()=="replays","A replay link did not open the Replays screen once the room was left");
  InputsJourney(h,false);
  h.view.replays.clear();h.view.replaysReady=false;h.view.replayLink.clear();h.Frame();}
}
