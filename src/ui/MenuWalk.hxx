#pragma once
#include "MenuNavigation.hxx"
#include <functional>
#include <string>

namespace sf4e { namespace ui {
// A walk through the menus by the keys a player presses: Home, Settings, a
// panel and a value's popup, Online play, a room created and its member popout, the room
// left, then Training from Home. Each frame it is told where the menu is and
// gives the buttons to hold. The in-game menu self-test (sf4e__SelfTest__Menu.cxx)
// and the UI lab (--walk) run it; each step's outcome goes to `say`.
class MenuWalk {
public:
    using Say = std::function<void(bool pass, const std::string& step, const std::string& detail)>;
    unsigned Frame(const MenuNavigation& nav, const Say& say);
    bool Done() const { return done_; }
    bool Failed() const { return failed_; }
private:
    unsigned Press(unsigned button);
    void Next(const MenuNavigation& nav, const Say& say);
    unsigned Fail(const std::string& detail, const Say& say);
    std::size_t step_ = 0;
    bool seeking_ = true, closing_ = false, done_ = false, failed_ = false;
    int frames_ = 0, presses_ = 0, hold_ = 0, rest_ = 0;
    unsigned holding_ = 0, direction_ = MenuInput::Down;
    std::string from_, before_;
};
} }
