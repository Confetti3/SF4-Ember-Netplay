#pragma once
#include "MenuNavigation.hxx"
#include "RoomCards.hxx"
#include "Theme.hxx"
#include <imgui.h>
#include <vector>
namespace sf4e { namespace ui {
enum class RoomBoardPart { Table, Cabinet, Matchup, Member, Action, MembersHeading, ChatHeading, Composer };
struct RoomBoardRow { std::size_t index; RoomBoardPart part; };
struct RoomBoardParts {
    std::vector<std::size_t> tables, cabinets, members, actions;
    std::vector<RoomBoardRow> ordered;
    const MenuEntry *membersHeading=nullptr, *chatHeading=nullptr;
    std::size_t composer=static_cast<std::size_t>(-1);
};
// Indices serve immutable drawing and mutable navigation without a second copy.
// Classify once. Both looks and the stacked board use the same membership;
// numeric entry keys remain the table/member identity.
inline RoomBoardParts PartitionRoomBoard(const std::vector<MenuEntry>& rows) {
    RoomBoardParts parts;
    for (std::size_t i=0; i<rows.size(); ++i) {
        const auto& row=rows[i];
        auto kind=RoomBoardPart::Action;
        if (row.id=="inline-chat") { kind=RoomBoardPart::Composer; parts.composer=i; }
        else if (row.id=="members-part") { kind=RoomBoardPart::MembersHeading; parts.membersHeading=&row; }
        else if (row.id=="chat-part") { kind=RoomBoardPart::ChatHeading; parts.chatHeading=&row; }
        else if (row.id.compare(0,6,"table-")==0) { kind=RoomBoardPart::Table; parts.tables.push_back(i); }
        else if (row.id.compare(0,8,"cabinet-")==0) { kind=RoomBoardPart::Cabinet; parts.cabinets.push_back(i); }
        else if (row.id.compare(0,8,"matchup-")==0) kind=RoomBoardPart::Matchup;
        else if (row.id.compare(0,7,"member-")==0) { kind=RoomBoardPart::Member; parts.members.push_back(i); }
        else parts.actions.push_back(i);
        parts.ordered.push_back({i,kind});
    }
    return parts;
}
inline float RoomChatComposerHeight() {
    return ImGui::GetFrameHeight()+ImGui::GetTextLineHeightWithSpacing()+ImGui::GetStyle().ItemSpacing.y;
}
inline float RoomChatMinimumHeight() {
    return RoomChatComposerHeight()+2*ImGui::GetTextLineHeightWithSpacing();
}
// Reserve the editor, one whole member card, both headings and the actions
// before choosing columns. A shorter window uses the scrolling stacked board.
inline bool WideRoomBoard(float width, float height, const RoomBoardParts& parts, int sideShare) {
    const float s=Scale(), side=(width-12*s)*sideShare/100.f;
    const int columns=side>=540*s?3:side>=258*s?2:1;
    const float actions=((parts.actions.size()+columns-1)/columns)*48*s+28*s;
    const float community=RoomChatMinimumHeight()+MemberCardHeight*s+2*(ImGui::GetFontSize()+8*s)+36*s;
    return width>=820*s && height>=actions+community+ImGui::GetFontSize()+16*s;
}
} }
