// The public room journeys for ordering, Quick join and the filter: the list's order by area and seats, the
// room Quick join asks for (near the player, or in the areas ticked), and the filters page with its areas
// page. Run from the public room journey test's main.
#include "public_rooms_journey_support.hxx"
#include "../ui/NameMarks.hxx"
#include <initializer_list>
namespace {
// A room whose creator is in `area`, named by its ID, with its seats, when it opened and whether it is locked.
netplay::publicrooms::Room Placed(const char* id,Area area,unsigned members,unsigned capacity,std::uint64_t createdAt=0,bool locked=false){
 auto room=MakeRoom(id,id,members,capacity);room.area=area;room.createdAt=createdAt;room.locked=locked;return room;
}
// The areas the journeys place rooms in: the player's own, one next to it, and a far one.
constexpr Area OwnArea=Area::NaEast,NextArea=Area::NaCentral,FarArea=Area::EuropeWest;
// The areas page, opened from the filters page's Area row: each of `codes` is switched with Select, the
// saved change handed back as the runtime does, and Back twice returns to the list.
void TickAreas(Journey& k,std::initializer_list<const char*> codes){
 auto& h=k.h;
 h.Choose("pr-filters");h.Choose("pr-filter-area");
 Check(h.shell.Navigation().Screen()==PublicRoomsPanel::AreasScreen,"The Area row did not open the areas page");
 for(const char* code:codes){h.FocusOn((std::string("pr-area:")+code).c_str());h.Press(MenuInput::Select);}
 h.Wait(1);
 for(std::size_t i=h.actions.size();i-->0;)if(const auto* save=As<commands::SavePreferences>(h.actions[i])){h.view.preferences=save->preferences;break;}
 h.Frame(0,2);h.Press(MenuInput::Back);h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="public-rooms","Back from the areas page did not return to the list");
}
// The rooms on screen, in the order shown, as their IDs run together.
std::string Shown(const Journey& j){
 std::string out;for(const auto& r:j.rows)if(r.id.compare(0,8,"pr-room:")==0)out+=r.id.substr(8);return out;
}

// Rooms that can be joined come first, those in the player's area and then those
// next to it before far ones once the player's area is set, then the most free
// seats, the most players and the oldest; locked and full rooms are last and
// cannot be pressed. Without an area of the player's own nothing is near, and
// the relay this PC connects through says nothing about it.
void SortJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 j.List({Placed("x",FarArea,1,8,5),Placed("f",OwnArea,4,4,6),Placed("l",OwnArea,1,8,7,true),Placed("n",OwnArea,2,8,20),Placed("m",OwnArea,5,8,9),Placed("p",OwnArea,2,8,10)});
 Check(Shown(j)=="xpnmlf",(std::string("Rooms are not sorted by free seats when the player's area is not set: ")+Shown(j)).c_str());
 Check(j.row("pr-room:p")->enabled&&!j.row("pr-room:l")->enabled&&!j.row("pr-room:f")->enabled,"A locked or full room can be pressed");
 h.view.netReport.relay="use1";h.Frame(0,2);
 Check(Shown(j)=="xpnmlf","The relay was taken for the player's area");
 h.view.automaticArea=OwnArea;h.Frame(0,2);
 Check(Shown(j)=="pnmxlf",(std::string("Near rooms do not come before far ones: ")+Shown(j)).c_str());
 // An area the player hid in Settings is hidden from others only: their own list still sorts by it.
 h.view.preferences.area=AreaPreference::Hidden();h.Frame(0,2);
 Check(Shown(j)=="pnmxlf",(std::string("An area hidden in Settings stopped the own list sorting by it: ")+Shown(j)).c_str());
 h.view.preferences.area=AreaPreference::Automatic();h.Frame(0,2);
 // The order does not wobble from one refresh to the next.
 j.List({Placed("p",OwnArea,2,8,10),Placed("n",OwnArea,2,8,20),Placed("x",FarArea,1,8,5),Placed("m",OwnArea,5,8,9),Placed("f",OwnArea,4,4,6),Placed("l",OwnArea,1,8,7,true)});
 Check(Shown(j)=="pnmxlf","The same rooms in another bridge order were sorted differently");
 // The order is kept between frames for one refresh, but what the list holds decides it: rooms
 // changed under the same refresh count are sorted again, and a new count with the same rooms
 // keeps the order.
 for(auto& room:h.view.publicRooms.rooms)if(room.id=="n")room.members=7;
 h.Frame(0,2);
 Check(Shown(j)=="pmnxlf",(std::string("A room that filled up under the same refresh kept its place: ")+Shown(j)).c_str());
 ++h.view.publicRooms.listed;h.Frame(0,2);
 Check(Shown(j)=="pmnxlf","A new refresh count with the same rooms changed the order");
 // The player's own area comes before its neighbours, which come before the rest and rooms without an area.
 j.List({Placed("u",FarArea,1,8,3),Placed("t",NextArea,1,8,2),Placed("w",Area::None,1,8,4),Placed("s",OwnArea,6,8,1)});
 Check(Shown(j)=="stuw",(std::string("The player's area, then its neighbours, do not come first: ")+Shown(j)).c_str());
}

// Quick join asks for the first room that can be joined, near the player's area when
// it is set (not a far room with more seats) and then any other room that can be joined, skips the rooms that refused this
// visit, and with none left offers to create one.
void QuickJoinJourney(){
 {
 Journey j;auto& h=j.h;j.OpenList();
 h.view.automaticArea=OwnArea;
 j.List({Placed("x",FarArea,1,8,5),Placed("f",OwnArea,4,4,6),Placed("l",OwnArea,1,8,7,true),Placed("n",OwnArea,2,8,20),Placed("m",OwnArea,5,8,9)});
 Check(j.row("pr-quick")&&j.row("pr-quick")->enabled,"Quick join is not offered on a list");
 h.Choose("pr-quick");
 Check(tickets(j)==1&&lastTicketIs(j,"brg_1","n"),"Quick join did not ask for the near room with the most free seats");
 Check(h.shell.Navigation().Focus()=="pr-room:n"&&j.row("pr-room:n")->value==loc::T("public.joining_value"),"Quick join did not show the room it picked");
 // A refusal that says the room is not available is remembered: the next pick is another room.
 j.Admit("room_full");
 h.Choose("pr-quick");
 Check(tickets(j)==2&&lastTicketIs(j,"brg_1","m"),"Quick join asked again for a room that was full");
 j.Admit("room_locked");
 // Nothing near has a seat left (the others are locked or full): the far room that has one is the fallback.
 h.Choose("pr-quick");
 Check(tickets(j)==3&&lastTicketIs(j,"brg_1","x"),"Quick join did not fall back to the far room when no near room was left");
 j.Admit("room_full");
 // Every room that can be joined has refused.
 h.Choose("pr-quick");
 Check(tickets(j)==3,"Quick join asked for a room none of which qualified");
 Check(j.noticeOpen(true),"Quick join with no room did not offer to create one");
 h.Press(MenuInput::Select);h.Frame(0,3);
 Check(h.shell.Navigation().Screen()=="public-rooms"&&!j.noticeOpen(),"OK on the notice left Public rooms or kept the notice");
 // Leaving the screen ends the visit: the refused room is a candidate again.
 h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);
 h.Choose("pr-quick");
 Check(tickets(j)==4&&lastTicketIs(j,"brg_1","n"),"A new visit did not forget the rooms that refused");
 j.Admit("banned");h.Choose("pr-quick");
 Check(lastTicketIs(j,"brg_1","m")&&tickets(j)==5,"A room that banned the player was asked for again");
 // With nothing to join the notice's other button opens Create on Public.
 j.Admit("room_full");h.Choose("pr-quick");
 j.Admit("room_full");h.Choose("pr-quick");
 Check(j.noticeOpen(true),"The notice with no room does not carry the Create button");
 h.Press(MenuInput::Right);h.Press(MenuInput::Select);h.Frame(0,4);
 Check(h.shell.Navigation().Screen()=="create"&&j.row("visibility")&&j.row("visibility")->value==loc::T("public.visibility.public"),"Create room on the notice did not open Create on Public");
 }
 {
  // Without an area of the player's own any room with a seat will do: the one with the most free seats.
  Journey k;k.OpenList();k.List({Placed("x",FarArea,1,8,5),Placed("n",OwnArea,2,8,20),Placed("f",OwnArea,4,4,6)});
  k.h.Choose("pr-quick");
  Check(tickets(k)==1&&lastTicketIs(k,"brg_1","x"),"Quick join without an area did not ask for the room with the most seats");
 }
 {
  // A neighbour's room is near too: taken before a far room with more seats.
  Journey k;k.OpenList();k.h.view.automaticArea=OwnArea;k.List({Placed("x",FarArea,1,8,5),Placed("t",NextArea,6,8,20)});
  k.h.Choose("pr-quick");
  Check(tickets(k)==1&&lastTicketIs(k,"brg_1","t"),"Quick join did not take the room next to the player's area");
 }
 {
  // The filter narrows Quick join: a room type, and an area the player chose in place of the near default.
  Journey k;auto& h=k.h;k.OpenList();h.view.automaticArea=OwnArea;
  auto arcade=Placed("a",OwnArea,2,8,1);arcade.type=room::RoomType::Arcade;
  auto classic=Placed("c",OwnArea,2,8,2);classic.type=room::RoomType::Classic;
  auto distant=Placed("f",FarArea,1,8,3);distant.type=room::RoomType::Classic;
  k.List({arcade,classic,distant});
  const auto settle=[&]{
   h.Wait(1);
   for(std::size_t i=h.actions.size();i-->0;)if(const auto* save=As<commands::SavePreferences>(h.actions[i])){h.view.preferences=save->preferences;break;}
   h.Frame(0,2);
  };
  // The filters page: steps a row, saves, and goes back to the list.
  const auto step=[&](const char* row,unsigned direction,int presses){
   h.Choose("pr-filters");h.FocusOn(row);for(int i=0;i<presses;++i)h.Press(direction);settle();h.Press(MenuInput::Back);
   Check(h.shell.Navigation().Screen()=="public-rooms","Back from the filters page did not return to the list");
  };
  step("pr-filter-type",MenuInput::Right,1);
  h.Choose("pr-quick");
  Check(tickets(k)==1&&lastTicketIs(k,"brg_1","c"),"Quick join asked for a room the type filter hides");
  k.Admit("room_full");
  // Europe West ticked on the areas page.
  TickAreas(k,{"euw"});
  Check(k.row("pr-filters")->value==std::string(loc::T("area.euw"))+" \xC2\xB7 "+RoomTypeName(room::RoomType::Classic),"The Filters row does not say Europe West and Classic");
  h.Choose("pr-quick");
  Check(tickets(k)==2&&lastTicketIs(k,"brg_1","f"),"Quick join stayed near the player's area after the filter named another");
  k.Admit("room_full");
  step("pr-filter-type",MenuInput::Right,1);
  h.Choose("pr-quick");
  Check(tickets(k)==2&&k.noticeOpen(true),"Quick join with no room under the filter did not offer to create one");
 }
 {
  // A service that lists no areas, or creators who hid theirs: no room is near, so Quick join takes a room with a seat
  // all the same, and Near me is not offered because it could tell no room apart.
  Journey k;k.OpenList();k.h.view.automaticArea=OwnArea;k.List({Placed("p",Area::None,6,8,1),Placed("q",Area::None,2,8,2)});
  Check(PublicRoomsPanel::HomeArea(k.h.view)==OwnArea&&PublicRoomsPanel::FilterHome(k.h.view)==Area::None&&!PublicRoomsPanel::AreasListed(k.h.view),"A list with no area names one for Near me");
  k.h.Choose("pr-quick");
  Check(tickets(k)==1&&lastTicketIs(k,"brg_1","q"),"Quick join found no room on a list where no room names an area");
  const auto nearMe=[]{netplay::publicrooms::RoomFilter f;f.scope=netplay::publicrooms::RoomFilter::Place::Near;return f;}();
  Check(PublicRoomsPanel::FilterRows(nearMe,false,{OwnArea,false,false,true})[0].detail==loc::T("public.filter_near_unlisted")&&PublicRoomsPanel::FilterRows(nearMe,false,{OwnArea,true,false,true})[0].detail==loc::T("public.filter_area_detail"),
   "Near me on a list without areas does not say why it lists every room");
 }
 {
  // Rooms are near no one without an area, so when none is near Quick join takes the one that is joinable.
  Journey k;k.OpenList();k.h.view.automaticArea=OwnArea;k.List({Placed("x",FarArea,8,8,1),Placed("y",Area::None,3,8,2)});
  Check(PublicRoomsPanel::FilterHome(k.h.view)==OwnArea,"A list where a room names an area does not offer Near me");
  k.h.Choose("pr-quick");
  Check(tickets(k)==1&&lastTicketIs(k,"brg_1","y"),"Quick join skipped a room with no area when none was near");
 }
 {
  // Several ticked areas: Quick join takes a room in any of them, never a near room outside them or one that names no area.
  Journey k;auto& h=k.h;k.OpenList();h.view.automaticArea=OwnArea;
  k.List({Placed("n",OwnArea,2,8,1),Placed("e",FarArea,1,8,2),Placed("a",Area::EastAsia,1,8,3),Placed("w",Area::None,1,8,4)});
  TickAreas(k,{"euw","eas"});
  h.Choose("pr-quick");
  const auto inTicked=[&]{return lastTicketIs(k,"brg_1","e")||lastTicketIs(k,"brg_1","a");};
  Check(tickets(k)==1&&inTicked(),"Quick join asked for a room outside the ticked areas");
  k.Admit("room_full");h.Choose("pr-quick");
  Check(tickets(k)==2&&inTicked(),"Quick join did not take the other ticked area's room");
  k.Admit("room_full");h.Choose("pr-quick");
  Check(tickets(k)==2&&k.noticeOpen(true),"Quick join took a room outside the ticked areas once theirs were refused");
 }
 // An empty list also offers to create.
 Journey e;e.OpenList();e.List({});
 e.h.Choose("pr-quick");Check(tickets(e)==0&&e.noticeOpen(true),"Quick join on an empty list did not offer to create a room");
}

// With classes listed (docs/design/QUICK_JOIN.md), Quick join takes the near room whose waiting player is near the
// player's class before one with more seats, says once when no room near their class had a seat, and asks for any
// class once the Filters page says so. Without a class of the player's own the Filters row says what it goes by.
void QuickJoinClassJourney(){
 Journey j;auto& h=j.h;j.OpenList();h.view.automaticArea=OwnArea;
 auto& standing=h.view.rankings.standing.value;
 standing.kind=netplay::rankings::Standing::Kind::Ranked;standing.classesShown=true;standing.badge=SkillBadge::OfClass(5,true);
 const auto waiting=[](const char* id,unsigned members,std::uint8_t band,Area area=OwnArea){
  auto room=Placed(id,area,members,8,1);room.type=room::RoomType::Classic;room.waiting={{band,area}};room.classes={band};return room;
 };
 j.List({waiting("b",1,9),waiting("a",4,6)});
 h.Choose("pr-quick");
 Check(tickets(j)==1&&lastTicketIs(j,"brg_1","a"),"Quick join did not take the room near the player's class over one with more seats");
 Check(j.status!=loc::T("public.quick_wider"),"Quick join said it looked wider when it found the player's class");
 Check(j.row("pr-room:a")->detail.find(loc::Tf("public.room_detail_class",ClassName(6)))!=std::string::npos,"A room's card detail does not name its class");
 j.Admit("room_full");
 h.Choose("pr-quick");h.Frame(0,2);
 Check(tickets(j)==2&&lastTicketIs(j,"brg_1","b"),"Quick join did not take the far class once the near one was full");
 Check(j.status==loc::T("public.quick_wider"),"Quick join did not say it looked past the player's class");
 j.Admit("room_full");
 // Any class: the room with the most free seats again, and nothing said about classes.
 h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);
 h.Choose("pr-filters");h.FocusOn("pr-filter-quick");
 Check(j.row("pr-filter-quick")->value==loc::T("public.filter_quick_similar")&&j.row("pr-filter-quick")->detail==loc::T("public.filter_quick_detail"),
  "The Quick join row does not start at Near my class first");
 h.Press(MenuInput::Right);h.Wait(1);
 for(std::size_t i=h.actions.size();i-->0;)if(const auto* save=As<commands::SavePreferences>(h.actions[i])){h.view.preferences=save->preferences;break;}
 h.Frame(0,2);
 Check(h.view.preferences.quickJoinAnyClass&&!h.view.preferences.roomFilter.Active(),"Any class was not saved, or counted as a filter");
 h.Press(MenuInput::Back);
 j.List({waiting("b",1,9),waiting("a",4,6)});
 h.Choose("pr-quick");h.Frame(0,2);
 Check(lastTicketIs(j,"brg_1","b")&&j.status!=loc::T("public.quick_wider"),"Any class did not take the room with the most seats quietly");
 // A player still placing is told Quick join goes by area and seats.
 standing.badge=SkillBadge::Placing();
 const auto rows=PublicRoomsPanel::FilterRows(h.view.preferences.roomFilter,h.view.preferences.quickJoinAnyClass,PublicRoomsPanel::FilterContextOf(h.view));
 const auto quick=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="pr-filter-quick";});
 Check(quick!=rows.end()&&quick->detail==loc::T("public.filter_quick_no_class"),
  "The Quick join row does not say a player without a class gets rooms by area and seats");
 // Ping comes before class: the near room with a far class is taken over a far-away room with a close one, and since a
 // room near the player's class was listed, Quick join does not say it looked wider.
 Journey k;k.OpenList();k.h.view.automaticArea=OwnArea;
 k.h.view.rankings.standing.value=standing;k.h.view.rankings.standing.value.badge=SkillBadge::OfClass(5,true);
 k.List({waiting("n",1,9),waiting("f",1,6,FarArea)});
 k.h.Choose("pr-quick");k.h.Frame(0,2);
 Check(tickets(k)==1&&lastTicketIs(k,"brg_1","n")&&k.status!=loc::T("public.quick_wider"),
  "Quick join told the player it looked wider although a room near their class was listed");
}

// The filter's rows (Area, Room type, Open seats, Connection) step with Left and Right,
// are saved with the preferences, and a filter with nothing to show says so and offers to
// clear it. A room that sent no type shows only under All types.
void FilterJourney(){
 Journey j;auto& h=j.h;j.OpenList();
 const auto relist=[&]{
  auto a=Placed("a",Area::NaWest,3,8,1);a.type=room::RoomType::Classic;
  auto b=Placed("b",FarArea,2,4,2);b.type=room::RoomType::Arcade;b.wiredOnly=true;
  auto c=Placed("c",Area::NaWest,4,4,3);c.type=room::RoomType::King;
  auto d=Placed("d",Area::EastAsia,1,8,4);
  j.List({a,b,c,d});
 };
 relist();
 // The saved change the shell sent, handed back as the runtime does once it has taken it.
 std::size_t seen=h.actions.size();
 const auto settle=[&]{
  h.Wait(1);
  for(std::size_t i=h.actions.size();i-->seen;)
   if(const auto* save=As<commands::SavePreferences>(h.actions[i])){h.view.preferences=save->preferences;break;}
  seen=h.actions.size();h.Frame(0,2);
 };
 // The page's values as the last step left them, and the Filters row's summary on the list.
 std::map<std::string,std::string> pageValues;
 const auto step=[&](const char* row,unsigned direction=MenuInput::Right,int presses=1){
  h.Choose("pr-filters");
  Check(h.shell.Navigation().Screen()==PublicRoomsPanel::FiltersScreen,"Filters did not open its page");
  h.FocusOn(row);for(int i=0;i<presses;++i)h.Press(direction);settle();
  for(const auto& r:j.rows)pageValues[r.id]=r.value;
  h.Press(MenuInput::Back);
  Check(h.shell.Navigation().Screen()=="public-rooms"&&h.shell.Navigation().Focus()=="pr-filters","Back from the filters page did not return to the Filters row");
 };
 // The areas page: Select on one row, which switches an area or Near me or shows every area; the values of the
 // areas page and then of the filters page are kept for value().
 const auto pick=[&](const char* row){
  h.Choose("pr-filters");h.Choose("pr-filter-area");
  Check(h.shell.Navigation().Screen()==PublicRoomsPanel::AreasScreen,"Select on Area did not open the areas page");
  h.FocusOn(row);h.Press(MenuInput::Select);settle();
  for(const auto& r:j.rows)pageValues[r.id]=r.value;
  h.Press(MenuInput::Back);
  Check(h.shell.Navigation().Screen()==PublicRoomsPanel::FiltersScreen&&h.shell.Navigation().Focus()=="pr-filter-area","Back from the areas page did not return to the Area row");
  for(const auto& r:j.rows)pageValues[r.id]=r.value;
  h.Press(MenuInput::Back);
  Check(h.shell.Navigation().Screen()=="public-rooms","Back from the filters page did not return to the list");
 };
 const auto value=[&](const char* row){return pageValues[row];};
 const std::string dot=" \xC2\xB7 ";
 const auto summary=[&]{return j.row("pr-filters")->value;};
 std::vector<std::string> order;for(const auto& r:j.rows)order.push_back(r.id);
 Check(order==std::vector<std::string>({"pr-quick","pr-create","pr-paste","pr-filters","pr-room:d","pr-room:a","pr-room:b","pr-room:c"}),"The Filters row does not follow the toolbar");
 Check(j.row("pr-filters")->wide&&j.row("pr-filters")->height<=48&&j.row("pr-filters")->enabled&&j.row("pr-filters")->value==loc::T("public.filter_summary_all")&&Shown(j)=="dabc",
  "The Filters row does not start on every room");
 // Select opens the page: the four values, which Left and Right step, Quick join's class, and Clear filters, off while nothing is filtered.
 h.Choose("pr-filters");
 {std::vector<std::string> page;for(const auto& r:j.rows)page.push_back(r.id);
  Check(h.shell.Navigation().Screen()==PublicRoomsPanel::FiltersScreen&&page==std::vector<std::string>({"pr-filter-area","pr-filter-type","pr-filter-seats","pr-filter-wired","pr-filter-quick","pr-filters-clear"}),
   "The filters page does not hold the four values, Quick join's row and Clear filters");}
 for(const char* row:{"pr-filter-area","pr-filter-type","pr-filter-seats","pr-filter-wired"})
  Check(j.row(row)->adjustable&&j.row(row)->enabled&&!j.row(row)->detail.empty(),"A filter row on the page is not adjustable or is disabled");
 Check(!j.row("pr-filters-clear")->enabled&&j.row("pr-filters-clear")->label==loc::T("public.clear_filters"),"Clear filters is offered with nothing filtered");
 Check(j.row("pr-filter-area")->value==loc::T("public.filter_area_all")&&j.row("pr-filter-type")->value==loc::T("public.filter_type_all")&&
  j.row("pr-filter-seats")->value==loc::T("common.off")&&j.row("pr-filter-wired")->value==loc::T("public.filter_wired_all"),"The filters page does not start on every room");
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="public-rooms"&&h.shell.Navigation().Focus()=="pr-filters","Back did not return to the Filters row");

 // Area: Left and Right switch between all areas and Near me, which is not offered until the player's area is set.
 step("pr-filter-area");
 Check(value("pr-filter-area")==loc::T("public.filter_area_all")&&summary()==loc::T("public.filter_summary_all")&&Shown(j)=="dabc","Right on Area without an area of the player's own did not stay on All areas");
 // Select opens the areas page: Near me, a switch for each area in the table's order, and Show every area, off while all are shown.
 h.Choose("pr-filters");h.Choose("pr-filter-area");
 {std::vector<std::string> page,expected={"pr-area-near"};
  for(const auto& r:j.rows)page.push_back(r.id);
  for(const auto& row:areas::Table)expected.push_back(std::string("pr-area:")+row.code);
  expected.push_back("pr-area-all");
  Check(h.shell.Navigation().Screen()==PublicRoomsPanel::AreasScreen&&page==expected,"The areas page does not hold Near me, each area and Show every area");}
 Check(!j.row("pr-area-all")->enabled&&j.row("pr-area:naw")->adjustable&&j.row("pr-area:naw")->value==loc::T("common.off")&&j.row("pr-area-near")->value==loc::T("common.off"),
  "The areas page does not start with every switch off");
 h.Press(MenuInput::Back);h.Press(MenuInput::Back);
 const auto on=std::string(loc::T("common.on")),off=std::string(loc::T("common.off"));
 pick("pr-area:naw");
 Check(value("pr-area:naw")==on&&value("pr-filter-area")==loc::T("area.naw")&&summary()==loc::T("area.naw")&&Shown(j)=="ac","Ticking NA West did not list its rooms alone");
 pick("pr-area:eas");
 Check(value("pr-area:eas")==on&&value("pr-filter-area")==loc::Tf("public.filter_areas_count",2)&&summary()==loc::Tf("public.filter_areas_count",2)&&Shown(j)=="dac",
  "Two ticked areas did not list the rooms of both and say how many");
 pick("pr-area:naw");
 Check(value("pr-area:naw")==off&&summary()==loc::T("area.eas")&&Shown(j)=="d","Unticking one of two areas did not keep the other");
 pick("pr-area:eas");pick("pr-area:sas");
 Check(Shown(j).empty()&&j.row("pr-clear-filters")&&j.row("pr-clear-filters")->label==loc::T("public.none_filtered")&&j.row("pr-clear-filters")->hint==loc::T("public.clear_filters")&&
  j.row("pr-clear-filters")->wide&&j.row("pr-clear-filters")->height==110&&!j.row("pr-none")&&!j.row("pr-error"),"An area with no rooms has no card that clears the filter");
 pick("pr-area-all");
 Check(value("pr-filter-area")==loc::T("public.filter_area_all")&&value("pr-area:sas")==off&&summary()==loc::T("public.filter_summary_all")&&Shown(j)=="dabc","Show every area did not list every room again");
 // Near me while the player's area is not set says how to set it.
 h.view.preferences.roomFilter.scope=netplay::publicrooms::RoomFilter::Place::Near;
 Check(PublicRoomsPanel::FilterRows(h.view.preferences.roomFilter,false,{Area::None,true,false,true})[0].detail==loc::T("public.filter_near_unset"),"Near me without an area does not say to set one");
 h.view.preferences.roomFilter=netplay::publicrooms::RoomFilter();
 h.view.automaticArea=Area::NaCentral;h.Frame(0,2);
 step("pr-filter-area");
 Check(value("pr-filter-area")==loc::T("public.filter_region_near")&&summary()==loc::T("public.filter_region_near")&&Shown(j)=="ac","Near me lists rooms that are not in or next to the player's area");
 relist();
 Check(j.status==loc::Tf("public.status_filtered",std::size_t(2),std::size_t(4),loc::T("public.updated_now")),"The status does not say how many of the rooms are shown");
 step("pr-filter-area",MenuInput::Left);
 Check(value("pr-filter-area")==loc::T("public.filter_area_all")&&Shown(j)=="adbc","Left on Near me did not return to every area");
 // (The player now has an area, so the near room a lists before the roomier d.)
 // Near me as a switch on the areas page, and ticking an area leaves it.
 pick("pr-area-near");
 Check(value("pr-area-near")==on&&summary()==loc::T("public.filter_region_near")&&Shown(j)=="ac","The Near me switch did not list the near rooms");
 pick("pr-area:eas");
 Check(value("pr-area-near")==off&&value("pr-area:eas")==on&&summary()==loc::T("area.eas")&&Shown(j)=="d"&&h.view.preferences.roomFilter.scope==netplay::publicrooms::RoomFilter::Place::Areas,
  "Ticking an area did not leave Near me");
 pick("pr-area:eas");
 Check(summary()==loc::T("public.filter_summary_all")&&!h.view.preferences.roomFilter.Active(),"Unticking the last area did not return to All");

 // Room type: a room that sent no type (d) passes under All types only.
 step("pr-filter-type");
 Check(value("pr-filter-type")==RoomTypeName(room::RoomType::Classic)&&summary()==RoomTypeName(room::RoomType::Classic)&&Shown(j)=="a","Classic lists a room of another type or none");
 step("pr-filter-type");
 Check(value("pr-filter-type")==RoomTypeName(room::RoomType::Arcade)&&Shown(j)=="b","Arcade lists a room of another type");
 step("pr-filter-type");
 Check(value("pr-filter-type")==RoomTypeName(room::RoomType::King)&&Shown(j)=="c","King of the Hill lists a room of another type");
 step("pr-filter-type");
 Check(value("pr-filter-type")==loc::T("public.filter_type_all")&&Shown(j)=="adbc","Stepping past King of the Hill did not return to every type");

 // Open seats: not full, not locked. Right is on and Left off.
 step("pr-filter-seats");
 Check(value("pr-filter-seats")==loc::T("common.on")&&Shown(j)=="adb","Open seats lists a full room");
 step("pr-filter-seats",MenuInput::Left);
 Check(value("pr-filter-seats")==loc::T("common.off")&&Shown(j)=="adbc","Open seats off did not list the full room again");

 // Connection: only the rooms that take wired players alone.
 step("pr-filter-wired");
 Check(value("pr-filter-wired")==loc::T("public.filter_wired_only")&&summary()==loc::T("public.filter_wired_only")&&Shown(j)=="b","Wired only lists a room open to every connection");

 // The settings narrow together, and a filter with nothing to show offers to clear it.
 step("pr-filter-seats");step("pr-filter-type",MenuInput::Right,3);
 Check(Shown(j).empty()&&j.row("pr-clear-filters"),"Filters that match no room did not offer to clear them");

 // Saved with the preferences, and kept on leaving and coming back.
 const auto saved=std::count_if(h.actions.begin(),h.actions.end(),[](const ShellAction& a){return Is<commands::SavePreferences>(a);});
 Check(saved>=1&&h.view.preferences.roomFilter.wiredOnly&&h.view.preferences.roomFilter.openSeats&&h.view.preferences.roomFilter.type==netplay::publicrooms::RoomFilter::Type::King&&
  h.view.preferences.roomFilter.scope==netplay::publicrooms::RoomFilter::Place::All,"The filter was not sent to be saved");
 h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);relist();
 Check(summary()==std::string(RoomTypeName(room::RoomType::King))+dot+loc::T("public.filter_summary_seats")+dot+loc::T("public.filter_wired_only")&&j.row("pr-clear-filters"),"The filter was forgotten on leaving Public rooms");

 // Clear filters lists every room again, and that is saved too.
 h.Choose("pr-clear-filters");settle();
 Check(!j.row("pr-clear-filters")&&Shown(j)=="adbc"&&!h.view.preferences.roomFilter.Active(),"Clear filters did not clear the filter");

 // Every room closes while the filter is on: the card is the empty list's, and one press opens Create on Public.
 step("pr-filter-wired");
 j.List({});
 Check(j.row("pr-none")&&j.row("pr-none")->hint==loc::T("public.create_room")&&!j.row("pr-clear-filters"),"An empty list under a filter offered to clear it");
 h.Choose("pr-none");
 Check(h.shell.Navigation().Screen()=="create","The empty list's card under a filter did not open Create in one press");
 h.Screen("online");h.Choose("public-rooms");h.Frame(0,2);
 step("pr-filter-wired",MenuInput::Left);
 j.List({});
 Check(j.row("pr-none")&&j.row("pr-none")->label==loc::T("public.none_all"),"An empty list under no filter has no empty card");

 // Preferences that cannot be edited (a room locks them) keep the filter as it is.
 relist();
 h.view.canEditPreferences=false;h.Choose("pr-filters");
 Check(!j.row("pr-filter-seats")->enabled,"A filter row is offered while the preferences cannot be edited");
 h.FocusOn("pr-filter-seats");h.Press(MenuInput::Right);h.Press(MenuInput::Back);
 Check(summary()==loc::T("public.filter_summary_all")&&Shown(j)=="adbc","A filter changed while the preferences could not be edited");
}
}
void RunPublicRoomsFilterJourneys(){
 SortJourney();QuickJoinJourney();QuickJoinClassJourney();FilterJourney();
}
