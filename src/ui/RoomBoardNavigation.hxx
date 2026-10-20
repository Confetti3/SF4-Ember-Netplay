#pragma once
#include "RoomBoardLayout.hxx"

namespace sf4e { namespace ui {
inline void LinkRoomBoard(std::vector<MenuEntry>& rows, std::size_t columns) {
    // Stacked rows already follow visible order, with the composer last.
    if (columns == 1) return;
    const auto parts=PartitionRoomBoard(rows);
    MenuEntry* composer=parts.composer<rows.size()?&rows[parts.composer]:nullptr;
    std::vector<MenuEntry*> tables,members,actions;
    for(auto i:parts.tables)tables.push_back(&rows[i]);
    for(auto i:parts.members)members.push_back(&rows[i]);
    for(auto i:parts.actions)actions.push_back(&rows[i]);
    if (tables.empty() || actions.empty()) return;
    const auto communityEnd=composer?composer->id:actions.front()->id;
    const auto linkColumn = [&](std::vector<MenuEntry*>& column) {
        for (std::size_t i = 0; i < column.size(); ++i) {
            column[i]->up = column[i ? i - 1 : i]->id;
            column[i]->down = i + 1 < column.size() ? column[i + 1]->id : communityEnd;
        }
    };
    linkColumn(tables); linkColumn(members);
    for (std::size_t i = 0; i < tables.size(); ++i) {
        tables[i]->left = tables[i]->id;
        tables[i]->right = members.empty() ? communityEnd : members[(std::min)(i, members.size() - 1)]->id;
    }
    for (std::size_t i = 0; i < members.size(); ++i) {
        members[i]->left = tables[(std::min)(i, tables.size() - 1)]->id;
        members[i]->right = members[i]->id;
    }
    if(composer){
        composer->up=members.empty()?tables.back()->id:members.back()->id;
        composer->down=actions.front()->id;composer->left=tables.back()->id;composer->right=composer->id;
    }
    for (std::size_t i = 0; i < actions.size(); ++i) {
        auto& row = *actions[i];
        row.left = i % columns ? actions[i - 1]->id : tables.back()->id;
        row.right = i % columns + 1 < columns && i + 1 < actions.size() ? actions[i + 1]->id : row.id;
        row.up = i >= columns ? actions[i - columns]->id : composer ? composer->id : members.empty() ? tables.back()->id : members.back()->id;
        row.down = i / columns < (actions.size() - 1) / columns ? actions[(std::min)(i + columns, actions.size() - 1)]->id : row.id;
    }
}
} }
