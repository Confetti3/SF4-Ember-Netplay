#include "MenuWalk.hxx"
#include <cstring>
#include <iterator>

namespace sf4e { namespace ui {
namespace {
// Arrive waits for `screen`; Open selects `focus` and waits for `screen` (none:
// the game takes over, as Training does); Popup selects `focus` until a choice
// opens, then closes it; Back presses Back until `screen`; Confirm selects
// `focus`, answers Yes and waits for the screen to change.
enum class Kind { Arrive, Open, Popup, Back, Confirm };
struct Step { const char* name; const char* focus; const char* screen; Kind kind; int seconds; };
// A focus ending in '-' matches any row it starts (the first member: "member-").
const Step Steps[] = {
    {"home", nullptr, "home", Kind::Arrive, 20},
    {"settings", "settings", "tab-player", Kind::Open, 5},
    {"settings panel", "tab-access/1", "tab-access/1", Kind::Open, 5},
    {"value popup", "language", nullptr, Kind::Popup, 5},
    {"settings back", nullptr, "home", Kind::Back, 5},
    {"online", "online", "online", Kind::Open, 5},
    {"create", "create", "create", Kind::Open, 5},
    {"room", "host", "room", Kind::Open, 60},
    {"member popout", "member-", nullptr, Kind::Popup, 5},
    {"leave room", "leave", nullptr, Kind::Confirm, 30},
    {"back home", nullptr, "home", Kind::Back, 5},
    {"training", "training", nullptr, Kind::Open, 5},
};
// A press is held four frames, then nothing for eight, so the menu sees each once.
constexpr int Hold = 4, Rest = 8, Second = 60;
bool Matches(const std::string& id, const char* focus) {
    const std::size_t n = std::strlen(focus);
    return focus[n - 1] == '-' ? id.compare(0, n, focus) == 0 : id == focus;
}
}

unsigned MenuWalk::Press(unsigned button) { holding_ = button; hold_ = Hold; rest_ = Rest; return button; }

unsigned MenuWalk::Fail(const std::string& detail, const Say& say) {
    say(false, Steps[step_].name, detail);
    done_ = failed_ = true;
    return 0;
}

void MenuWalk::Next(const MenuNavigation& nav, const Say& say) {
    say(true, Steps[step_].name, nav.Screen() + (nav.Focus().empty() ? "" : ", " + nav.Focus()));
    seeking_ = true; closing_ = false; frames_ = presses_ = 0; before_.clear(); direction_ = MenuInput::Down;
    done_ = ++step_ == std::size(Steps);
}

unsigned MenuWalk::Frame(const MenuNavigation& nav, const Say& say) {
    if (done_) return 0;
    ++frames_;
    if (hold_ > 0) { --hold_; return holding_; }
    if (rest_ > 0) { --rest_; return 0; }
    const Step& s = Steps[step_];
    using B = MenuInput;
    if (frames_ > (s.seconds + 30) * Second) return Fail("stuck on " + nav.Screen() + ", " + nav.Focus(), say);
    // Already there (a room joined while its row was being found): nothing to press.
    if (seeking_ && s.kind == Kind::Open && s.screen && nav.Screen() == s.screen) seeking_ = false;
    if (seeking_ && s.focus) {
        if (Matches(nav.Focus(), s.focus)) { seeking_ = false; frames_ = 0; from_ = nav.Screen(); return Press(B::Select); }
        if (++presses_ > 60) return Fail(std::string("no row ") + s.focus + " on " + nav.Screen(), say);
        // Down walks the column, then Up from where it stopped; then the next tab, or on a
        // room's board the next column (Right on a setting would change it).
        const std::string at = nav.Screen() + "/" + nav.Focus();
        const bool board = nav.Screen().compare(0, 4, "room") == 0;
        if (at == before_) direction_ = direction_ == B::Down ? B::Up : direction_ != B::Up ? B::Down : board ? B::Right : B::NextTab;
        else if (direction_ != B::Up) direction_ = B::Down;
        before_ = at;
        return Press(direction_);
    }
    seeking_ = false;
    const bool late = frames_ > s.seconds * Second;
    switch (s.kind) {
    case Kind::Arrive: case Kind::Open:
        if (!s.screen || nav.Screen() == s.screen) Next(nav, say);
        else if (late) return Fail("on " + nav.Screen() + ", not " + s.screen, say);
        return 0;
    case Kind::Back:
        if (nav.Screen() == s.screen) { Next(nav, say); return 0; }
        return late ? Fail("on " + nav.Screen() + ", not " + s.screen, say) : Press(B::Back);
    case Kind::Popup:
        if (closing_) { if (!nav.Asking()) Next(nav, say); return nav.Asking() ? Press(B::Back) : 0; }
        if (nav.Choosing()) { say(true, std::string(s.name) + " open", nav.DialogId()); closing_ = true; return Press(B::Back); }
        return late ? Fail("no choice opened on " + nav.Focus(), say) : 0;
    case Kind::Confirm:
        if (nav.Confirming()) return Press(nav.ConfirmSelected() ? B::Select : B::Right);
        if (nav.Screen() != from_) Next(nav, say);
        else if (late) return Fail("still on " + nav.Screen(), say);
        return 0;
    }
    return 0;
}
} }
