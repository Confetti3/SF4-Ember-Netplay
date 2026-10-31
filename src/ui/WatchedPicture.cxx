#include "WatchedPicture.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace sf4e { namespace ui {
// The watched game's picture in a small window of its own in the top right
// corner: over the menu's panels there, under its dialogs. It takes the
// mouse, so the rows it hides are never hovered or clicked through it; NoNav
// keeps the pad and keys off it (the shell's navigation is its own anyway).
// It sits in the corner and at the size of `spot`
// (PlayerPreferences::pictureSpot), and the mouse can drag it to another
// corner, where it snaps when let go, or drag its grip to resize it, while
// `movable`; not movable it has neither, and its place stays. True when that
// changed `spot`; the pad and keys change it in Settings.
bool DrawWatchedPicture(ImTextureID picture,int& spot,bool movable) {
 const auto* vp=ImGui::GetMainViewport();const float s=Scale();
 const int corner=spot%4;
 static bool moving=false,resizing=false;static ImVec2 grab;static float pulled=0;
 // A drag lasts while its button holds the mouse: a picture gone mid-drag (its
 // game over) let it go, so the next one does not jump to the pointer.
 if(!ImGui::GetActiveID()||!movable)moving=resizing=false;
 float share=WatchedPictureShareFor(spot);
 if(resizing)share=pulled;
 const ImVec2 size(vp->Size.x*share,vp->Size.y*share);
 // The bottom corners keep clear of the hint bar.
 const float margin=16*s,bottomMargin=72*s;
 const bool right=corner==0||corner==2,bottom=corner>=2;
 ImVec2 at(right?vp->Pos.x+vp->Size.x-margin-size.x:vp->Pos.x+margin,bottom?vp->Pos.y+vp->Size.y-bottomMargin-size.y:vp->Pos.y+margin);
 if(moving)at=ImVec2(ImGui::GetMousePos().x-grab.x,ImGui::GetMousePos().y-grab.y);
 ImGui::SetNextWindowPos(at);
 ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,1);
 bool changed=false;
 if(ImGui::Begin("Watched game",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoSavedSettings|
  ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoBringToFrontOnFocus)){
  const auto p=ImGui::GetCursorScreenPos();
  ImGui::Image(picture,size);
  auto* d=ImGui::GetWindowDrawList();const ImVec2 q(p.x+size.x,p.y+size.y);
  // The grip sits on the corner facing the screen's middle.
  const float g=18*s;const ImVec2 grip(right?p.x:q.x-g,bottom?p.y:q.y-g);
  if(movable){
   ImGui::SetCursorScreenPos(p);
   ImGui::InvisibleButton("##move",size);
   if(ImGui::IsItemActivated()){moving=true;grab=ImVec2(ImGui::GetMousePos().x-at.x,ImGui::GetMousePos().y-at.y);}
   ImGui::SetCursorScreenPos(grip);
   ImGui::SetNextItemAllowOverlap();
   ImGui::InvisibleButton("##size",ImVec2(g,g));
   if(ImGui::IsItemActivated()){resizing=true;moving=false;pulled=share;}
  }
  if(resizing){
   // The far edge follows the pointer; the corner it is pinned to stays.
   const float wide=right?q.x-ImGui::GetMousePos().x:ImGui::GetMousePos().x-p.x;
   pulled=(std::max)(.18f,(std::min)(.5f,wide/vp->Size.x));
  }
  const bool hot=movable&&(moving||resizing||ImGui::IsWindowHovered());
  const auto& c=v2::Colors();
  d->AddRect(p,q,hot?c.accent:c.border,0,0,(hot?2:1)*s);
  const ImVec2 a(right?grip.x+3*s:grip.x+g-3*s,bottom?grip.y+3*s:grip.y+g-3*s);
  if(movable)d->AddTriangleFilled(a,ImVec2(a.x+(right?10:-10)*s,a.y),ImVec2(a.x,a.y+(bottom?10:-10)*s),hot?c.accent:c.muted);
  if(!ImGui::IsMouseDown(ImGuiMouseButton_Left)&&(moving||resizing)){
   int place=corner,step=spot/4;
   if(moving){const auto m=ImGui::GetMousePos();const bool r=m.x-grab.x+size.x*.5f>vp->Pos.x+vp->Size.x*.5f,b=m.y-grab.y+size.y*.5f>vp->Pos.y+vp->Size.y*.5f;place=r?(b?2:0):(b?3:1);}
   if(resizing){step=0;for(int i=1;i<3;++i)if(std::fabs(WatchedPictureShareFor(i*4)-pulled)<std::fabs(WatchedPictureShareFor(step*4)-pulled))step=i;}
   changed=place+4*step!=spot;spot=place+4*step;moving=resizing=false;
  }
  if(!ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId))ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
 }
 ImGui::End();ImGui::PopStyleVar(2);
 return changed;
}
} }
