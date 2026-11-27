#pragma once
// Rankings' own detail pane, as the site's card and rating pages give the same
// answers: your class, your rating details, a points board and a player of
// either board.
#include "MenuNavigation.hxx"
#include <string>

namespace sf4e { namespace ui {
struct ShellView;
// Whether the focused row `id` takes this pane instead of the menu's title
// and text: false while the answer it would show is missing.
bool RankingsDrawsDetail(const ShellView& v, const std::string& id);
// The pane for the focused row `e`, at the detail pane's cursor.
void DrawRankingsDetail(const ShellView& v, const MenuEntry& e);
} }
