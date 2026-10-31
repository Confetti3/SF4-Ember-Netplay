#pragma once
// The room board's parts (Room.dc.html), drawn by ApplicationShell::DrawRoomBoard
// in RoomPanelTables.cxx: the tables with the room's action bar at their foot
// (RoomPanelTables.cxx), an Arcade room's cabinets in their place
// (RoomPanelArcade.cxx), and the Members over Chat column with the layout
// that sizes it (RoomPanelColumn.cxx: ApplicationShell::DrawRoomMembersPanel,
// DrawRoomChatPanel). The rows are built in RoomPanel.cxx.
// Every box that is a row goes through RoomBoardFrame::Press, so the pad
// reaches exactly what the pointer can (V2_KIT.md, Moving on a board).
#include "ApplicationShell.hxx"
#include "V2.hxx"
#include <string>
#include <vector>

namespace sf4e { namespace ui {

// One frame of the board: its rows, the focus, the press being handled and
// where a focused popout row's choices open.
struct RoomBoardFrame {
    const ShellView& v;
    const std::vector<MenuEntry>& rows;
    MenuNavigation& nav;
    MenuAction& action;
    const MenuVisualFeedback& feedback;
    // The focused row this frame, and whether the focus moved since the last.
    std::string focus;
    bool focusChanged;
    // GameMenu::bodyChoices: the focused popout row's box and the column its choices open in.
    decltype(GameMenu::bodyChoices)& choices;
    float s;

    // The row `id`, or null when the board has none this frame.
    const MenuEntry* Find(const char* id) const;
    // A row's box: a press is Select on it, the moving pointer focuses it.
    // True while hovered. With `choose` false a press only focuses it: your
    // own card, whose actions are its strip's buttons.
    bool Press(const MenuEntry& e, const v2::Box& b, bool choose = true) const;
    // A box's ground and edge on the board.
    void Ground(ImDrawList* d, const v2::Box& b, ImU32 fill, ImU32 edge) const;
    // A panel's ground: over a watched game the game shows through it.
    void Panel(ImDrawList* d, const v2::Box& b) const;
    // A panel's Hide (Show once folded): a small row button whose top right
    // corner is at `corner`. Returns its width.
    float HideButton(ImDrawList* d, const MenuEntry& e, ImVec2 corner) const;
};

} }
