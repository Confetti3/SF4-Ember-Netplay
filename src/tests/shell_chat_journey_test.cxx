#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
// The room chat through the shell: the transcript a client keeps, the unread
// count, and the Chat screen's message box.
#include "../ui/ChatTranscript.hxx"
#include <cmath>

namespace {
// A joined room of three members (1 is this player) the chat journeys draw.
void JoinChatRoom(Harness& h,std::uint64_t epoch){
 using namespace sf4e;
 h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;
 h.view.room=room::Snapshot{};
 h.view.room.roomEpoch=epoch;h.view.room.localMember=1;h.view.room.host=1;h.view.room.name="Chat room";h.view.room.revision=3;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
 for(room::MemberId id=1;id<=3;++id){room::Member m;m.id=id;m.name=id==1?"Local":id==2?"Peer":"Third";h.view.room.members.push_back(m);}
}
ImGuiWindow* ActiveWindow(const char* fragment){
 ImGuiWindow* found=nullptr;
 for(auto* window:GImGui->Windows)if(window->Active&&std::strstr(window->Name,fragment))found=window;
 return found;
}
// What the transcript keeps, without a screen: messages by sequence, and a line for each change of the room.
void TranscriptLogic(){
 using namespace sf4e;using ui::ChatLine;
 room::Snapshot s;s.roomEpoch=5;s.localMember=1;s.host=1;
 for(room::MemberId id=1;id<=2;++id){room::Member m;m.id=id;m.name=id==1?"Local":"Peer";s.members.push_back(m);}
 for(int i=0;i<4;++i)s.tables[i].id=i;
 s.chat.push_back({1,2,"Said before we arrived"});
 ui::ChatTranscript t;
 Check(t.Update(s)&&t.Lines().size()==1&&t.Unread({})==0,"Joining a room did not take its history in as read, with no events");
 Check(!t.Update(s)&&t.Lines().size()==1,"The same snapshot again changed the transcript");
 // A snapshot that repeats old messages adds none; a new one is unread unless it is ours or muted.
 s.chat.push_back({2,2,"Hello"});s.chat.push_back({3,1,"Hi Peer"});
 t.Update(s);t.Update(s);
 Check(t.Lines().size()==3&&t.Lines()[1].text=="Hello"&&t.Lines()[1].name=="Peer"&&!t.Lines()[1].own&&t.Lines()[2].own,"Messages were not added once, by sequence");
 Check(t.Unread({})==1&&t.Unread({2})==0,"Unread counted our own message, or a muted member's");
 t.MarkRead();Check(t.Unread({})==0,"Reading did not clear the unread count");
 // Who comes and goes, and who hosts.
 room::Member third;third.id=3;third.name="Third";s.members.push_back(third);
 t.Update(s);
 Check(t.Lines().back().kind==ChatLine::Kind::Joined&&t.Lines().back().name=="Third","A new member was not announced");
 s.host=2;t.Update(s);
 Check(t.Lines().back().kind==ChatLine::Kind::NewHost&&t.Lines().back().name=="Peer","A new host was not announced");
 // A member's words stay after they go, under the name they had; the room itself drops them.
 s.chat.push_back({4,3,"Bye now"});t.Update(s);
 s.members.pop_back();s.chat.pop_back();
 t.Update(s);
 const auto& lines=t.Lines();
 Check(lines.back().kind==ChatLine::Kind::Left&&lines.back().name=="Third"&&lines[lines.size()-2].text=="Bye now"&&lines[lines.size()-2].name=="Third",
  "A member who left lost their messages or their name");
 // Results: a game won is one seat's score going up with the pair unchanged.
 s.tables[0].p1=1;s.tables[0].p2=2;t.Update(s);
 const auto count=t.Lines().size();
 s.tables[0].score[1]=1;t.Update(s);
 Check(t.Lines().size()==count+1&&t.Lines().back().kind==ChatLine::Kind::GameWon&&t.Lines().back().name=="Peer"&&t.Lines().back().table==0&&
  t.Lines().back().score[0]==1&&t.Lines().back().score[1]==0,"A game won was not announced with its winner first");
 s.tables[0].score[0]=s.tables[0].score[1]=0;t.Update(s);
 Check(t.Lines().size()==count+1,"A score reset was announced");
 // A finished set names its winner and its score, once, and not as a game as well.
 s.tables[0].lastSet.generation=7;s.tables[0].lastSet.p1=1;s.tables[0].lastSet.p2=2;s.tables[0].lastSet.winnerSeat=0;
 s.tables[0].lastSet.score[0]=3;s.tables[0].lastSet.score[1]=1;t.Update(s);t.Update(s);
 Check(t.Lines().size()==count+2&&t.Lines().back().kind==ChatLine::Kind::SetWon&&t.Lines().back().name=="Local"&&
  t.Lines().back().score[0]==3&&t.Lines().back().score[1]==1,"A set won was not announced once, with its winner and score");
 // A draw changes no score, so says nothing.
 const auto quiet=t.Lines().size();s.tables[0].phase=room::TablePhase::Playing;t.Update(s);s.tables[0].phase=room::TablePhase::Waiting;t.Update(s);
 Check(t.Lines().size()==quiet,"A game with no winner was announced");
 // Only the newest lines are kept.
 for(unsigned i=0;i<300;++i){
  s.chat.push_back({5+i,2,"Line "+std::to_string(i)});
  if(s.chat.size()>room::MaximumChatMessages)s.chat.erase(s.chat.begin());
  t.Update(s);
 }
 Check(t.Lines().size()==ui::ChatTranscript::MaximumLines&&t.Lines().back().text=="Line 299","The transcript is not capped at its newest lines");
 // A different room starts again from its own history.
 s.roomEpoch=6;s.chat.clear();s.chat.push_back({1,2,"New room"});
 Check(t.Update(s)&&t.Lines().size()==1&&t.Lines()[0].text=="New room"&&t.Unread({})==0,"A different room kept the old transcript or counted its history as unread");
}
}
void ChatJourneys(){
 using namespace sf4e;
 TranscriptLogic();
 Harness h;h.Frame();
 auto& io=ImGui::GetIO();
 const auto key=[&](ImGuiKey k){h.Frame();io.AddKeyEvent(k,true);h.Frame();io.AddKeyEvent(k,false);h.Frame();};
 SetMenuGlyphs(input::PadKeyboard,0,0);
 JoinChatRoom(h,20);
 std::uint64_t sequence=0;
 // The room keeps its newest hundred messages, as the real one does.
 const auto say=[&](room::MemberId who,std::string text){
  h.view.room.chat.push_back({++sequence,who,std::move(text)});
  if(h.view.room.chat.size()>room::MaximumChatMessages)h.view.room.chat.erase(h.view.room.chat.begin());
 };
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
 const auto row=[&](const char* id)->const MenuEntry&{
  const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
  if(it==rows.end())throw std::runtime_error(std::string("No row ")+id);
  return *it;
 };
 say(2,"Earlier");say(3,"Earlier still");
 h.Screen("room");h.Frame(0,3);
 // What was said before the player arrived is not news.
 Check(row("room-chat").value.empty(),"Chat history from before joining counted as unread");
 // The unread count: others' messages, not our own or a muted member's, on the Chat row and the board.
 say(2,"Anyone here?");say(1,"Yes, me");say(3,"Hello");h.Frame();
 Check(row("room-chat").value=="2"&&row("room-chat").detail.find(loc::Tf("room.chat.unread",2))!=std::string::npos,"The Chat row does not count messages from others");
 h.Screen("room-members");h.Choose("member-3");h.Choose("mute");h.Screen("room");h.Frame();
 Check(row("room-chat").value=="1","A muted member's message still counts as unread");
 h.Screen("room-members");h.Choose("member-3");h.Choose("mute");h.Screen("room");h.Frame();
 Check(row("room-chat").value=="2","Unmuting did not bring the count back");
 // Opening Chat clears it; so does reading it where the keyboard opens it.
 h.Press(MenuInput::Chat);Check(h.shell.Navigation().Screen()=="room-chat","View did not open chat");
 h.Frame(0,2);h.Press(MenuInput::Chat);h.Frame();
 Check(h.shell.Navigation().Screen()=="room"&&row("room-chat").value.empty(),"Opening Chat did not clear the unread count");
 say(2,"Another");h.Frame();Check(row("room-chat").value=="1","A message after reading did not count");
 h.Screen("home");say(2,"While on Home");h.Frame();h.Screen("room");h.Frame();
 Check(row("room-chat").value=="2","Messages that arrived away from the room screens did not count");
 // The message box is live as soon as the screen opens: letters, F, T and C are text, and the keys are not advertised.
 std::set<std::string> legend;
 h.Screen("room");h.Press(MenuInput::Chat);h.Frame(0,4);
 SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))legend.insert(id+7);});
 h.Frame();SetMenuTextProbe({});
 Check(!legend.count(loc::T("room.legend_fighter"))&&legend.count(loc::T("common.back")),"The chat legend offers keys that type letters, or lacks Back");
 Check(!row("compose").enabled&&row("compose").value.empty(),"An empty draft offers Send");
 io.AddInputCharactersUTF8("fct ");h.Frame(0,2);key(ImGuiKey_F);key(ImGuiKey_Space);
 Check(h.shell.Navigation().Screen()=="room-chat"&&row("compose").value=="fct ","Typing in chat pressed a menu key or was lost");
 io.AddInputCharactersUTF8("Hel");h.Frame(0,2);
 Check(row("compose").value=="fct Hel"&&row("compose").enabled,"Typed text did not reach the draft");
 // Escape leaves without taking the draft; coming back and typing adds to it.
 key(ImGuiKey_Escape);
 Check(h.shell.Navigation().Screen()=="room","Escape did not return to the board");
 key(ImGuiKey_C);h.Frame(0,4);
 Check(h.shell.Navigation().Screen()=="room-chat"&&row("compose").value=="fct Hel","Leaving the Chat screen lost the draft");
 io.AddInputCharactersUTF8("lo");h.Frame(0,2);
 Check(row("compose").value=="fct Hello","Typing after returning replaced the draft instead of adding to it");
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="room","Back did not return to the board");
 h.Press(MenuInput::Chat);h.Frame(0,4);Check(row("compose").value=="fct Hello","Back from Chat lost the draft");
 // Enter sends the draft, once; the draft stays until the room's chat has it.
 for(int i=0;i<3;++i)key(ImGuiKey_Backspace);
 Check(row("compose").value=="fct He","Backspace in the box did not edit the draft");
 io.AddInputCharactersUTF8("llo");h.Frame(0,2);
 auto sent=h.actions.size();
 key(ImGuiKey_Enter);
 Check(h.actions.size()==sent+1&&h.actions.back().roomAction.kind==room::ActionKind::Chat&&h.actions.back().roomAction.text=="fct Hello",
  "Enter did not send the draft");
 key(ImGuiKey_Enter);
 Check(h.actions.size()==sent+1,"Enter sent the same text twice while it was on its way");
 Check(row("compose").value=="fct Hello"&&h.shell.Navigation().Screen()=="room-chat","The draft left the box before the room had the message");
 // A different member saying the same words is not our message arriving.
 say(2,"fct Hello");h.Frame(0,2);
 Check(row("compose").value=="fct Hello","Another member's identical message cleared the draft");
 say(1,"fct Hello");h.Frame(0,2);
 Check(row("compose").value.empty()&&!row("compose").enabled,"The draft stayed after the room's chat had the message");
 // A send the room refuses, or that never arrives, keeps the draft and can be sent again.
 io.AddInputCharactersUTF8("Try again");h.Frame(0,2);
 sent=h.actions.size();key(ImGuiKey_Enter);
 Check(h.actions.size()==sent+1,"The second message was not sent");
 h.Wait(9);h.Frame();
 Check(row("compose").value=="Try again"&&row("compose").enabled,"A message the room never took lost its draft");
 key(ImGuiKey_KeypadEnter);
 Check(h.actions.size()==sent+2&&h.actions.back().roomAction.text=="Try again","A refused message could not be sent again, or the numpad's Enter does not send");
 // A send the shell refuses outright (the connection is lost) also keeps it.
 h.view.session.control=netplay::Health::Lost;h.Frame(0,2);
 Check(!row("compose").enabled&&row("compose").value=="Try again","A lost connection offered Send or took the draft");
 h.view.session.control=netplay::Health::Healthy;h.Frame(0,2);
 // The draft is the player's own for the room they are in: another room starts empty.
 JoinChatRoom(h,21);h.Frame(0,3);
 Check(row("compose").value.empty()&&h.shell.Navigation().Screen()=="room-chat","A different room kept the draft");
 // Long history scrolls with the pad or the keys, follows new messages only from the bottom, and starts at the newest.
 sequence=0;JoinChatRoom(h,22);
 for(unsigned i=0;i<60;++i)say(i%2?2:3,"Message number "+std::to_string(i)+" with enough words to be worth reading in full.");
 h.Screen("room");h.Frame(0,2);h.Screen("room-chat");h.Frame(0,4);
 auto* log=ActiveWindow("Chat transcript");
 Check(log&&log->ScrollMax.y>0&&std::fabs(log->Scroll.y-log->ScrollMax.y)<1.5f,"Chat did not open at its newest message");
 const float bottom=log->Scroll.y;
 h.Frame(MenuInput::Up,20);h.Frame(0,2);
 Check(log->Scroll.y<bottom-20,"Holding Up did not scroll the chat back");
 const float reading=log->Scroll.y;
 say(2,"Arrives while reading");h.Frame(0,3);
 Check(std::fabs(log->Scroll.y-reading)<1.5f,"A new message pulled the reader away from earlier ones");
 h.Frame(MenuInput::Down,200);h.Frame(0,2);
 Check(std::fabs(log->Scroll.y-log->ScrollMax.y)<1.5f,"Holding Down did not return to the newest message");
 say(3,"Arrives at the bottom");h.Frame(0,3);
 Check(std::fabs(log->Scroll.y-log->ScrollMax.y)<1.5f,"The chat stopped following new messages at the bottom");
 key(ImGuiKey_PageUp);
 Check(log->Scroll.y<log->ScrollMax.y-50,"Page Up did not scroll the chat back");
 // The room's own epoch change empties the transcript.
 JoinChatRoom(h,23);h.Frame(0,2);
 Check(h.shell.Transcript().Lines().empty(),"A different room kept the old chat");
 // With Ember hidden only Background runs, and it keeps the chat as a drawn frame does.
 const auto hidden=[&](int count,float seconds){
  for(int i=0;i<count;++i){
   io.DeltaTime=seconds;ImGui::NewFrame();
   h.shell.Background(h.view,h.view.room,[&](ShellAction a){h.actions.push_back(a);return h.accept;});ImGui::Render();
  }
 };
 // The room drops a member's messages when they go, as the real one does.
 const auto leave=[&](room::MemberId id){
  auto& r=h.view.room;
  r.members.erase(std::remove_if(r.members.begin(),r.members.end(),[&](const room::Member& m){return m.id==id;}),r.members.end());
  r.chat.erase(std::remove_if(r.chat.begin(),r.chat.end(),[&](const room::ChatMessage& m){return m.sender==id;}),r.chat.end());
 };
 // Hidden on the Chat screen: a member who speaks and goes, a visit and two games all land, and none of it is read.
 sequence=0;JoinChatRoom(h,24);h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;
 h.Screen("room-chat");h.Frame(0,3);
 hidden(1,1.f/60);say(3,"Before I go");hidden(1,1.f/60);leave(3);hidden(1,1.f/60);
 room::Member fourth;fourth.id=4;fourth.name="Fourth";h.view.room.members.push_back(fourth);hidden(1,1.f/60);leave(4);hidden(1,1.f/60);
 h.view.room.tables[0].score[1]=1;hidden(1,1.f/60);h.view.room.tables[0].score[0]=1;hidden(1,1.f/60);
 {
  using ui::ChatLine;
  const auto& lines=h.shell.Transcript().Lines();
  Check(lines.size()==6&&lines[0].kind==ChatLine::Kind::Message&&lines[0].text=="Before I go"&&lines[0].name=="Third"&&
   lines[1].kind==ChatLine::Kind::Left&&lines[1].name=="Third","A message sent and taken back by leaving while Ember was hidden was lost");
  Check(lines[2].kind==ChatLine::Kind::Joined&&lines[2].name=="Fourth"&&lines[3].kind==ChatLine::Kind::Left&&lines[3].name=="Fourth",
   "A member who came and went while Ember was hidden was not announced");
  Check(lines[4].kind==ChatLine::Kind::GameWon&&lines[4].name=="Peer"&&lines[5].kind==ChatLine::Kind::GameWon&&lines[5].name=="Local"&&
   lines[5].score[0]==1&&lines[5].score[1]==1,"Games won while Ember was hidden were not announced");
 }
 Check(h.shell.Transcript().Unread({})==1,"Hiding Ember on Chat read the messages that arrived while hidden");
 h.Screen("room");h.Frame();
 Check(row("room-chat").value=="1"&&h.shell.Transcript().Lines().size()==6,"Reopening Ember lost what arrived while hidden, or read it on the board");
 h.Screen("room-chat");h.Frame(0,2);
 Check(h.shell.Transcript().Unread({})==0,"Opening Chat after Ember was hidden did not read it");
 // A message sent just before Ember is hidden takes the draft when it arrives, even past its 8 seconds.
 io.AddInputCharactersUTF8("Sent then hidden");h.Frame(0,2);
 sent=h.actions.size();key(ImGuiKey_Enter);
 Check(h.actions.size()==sent+1,"The message before hiding was not sent");
 hidden(1,9);say(1,"Sent then hidden");hidden(1,1.f/60);h.Frame(0,2);
 Check(row("compose").value.empty()&&h.actions.size()==sent+1,"A message that arrived while Ember was hidden stayed in the draft");
 // Past its 8 seconds the same text may be sent again, and either arrival takes the draft.
 io.AddInputCharactersUTF8("Slow one");h.Frame(0,2);
 sent=h.actions.size();key(ImGuiKey_Enter);
 h.Wait(9);h.Frame();
 Check(row("compose").value=="Slow one"&&row("compose").enabled,"A message past its 8 seconds could not be sent again");
 key(ImGuiKey_Enter);key(ImGuiKey_Enter);
 Check(h.actions.size()==sent+2,"A message past its 8 seconds was not sent again once, or was sent twice");
 say(1,"Slow one");h.Frame(0,2);
 Check(row("compose").value.empty(),"A message that arrived after its 8 seconds stayed in the draft");
 // A late arrival leaves a draft the player has since changed.
 io.AddInputCharactersUTF8("Edited later");h.Frame(0,2);
 key(ImGuiKey_Enter);h.Wait(9);h.Frame();
 io.AddInputCharactersUTF8(" more");h.Frame(0,2);
 say(1,"Edited later");h.Frame(0,2);
 Check(row("compose").value=="Edited later more","A late arrival took a draft the player had changed");
 SetMenuEntriesProbe({});
}
