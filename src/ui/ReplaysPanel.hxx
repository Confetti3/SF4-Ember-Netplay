#pragma once
#include "MenuNavigation.hxx"
#include "ReplaysView.hxx"
#include "../common/ReplayRequest.hxx"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;
struct ShellAction;
enum class Tone;

// Ember's replay archive: the Replays list and what Select offers on a row
// (watch, add to the game's list, export a video, read its inputs), the
// export's caption page, the Inputs and stats screen, and the question a
// replay link asks. It reads ShellView::replays as the runtime published it;
// the archive's listing and an entry's detail are made off the drawing
// thread by workers the caller owns, which Wanted says to ask.
class ReplaysPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    static bool Owns(const std::string& screen) { return screen == "replays" || screen == "replay-inputs" || screen == "replay-export"; }
    // Home, then the Replays screen: a replay link's question, and the way
    // back once the game's battle log returns to the main menu.
    static void Show(MenuNavigation& navigation) { navigation.Home(); navigation.Push("replays"); }
    // What `screen` needs read: a fresh listing on the Replays screen, and on
    // Inputs and stats its file under the revision this entry was given.
    struct Wants {
        bool listing = false, detail = false;
        std::string detailFile;
        std::uint64_t detailRevision = 0;
    };
    Wants Wanted(const std::string& screen) const;
    // Every frame, before the rows: a new replay link takes the menu to its
    // question, only with no room open; in a room it waits until it is left.
    void Update(const ShellView& view, MenuNavigation& navigation);
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& screen, bool idle, std::string& title) const;
    // True when the row was the panel's own. `error` is what the shell shows
    // when a request could not be queued.
    bool Activate(const MenuAction& action, const ShellView& view, MenuNavigation& navigation, const Submit& submit, std::string& error);
    // A caption page's value or text.
    void Accept(const MenuAction& action);
    // An option chosen on a replay's row or on the link's question.
    void Choose(const MenuAction& action, const ShellView& view, MenuNavigation& navigation, const Submit& submit, std::string& error);
    // The last request's outcome on the Replays screen, once nothing else has
    // anything to say: only an empty `status` takes it.
    bool Status(const ShellView& view, const std::string& screen, std::string& status, Tone& tone) const;
    // The replay file the Inputs and stats screen wants read, as its row named it.
    const std::string& InputsFile() const { return inputsFile_; }
    std::uint64_t InputsRevision() const { return inputsRevision_; }
private:
    void InputsRows(const ShellView& view, std::vector<MenuEntry>& rows) const;
    void OpenExport(const ShellView& view, const platform::replays::ArchivedReplay& replay);
    // The replay the Inputs and stats screen is of, as its row was when it
    // was chosen. The lister hands over its match, summary and round logs;
    // drawing the screen never reads, parses or counts the replay.
    std::string inputsFile_;
    std::uint64_t inputsRevision_ = 0;
    // The replay and caption sent when Generate video is chosen.
    std::string exportPath_;
    replay::Caption caption_;
    // The replay link last seen, so its question opens the Replays screen once.
    std::string linkSeen_;
};
} }
