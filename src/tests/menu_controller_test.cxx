#include "shell_journey_support.hxx"
#include "../ui/RoomBoardNavigation.hxx"
int main() try {
 using namespace sf4e;
 for(std::size_t columns:{0u,1u,2u,3u}){
  std::vector<MenuEntry> rows;
  for(int i=0;i<4;++i)rows.push_back(Row("table-"+std::to_string(i),"Table",""));
  rows.push_back(Row("members-part","Members",""));
  for(int i=0;i<7;++i)rows.push_back(Row("member-"+std::to_string(i),"Member",""));
  rows.push_back(Row("chat-part","Chat",""));
  for(int i=0;i<13;++i)rows.push_back(Row("action-"+std::to_string(i),"Action",""));
  rows.push_back(Row("inline-chat","Message",""));
  LinkRoomBoard(rows,columns);
  for(const auto& target:rows){
   MenuNavigation nav;nav.Reconcile(rows);
   Check(WalkMenuNavigation(nav,[&](unsigned direction){nav.Update({},rows);nav.Update({direction,1},rows);},[&]{return nav.Focus()==target.id;}),"A board item is unreachable");
  }
 }
 for(bool modern:{false,true})for(bool arcade:{false,true}){
  Harness h;FighterSelector selector;selection::Pick pick;h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.preferences.lookV2=modern;h.Frame();JoinedRoom(h,12);
  h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=2;
  room::Member local;local.id=1;local.name="You";local.table=2;local.seat=0;
  room::Member peer;peer.id=2;peer.name="Host";peer.table=2;peer.seat=1;
  h.view.room.members={local,peer};auto& table=h.view.room.tables[2];table.p1=1;table.p2=2;table.phase=room::TablePhase::Waiting;
  h.view.room.arcade=arcade?room::ArcadeRoom:room::ClassicRoom;if(arcade){h.view.room.cabinets[2]=1;h.view.room.cabinets[10]=2;}
  h.view.canEditSelection=h.view.canReady=true;
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const auto& entries){rows=entries;});
  const char* card=arcade?"cabinet-2":"table-2";
  const auto choose=[&](const char* id){
   h.Choose(card);
   const auto entry=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==card;});
   Check(entry!=rows.end(),"Missing own card");
   const auto choice=std::find_if(entry->choices.begin(),entry->choices.end(),[&](const MenuChoice& c){return c.id==id;});
   Check(choice!=entry->choices.end(),"Missing own-card choice");
   const auto count=choice-entry->choices.begin();
   for(int i=0;i<count;++i)h.Press(MenuInput::Right);
   h.Press(MenuInput::Select);
  };
  h.Screen("room");choose("ready");Check(h.actions.back().command.kind==Kind::Ready,"Ready choice failed");
  choose("selection");Check(h.shell.Navigation().Screen()=="selection","Fighter choice failed");h.Press(MenuInput::Back);
  choose("options");Check(h.shell.Navigation().Screen()=="room-table","More choice failed");h.Press(MenuInput::Back);
  auto before=h.actions.size();h.FocusOn(card);h.Press(MenuInput::Back);
  Check(h.shell.Navigation().Screen()=="home"&&h.actions.size()==before,"Back changed seat");h.Screen("room");
  choose("leave-place");Check(h.actions.size()==before+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue,"Leave choice failed");
  table.score[0]=1;before=h.actions.size();choose("leave-place");Check(h.shell.Navigation().Choosing(),"Score departure did not ask");
  h.Press(MenuInput::Back);Check(h.actions.size()==before,"Cancel sent departure");table.score[0]=0;
  h.Choose(card);h.view.room.chat.push_back({1,2,"Unread behind choices"});h.Frame(0,3);
  Check(h.shell.UnreadChat()==1,"Choices marked chat read");h.Press(MenuInput::Back);
  h.Screen("interface");h.Press(MenuInput::Options);h.Press(MenuInput::Options);
  Check(h.shell.Navigation().Screen()=="interface","Shortcut lost opener");
  SetMenuEntriesProbe({});
 }
 std::cout<<"Controller choices, Back, shortcuts and board paths passed.\n";return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
