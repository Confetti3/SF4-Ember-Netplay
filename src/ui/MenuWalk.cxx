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
// `path`: a later step needs it done (it moves the menu on), so a walk started
// past it still runs it; a detour leaves the menu where it found it.
struct Step { const char* name; const char* focus; const char* screen; Kind kind; int seconds; bool path; };
// A focus ending in '-' matches any row it starts (the first member: "member-").
const Step Steps[] = {
    {"home", nullptr, "home", Kind::Arrive, 20, true},
    {"settings", "settings", "tab-player", Kind::Open, 5, false},
    {"settings panel", "tab-access/text", "tab-access/text", Kind::Open, 5, false},
    {"value popup", "language", nullptr, Kind::Popup, 5, false},
    {"settings back", nullptr, "home", Kind::Back, 5, false},
    {"online", "online", "online", Kind::Open, 5, true},
    {"create", "create", "create", Kind::Open, 5, true},
    {"room", "host", "room", Kind::Open, 60, true},
    {"member popout", "member-", nullptr, Kind::Popup, 5, false},
    {"leave room", "leave", nullptr, Kind::Confirm, 30, true},
    {"back home", nullptr, "home", Kind::Back, 5, true},
    {"training", "training", nullptr, Kind::Open, 5, true},
};
// A press is held four frames, then nothing for eight, so the menu sees each once.
constexpr int Hold = 4, Rest = 8, Second = 60;
bool Matches(const std::string& id, const char* focus) {
    const std::size_t n = std::strlen(focus);
    return focus[n - 1] == '-' ? id.compare(0, n, focus) == 0 : id == focus;
}
}

bool MenuWalk::StartAt(const std::string& step) {
    for (std::size_t i = 0; i < std::size(Steps); ++i) {
        if (step != Steps[i].name) continue;
        start_ = i; step_ = 0;
        while (step_ < start_ && !Steps[step_].path) ++step_;
        return true;
    }
    return false;
}

unsigned MenuWalk::Press(unsigned button) { holding_ = button; hold_ = Hold; rest_ = Rest; return button; }

unsigned MenuWalk::Fail(const std::string& detail, const Say& say) {
    // A step on the way to the start fails as itself: the rerun did not get there.
    say(false, Steps[step_].name, detail);
    done_ = failed_ = true;
    return 0;
}

void MenuWalk::Next(const MenuNavigation& nav, const Say& say) {
    // Steps on the way to the start are not said: the run reports from where it began.
    if (step_ >= start_) say(true, Steps[step_].name, nav.Screen() + (nav.Focus().empty() ? "" : ", " + nav.Focus()));
    seeking_ = true; closing_ = false; frames_ = presses_ = 0; before_.clear(); direction_ = MenuInput::Down;
    // Before the start only the steps later ones need run.
    do ++step_; while (step_ < start_ && !Steps[step_].path);
    done_ = step_ == std::size(Steps);
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
        if (++presses_ > 150) return Fail(std::string("no row ") + s.focus + " on " + nav.Screen(), say);
        // Down walks the column, then Up from where it stopped; then the next tab, or on a
        // room's board the columns to the right and to the left (Right on a setting would
        // change it), and then the next tab: a narrow board shows Members and Chat behind RB.
        const std::string at = nav.Screen() + "/" + nav.Focus();
        const bool board = nav.Screen().compare(0, 4, "room") == 0;
        // On a board, Left after Right: Members and Chat sit right of the tables and
        // their action bar (Leave room), so a walk that starts in Members goes back left.
        if (at == before_) direction_ = direction_ == B::Down ? B::Up : direction_ == B::Up ? (board ? B::Right : B::NextTab) :
            direction_ == B::Right ? (board ? B::Left : B::NextTab) : direction_ == B::Left ? B::NextTab : B::Down;
        // Right that moved keeps going: a board's cards are a grid, and walking each
        // column down and up again before the next ran out of presses before Members.
        else if (direction_ != B::Up && direction_ != B::Right && direction_ != B::Left) direction_ = B::Down;
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
