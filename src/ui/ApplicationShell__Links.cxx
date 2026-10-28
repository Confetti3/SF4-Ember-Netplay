#include "ApplicationShell.hxx"
#include "../common/Localization.hxx"
#include <imgui.h>
#include <cstdio>
#include <utility>

namespace sf4e { namespace ui {
void ApplicationShell::CopyShortInvitation(const ShellView& v,const Submit& submit) {
 error_.clear();
 if(!v.shortInvitation.empty()){
  ImGui::SetClipboardText(v.shortInvitation.c_str());
  ShowNotice(loc::T("room.short_invitation_copied"),Tone::Success,ImGui::GetTime()+3);shortCopyPending_=false;
  return;
 }
 ShellAction request;request.command.generation=v.session.generation;request.shortInvitation=true;
 shortFailuresSeen_=v.shortInvitationFailures;
 shortCopyPending_=true;shortCopyUntil_=ImGui::GetTime()+12;
 // A request that cannot be queued falls back at once, in UpdateShortCopy.
 if(!submit(std::move(request)))shortCopyUntil_=0;
 ShowNotice(loc::T("room.short_invitation_pending"),Tone::Pending,ImGui::GetTime()+12);
}
// The short link answers within a few seconds. Without one, the full
// invitation goes to the clipboard so the press still shares the room.
void ApplicationShell::UpdateShortCopy(const ShellView& v,double now) {
 if(!shortCopyPending_)return;
 if(v.session.room==netplay::RoomState::Idle||v.invitation.empty()){shortCopyPending_=false;return;}
 if(!v.shortInvitation.empty()){
  shortCopyPending_=false;ImGui::SetClipboardText(v.shortInvitation.c_str());
  ShowNotice(loc::T("room.short_invitation_copied"),Tone::Success,now+3);
  return;
 }
 if(v.shortInvitationFailures==shortFailuresSeen_&&now<shortCopyUntil_)return;
 shortCopyPending_=false;ImGui::SetClipboardText(v.invitation.c_str());
 ShowNotice(loc::T("room.short_invitation_unavailable"),Tone::Neutral,now+6);
}
// A room link opened from the browser joins its room at once when the
// player is free, as soon as a room can be opened. While a room is open it
// waits, so a link never moves the player out of a room or a match; that
// link, or one the runtime no longer offers for a direct join, fills the
// Join screen and joining is the player's own press. Neither happens over
// a dialog, a notice, an Ember ID screen, whose drafts the move would discard,
// or a public room being asked for.
void ApplicationShell::UpdateJoinLink(const ShellView& v,double now,const Submit& submit) {
 using namespace netplay; auto& nav=menu_.navigation;
 const bool free=v.session.room==RoomState::Idle&&v.session.match==MatchState::None;
 if(v.pendingJoinSequence!=joinLinkSeen_){
  joinLinkSeen_=v.pendingJoinSequence;joinLink_=v.pendingJoinLink;
  joinLinkDirect_=free&&v.pendingJoinDirect;
  if(!joinLink_.empty()&&v.session.room!=RoomState::Idle)Say(loc::T("room.link_waiting"),Tone::Pending,8,ToastKind::Invitation);
 }
 if(joinLink_.empty())return;
 if(!free){joinLinkDirect_=false;return;}
 const bool direct=joinLinkDirect_&&v.pendingJoinDirect;
 if(direct&&!v.canOpenRoom)return;
 if(nav.Editing()||nav.Reading()||nav.Confirming()||nav.Choosing()||menu_.NoticeOpen()||IdentityPanel::Owns(nav.Screen())||publicRooms_.Busy())return;
 std::snprintf(invitation_,sizeof(invitation_),"%s",joinLink_.c_str());joinLink_.clear();joinLinkDirect_=false;
 nav.Cancel();nav.Home();nav.Push("online");nav.Push("join");
 error_.clear();noticeTone_=Tone::Neutral;noticeUntil_=now+8;
 if(direct&&Send(CommandKind::JoinInvite,v,submit)){notice_=loc::T("room.link_joining");noticeTone_=Tone::Pending;}
 else notice_=loc::T("room.link_opened");
}
// A public room link opens Public rooms on its service and asks for its room
// (PublicRoomsPanel). Like a match link it never moves a player who is in a
// room or a game: it waits there as a row, and says so. A link that arrived
// while the player was free also waits, for as long as a dialog, a notice or
// an Ember ID screen, whose drafts the move would discard, is open.
void ApplicationShell::UpdatePublicRoomLink(const ShellView& v,double now) {
 using namespace netplay; auto& nav=menu_.navigation;
 const auto& link=v.tournament.roomLink;
 if(link.sequence!=roomLinkSeen_){
  roomLinkSeen_=link.sequence;
  roomLinkOpening_=link.free&&v.session.room==RoomState::Idle&&v.session.match==MatchState::None;
  publicRooms_.OpenLink(link.bridge,link.room,roomLinkOpening_);
  if(!roomLinkOpening_){ShowNotice(loc::T("public.link_waiting"),Tone::Pending,now+15);}
 }
 if(!roomLinkOpening_)return;
 // Busy by the time it can go on (a hidden menu, a dialog): it waits as a row instead.
 if(v.session.room!=RoomState::Idle||v.session.match!=MatchState::None){roomLinkOpening_=false;publicRooms_.HoldLink();return;}
 if(nav.Editing()||nav.Reading()||nav.Confirming()||nav.Choosing()||menu_.NoticeOpen()||IdentityPanel::Owns(nav.Screen()))return;
 roomLinkOpening_=false;
 identity_.Probe(); // the trusted services, fresh, before the link is judged
 if(nav.Screen()!="public-rooms"){nav.Cancel();nav.Home();nav.Push("online");nav.Push("public-rooms");}
 error_.clear();
}
} }
