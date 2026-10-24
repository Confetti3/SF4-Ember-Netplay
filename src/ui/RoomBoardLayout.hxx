#pragma once
#include "MenuNavigation.hxx"
#include "Theme.hxx"
#include <cmath>

namespace sf4e { namespace ui {
// Indices keep the partition valid for both the immutable renderer and mutable links.
struct RoomBoardPartition {
    std::vector<std::size_t> tables, members, actions;
    std::size_t composer = static_cast<std::size_t>(-1);
};
inline RoomBoardPartition PartitionRoomBoard(const std::vector<MenuEntry>& rows) {
    RoomBoardPartition parts;
    for(std::size_t i=0;i<rows.size();++i) {
        const auto& id=rows[i].id;
        if(id.compare(0,6,"table-")==0)parts.tables.push_back(i);
        else if(id.compare(0,7,"member-")==0)parts.members.push_back(i);
        else if(id=="inline-chat")parts.composer=i;
        else parts.actions.push_back(i);
    }
    return parts;
}
struct RoomBoardLayout {
    bool wide = false;
    int columns = 1;
    float leftWidth = 0, rightWidth = 0, toolsHeight = 0, communityHeight = 0;
    float composerMinimum = 0, explanationHeight = 0, memberHeight = 0;
    float stackedExplanation = 0, stackHeight = 0, chatHeight = 0;
};
inline RoomBoardLayout MeasureRoomBoard(const RoomBoardPartition& parts,float width,float height) {
    const float s=Scale(),gap=12*s,spacing=ImGui::GetStyle().ItemSpacing.y;
    RoomBoardLayout layout;
    layout.leftWidth=(width-gap)*.5f;layout.rightWidth=width-layout.leftWidth-gap;
    layout.columns=layout.rightWidth>=3*170*s?3:2;
    // Reserve the heading, sender and one message line as well as the editor.
    // A one-pixel log could never acknowledge a visible newest message.
    layout.composerMinimum=3*ImGui::GetTextLineHeight()+ImGui::GetFrameHeight()+4*spacing+12*s;
    layout.explanationHeight=ImGui::GetTextLineHeight()+12*s;
    layout.toolsHeight=std::ceil(parts.actions.size()/float(layout.columns))*48*s+8*s;
    layout.communityHeight=height-layout.toolsHeight-layout.explanationHeight-2*gap;
    const float memberMinimum=ImGui::GetTextLineHeight()+spacing+58*s+12*s;
    layout.wide=width>=820*s&&layout.communityHeight>=12*s+memberMinimum+spacing+layout.composerMinimum;
    const float membersAvailable=layout.communityHeight-12*s-ImGui::GetTextLineHeight()-spacing-layout.composerMinimum-spacing;
    const float pitch=58*s+spacing;
    const int memberRows=(std::max)(1,static_cast<int>(((std::min)(membersAvailable,layout.communityHeight*.5f)-12*s+spacing)/pitch));
    layout.memberHeight=memberRows*pitch-spacing+12*s;
    layout.stackedExplanation=height>=layout.composerMinimum+12*s+136*s+64*s?64*s:layout.explanationHeight+4*s;
    const float body=height-layout.stackedExplanation;
    layout.chatHeight=(std::min)(body-gap-1,(std::max)(layout.composerMinimum+12*s,(std::min)(180*s,body*.36f)));
    layout.stackHeight=(std::max)(1.f,body-layout.chatHeight-gap);
    return layout;
}
} }
