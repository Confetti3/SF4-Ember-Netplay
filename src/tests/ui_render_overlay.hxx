#pragma once
// Exercise the production draw boundary with each passive layer and each
// alert in isolation, so one surviving draw cannot mask another missing one.
#include "ui_render_support.hxx"
#include "../common/BattlePause.hxx"
#include "../ui/OverlayLayers.hxx"
#include <imgui.h>
// The layers the test draws in `mode` (ui_render_overlay.cxx).
sf4e::ui::OverlayLayersView OverlayRenderLayers(int mode,const sf4e::ui::MatchStripView& match);
namespace {
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
        layers.shellAvailable=layers.matchWaitsForMenu=true;draw();
        for(const char* window:{"Training frame meter","Training shortcuts","Ember shortcut","Ember match waiting"})
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
        reset();training.leavingIn=120;draw();
        Require(foreground(),"Open window or native pause hid challenger banner");
        reset();layers.captionShown=true;layers.caption.names[0]="Caption One";layers.caption.names[1]="Caption Two";
        layers.caption.line="Export caption";layers.caption.mark=true;draw();
        Require(foreground(),"Open window or native pause hid export caption");
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
}
}
