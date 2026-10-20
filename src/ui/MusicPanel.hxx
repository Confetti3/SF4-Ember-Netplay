#pragma once
#include "MenuNavigation.hxx"
#include "../netplay/PlayerPreferences.hxx"

namespace sf4e { namespace ui {
struct ShellView;
struct MusicPanel {
    static std::vector<MenuEntry> Rows(const netplay::PlayerPreferences& preferences, const ShellView& view);
    // The owning screen supplies actions; row ids are private to this panel.
    static bool Accept(const MenuAction& action, const ShellView& view, netplay::PlayerPreferences& preferences);
};
} }
