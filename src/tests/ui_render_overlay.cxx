// The layers of each overlay mode ui_render_test.cxx draws (ui_render_overlay.hxx).
#include "ui_render_overlay.hxx"
sf4e::ui::OverlayLayersView OverlayRenderLayers(int mode,const sf4e::ui::MatchStripView& match) {
    sf4e::ui::OverlayLayersView layers;
    layers.trainingControlsOpen=mode==1;layers.nativePaused=sf4e::battlePause.Paused();
    layers.trainingHud=mode==1||mode==2;layers.matchActive=mode==3||mode==5||mode==6;
    layers.showMatchHud=mode==3;layers.match=match;
    if(mode==5)layers.controllerWarning="Match input blocked: reconnect your controller. If its slot changed, return to the room to reassign it.";
    return layers;
}
