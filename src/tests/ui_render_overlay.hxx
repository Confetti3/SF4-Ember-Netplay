#pragma once
// Exercise the production draw boundary with each passive layer and each
// alert in isolation, so one surviving draw cannot mask another missing one.
#include "ui_render_support.hxx"
#include "../common/BattlePause.hxx"
#include "../ui/OverlayLayers.hxx"
#include <imgui.h>
#include <string>
#include <vector>
// The layers the test draws in `mode` (ui_render_overlay.cxx).
sf4e::ui::OverlayLayersView OverlayRenderLayers(int mode,const sf4e::ui::MatchStripView& match);
namespace {
template<class Draw>
void CheckExportPasses(sf4e::training::View& training,sf4e::ui::OverlayLayersView& layers,bool& tooltip,int& mode,const Draw& draw);
template<class Draw>
void CheckOverlayVisibility(sf4e::training::View& training,sf4e::ui::OverlayLayersView& layers,
    bool& tooltip,int& mode,const Draw& draw) {
    using namespace sf4e;using namespace ui;
    const auto saved=training;
    mode=7;
    for(int state=0;state<4;++state) {
        const bool passive=state==0;
        SetNativePauseForTest(state==3);
        const auto reset=[&] {
            training=saved;training.available=training.watching=false;training.leavingIn=0;
            layers={};layers.trainingHud=false;layers.showMatchHud=false;
            layers.shellVisible=state==1;layers.trainingControlsOpen=state==2;
            layers.nativePaused=battlePause.Paused();
        };
        const auto foreground=[] {return ImGui::GetForegroundDrawList()->VtxBuffer.Size>0;};
        reset();training.available=true;layers.trainingHud=true;
        layers.shellAvailable=layers.matchWaitsForMenu=true;
        layers.padHint="Saved pad is not connected. Using Other pad for now.";
        layers.trainingRoom.room="Room";layers.trainingRoom.table="Table 1";draw();
        for(const char* window:{"Training frame meter","Training shortcuts","Training room status","Ember shortcut","Ember match waiting","Ember controller"})
            Require(WindowDrawn(window)==passive,"Open window or native pause did not gate a passive training layer or hint");
        Require(!foreground(),"Training visibility fixture unexpectedly drew a foreground layer");
        for(int layout:{0,1}) {
            reset();training.watching=true;layers.matchActive=layers.showMatchHud=true;
            layers.match.layout=layout;layers.match.names[0]="Player One";layers.match.names[1]="Player Two";
            draw();
            Require(WindowDrawn("Training frame meter")==passive,"Open window or native pause did not gate match meter");
            Require(!WindowDrawn("Training shortcuts"),"Match meter drew training chips");
            Require(foreground()==passive,"Open window or native pause did not gate the match strip or split HUD");
            Require(!WindowDrawn("Match notice"),"Suppressed ordinary telemetry became a standalone alert");
        }
        // Spectators use the bubble; Training keeps its existing room line.
        // Neither can leak a bubble over menus, pause, or a hidden HUD.
        for(bool spectating:{false,true}) {
            reset();layers.chat={18,true};layers.matchActive=spectating;
            layers.match.spectator=spectating;layers.showMatchHud=true;
            training.available=!spectating;layers.trainingHud=true;
            draw();Require(WindowDrawn("Chat notification")== (passive&&spectating),"Chat notification ignored passive visibility");
            layers.chat.expanded=false;draw();
            Require(WindowDrawn("Chat notification")== (passive&&spectating),"Unread badge disappeared at notification expiry");
            layers.focused=false;draw();Require(!WindowDrawn("Chat notification"),"Unfocused game showed chat attention");
            layers.focused=true;layers.trainingHud=layers.showMatchHud=false;draw();
            Require(!WindowDrawn("Chat notification")&&!WindowDrawn("Match notice"),"HUD off leaked routine chat through an alert");
        }
        for(int position:{1,2}) {
            reset();layers.matchActive=layers.match.spectator=true;
            layers.match.queuePosition=position;layers.match.queueLength=3;draw();
            Require(WindowDrawn("Match notice")==passive,"HUD off lost the spectator queue position");
        }
        reset();layers.matchActive=layers.showMatchHud=true;layers.chat={18,true};draw();
        Require(!WindowDrawn("Chat notification"),"Chat notification interrupted an active fighter");
        layers.match.spectator=true;layers.match.connectionWarning=true;draw();
        Require(!WindowDrawn("Chat notification"),"Chat competed with a connection warning");
        layers.match.connectionWarning=false;layers.match.notice="Connection restored";layers.match.noticeSeverity=0;draw();
        Require(WindowDrawn("Chat notification")==passive,"A routine notice hid the unread badge");
        layers.match.notice.clear();layers.chat={};draw();
        Require(!WindowDrawn("Chat notification"),"Empty chat drew a badge");
        // The Ember shortcut never covers a fight's timer, even where the menu opens over it.
        reset();layers.matchActive=layers.shellAvailable=true;layers.padHint="Saved pad connected. You play with it.";draw();
        Require(!WindowDrawn("Ember shortcut"),"The Ember shortcut was drawn over a fight");
        Require(!WindowDrawn("Ember controller"),"The controller hint was drawn over a fight");
        // Faded out or turned off, the shortcut is not drawn at all.
        reset();layers.shellAvailable=true;layers.shortcutHintAlpha=0;draw();
        Require(!WindowDrawn("Ember shortcut"),"The Ember shortcut was drawn faded out");
        reset();training.leavingIn=120;layers.challenger.called=true;layers.challenger.opponent="Opponent";draw();
        Require(foreground(),"Open window or native pause hid challenger banner");
        // The banner is the call's: a countdown with no call standing draws none.
        reset();training.leavingIn=120;layers.challenger.opponent="Opponent";draw();
        Require(!foreground(),"A countdown without a call drew the challenger banner");
        reset();layers.captionShown=true;layers.caption.names[0]="Caption One";layers.caption.names[1]="Caption Two";
        layers.caption.line="Export caption";layers.caption.mark=true;draw();
        Require(WindowDrawn(ExportCaptionWindow),"Open window or native pause hid export caption");
        reset();layers.matchActive=true;layers.controllerWarning="Reconnect the controller.";draw();
        Require(WindowDrawn("Controller warning"),"Open window or native pause hid controller warning");
        for(int alert=0;alert<3;++alert) {
            reset();layers.matchActive=true;layers.showMatchHud=!passive;
            if(alert==0){layers.match.notice="Hard match error";layers.match.noticeSeverity=2;}
            else if(alert==1)layers.match.predictionStalled=true;
            else {layers.match.connectionWarning=true;layers.match.disconnectCountdownMs=2200;}
            draw();
            Require(WindowDrawn("Match notice"),"Suppressed match strip lost an urgent standalone alert");
            Require(!foreground(),"Standalone match alert unexpectedly drew passive telemetry");
        }
        reset();layers.matchActive=true;layers.match.notice="Ordinary match status";draw();
        Require(WindowDrawn("Match notice")==passive,"Open window promoted ordinary match status to an alert");
        reset();tooltip=true;draw();
        Require(WindowDrawn("##Tooltip"),"Open window or native pause hid an open-window tooltip");
        tooltip=false;
    }
    battlePause.CloseBattle();training=saved;layers={};mode=2;draw();
    // Review captures of the production widget, including capped counts.
    mode=7;training.available=training.watching=false;layers={};layers.matchActive=layers.match.spectator=true;layers.chat={18,true};
    layers.match.names[0]="Player One";layers.match.names[1]="Player Two";layers.match.appliedDelay=1;layers.match.behindFrames=2;
    draw("chat-notification-expanded");const float expandedWidth=FindWindow("Chat notification")->Size.x;
    layers.chat.expanded=false;draw("chat-notification-badge");
    const auto* badge=FindWindow("Chat notification");const auto* vp=ImGui::GetMainViewport();
    Require(badge->Size.x<expandedWidth&&badge->Size.x<vp->Size.x*.5f,"Unread badge did not shrink after expiry");
    Require(badge->Pos.x>=vp->Pos.x&&badge->Pos.y>=vp->Pos.y&&
        badge->Pos.x+badge->Size.x<=vp->Pos.x+vp->Size.x&&badge->Pos.y+badge->Size.y<=vp->Pos.y+vp->Size.y,
        "Chat badge escaped the viewport");
    Require((badge->Flags&ImGuiWindowFlags_NoInputs)==ImGuiWindowFlags_NoInputs,"Passive chat stole input");
    layers.chat.unread=200;draw("chat-notification-many");
    // Test each actual occupied HUD box, including raised bottom right and
    // split name plates, at every viewport/locale/scale in the harness.
    for(int layout:{0,1})for(int anchor=0;anchor<6;++anchor)for(int size=0;size<3;++size)for(bool raised:{false,true}) {
        layers.match.layout=layout;layers.match.anchor=anchor;layers.match.size=size;layers.match.raised=raised;
        for(bool expanded:{false,true}) {
            layers.chat.expanded=expanded;draw();
            const auto* bubble=FindWindow("Chat notification");const auto bounds=MatchStripGeometry(layers.match);
            for(const auto& box:{bounds.panel,bounds.state,bounds.names[0],bounds.names[1]})
                Require(!box.valid||bubble->Pos.x>=box.x1||bubble->Pos.x+bubble->Size.x<=box.x0||bubble->Pos.y>=box.y1||bubble->Pos.y+bubble->Size.y<=box.y0,
                    ("Chat overlaps the spectator HUD: layout="+std::to_string(layout)+" anchor="+std::to_string(anchor)+" size="+std::to_string(size)+" raised="+std::to_string(raised)+" expanded="+std::to_string(expanded)+" bubble="+std::to_string(bubble->Pos.x)+","+std::to_string(bubble->Pos.y)+","+std::to_string(bubble->Size.x)+","+std::to_string(bubble->Size.y)+" box="+std::to_string(box.x0)+","+std::to_string(box.y0)+","+std::to_string(box.x1)+","+std::to_string(box.y1)).c_str());
            Require(bubble->Pos.x>=vp->Pos.x&&bubble->Pos.y>=vp->Pos.y&&bubble->Pos.x+bubble->Size.x<=vp->Pos.x+vp->Size.x&&bubble->Pos.y+bubble->Size.y<=vp->Pos.y+vp->Size.y,
                "Positioned chat escaped the viewport");
        }
    }

    training=saved;training.available=false;training.watching=true;
    layers={};layers.matchActive=layers.match.spectator=true;layers.chat={18,true};draw();
    const auto* meter=FindWindow(FrameMeterWindow);const auto* bubble=FindWindow("Chat notification");
    Require(bubble->Pos.x>=meter->Pos.x+meter->Size.x||bubble->Pos.x+bubble->Size.x<=meter->Pos.x||
        bubble->Pos.y>=meter->Pos.y+meter->Size.y||bubble->Pos.y+bubble->Size.y<=meter->Pos.y,
        "Spectator chat overlaps the frame meter");
    training=saved;
    layers={};mode=2;
    CheckExportPasses(training,layers,tooltip,mode,draw);
}

// An export's video takes its caption and frame meter and nothing else of
// Ember's: split at the drawing boundary, a window opened over the export
// (standing in for Ember's menu) stays in the second pass on the frame it
// opens, while it is focused, when focus moves and after it closes, and
// the two layers keep drawing into the first pass all the while.
template<class Draw>
void CheckExportPasses(sf4e::training::View& training,sf4e::ui::OverlayLayersView& layers,
    bool& tooltip,int& mode,const Draw& draw) {
    using namespace sf4e;using namespace ui;
    const auto saved=training;
    mode=7;
    const auto owners=[](const ImDrawData& pass){
        std::vector<std::string> names;
        for(const ImDrawList* list:pass.CmdLists)names.push_back(list->_OwnerName?list->_OwnerName:"");
        return names;
    };
    const auto has=[](const std::vector<std::string>& names,const char* fragment){
        for(const auto& name:names)if(name.find(fragment)!=std::string::npos)return true;
        return false;
    };
    // 0 closed, 1 the frame it opens, 2 open, 3 focus moved to it, 4 closed again.
    for(int state=0;state<5;++state){
        const bool open=state>=1&&state<=3;
        training=saved;training.available=false;training.watching=true;
        layers={};layers.trainingHud=false;layers.exporting=true;layers.shellVisible=open;
        layers.captionShown=true;layers.caption.names[0]="Caption One";layers.caption.names[1]="Caption Two";layers.caption.line="Export caption";
        layers.replay.meter=true;
        tooltip=open;
        if(state==3)ImGui::SetWindowFocus("Open window tooltip");
        draw(nullptr,0,state==1?1:3);
        ExportPasses passes;SplitExportPasses(*ImGui::GetDrawData(),passes);
        const auto video=owners(passes.video),rest=owners(passes.rest);
        Require(video.size()==2&&has(video,ExportCaptionWindow)&&has(video,FrameMeterWindow),"An export's video lost its caption or meter under a window");
        Require(!has(rest,ExportCaptionWindow)&&!has(rest,FrameMeterWindow),"An export's layers were drawn again after its picture");
        Require(has(rest,"Open window tooltip")==open&&!has(video,"Open window tooltip")&&!has(video,"##Tooltip"),"A window over an export reached its video");
        Require(passes.video.TotalVtxCount+passes.rest.TotalVtxCount==ImGui::GetDrawData()->TotalVtxCount,"The passes lost or doubled drawing");
    }
    // Outside an export the replay's meter still hides under a window.
    layers.exporting=false;layers.captionShown=false;layers.shellVisible=true;tooltip=true;draw();
    Require(!WindowDrawn(FrameMeterWindow),"A replay's meter showed over a window outside an export");
    tooltip=false;training=saved;layers={};mode=2;draw();
}
}
