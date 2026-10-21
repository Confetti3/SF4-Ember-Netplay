#pragma once
#include "MenuNavigation.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../netplay/Rankings.hxx"
#include "../netplay/TournamentStatus.hxx"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;
struct ShellAction;
enum class Tone;

// Rankings (docs/design/SKILL_BADGE.md, "Rankings"): the player's own class and
// their choice to take part, and the board of players by class. It works from
// the service the Ember ID panel makes usable, like public rooms.
//
// The bridge counts every ranking request against one small budget, and reading
// the standing marks the class as shown, so nothing here refreshes by itself:
// the standing is read when the player opens Rankings, the board when they open
// it, and either again on Options. While the player takes part and classes are
// shown, opening Rankings also sends the name and flag rooms show for them, when
// the board lists them otherwise: nobody is listed before seeing this page.
class RankingsPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    static constexpr const char* Screen = "rankings";
    static constexpr const char* BoardScreen = "rankings-board";
    static bool Owns(const std::string& screen) { return screen == Screen || screen == BoardScreen; }
    // A refused request or a failed read, by its stable code, in words.
    static std::string FailureText(const std::string& code);
    // How this player is listed: the name and flag rooms show for them.
    static netplay::rankings::Listing OwnListing(const netplay::PlayerPreferences& preferences, const CountryCode& automaticCountry);
    // The Online play row's value: the last standing this session from `bridge`,
    // or empty before Rankings has read one.
    static std::string EntryValue(const ShellView& view, const std::string& bridge);
    // Every frame: asks for what entering a screen or pressing Options wants, sends
    // the listing and the player's choice, and takes their answers. `bridge` is
    // the usable service, empty without one; `own` is OwnListing.
    void Update(const ShellView& view, const std::string& screen, const std::string& bridge,
                const netplay::rankings::Listing& own, const Submit& submit);
    // `identityPending` says the Ember ID is still being asked about.
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& screen, bool identityPending, std::string& title) const;
    // True when the row was the panel's own. Connect Discord is the Ember ID panel's.
    bool Activate(const MenuAction& action, const ShellView& view, MenuNavigation& navigation);
    // The Options button or key (Y, T) reads the screen's answer again.
    bool Shortcut(const MenuAction& action);
    bool Refreshable() const { return Owns(lastScreen_) && !bridge_.empty(); }
    // What a request in flight or the board's own state says on `screen`.
    bool Status(const ShellView& view, const std::string& screen, std::string& status, Tone& tone) const;
    // What the panel has to say, once.
    bool TakeSaid(std::string& text);
    // The shell is hidden: what was not asked yet is dropped, a choice on its
    // way is kept, and showing it again reads afresh.
    void Conceal();
private:
    // The one choice the panel acts on, identified by Command::request as
    // public rooms identify theirs.
    struct Pending {
        netplay::tournament::Command command;
        bool sent = false;
    };
    // The standing from the usable service, when one has arrived.
    const netplay::rankings::Standing* Current(const ShellView& view) const;
    std::vector<MenuEntry> StandingRows(const ShellView& view, bool identityPending) const;
    std::vector<MenuEntry> BoardRows(const ShellView& view) const;
    void Choose(bool ranked);
    void TakeAnswers(const ShellView& view, bool owned);
    void Say(std::string text) { said_ = std::move(text); }

    std::string bridge_, lastScreen_, said_;
    netplay::rankings::Listing own_;
    bool wantStanding_ = false, wantBoard_ = false;
    // The visit sent its listing already, so an answer that still differs cannot loop.
    bool listingSent_ = false;
    std::optional<Pending> pending_;
    std::uint64_t lastRequest_ = 0;
    // The finished counts and standing revision last handled, so each answer is
    // acted on once. How old a value is comes with the status.
    std::uint64_t standingSeen_ = 0, boardSeen_ = 0, standingRevision_ = 0;
};
} }
