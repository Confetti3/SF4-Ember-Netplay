#include "ApplicationShell.hxx"
#include "RoomControls.hxx"
#include "RoomFeedback.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include <imgui.h>

namespace sf4e { namespace ui {
namespace {
const room::Table* RematchTable(const ShellView& v) {
    const auto place=room::PlaceOf(v.room,v.room.localMember);
    if(place.kind!=room::Place::Kind::Seat||v.room.closed||v.session.room!=netplay::RoomState::Joined)return nullptr;
    const auto& table=v.room.tables[place.table];
    return table.rematch.state==room::RematchOffer::Offered?&table:nullptr;
}
}
void ApplicationShell::UpdateRematch(const ShellView& v) {
    auto& nav=menu_.navigation;
    const auto* table=RematchTable(v);
    const bool showing=nav.Screen()=="room-rematch"||(nav.Screen()=="selection"&&nav.Parent()=="room-rematch");
    if(v.session.control!=netplay::Health::Healthy||v.session.recovery!=netplay::Recovery::None){
        if(showing){nav.Home();nav.Push("room");}
        rematchScreen_={};return;
    }
    if(!table){
        if(showing){
            if(rematchScreen_.epoch==v.room.roomEpoch&&rematchScreen_.table>=0){
                const auto& last=v.room.tables[rematchScreen_.table];
                if(last.matchGeneration==rematchScreen_.generation){
                    notice_=loc::T(last.rematch.state==room::RematchOffer::Expired?"rematch.expired":"rematch.cancelled");
                    noticeTone_=Tone::Neutral;noticeUntil_=ImGui::GetTime()+8;
                }
            }
            nav.Home();if(v.session.room==netplay::RoomState::Joined)nav.Push("room");
        }
        rematchScreen_={};return;
    }
    if(room_controls::GameLive(v))return;
    const bool fresh=rematchScreen_.epoch!=v.room.roomEpoch||rematchScreen_.table!=table->id||rematchScreen_.generation!=table->matchGeneration;
    if(fresh){
        rematchScreen_={v.room.roomEpoch,table->matchGeneration,table->id};
        selectedTable_=table->id;
        nav.Home();nav.Push("room");nav.Push("room-rematch");nav.Prefer("rematch-ready");
    }
    const int seat=table->p1==v.room.localMember?0:1;
    if(table->ready[seat]||v.readyFailureSequence!=rematchScreen_.failureSequence)rematchScreen_.submitting=false;
}
std::string ApplicationShell::RematchStatus(const ShellView& v) const {
    const auto* table=RematchTable(v);if(!table)return {};
    if(rematchScreen_.cancelling)return loc::T("rematch.returning");
    if(table->phase==room::TablePhase::Ready)return loc::T("room.preparing_match");
    const int seat=table->p1==v.room.localMember?0:1;
    std::string text=table->ready[seat]?loc::T("rematch.waiting"):loc::T("rematch.choose");
    if(table->rematch.remainingMs)text=loc::Tf("rematch.countdown",(table->rematch.remainingMs+999)/1000)+"  "+text;
    return text;
}
std::vector<MenuEntry> ApplicationShell::RematchEntries(const ShellView& v) const {
    const auto* table=RematchTable(v);if(!table)return {};
    const int seat=table->p1==v.room.localMember?0:1;
    const auto control=room_controls::DescribeReady(v,*table,seat);
    const bool pending=v.readyRequested||rematchScreen_.submitting||rematchScreen_.cancelling;
    const bool ready=table->ready[seat];
    const std::string score=loc::Tf("rematch.score",table->score[0],table->score[1],static_cast<int>(table->rules.format));
    auto rows=std::vector<MenuEntry>{
        Row("rematch-ready",ready?loc::T("room.unready"):loc::T("rematch.play"),
            score+"\n\n"+loc::T("rematch.ready_detail")+"\n"+v.selectionSummary,
            !pending&&control.kind!=room_controls::ReadyControl::None),
        Row("rematch-character",loc::T("rematch.character"),loc::T("rematch.character_detail"),
            !pending&&!ready&&room_controls::SelectionBlocker(v).empty()),
        Row("rematch-room",loc::T("rematch.return"),loc::T("rematch.return_detail"),
            !rematchScreen_.cancelling&&RoomActionsAvailable(v)&&(table->phase==room::TablePhase::Waiting||room::ReadyCancellable(*table,seat)))};
    if(!control.refusal.empty())rows[0].detail+="\n"+control.refusal;
    return rows;
}
void ApplicationShell::ConfirmRematch(const ShellView& v,const Submit& submit) {
    const auto* table=RematchTable(v);
    if(!table||rematchScreen_.cancelling||rematchScreen_.submitting||v.readyRequested)return;
    const int seat=table->p1==v.room.localMember?0:1;
    const auto control=room_controls::DescribeReady(v,*table,seat);
    if(control.kind!=room_controls::ReadyControl::Ready&&control.kind!=room_controls::ReadyControl::Rematch)return;
    ShellAction action;action.command.kind=control.kind==room_controls::ReadyControl::Rematch?netplay::CommandKind::Rematch:netplay::CommandKind::Ready;
    action.command.generation=v.session.generation;action.preferences=preferences_;
    action.roomAction.table=table->id;action.roomAction.matchGeneration=table->matchGeneration;
    if(submit(std::move(action))){rematchScreen_.failureSequence=v.readyFailureSequence;rematchScreen_.submitting=true;error_.clear();}
    else error_=loc::T("error.queue_failed");
}
void ApplicationShell::RematchAction(const MenuAction& action,const ShellView& v,const Submit& submit) {
    const auto* table=RematchTable(v);if(!table||rematchScreen_.cancelling)return;
    const int seat=table->p1==v.room.localMember?0:1;
    if(action.id=="rematch-ready"){
        if(table->ready[seat])ToggleReady(v,submit);else ConfirmRematch(v,submit);
    }else if(action.id=="rematch-character"&&!table->ready[seat]&&!v.readyRequested&&!rematchScreen_.submitting&&room_controls::SelectionBlocker(v).empty()){
        selectionFresh_=true;selectionOpenOn_="roster";menu_.navigation.Push("selection");
    }else if(action.id=="rematch-room"){
        room::Action request;request.kind=room::ActionKind::CancelRematch;request.table=table->id;request.matchGeneration=table->matchGeneration;
        rematchScreen_.cancelling=SendRoom(request,v,submit);
    }
}
} }
