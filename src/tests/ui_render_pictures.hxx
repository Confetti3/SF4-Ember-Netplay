#pragma once
// The room shots of Settings › Room layout (ui_render_test.cxx): Tables as a
// grid, and Pictures as characters (the default) and as portraits, on one
// table per line and on the grid. Each member has a costume, colour and
// Ultra, one of them a custom costume this PC has no picture of (drawn in
// the original outfit); the looks stay on the view, so the room shots after
// these (the Arcade cabinets among them, ui_render_training_room.hxx) show
// them too.
#include "../ui/ApplicationShell.hxx"
#include <chrono>
#include <thread>
#include <utility>
#include <vector>
namespace {
// Draws until every fighter's picture the board draws has loaded, or is
// known to be missing (at most ten seconds): the shots before these queue
// many decodes, and a new picture waits its turn.
template<class Draw>
void SettleRoomPictures(const Draw& draw) {
    using namespace sf4e::ui;
    std::vector<std::pair<FighterPicture,float>> drawn;
    SetPortraitProbe([&](const FighterPicture& picture,ImVec2 min,ImVec2 max){drawn.push_back({picture,max.y-min.y});});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(bool loaded=false;!loaded&&std::chrono::steady_clock::now()<deadline;){
        drawn.clear();draw(nullptr,0,1);
        loaded=true;
        for(const auto& [picture,height]:drawn){
            if(!MenuArt()||!sf4e::selection::FindFighter(picture.fighter))continue;
            const auto image=FighterImage(picture,height);
            loaded=loaded&&(image.texture||image.missing);
        }
        if(!loaded)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    SetPortraitProbe({});
}
template<class Draw,class Page>
void ShootRoomPictures(sf4e::ui::ShellView& view,const Draw& draw,const Page& page) {
    for(auto& member:view.room.members){
        const int i=static_cast<int>(member.id);
        member.costume=i==4?sf4e::selection::FirstCustomCostume+1:i%3==0?1:0;
        member.color=i%5;member.ultra=i%3;
    }
    view.preferences.roomTableGrid=true;page("room");SettleRoomPictures(draw);draw("room-grid");
    view.preferences.roomCharacterPictures=false;SettleRoomPictures(draw);draw("room-grid-portraits");
    view.preferences.roomTableGrid=false;SettleRoomPictures(draw);draw("room-portraits");
    view.preferences.roomCharacterPictures=true;draw();
}
}
