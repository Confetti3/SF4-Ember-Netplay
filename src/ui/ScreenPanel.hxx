#pragma once
#include "GameMenu.hxx"
#include "MenuNavigation.hxx"
#include <functional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
class ApplicationShell;
struct ShellView;
struct ShellAction;
enum class Tone;

// One drawn frame's screen, as the shell hands it to the panel that owns it.
struct ScreenFrame {
    ApplicationShell& shell;
    const ShellView& view;
    const std::string& screen;
    // The screen's rows as built for this frame; null while Rows builds them.
    const std::vector<MenuEntry>* rows = nullptr;
};
// How a screen without a Body lays its rows out: in columns of cards this tall.
struct ScreenGrid { int columns = 1; float cardHeight = 100; };

// A panel that owns screens: the shell asks it, not each panel in turn, for
// the parts of a drawn frame that belong to the screen on show. The shell's
// own screens (Settings, Online play, News, ...) are panels too
// (ApplicationShell::ScreenOf), so Draw itself is the frame loop. Its Update
// and its hidden-frame work are not here: the shell calls those on every
// panel, on every frame, whichever screen shows. The shell's own rules come
// first where they did before: the room's screens and shortcuts take their
// actions before any panel, and an Activate a panel does not take goes on to
// the shell's own rows.
class ScreenPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    virtual ~ScreenPanel() = default;
    virtual bool Owns(const std::string& screen) const = 0;
    // The screen's rows; `title` comes in as Home's.
    virtual std::vector<MenuEntry> Rows(const ScreenFrame& frame, bool idle, std::string& title) const = 0;
    // The panel's word on the status line, after the shell's own reports and
    // before its errors, which outrank it.
    virtual void Status(const ScreenFrame&, std::string&, Tone&) const {}
    // Select on a row, or Back on a screen that takes Back itself
    // (GameMenu::ownsBack). False: the row is not the panel's, and the shell's
    // own rows are tried next.
    virtual bool Activate(const ScreenFrame&, const MenuAction&, const Submit&) const { return false; }
    // Left or Right on a value row, or accepted text. False: the shell's own
    // rows are tried next.
    virtual bool Adjust(const ScreenFrame&, const MenuAction&) const { return false; }
    // An option chosen from a row's list. False: the shell's own rows are tried next.
    virtual bool Choose(const ScreenFrame&, const MenuAction&, const Submit&) const { return false; }
    // The Options button or key.
    virtual void Shortcut(const ScreenFrame&, const MenuAction&) const {}
    // What paints the screen's rows and its detail pane; empty for plain ones.
    virtual GameMenu::Card Card(const ScreenFrame&, double) const { return {}; }
    virtual GameMenu::Detail Detail(const ScreenFrame&) const { return {}; }
    // What draws the screen's rows itself (a board); empty for the menu's list.
    virtual GameMenu::Body Body(const ScreenFrame&) const { return {}; }
    virtual ScreenGrid Grid(const ScreenFrame&) const { return {}; }
    // The menu around the rows, after the shell has set the frame's defaults:
    // the header's eyebrow and chips, the tabs, the list's and pane's widths,
    // Back's words.
    virtual void Dress(const ScreenFrame&, GameMenu&) const {}
    // The screen edits saved preferences, so the status line says how their
    // save stands (Saving, Saved).
    virtual bool ReportsSave(const std::string&) const { return false; }
};
} }
