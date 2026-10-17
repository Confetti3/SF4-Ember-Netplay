#pragma once
#include "MenuNavigation.hxx"

namespace sf4e { namespace ui {
// Shared by the board renderer and its focus map. Width is in unscaled pixels.
inline int RoomActionColumns(float width) {
    return width < 820 ? 1 : (width - 12) * .5f >= 3 * 170 ? 3 : 2;
}
inline void LinkRoomBoard(std::vector<MenuEntry>& rows, float width) {
    if (RoomActionColumns(width) == 1) return;
    std::vector<MenuEntry*> tables, members, actions;
    for (auto& row : rows) {
        if (row.id.compare(0, 6, "table-") == 0) tables.push_back(&row);
        else if (row.id.compare(0, 7, "member-") == 0) members.push_back(&row);
        else actions.push_back(&row);
    }
    if (tables.empty() || actions.empty()) return;
    const auto linkColumn = [&](std::vector<MenuEntry*>& column) {
        for (std::size_t i = 0; i < column.size(); ++i) {
            column[i]->up = column[i ? i - 1 : i]->id;
            column[i]->down = i + 1 < column.size() ? column[i + 1]->id : actions.front()->id;
        }
    };
    linkColumn(tables); linkColumn(members);
    for (std::size_t i = 0; i < tables.size(); ++i) {
        tables[i]->left = tables[i]->id;
        tables[i]->right = members.empty() ? actions.front()->id : members[(std::min)(i, members.size() - 1)]->id;
    }
    for (std::size_t i = 0; i < members.size(); ++i) {
        members[i]->left = tables[(std::min)(i, tables.size() - 1)]->id;
        members[i]->right = members[i]->id;
    }
    const std::size_t columns = RoomActionColumns(width);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        auto& row = *actions[i];
        row.left = i % columns ? actions[i - 1]->id : row.id;
        row.right = i % columns + 1 < columns && i + 1 < actions.size() ? actions[i + 1]->id : row.id;
        row.up = i >= columns ? actions[i - columns]->id : members.empty() ? tables.back()->id : members.back()->id;
        row.down = i / columns < (actions.size() - 1) / columns ? actions[(std::min)(i + columns, actions.size() - 1)]->id : row.id;
    }
}
} }
