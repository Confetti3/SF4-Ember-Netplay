#pragma once
#include "ShellAction.hxx"
#include "GameMenu.hxx"
#include "MenuNavigation.hxx"
#include "CardSets.hxx"
#include "PlayerCard.hxx"
#include "WatchQueue.hxx"
#include <functional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;

// Profile (docs/design/PROFILE.md): the player's own pages under one strip,
// Card and Fighters here, Friends (FriendsPanel) and the awards wall
// (ApplicationShell__Awards.cxx), LB and RB stepping through them. Home's card opens it on Card.
//
// The levels are read from the helper when the player opens any of them, and
// once at start for the service and Ember ID the interface can use, as
// RankingsPanel reads the standing for the card on Home. A read the queue
// refused is asked again next frame; one that went out and failed is not
// asked again until Profile is opened.
class ProfilePanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    static constexpr const char* CardScreen = "card";
    static constexpr const char* FightersScreen = "fighters";
    static constexpr const char* FriendsScreen = "friends";
    // The Sets tab and another player's profile page (PlayerPage): the screens All sets and Most played open.
    static constexpr const char* SetsScreen = "sets";
    static constexpr const char* PlayerScreen = "player";
    static bool Owns(const std::string& screen) { return screen == CardScreen || screen == FightersScreen; }
    // Every screen under the strip, Friends and the awards wall included.
    static bool InStrip(const std::string& screen) { return Owns(screen) || screen == FriendsScreen || screen == SetsScreen || screen == "awards"; }
    // The strip over `screen` and the eyebrow "Profile · name". Friends says
    // how many requests wait (the published card's count).
    static void Dress(GameMenu& menu, const std::string& screen, const std::string& name);
    // Every frame: the levels read on opening and at start. `bridge` is the
    // usable service, empty without one.
    void Update(const ShellView& view, const std::string& screen, const std::string& bridge, const Submit& submit);
    // The shell is hidden: a read not sent yet is dropped, and showing it again reads afresh.
    void Conceal();
    // The screen's rows, from the card the shell published (`card`), the
    // fighter read for `main` and whether the player can change it now.
    // `connect`: no Discord account is linked (IdentityPanel::Discord), so the way to one ends the rows.
    std::vector<MenuEntry> Rows(const ShellView& view, const netplay::PlayerPreferences& preferences, const std::string& screen, const PlayerCardView& card,
        bool editable, bool connect, std::string& title) const;
    // Select on a row of the Card tab that is the card's own (a strip tile, an award tile, Most played, a set, All sets, Edit
    // card). False: not this panel's. `queue` is the shared set queue a watched set fills.
    bool Activate(const ShellView& view, const MenuAction& action, MenuNavigation& navigation, const Submit& submit, WatchQueue& queue, std::string& error);
    // The player's own subject for a card (CardModel.hxx) from the view and the preferences being edited, and the Panel it
    // makes as others see it (Edit card's and the Awards tab's preview).
    static player_card::Subject OwnSubjectOf(const ShellView& view, const netplay::PlayerPreferences& preferences);
    static OtherCardView OwnPanel(const ShellView& view, const netplay::PlayerPreferences& preferences);
    // The screen's board at the cursor, in a body `height` tall: the Card tab
    // (PlayerCard.cxx) or the Fighters tab.
    void Draw(const ShellView& view, const netplay::PlayerPreferences& preferences, const std::string& screen, const std::vector<MenuEntry>& rows, MenuNavigation& navigation, MenuAction& action, float height);
private:
    // The own subject with the recent sets and the hero's buttons: the Full card's data.
    player_card::Subject SubjectOf(const ShellView& view, const netplay::PlayerPreferences& preferences) const;
    CardSets sets_;
    std::string bridge_;
    std::string read_, focus_;
    bool visiting_ = false, want_ = false;
};
} }
