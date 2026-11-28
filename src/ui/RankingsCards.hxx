#pragma once
// The rows of the Rankings standing page that carry a class emblem or a flag:
// your class's card and how the board lists you, and the state cards. They
// stay ordinary menu rows, so focus, dispatch and the render probe work as
// everywhere; these only paint them. The boards draw themselves (RankingsBoard).
#include "MenuNavigation.hxx"
#include <imgui.h>

namespace sf4e { namespace ui {
struct ShellView;
// Paints `entry` between `min` and `max`; false for a row the menu draws plain.
bool DrawRankingsRow(const ShellView& view, const MenuEntry& entry, ImVec2 min, ImVec2 max);
} }
