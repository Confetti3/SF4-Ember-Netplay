#pragma once
// The room board's areas: where each of its rows is drawn (RoomPanelTables.cxx)
// and so where LB and RB step (ApplicationShell::StepRoomArea). The action bar,
// the stepping and the narrow board's tabs all ask BoardAreaOf.
#include "RowId.hxx"
#include <initializer_list>
#include <string>

namespace sf4e { namespace ui {

// In the order LB and RB step them. Floor: the tables or cabinets, their
// matchups and the watched picture's Hide over them. Members: the list, its
// heading and its Hide or Show. Chat: the message box and its Hide or Show.
// Bar: every other row, as the action bar draws them, among them the room's
// status while it opens (there is no floor yet).
enum class BoardArea { Floor, Bar, Members, Chat, Count };

inline BoardArea BoardAreaOf(const std::string& id) {
    for (const char* prefix : {"table-", "cabinet-", "matchup-"})
        if (HasPrefix(id, prefix)) return BoardArea::Floor;
    if (id == "room-hide-picture") return BoardArea::Floor;
    if (HasPrefix(id, "member-") || id == "room-members" || id == "room-hide-members" || id == "room-show-members") return BoardArea::Members;
    if (id == "room-chat" || id == "room-hide-chat" || id == "room-show-chat") return BoardArea::Chat;
    return BoardArea::Bar;
}

// The stops LB and RB step: each area on a wide board; on a narrow one its
// three tabs (Board, Members, Chat), the bar under the floor's.
inline int BoardTabCount(bool narrow) { return narrow ? 3 : static_cast<int>(BoardArea::Count); }
inline int BoardTabOf(BoardArea area, bool narrow) {
    const int index = static_cast<int>(area);
    return narrow && index > 0 ? index - 1 : index;
}

} }
