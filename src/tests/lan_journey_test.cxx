#include "shell_journey_support.hxx"
#include "../ui/LanRoomsPanel.hxx"
#include <iostream>
using sf4e::netplay::CommandKind;

int main(){
 try{
  Harness h;h.view.build="build";h.view.preferences.roomPublic=true;h.Frame();
  h.Choose("online");h.Choose("lan");
  Check(h.shell.Navigation().Screen()=="lan","LAN browser is not reachable with a controller");
  Check(h.shell.LanDrawn(),"LAN browser did not start discovery");
  const std::string id(64,'a');
  h.view.lanRooms.status="ready";h.view.lanRooms.rooms={{id,"Local room","build",1,2}};h.Frame();
  h.Choose(("lan-join-"+id).c_str());
  Check(h.actions.back().command.kind==CommandKind::JoinInvite&&h.actions.back().command.invitation=="sf4lan:"+id,"LAN join did not select the discovered host");
  h.view.lanRooms.rooms[0].build="other";
  auto rows=sf4e::ui::LanRoomRows(h.view.lanRooms,h.view.build,true);
  Check(!rows[0].enabled,"Incompatible LAN room can be joined");
  h.view.lanRooms.rooms[0].build="build";h.view.lanRooms.rooms[0].members=2;
  rows=sf4e::ui::LanRoomRows(h.view.lanRooms,h.view.build,true);Check(!rows[0].enabled,"Full LAN room can be joined");
  h.Choose("lan-host");h.Choose("host");
  Check(h.actions.back().command.kind==CommandKind::HostRoom&&h.actions.back().transport==sf4e::netplay::RoomTransport::Lan,"LAN host was sent to the online/public path");
  h.Screen("online");h.Choose("create");
  // A saved public preference is still public after visiting LAN Create.
  std::vector<MenuEntry> visible;SetMenuEntriesProbe([&](const auto& r){visible=r;});h.Frame();
  Check(std::any_of(visible.begin(),visible.end(),[](const auto& r){return r.id=="visibility";}),"LAN creation leaked into ordinary Create");
  JoinedRoom(h,1);h.view.invitation="opaque invitation";h.view.transport=sf4e::netplay::RoomTransport::Lan;
  h.Screen("room");
  // The consolidated menu puts sharing on Invite; follow the visible action
  // when present so the same journey covers both standalone and combined PRs.
  if(std::any_of(visible.begin(),visible.end(),[](const auto& r){return r.id=="room-invite";}))h.Choose("room-invite");
  Check(std::none_of(visible.begin(),visible.end(),[](const auto& r){return r.id=="copy-short";}),"LAN offered an unavailable short link");
  h.view.transport=sf4e::netplay::RoomTransport::Online;h.Frame();
  Check(std::any_of(visible.begin(),visible.end(),[](const auto& r){return r.id=="copy-short";}),"Online room lost short-link sharing");
  SetMenuEntriesProbe({});
  const nlohmann::json valid={{"status","ready"},{"rooms",nlohmann::json::array({{{"endpoint",id},{"n","Room"},{"b","build"},{"m",1},{"c",2}}})}};
  Check(sf4e::netplay::ReadLanRooms(valid).rooms.size()==1,"LAN helper view was not decoded");
  auto invalid=valid;invalid["rooms"][0]["c"]=100;
  bool rejected=false;try{sf4e::netplay::ReadLanRooms(invalid);}catch(const nlohmann::json::exception&){rejected=true;}
  Check(rejected,"Unbounded LAN metadata was accepted");
  std::cout<<"LAN controller journeys and metadata validation passed.\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
