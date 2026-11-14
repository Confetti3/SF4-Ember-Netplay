#include "TrainingRemote.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "../third_party/qrcodegen/qrcodegen.hpp"
#include "../common/Localization.hxx"
#include "../training/ComboStore.hxx"
#include "../training/MoveInputs.hxx"
#include "../training/RemoteOptions.hxx"
#include <imgui.h>
#include <optional>

// The Phone remote page of the training controls, and the phone's changes to the lab.
namespace sf4e { namespace ui {
namespace {
using namespace training;
// White square with a quiet border, black modules: what a phone camera reads at any theme.
float QrSide(const std::string& text) {
    if(text.empty())return 0;
    const auto qr=qrcodegen::QrCode::encodeText(text.c_str(),qrcodegen::QrCode::Ecc::MEDIUM);
    return (qr.getSize()+8)*4*Scale();
}
void DrawQrCode(const std::string& text) {
    if(text.empty())return;
    const auto qr=qrcodegen::QrCode::encodeText(text.c_str(),qrcodegen::QrCode::Ecc::MEDIUM);
    const float cell=4*Scale(),border=4*cell,side=qr.getSize()*cell+2*border;
    const ImVec2 at=ImGui::GetCursorScreenPos();
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(at,ImVec2(at.x+side,at.y+side),IM_COL32_WHITE);
    for(int y=0;y<qr.getSize();++y)for(int x=0;x<qr.getSize();++x)if(qr.getModule(x,y))
        draw->AddRectFilled(ImVec2(at.x+border+x*cell,at.y+border+y*cell),ImVec2(at.x+border+(x+1)*cell,at.y+border+(y+1)*cell),IM_COL32_BLACK);
    ImGui::Dummy(ImVec2(side,side));
}
bool showQr=false; std::optional<TrainingRemote> s_remote_; const TrainingRemote* s_remote=nullptr;
}
void SetTrainingRemote(TrainingRemote remote) { s_remote_=std::move(remote); s_remote=&*s_remote_; }
bool TrainingRemoteOffered() { return s_remote!=nullptr; }
std::vector<MenuEntry> TrainingRemoteRows() {
 std::vector<MenuEntry> rows;
 if(!s_remote)return rows;
 const bool on=!s_remote->url().empty();
 rows.push_back(Value("remote-toggle",loc::T("training.remote"),loc::T(on?"common.on":"common.off"),loc::T("training.remote.detail")));
 if(on){
  rows.push_back(Row("remote-qr",loc::T("training.remote.qr"),loc::T("training.remote.qr.detail")));
  // Shown, not adjusted.
  rows.push_back(Value("remote-url",loc::T("training.remote.address"),s_remote->url(),loc::T("training.remote.address.detail")));
  rows.push_back(Value("remote-pin",loc::T("training.remote.pin"),s_remote->pin(),loc::T("training.remote.pin.detail")));
  rows.push_back(Row("remote-newpin",loc::T("training.remote.newpin"),loc::T("training.remote.newpin.detail")));
  rows.push_back(Value("remote-hud",loc::T("training.remote.hud"),loc::T(s_remote->hudAllowed()?"common.on":"common.off"),loc::T("training.remote.hud.detail")));
  rows[2].adjustable=rows[3].adjustable=false;
 }
 return rows;
}
GameMenu::ListCard TrainingRemoteCard(bool onPage) {
 if(!onPage||!s_remote||s_remote->url().empty())showQr=false;
 if(!showQr)return {};
 return {[](float){return QrSide(s_remote->link())+2*12*Scale();},[]{DrawQrCode(s_remote->link());}};
}
void HandleTrainingRemote(const MenuAction& a,std::string& error) {
 if(!s_remote)return;
 // Select flips it; Right turns it On and Left Off, as the other On/Off rows do.
 // Select on "Scan to connect" shows the code over the list, and again hides it.
 if(a.kind==MenuAction::Activate&&a.id=="remote-qr")showQr=!showQr;
 // Select on the address opens it, PIN included, in this PC's browser.
 if(a.kind==MenuAction::Activate&&a.id=="remote-url"&&s_remote->open&&!s_remote->open())error=loc::T("training.remote.open_failed");
 // A new PIN: the server starts over, so the old one and every phone signed in with it stop working.
 if(a.kind==MenuAction::Activate&&a.id=="remote-newpin"){s_remote->stop();if(!s_remote->start())error=loc::T("training.remote.failed");}
 if((a.kind==MenuAction::Activate||a.kind==MenuAction::Adjust)&&a.id=="remote-hud")s_remote->allowHud(a.kind==MenuAction::Adjust?a.delta>0:!s_remote->hudAllowed());
 if((a.kind==MenuAction::Activate||a.kind==MenuAction::Adjust)&&a.id=="remote-toggle"){
  const bool on=!s_remote->url().empty(),want=a.kind==MenuAction::Adjust?a.delta>0:!on;
  if(want&&!on){if(!s_remote->start())error=loc::T("training.remote.failed");}else if(!want&&on){s_remote->stop();showQr=false;}
 }
}
// What the phone changed since the last frame, applied as the menu applies it, and the settings it shows.
void ServeTrainingPhone(const training::View& view,const TrainingSubmit& submit) {
    const TrainingLabParts lab=TrainingLab();
    // The saved combos live beside training.json.
    if(!lab.directory.empty()&&!training::Combos().Ready()) training::Combos().SetDirectory(lab.directory/L"combos");
    auto& phone=remote::Phone();
    remote::Settings now{lab.meter,lab.plan,lab.check};
    bool changed=false,reply=false;
    static std::string replyError;
    // Moves typed or picked on the phone go in the first slot, through the
    // same checks as the menu's; none turns every slot off, so the
    // recordings play.
    for(const auto& order:phone.TakeText()) {
        training::ReplyChoice choice; std::string error; const auto text=combo::Clean(order.second);
        if(!text.empty()&&!training::BuildReply(text,choice,error)) { replyError=error; continue; }
        replyError.clear();
        if(text.empty()) for(auto& slot:lab.slots) slot.on=false;
        else { lab.slots[0].moves=text; lab.slots[0].on=true; }
        changed=reply=true;
    }
    for(const auto& order:phone.Take()) {
        // Combo check's start needs the fighters picked on the PC.
        if(order.first=="comboStart") {
            Command command; command.action=Action::ComboStart; command.generation=view.generation; command.check=lab.check;
            if(view.available&&submit) submit(command);
        }
        else if(remote::WriteLab(now,order.first,order.second)) { changed=true; reply|=remote::FindOption(order.first)->tab==remote::Tab::Reply; }
    }
    if(changed) {
        lab.meter=now.meter; lab.plan=now.plan; lab.check=now.check; SaveTrainingLab();
        if(reply) SendTrainingPlan(view,submit);
    }
    static nlohmann::json shown;
    auto values=remote::ReadLab(now);
    values["replyMoves"]=lab.slots[0].on?lab.slots[0].moves:std::string(); values["replyError"]=replyError;
    if(values!=shown) { shown=values; phone.Publish(std::move(values)); }
}
} }
