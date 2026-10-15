#pragma once
// The replay controls over a replay: the strip in each state with keyboard and
// pad glyphs, the frame meter above it, the input lanes, and that all of them
// hide like the other passive layers. Drawn through the production layer
// boundary in the overlay test mode (7), which takes the layers as set here.
#include "ui_render_support.hxx"
#include "../common/BattlePause.hxx"
#include "../common/ReplayInputs.hxx"
#include "../ui/OverlayLayers.hxx"
#include <imgui.h>
#include <algorithm>
namespace {
template<class Draw>
void ShootReplayControls(sf4e::training::View& training,sf4e::ui::OverlayLayersView& layers,int& mode,int h,const Draw& draw) {
    using namespace sf4e;using namespace ui;using namespace replayinputs;
    const auto saved=training;
    mode=7;
    training.available=false;training.watching=false;training.leavingIn=0;
    layers={};layers.trainingHud=false;layers.showMatchHud=false;
    auto& replay=layers.replay;
    replay.shown=true;replay.armed=true;replay.round=1;replay.cursor=2477;replay.inputsKnown=true;replay.stripAlpha=1;
    // The strip stays within its 720p budget, scaled like the training HUD, and inside the screen.
    const auto checkStrip=[&](const char* what){
        Require(WindowDrawn("Replay strip"),what);
        const auto* strip=FindWindow("Replay strip");
        const float s=(std::max)(1.f,(std::min)(1.5f,h/900.f));
        Require(strip->Size.x<=ReplayStripMostWidth*s+1&&strip->Size.y<=ReplayStripMostHeight*s+1,"Replay strip is larger than 460 by 30 at 720p");
        const auto* vp=ImGui::GetMainViewport();
        Require(strip->Pos.x>=vp->Pos.x&&strip->Pos.x+strip->Size.x<=vp->Pos.x+vp->Size.x&&strip->Pos.y+strip->Size.y<=vp->Pos.y+vp->Size.y,"Replay strip left the screen");
        Require(!ImGui::GetIO().WantCaptureKeyboard&&!ImGui::GetIO().WantCaptureMouse,"Replay strip captured gameplay input");
    };
    replay.pad=false;draw("replay-strip-keyboard");checkStrip("Replay strip missing at 1x");
    replay.paused=true;draw("replay-strip-paused");checkStrip("Replay strip missing while paused");
    replay.paused=false;replay.divisor=2;replay.pad=true;draw("replay-strip-half-pad");checkStrip("Replay strip missing at 1/2");
    replay.divisor=4;draw("replay-strip-quarter-pad");checkStrip("Replay strip missing at 1/4");
    replay.pad=false;draw("replay-strip-quarter-keyboard");checkStrip("Replay strip missing at 1/4 with keys");
    replay.paused=true;replay.pad=true;draw("replay-strip-paused-pad");checkStrip("Replay strip missing while paused with a pad");
    // Out of the fight: the chosen speed dimmed, and a control pressed there says when they work.
    replay.paused=false;replay.armed=false;replay.unavailable=true;draw("replay-strip-unavailable");
    Require(WindowDrawn("Replay notice"),"Controls pressed outside the fight did not say so");
    // The replay's frame meter sits above the strip.
    replay.armed=true;replay.unavailable=false;replay.divisor=1;replay.pad=false;replay.meter=true;training.watching=true;
    draw("replay-strip-meter");checkStrip("Replay strip missing under the meter");
    Require(WindowDrawn("Training frame meter"),"A replay asked for with the meter did not draw it");
    Require(FindWindow("Training frame meter")->Pos.y+FindWindow("Training frame meter")->Size.y<=FindWindow("Replay strip")->Pos.y+1,"Replay strip overlaps the frame meter");
    // The "works during the fight" chip with the meter and the strip both shown: it meets neither and stays on screen.
    replay.unavailable=true;draw("replay-strip-unavailable-meter");
    {
        Require(WindowDrawn("Replay notice"),"Controls pressed outside the fight did not say so under the meter");
        const auto* chip=FindWindow("Replay notice");const auto* meter=FindWindow("Training frame meter");const auto* strip=FindWindow("Replay strip");
        const auto apart=[&](const ImGuiWindow* a,const ImGuiWindow* b){
            return a->Pos.x+a->Size.x<=b->Pos.x+.5f||b->Pos.x+b->Size.x<=a->Pos.x+.5f||a->Pos.y+a->Size.y<=b->Pos.y+.5f||b->Pos.y+b->Size.y<=a->Pos.y+.5f;
        };
        Require(apart(chip,meter),"The replay controls chip overlaps the frame meter");
        Require(apart(chip,strip),"The replay controls chip overlaps the strip");
        const auto* vp=ImGui::GetMainViewport();
        Require(chip->Pos.x>=vp->Pos.x&&chip->Pos.x+chip->Size.x<=vp->Pos.x+vp->Size.x&&chip->Pos.y>=vp->Pos.y&&chip->Pos.y+chip->Size.y<=vp->Pos.y+vp->Size.y,
            "The replay controls chip left the screen");
    }
    replay.unavailable=false;
    // The lanes: a few rows with presses and held buttons, both sides.
    replay.meter=false;training.watching=false;replay.lanes=true;
    const unsigned held[][3]={{Down|Right,LP|MP,LP},{Down|Right,LP,LP},{Down,0,0},{Left,HK,HK},{0,0,0},{Down|Left,MK|LK,MK},{Up|Right,0,0}};
    const std::uint32_t frames[]={1,3,12,2,40,7,999};
    replay.rowCount[0]=7;replay.rowCount[1]=5;
    for(int side=0;side<2;++side)for(int i=0;i<replay.rowCount[side];++i){
        const int at=(i+side*2)%7;
        replay.rows[side][i].held=static_cast<std::uint16_t>(held[at][0]|held[at][1]);
        replay.rows[side][i].pressed=static_cast<std::uint16_t>(held[at][2]);
        replay.rows[side][i].frames=frames[at];
    }
    replay.rowCount[0]=replaylane::kRows;
    for(int i=7;i<replaylane::kRows;++i)replay.rows[0][i]=replay.rows[0][i-7];
    draw("replay-lanes");
    Require(WindowDrawn("Replay inputs 1")&&WindowDrawn("Replay inputs 2"),"Input lanes not drawn");
    {
        const auto* left=FindWindow("Replay inputs 1");const auto* right=FindWindow("Replay inputs 2");
        Require(left->Pos.x+left->Size.x<right->Pos.x,"Input lanes meet");
        Require(left->Pos.y+left->Size.y<FindWindow("Replay strip")->Pos.y,"Input lanes reach the strip");
    }
    replay.paused=true;replay.pad=true;draw("replay-lanes-paused-pad");
    // At 1x, faded out: no strip, while the lanes stay.
    replay.paused=false;replay.stripAlpha=0;draw();
    Require(!WindowDrawn("Replay strip")&&WindowDrawn("Replay inputs 1"),"A faded strip still drew, or took the lanes with it");
    // Under Ember's menu, the training controls and the game's pause menu, everything hides.
    replay.stripAlpha=1;replay.meter=true;training.watching=true;
    for(int state=1;state<4;++state){
        SetNativePauseForTest(state==3);
        layers.shellVisible=state==1;layers.trainingControlsOpen=state==2;layers.nativePaused=battlePause.Paused();
        draw();
        Require(!WindowDrawn("Replay strip")&&!WindowDrawn("Replay inputs")&&!WindowDrawn("Training frame meter"),
            "Open window or native pause did not hide the replay controls");
    }
    battlePause.CloseBattle();layers.shellVisible=layers.trainingControlsOpen=layers.nativePaused=false;
    // An export: no strip and no lanes in the video; the meter it asked for stays.
    replay.shown=false;draw();
    Require(!WindowDrawn("Replay strip")&&!WindowDrawn("Replay inputs")&&WindowDrawn("Training frame meter"),"An export drew the replay controls or lost its meter");
    // A netplay match draws its meter as before and no replay layers.
    layers={};layers.matchActive=true;layers.replay.meter=false;draw();
    Require(!WindowDrawn("Replay strip"),"A match drew the replay strip");
    training=saved;layers={};mode=2;draw();
}
}
