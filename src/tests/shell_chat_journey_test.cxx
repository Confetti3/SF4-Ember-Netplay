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
 h.view.room=room::Snapshot{};
 JoinedRoom(h,epoch);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;
 h.view.room.host=1;h.view.room.name="Chat room";h.view.room.revision=3;
 for(auto& table:h.view.room.tables)table.revision=7;
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
 std::vector<ui::RoomNotice> n;
 Check(t.Update(s,n)&&t.Lines().size()==1&&t.Unread({})==0,"Joining a room did not take its history in as read, with no events");
 Check(!t.Update(s,n)&&t.Lines().size()==1,"The same snapshot again changed the transcript");
 // A snapshot that repeats old messages adds none; a new one is unread unless it is ours or muted.
 s.chat.push_back({2,2,"Hello"});s.chat.push_back({3,1,"Hi Peer"});
 t.Update(s,n);t.Update(s,n);
 Check(t.Lines().size()==3&&t.Lines()[1].text=="Hello"&&t.Lines()[1].name=="Peer"&&!t.Lines()[1].own&&t.Lines()[2].own,"Messages were not added once, by sequence");
 Check(t.Unread({})==1&&t.Unread({2})==0,"Unread counted our own message, or a muted member's");
 t.MarkRead();Check(t.Unread({})==0,"Reading did not clear the unread count");
 // Who comes and goes, and who hosts.
 room::Member third;third.id=3;third.name="Third";s.members.push_back(third);
 t.Update(s,n);
 Check(t.Lines().back().kind==ChatLine::Kind::Joined&&t.Lines().back().name=="Third","A new member was not announced");
 s.host=2;t.Update(s,n);
 Check(t.Lines().back().kind==ChatLine::Kind::NewHost&&t.Lines().back().name=="Peer","A new host was not announced");
 // A member's words stay after they go, under the name they had; the room itself drops them.
 s.chat.push_back({4,3,"Bye now"});t.Update(s,n);
 s.members.pop_back();s.chat.pop_back();
 t.Update(s,n);
 const auto& lines=t.Lines();
 Check(lines.back().kind==ChatLine::Kind::Left&&lines.back().name=="Third"&&lines[lines.size()-2].text=="Bye now"&&lines[lines.size()-2].name=="Third",
  "A member who left lost their messages or their name");
 // Results: a game won is one seat's score going up with the pair unchanged.
 s.tables[0].p1=1;s.tables[0].p2=2;t.Update(s,n);
 const auto count=t.Lines().size();
 s.tables[0].score[1]=1;t.Update(s,n);
 Check(t.Lines().size()==count+1&&t.Lines().back().kind==ChatLine::Kind::GameWon&&t.Lines().back().name=="Peer"&&t.Lines().back().table==0&&
  t.Lines().back().score[0]==1&&t.Lines().back().score[1]==0,"A game won was not announced with its winner first");
 s.tables[0].score[0]=s.tables[0].score[1]=0;t.Update(s,n);
 Check(t.Lines().size()==count+1,"A score reset was announced");
 // A finished set names its winner and its score, once, and not as a game as well.
 s.tables[0].lastSet.generation=7;s.tables[0].lastSet.p1=1;s.tables[0].lastSet.p2=2;s.tables[0].lastSet.winnerSeat=0;
 s.tables[0].lastSet.score[0]=3;s.tables[0].lastSet.score[1]=1;t.Update(s,n);t.Update(s,n);
 Check(t.Lines().size()==count+2&&t.Lines().back().kind==ChatLine::Kind::SetWon&&t.Lines().back().name=="Local"&&
  t.Lines().back().score[0]==3&&t.Lines().back().score[1]==1,"A set won was not announced once, with its winner and score");
 // A draw changes no score, so says nothing.
 const auto quiet=t.Lines().size();s.tables[0].phase=room::TablePhase::Playing;t.Update(s,n);s.tables[0].phase=room::TablePhase::Waiting;t.Update(s,n);
 Check(t.Lines().size()==quiet,"A game with no winner was announced");
 // A Ready timeout arrives as a room notice. It names the member and cause
 // once, even after the queue has filled the seat; an ordinary unseat says nothing.
 s.tables[0].p2=0;t.Update(s,n);
 Check(t.Lines().size()==quiet,"An ordinary unseat was announced as a timeout");
 const auto timeout=[](room::MemberId member,std::uint8_t table){return room::Event{room::Event::Kind::ReadyTimeout,table,0,member,room::MatchResult::Abort};};
 n.push_back({1,timeout(2,0)});
 t.Update(s,n);t.Update(s,n);
 Check(t.Lines().size()==quiet+1&&t.Lines().back().kind==ChatLine::Kind::ReadyTimeout&&
  t.Lines().back().name=="Peer"&&t.Lines().back().table==0,"A Ready timeout was not announced once under the fighter's name");
 // Two timeouts, of the same fighter, between two frames: neither is lost.
 n.push_back({2,timeout(2,0)});n.push_back({3,timeout(2,1)});t.Update(s,n);
 Check(t.Lines().size()==quiet+3&&t.Lines()[t.Lines().size()-2].table==0&&t.Lines().back().table==1,"A second timeout of the same fighter was lost");
 ui::ChatTranscript arrived;arrived.Update(s,n);
 Check(arrived.Lines().size()==s.chat.size(),"A new arrival announced an old Ready timeout");
 // Only the newest lines are kept.
 for(unsigned i=0;i<300;++i){
  s.chat.push_back({5+i,2,"Line "+std::to_string(i)});
  if(s.chat.size()>room::MaximumChatMessages)s.chat.erase(s.chat.begin());
  t.Update(s,n);
 }
 Check(t.Lines().size()==ui::ChatTranscript::MaximumLines&&t.Lines().back().text=="Line 299","The transcript is not capped at its newest lines");
 // A different room starts again from its own history.
 s.roomEpoch=6;s.chat.clear();s.chat.push_back({1,2,"New room"});
 Check(t.Update(s,n)&&t.Lines().size()==1&&t.Lines()[0].text=="New room"&&t.Unread({})==0,"A different room kept the old transcript or counted its history as unread");
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
 // A connection that is lost leaves the box and the draft alone. Enter holds the message and it goes out once, when the room is back.
 h.Wait(9);h.Frame();
 h.view.session.control=netplay::Health::Lost;h.Frame(0,2);
 Check(row("compose").enabled&&row("compose").value=="Try again"&&io.WantTextInput,"A lost connection took the message box or the draft");
 sent=h.actions.size();key(ImGuiKey_Enter);
 Check(h.actions.size()==sent&&!row("compose").enabled,"A message was sent into a lost connection, or Enter did not hold it");
 h.view.session.control=netplay::Health::Healthy;h.Frame(0,5);
 Check(h.actions.size()==sent+1&&h.actions.back().roomAction.text=="Try again","A held message did not go out when the connection came back");
 h.Frame(0,5);Check(h.actions.size()==sent+1&&row("compose").value=="Try again","A held message went out twice, or its draft left before the room had it");
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
 // Hidden on the Chat screen: a member who speaks and goes, a visit, two games and a Ready timeout all land, and none of it is read.
 sequence=0;JoinChatRoom(h,24);h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;
 h.Screen("room-chat");h.Frame(0,3);
 hidden(1,1.f/60);say(3,"Before I go");hidden(1,1.f/60);leave(3);hidden(1,1.f/60);
 room::Member fourth;fourth.id=4;fourth.name="Fourth";h.view.room.members.push_back(fourth);hidden(1,1.f/60);leave(4);hidden(1,1.f/60);
 h.view.room.tables[0].score[1]=1;hidden(1,1.f/60);h.view.room.tables[0].score[0]=1;hidden(1,1.f/60);
 h.view.roomNotices.push_back({1,room::Event{room::Event::Kind::ReadyTimeout,1,0,2,room::MatchResult::Abort}});hidden(1,1.f/60);
 {
  using ui::ChatLine;
  const auto& lines=h.shell.Transcript().Lines();
  Check(lines.size()==7&&lines[0].kind==ChatLine::Kind::Message&&lines[0].text=="Before I go"&&lines[0].name=="Third"&&
   lines[1].kind==ChatLine::Kind::Left&&lines[1].name=="Third","A message sent and taken back by leaving while Ember was hidden was lost");
  Check(lines[2].kind==ChatLine::Kind::Joined&&lines[2].name=="Fourth"&&lines[3].kind==ChatLine::Kind::Left&&lines[3].name=="Fourth",
   "A member who came and went while Ember was hidden was not announced");
  Check(lines[4].kind==ChatLine::Kind::GameWon&&lines[4].name=="Peer"&&lines[5].kind==ChatLine::Kind::GameWon&&lines[5].name=="Local"&&
   lines[5].score[0]==1&&lines[5].score[1]==1,"Games won while Ember was hidden were not announced");
  Check(lines[6].kind==ChatLine::Kind::ReadyTimeout&&lines[6].name=="Peer"&&lines[6].table==1,
   "A Ready timeout while Ember was hidden was not announced");
 }
 Check(h.shell.Transcript().Unread({})==1,"Hiding Ember on Chat read the messages that arrived while hidden");
 h.Screen("room");h.Frame();
 Check(row("room-chat").value=="1"&&h.shell.Transcript().Lines().size()==7,"Reopening Ember lost what arrived while hidden, or read it on the board");
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
 // A room fence that opens and closes every frame (a room updating over a slow link) leaves the box, the screen and the keys alone.
 sequence=0;JoinChatRoom(h,30);h.view.session.coordinated=true;h.view.session.authorityWritable=true;
 h.Screen("room");h.Frame();h.Frame(MenuInput::Chat);
 const auto flip=[&]{h.view.session.authorityWritable=!h.view.session.authorityWritable;};
 // What each frame offered: the keys that type letters are never advertised, and Send follows the draft alone.
 std::set<std::string> offered;
 SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))offered.insert(id+7);});
 const auto flicker=[&](const std::string& text){
  for(const char c:text){
   flip();
   const ImGuiKey letter=c=='f'?ImGuiKey_F:c=='c'?ImGuiKey_C:c=='t'?ImGuiKey_T:c==' '?ImGuiKey_Space:ImGuiKey_X;
   io.AddKeyEvent(letter,true);io.AddInputCharactersUTF8(std::string(1,c).c_str());h.Frame();io.AddKeyEvent(letter,false);
   Check(h.shell.Navigation().Screen()=="room-chat"&&io.WantTextInput,"The fence flipping took the message box or the screen");
   Check(row("compose").value.empty()||row("compose").enabled,"The fence flipping switched Send off while there was a draft");
  }
 };
 // The very first frame of the screen has a key down too: a letter pressed as Chat opens is text, not a shortcut.
 io.AddKeyEvent(ImGuiKey_C,true);io.AddInputCharactersUTF8("c");h.Frame();io.AddKeyEvent(ImGuiKey_C,false);
 Check(h.shell.Navigation().Screen()=="room-chat","A letter pressed as Chat opened acted as a menu key");
 flip();h.Frame(0,3);
 flicker("fct xtc f");
 h.Frame(0,2);
 SetMenuTextProbe({});
 Check(!offered.count(loc::T("room.legend_fighter"))&&offered.count(loc::T("common.back")),"The fence flipping changed the keys the chat legend offers");
 const std::string drafted=row("compose").value;
 Check(drafted.size()>=9&&drafted.compare(drafted.size()-9,9,"fct xtc f")==0,"Characters were lost while the fence flipped under the message box");
 // With the box not yet active, the keys are still text on this screen.
 io.AddKeyEvent(ImGuiKey_C,true);h.Frame();io.WantTextInput=false;
 Check((KeyboardMenuBits()&MenuInput::Chat)&&!(KeyboardMenuBits(true)&MenuInput::Chat),"A chat screen's letters still count as menu keys");
 io.AddKeyEvent(ImGuiKey_C,false);h.Frame();
 // Enter while the room is catching up holds the message; it goes out once, when the room can take it.
 h.view.session.authorityWritable=false;h.Frame(0,2);
 sent=h.actions.size();key(ImGuiKey_Enter);key(ImGuiKey_Enter);
 Check(h.actions.size()==sent&&!row("compose").enabled&&row("compose").value==drafted,"A message was sent while the room was fenced, or was not held");
 flip();h.Frame(0,3);
 Check(h.actions.size()==sent+1&&h.actions.back().roomAction.kind==room::ActionKind::Chat&&h.actions.back().roomAction.text==drafted,
  "A held message did not go out when the room could take it");
 for(int i=0;i<6;++i){flip();h.Frame();}
 Check(h.actions.size()==sent+1,"A held message went out again as the fence flipped");
 say(1,drafted);h.Frame(0,2);
 Check(row("compose").value.empty(),"The draft stayed after the room's chat had the held message");
 // A message held when the room closes is let go, and the draft stays for the player to decide.
 h.view.session.authorityWritable=false;h.Frame(0,2);
 io.AddInputCharactersUTF8("Too late");h.Frame(0,2);key(ImGuiKey_Enter);
 h.view.room.closed=true;h.Frame(0,2);
 Check(row("compose").value=="Too late","A closed room took the draft");
 sent=h.actions.size();h.view.room.closed=false;h.view.session.authorityWritable=true;h.Frame(0,4);
 Check(h.actions.size()==sent&&row("compose").enabled,"A message held when the room closed was sent later");
 // Only one message is held at a time: editing the draft and pressing Enter again changes nothing about what goes out.
 h.view.session.authorityWritable=false;h.Frame(0,2);
 sent=h.actions.size();key(ImGuiKey_Enter);h.Frame(0,2);
 io.AddInputCharactersUTF8("!");h.Frame(0,2);key(ImGuiKey_Enter);
 Check(h.actions.size()==sent,"A second Enter while a message was held was sent");
 Check(row("compose").value=="Too late!","The draft was not left to edit while a message was held");
 Check(!row("compose").enabled,"Send stayed offered while a message was held");
 h.view.session.authorityWritable=true;h.Frame(0,4);
 Check(h.actions.size()==sent+1&&h.actions.back().roomAction.text=="Too late","The held message was replaced by the edited draft, or not sent");
 say(1,"Too late");h.Frame(0,2);
 Check(row("compose").value=="Too late!"&&row("compose").enabled,"The held message's arrival took the edited draft, or the edited draft could not be sent");
 key(ImGuiKey_Enter);
 Check(h.actions.size()==sent+2&&h.actions.back().roomAction.text=="Too late!","The edited draft was not sent after the held message");
 say(1,"Too late!");h.Frame(0,2);
 // A retry held while the original is still on its way does not repeat it when the original turns up.
 io.AddInputCharactersUTF8("Retry me");h.Frame(0,2);
 key(ImGuiKey_Enter);h.Wait(9);h.Frame();
 sent=h.actions.size();
 h.view.session.authorityWritable=false;h.Frame(0,2);
 key(ImGuiKey_Enter);
 Check(h.actions.size()==sent&&row("compose").value=="Retry me","A retry was sent into a fenced room, or its draft was lost");
 say(1,"Retry me");h.Frame(0,2);
 Check(row("compose").value.empty(),"The original's arrival did not take the draft while a retry waited");
 h.view.session.authorityWritable=true;h.Frame(0,4);
 Check(h.actions.size()==sent,"A held retry was sent after the original had arrived");
 // Hiding Ember does not cancel a held message: it goes out once, unedited, when the room is back and Ember is shown.
 h.view.session.authorityWritable=false;h.Frame(0,2);
 io.AddInputCharactersUTF8("Away A");h.Frame(0,2);
 key(ImGuiKey_Enter);h.Frame(0,2);
 io.AddInputCharactersUTF8(" B");h.Frame(0,2);
 Check(row("compose").value=="Away A B"&&!row("compose").enabled,"The away fixture did not hold A with B in the draft");
 sent=h.actions.size();
 hidden(3,1.f/60);h.view.session.authorityWritable=true;hidden(3,1.f/60);
 Check(h.actions.size()==sent,"A held message was sent from a frame that could not see the room");
 h.Frame(0,3);
 Check(h.actions.size()==sent+1&&h.actions.back().roomAction.text=="Away A","A message held while Ember was hidden was lost, or the edited draft went instead");
 h.Frame(0,5);Check(h.actions.size()==sent+1&&row("compose").value=="Away A B","A message held while Ember was hidden went out twice, or touched the draft");
 say(1,"Away A");h.Frame(0,2);
 // A submission the runtime refuses leaves the message held; it is tried again later and goes out once.
 h.view.session.authorityWritable=false;h.Frame(0,2);
 key(ImGuiKey_Enter);h.Frame(0,2);
 io.AddInputCharactersUTF8("!");h.Frame(0,2);
 Check(row("compose").value=="Away A B!"&&!row("compose").enabled,"The refusal fixture did not hold the message with an edited draft");
 sent=h.actions.size();h.accept=false;h.view.session.authorityWritable=true;h.Frame();
 Check(h.actions.size()==sent+1&&h.actions.back().roomAction.text=="Away A B"&&!row("compose").enabled,"A refused submission dropped the held message");
 h.Frame(0,5);
 Check(h.actions.size()==sent+1,"A refused submission was tried again at once");
 // It stays held through more refusals than any limit would allow, each retry a little later than the last.
 for(int refusal=2;refusal<=7;++refusal){
  h.Wait(5.5f);h.Frame();
  Check(h.actions.size()==sent+refusal&&h.actions.back().roomAction.text=="Away A B"&&!row("compose").enabled&&row("compose").value=="Away A B!",
   "A message was given up after repeated refusals, or the edited draft went instead");
 }
 h.accept=true;h.Wait(5.5f);h.Frame(0,2);
 Check(h.actions.size()==sent+8&&h.actions.back().roomAction.text=="Away A B","A refused submission was not sent once the runtime took submissions again");
 h.Frame(0,5);Check(h.actions.size()==sent+8&&row("compose").value=="Away A B!","The retried message went out twice, or touched the draft");
 say(1,"Away A B");h.Frame(0,2);
 // After a long fence the hint holds through short writable gaps, as the status line does.
 std::vector<std::string> notes;
 SetMenuTextProbe([&](const char* id,float,float,float width,float available){
  if(std::strcmp(id,"chat-note"))return;
  const auto widthOf=[&](const char* text){return ImGui::CalcTextSize(text,nullptr,false,available).x;};
  notes.push_back(width==widthOf(loc::T("room.wait.updating"))?"updating":width==widthOf(loc::T("chat.sending"))?"sending":
   width==widthOf(loc::T("chat.detail"))?"detail":"other");
 });
 h.Wait(1);notes.clear();h.Frame(0,2);
 Check(!notes.empty()&&notes.back()=="detail","The chat hint was not its plain help in a writable room");
 h.view.session.authorityWritable=false;
 for(int i=0;i<4;++i)h.Wait(.1f);
 Check(notes.back()=="updating","A sustained fence did not show the room updating");
 notes.clear();
 for(int i=0;i<20;++i){flip();h.Frame();}
 Check(!notes.empty()&&std::all_of(notes.begin(),notes.end(),[](const std::string& n){return n=="updating";}),"The chat hint flickered through short writable gaps");
 h.view.session.authorityWritable=true;h.Wait(1);h.Frame(0,2);
 Check(notes.back()=="detail","The chat hint stayed on the update after the room settled");
 SetMenuTextProbe({});
 // The log says what changed, at most once a second, with how many changes each line stands for.
 std::vector<std::string> logged;h.shell.SetChatLog([&](const std::string& line){logged.push_back(line);});
 h.Wait(2);logged.clear();
 for(int i=0;i<120;++i){flip();h.Frame();}
 h.view.session.authorityWritable=true;h.Wait(1.5f);
 Check(!logged.empty()&&logged.size()<=4,"Chat availability changes were not logged at a limited rate");
 const auto counted=[&](const std::string& line){const auto at=line.find("last line: ");return at==std::string::npos?0:std::stoi(line.substr(at+11));};
 Check(std::any_of(logged.begin(),logged.end(),[&](const std::string& l){return counted(l)>=10;})&&logged.back().find("sending is writable")!=std::string::npos,
  "The log lines did not count the changes they stand for, or the last one was not the state the chat settled in");
 // A burst that ends where it began is still reported.
 // (Writable is the last line written, and the burst is within the second after it.)
 h.Wait(2);flip();h.Frame();flip();h.Wait(1.1f);logged.clear();
 flip();h.Frame();flip();h.Frame();h.Wait(1.5f);
 Check(logged.size()==1&&counted(logged.back())==2&&logged.back().find("sending is writable")!=std::string::npos,
  "A burst that settled back where it was logged was never reported");
 h.shell.SetChatLog({});
 h.view.session.coordinated=false;
 SetMenuEntriesProbe({});
}
