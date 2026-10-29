#pragma once
#include "RoomBoardLayout.hxx"

namespace sf4e { namespace ui {
inline void LinkRoomBoard(std::vector<MenuEntry>& rows, std::size_t columns) {
    // Stacked rows already follow visible order, with the composer last.
    if (columns == 0) { for(auto& row:rows)row.left=row.right=row.up=row.down=""; return; }
    const auto parts=PartitionRoomBoard(rows);
    MenuEntry* composer=parts.composer<rows.size()?&rows[parts.composer]:nullptr;
    const auto& tables=parts.tables;
    const auto& members=parts.members;
    const auto& actions=parts.actions;
    if (tables.empty() || actions.empty()) return;
    const auto communityEnd=composer?composer->id:rows[actions.front()].id;
    const auto linkColumn = [&](const std::vector<std::size_t>& column) {
        for (std::size_t i = 0; i < column.size(); ++i) {
            rows[column[i]].up = rows[column[i ? i - 1 : i]].id;
            rows[column[i]].down = i + 1 < column.size() ? rows[column[i + 1]].id : communityEnd;
        }
    };
    linkColumn(tables); linkColumn(members);
    for (std::size_t i = 0; i < tables.size(); ++i) {
        rows[tables[i]].left = rows[tables[i]].id;
        rows[tables[i]].right = members.empty() ? communityEnd : rows[members[(std::min)(i, members.size() - 1)]].id;
    }
    for (std::size_t i = 0; i < members.size(); ++i) {
        rows[members[i]].left = rows[tables[(std::min)(i, tables.size() - 1)]].id;
        rows[members[i]].right = rows[members[i]].id;
    }
    if(composer){
        composer->up=members.empty()?rows[tables.back()].id:rows[members.back()].id;
        composer->down=rows[actions.front()].id;composer->left=rows[tables.back()].id;composer->right=composer->id;
    }
    const std::string afterMembers=parts.chatHeading?parts.chatHeading->id:communityEnd;
    if(!members.empty())rows[members.back()].down=afterMembers;
    if(parts.membersHeading){
        auto& heading=rows[parts.membersHeading-&rows[0]];
        heading.left=rows[tables.back()].id;
        heading.down=members.empty()?afterMembers:rows[members.front()].id;
        rows[tables.back()].right=heading.id;
        if(!members.empty())rows[members.front()].up=heading.id;
    }else if(parts.chatHeading&&members.empty())rows[tables.back()].right=parts.chatHeading->id;
    if(parts.chatHeading){
        auto& heading=rows[parts.chatHeading-&rows[0]];
        heading.up=!members.empty()?rows[members.back()].id:parts.membersHeading?parts.membersHeading->id:rows[tables.back()].id;
        heading.down=communityEnd;
        if(composer)composer->up=heading.id;
    }
    for (std::size_t i = 0; i < actions.size(); ++i) {
        auto& row = rows[actions[i]];
        row.left = i % columns ? rows[actions[i - 1]].id : rows[tables.back()].id;
        row.right = i % columns + 1 < columns && i + 1 < actions.size() ? rows[actions[i + 1]].id : row.id;
        row.up = i >= columns ? rows[actions[i - columns]].id : composer ? composer->id : members.empty() ? rows[tables.back()].id : rows[members.back()].id;
        row.down = i / columns < (actions.size() - 1) / columns ? rows[actions[(std::min)(i + columns, actions.size() - 1)]].id : row.id;
    }
}
} }
