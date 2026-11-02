#pragma once
#include "MenuNavigation.hxx"
#include "ReplaysView.hxx"
#include "../platform/ReplaySets.hxx"
#include "../common/ReplayRequest.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include <cstdint>
#include <functional>
#include <imgui.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace ui {
// What the list's Show row lists (ReplaySummaryText.hxx).
enum class ReplayShow;
struct ShellView;
struct ShellAction;
enum class Tone;

// Ember's replay archive: the Replays list and what Select offers on a row
// (watch, add to the game's list, export a video, read its inputs, show its
// file in Explorer), the export's caption page, a running export's progress
// and Cancel, the Inputs and stats screen, and the question a replay link
// asks. It reads ShellView::replays as the runtime published it;
// the archive's listing and an entry's detail are made off the drawing
// thread by workers the caller owns, which Wanted says to ask.
class ReplaysPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    static bool Owns(const std::string& screen) { return screen == "replays" || screen == "replay-inputs" || screen == "replay-export" || screen == "replay-sets" || screen == "replay-set"; }
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
    // Once a set's video is made, the set's next game is exported by itself.
    // Sets (replay-sets) lists the sets the archive holds, and a set's own
    // screen (replay-set) its games (platform/ReplaySets.hxx).
    void Update(const ShellView& view, const netplay::PlayerPreferences& saved, MenuNavigation& navigation, const Submit& submit);
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& screen, bool idle, std::string& title) const;
    // True when the row was the panel's own. `error` is what the shell shows
    // when a request could not be queued.
    // `saved` is the shell's copy of the player's preferences, which an export's
    // caption page starts from (OpenExport).
    bool Activate(const MenuAction& action, const ShellView& view, const netplay::PlayerPreferences& saved, MenuNavigation& navigation, const Submit& submit, std::string& error);
    // A caption page's value or text. An on/off row returns the preference it
    // leaves for the next export, which the shell saves; anything else, null.
    bool netplay::PlayerPreferences::* Accept(const MenuAction& action);
    // An option chosen on a replay's row or on the link's question.
    void Choose(const MenuAction& action, const ShellView& view, const netplay::PlayerPreferences& saved, MenuNavigation& navigation, const Submit& submit, std::string& error);
    // The last request's outcome on the Replays screen (Show in folder's, or
    // the game's), once nothing else has anything to say: only an empty
    // `status` takes it.
    bool Status(const ShellView& view, const std::string& screen, std::string& status, Tone& tone) const;
    // The tabs over Replays (Matches, Sets when there are any, Export video
    // with a replay to export), and the Export video tab's caption, of the
    // focused row's replay, else the newest unless one is set up already.
    std::vector<std::pair<std::string, std::string>> Tabs(const ShellView& view) const;
    void ExportFrom(const ShellView& view, const netplay::PlayerPreferences& saved, const std::string& focus);
    // A match's or set's card (the players and score, the fighters, its
    // tags and date) and pane (its facts and Watch now, which opens the row's
    // choices); on Export video the pane is the caption over a small screen.
    bool DrawCard(const ShellView& view, const MenuEntry& row, ImVec2 min, ImVec2 max) const;
    void DrawDetail(const ShellView& view, const std::string& screen, const std::string& id, MenuNavigation& navigation, const std::vector<MenuEntry>& rows) const;
    // The replay file the Inputs and stats screen wants read, as its row named it.
    const std::string& InputsFile() const { return inputsFile_; }
    std::uint64_t InputsRevision() const { return inputsRevision_; }
private:
    void InputsRows(const ShellView& view, std::vector<MenuEntry>& rows) const;
    MenuEntry ReplayRow(const ShellView& view, const platform::replays::ArchivedReplay& replay, bool idle) const;
    void Watch(const ShellView& view, const std::string& path, bool meter, const Submit& submit, std::string& error);
    void OpenExport(const ShellView& view, const netplay::PlayerPreferences& saved, const platform::replays::ArchivedReplay& replay);
    void NextVideo(const ShellView& view, const netplay::PlayerPreferences& saved, MenuNavigation& nav);
    void QueueSet(const platform::replays::ArchivedSet& set, bool exporting, std::uint64_t videos);
    bool Generate(const ShellView& view, const Submit& submit);
    // The replay the Inputs and stats screen is of, as its row was when it
    // was chosen. The lister hands over its match, summary and round logs;
    // drawing the screen never reads, parses or counts the replay.
    std::string inputsFile_;
    std::uint64_t inputsRevision_ = 0;
    // The replay and caption sent when Generate video is chosen, with the
    // frame meter when its row is on. Its on/off parts start from the
    // player's saved choices (OpenExport).
    std::string exportPath_;
    replay::Caption caption_;
    bool meter_ = false;
    // The list's filter (ReplaySummaryText.hxx: ReplayShown): what Show lists,
    // and the Search text a player's or fighter's name must contain.
    ReplayShow show_{}; // All
    std::string search_;
    // The archive's sets (ReplaySets.hxx), grouped when Update sees a new
    // listing. The listing is held beside them, as the sets' games point into it.
    std::shared_ptr<const std::vector<platform::replays::ArchivedReplay>> setsFrom_;
    std::vector<platform::replays::ArchivedSet> sets_;
    // Show in folder was the last thing asked here: the status line has its answer.
    bool folderAsked_ = false;
    // The replay link last seen, so its question opens the Replays screen once.
    std::string linkSeen_;
    // Watch set: the set's games still to play, each started from the Next
    // game row on Replays, and how many the set has.
    std::vector<std::string> setQueue_;
    std::size_t setGames_ = 0;
    // The queue makes videos (set-export), not watches. The game at its front
    // stays there until its video is made: Sent says its export was accepted
    // and its video is awaited (the videos saved by then are videosSeen_);
    // Advance, that it was made and the next game's export goes by itself.
    // Either way a refused or failed export leaves the game to the Next video row.
    enum class SetStep { Manual, Sent, Advance };
    bool setExport_ = false;
    SetStep setStep_ = SetStep::Manual;
    std::uint64_t videosSeen_ = 0;
    // The set whose games replay-set lists.
    std::string setShown_;
    // A set pack was last asked for here (Save set pack, a pack link's Import):
    // the status line has its answer once the notice count passes this one.
    bool setAsked_ = false;
    std::uint64_t setNoticeAsked_ = 0;
};
} }
