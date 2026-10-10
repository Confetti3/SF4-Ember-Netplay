#include "../ui/ApplicationShell.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/Theme.hxx"
#include "imgui_test_support.hxx"
#include <stdexcept>
#include <iostream>
using namespace sf4e;
using namespace sf4e::ui;
static void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main() try {
    HeadlessImGui imgui;
    ShellView v;v.controllerReady=v.canReady=v.canEditSelection=true;
    v.session.room=netplay::RoomState::Joined;v.session.match=netplay::MatchState::PostMatch;
    v.session.control=netplay::Health::Healthy;v.session.generation.room=1;
    v.room.roomEpoch=1;v.room.localMember=v.room.host=1;v.room.capacity=8;
    room::Member a;a.id=1;a.table=0;a.seat=0;a.name="One";
    room::Member b;b.id=2;b.table=0;b.seat=1;b.name="Two";v.room.members={a,b};
    auto& t=v.room.tables[0];t.p1=1;t.p2=2;t.phase=room::TablePhase::Waiting;
    t.rules.format=static_cast<room::SetFormat>(2);t.matchGeneration=4;t.score[0]=1;
    v.selectionSummary="Ryu / Ultra I";
    ApplicationShell shell;FighterSelector selector;selection::Pick pick;int stage=0;
    std::vector<MenuEntry> rows;std::vector<ShellAction> actions;std::string status;bool open=true;
    SetMenuEntriesProbe([&](const auto& r){rows=r;});
    SetMenuStatusProbe([&](const char* text,Tone){status=text;});
    const auto frame=[&](unsigned buttons=0){
        SetMenuInput({buttons,0});imgui.io.DeltaTime=1.f/60;ImGui::NewFrame();
        shell.Draw(v,&open,[&](ShellAction action){if(action.command.kind==netplay::CommandKind::Ready||action.command.kind==netplay::CommandKind::Rematch||action.command.kind==netplay::CommandKind::RoomAction)actions.push_back(action);return true;},[&]{
            selector.Draw(pick,true,nullptr,[](int){selection::Availability a;a.ready=true;a.costumes=1;a.colors[0]=1;a.personalActions=0x3ff;return a;},&stage,v.canEditSelection);
        });ImGui::Render();
    };
    const auto row=[&](const char* id)->const MenuEntry&{for(const auto& r:rows)if(r.id==id)return r;throw std::runtime_error(std::string("Missing row ")+id);};
    const auto press=[&](const char* id){frame();shell.Navigation().Focus(id,rows);frame(MenuInput::Select);frame();};
    frame();Check(shell.Navigation().Screen()=="room","Legacy host gained a quick rematch screen");
    t.rematch.state=room::RematchOffer::Offered;frame();
    Check(shell.Navigation().Screen()=="room-rematch","Unfinished set did not open choices");
    Check(rows.size()==3,"Rematch screen is not the three choices");
    Check(row("rematch-ready").detail.find("1 - 0")!=std::string::npos,"Score missing");
    press("rematch-ready");Check(actions.size()==1&&actions.back().command.kind==netplay::CommandKind::Rematch,"Rematch did not ready once");
    Check(actions.back().roomAction.matchGeneration==4,"Rematch intent lost its completed generation");
    press("rematch-ready");Check(actions.size()==1,"Duplicate Ready submitted before publication");
    t.ready[0]=true;t.rematch.consent[0]=true;t.rematch.timed=true;t.rematch.remainingMs=30000;frame();
    Check(status.find("Confirm within 30s.")==0,"Countdown must lead the status so narrow views show the deadline first");
    Check(!row("rematch-character").enabled,"Ready player could edit a locked character");
    press("rematch-ready");Check(actions.back().roomAction.kind==room::ActionKind::Unready,"Cannot withdraw rematch");
    t.ready[0]=false;t.rematch.consent[0]=false;t.ready[1]=true;t.rematch.remainingMs=12000;frame();
    press("rematch-character");Check(shell.Navigation().Screen()=="selection","Change character did not open selector");
    Check(status.find("12s")!=std::string::npos,"Selector hides countdown");
    auto before=actions.size();frame(MenuInput::Back);frame();
    Check(shell.Navigation().Screen()=="room-rematch"&&actions.size()==before,"Back from selection accidentally readied");
    press("rematch-character");frame(MenuInput::Select);frame(); // saved fighter, then Ultra
    frame(MenuInput::Select);frame();
    Check(shell.Navigation().Screen()=="room-rematch","Confirming fighter/Ultra did not return to choices");
    Check(actions.size()==before+1&&actions.back().command.kind==netplay::CommandKind::Rematch,"Selection confirmation requires another Ready press");
    // An error must allow a deliberate retry, even if an older error was present.
    v.readyFailure="Failed";v.readyFailureSequence=1;frame();Check(row("rematch-ready").enabled,"Failed Ready stranded pending UI");
    v.readyFailure.clear();frame(MenuInput::Back);frame(); // dismiss the deliberate failure notice
    t.rematch.state=room::RematchOffer::Expired;t.rematch.timed=false;t.ready[1]=false;frame();
    Check(shell.Navigation().Screen()=="room","Timeout did not return to room");
    for(int i=0;i<3;++i)frame();Check(shell.Navigation().Screen()=="room","Timeout reopened itself");
    t.matchGeneration=5;t.rematch={};t.rematch.state=room::RematchOffer::Offered;frame();
    press("rematch-room");Check(actions.back().roomAction.kind==room::ActionKind::CancelRematch,"Return to room did not cancel both players");
    Check(actions.back().roomAction.matchGeneration==5,"Cancellation not fenced to this game");
    t.rematch.state=room::RematchOffer::Cancelled;frame();Check(shell.Navigation().Screen()=="room","Committed cancellation did not close choices");
    t.matchGeneration=6;t.rematch.state=room::RematchOffer::Offered;frame();
    press("rematch-character");t.rematch.state=room::RematchOffer::Expired;frame();
    Check(shell.Navigation().Screen()=="room","Timeout left character selector open");
    t.matchGeneration=7;t.rematch.state=room::RematchOffer::Offered;frame();
    frame(MenuInput::Back);frame();Check(actions.back().roomAction.kind==room::ActionKind::CancelRematch,"Back escaped without cancelling opponent's wait");
    // A complete set or departure has no offered continuation; no synthetic Ready.
    t.rematch={};t.score[0]=t.score[1]=0;t.lastSet.generation=7;before=actions.size();frame();
    Check(shell.Navigation().Screen()=="room"&&actions.size()==before,"Completed set auto-readied");
    t.matchGeneration=8;t.rematch.state=room::RematchOffer::Offered;frame();
    v.session.control=netplay::Health::Lost;frame();
    Check(shell.Navigation().Screen()=="room","Recovery controls hidden behind rematch");
    SetMenuEntriesProbe({});SetMenuStatusProbe({});
    std::cout<<"Rematch navigation and character confirmation passed\n";return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
