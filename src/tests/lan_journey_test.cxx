#include "shell_journey_support.hxx"
#include "../ui/LanRoomsPanel.hxx"
#include <iostream>
using sf4e::netplay::CommandKind;

int main(){
 try{
  sf4e::netplay::LanBrowserLease lease;
  const auto first=lease.Due(100);Check(first!=0&&!lease.Active(),"Unsent lease became active");
  lease.Sent(first,100);Check(!lease.Due(5099)&&lease.Due(5100)==first,"Lease renewal boundary");
  lease.Stop();const auto second=lease.Due(5200);Check(second>first,"Stopped browser reused its view");
  sf4e::netplay::LanBrowserPulse pulse;pulse.Note(100);
  Check(pulse.Shown(1599)&&!pulse.Shown(1600)&&!pulse.Shown(99),"Browser pulse expiry");
  Harness h;h.view.build="build";h.view.preferences.roomPublic=true;h.Frame();
  ImGui::GetPlatformIO().Platform_GetClipboardTextFn=[](ImGuiContext*){return "sf4e3:previously-pasted";};
  h.Screen("join");h.Choose("paste");h.Screen("online");h.Choose("lan");
  Check(h.shell.Navigation().Screen()=="lan","LAN browser is not reachable with a controller");
  Check(h.shell.LanDrawn(),"LAN browser did not start discovery");
  const std::string id(64,'a');
  auto found=std::make_shared<sf4e::netplay::LanRooms>(); found->status="ready";found->rooms={{id,"Local room","build",1,2}};h.view.lanRooms=found;h.Frame();
  h.Choose(("lan-join-"+id).c_str());
  Check(h.actions.back().command.kind==CommandKind::JoinInvite&&h.actions.back().command.lanHost==id&&h.actions.back().command.invitation.empty()&&h.actions.back().transport==sf4e::netplay::RoomKind::Lan,"LAN join did not select the discovered host");
  h.Screen("join");h.Choose("join-now");
  Check(h.actions.back().command.invitation=="sf4e3:previously-pasted","LAN browsing overwrote the pasted invitation");
  h.Screen("lan");
  found->rooms[0].build="other";
  auto rows=sf4e::ui::LanRoomRows(*h.view.lanRooms,h.view.build,true);
  Check(!rows[0].enabled,"Incompatible LAN room can be joined");
  found->rooms[0].build="build";found->rooms[0].members=2;
  rows=sf4e::ui::LanRoomRows(*h.view.lanRooms,h.view.build,true);Check(!rows[0].enabled,"Full LAN room can be joined");
  h.Choose("lan-host");h.Choose("host");
  Check(h.actions.back().command.kind==CommandKind::HostRoom&&h.actions.back().transport==sf4e::netplay::RoomKind::Lan,"LAN host was sent to the online/public path");
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
  ImGui::GetPlatformIO().Platform_GetClipboardTextFn=nullptr;
  const nlohmann::json valid={{"status","ready"},{"rooms",nlohmann::json::array({{{"endpoint",id},{"n","Room"},{"b","build"},{"m",1},{"c",2}}})}};
  Check(sf4e::netplay::ReadLanRooms(valid).rooms.size()==1,"LAN helper view was not decoded");
  auto invalid=valid;invalid["rooms"][0]["c"]=100;
  bool rejected=false;try{sf4e::netplay::ReadLanRooms(invalid);}catch(const nlohmann::json::exception&){rejected=true;}
  Check(rejected,"Unbounded LAN metadata was accepted");
  std::cout<<"LAN controller journeys and metadata validation passed.\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
