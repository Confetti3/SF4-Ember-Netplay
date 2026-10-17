#pragma once
#include "GameMenu.hxx"
#include "../common/DisplayPreferences.hxx"
#include <functional>

namespace sf4e { namespace ui {
class DisplayPanel {
public:
    using Saver = std::function<bool(const display::Preferences&, std::string&)>;
    DisplayPanel(std::vector<display::Monitor> monitors, display::Preferences saved, Saver saver);
    std::vector<MenuEntry> Rows() const;
    void Handle(const MenuAction& action);
    void Draw(GameMenu& menu);
    void Discard();
    void Refresh(std::vector<display::Monitor> monitors) { monitors_ = std::move(monitors); }
    const display::Preferences& Draft() const { return draft_; }
    const display::Preferences& Saved() const { return saved_; }
    const std::string& Error() const { return error_; }
    bool RefreshRequested() { bool value = refresh_; refresh_ = false; return value; }
private:
    std::vector<display::Monitor> monitors_;
    display::Preferences saved_, draft_;
    Saver saver_;
    std::string error_;
    bool refresh_ = false, savedNotice_ = false;
};
} }
