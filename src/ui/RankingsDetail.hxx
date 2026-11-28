#pragma once
// The standing page's own detail pane, as the site's Rating details page: your
// class, and your rating with where it stands among the classes.
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
