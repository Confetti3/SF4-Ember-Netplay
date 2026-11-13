#pragma once
#include "ShellAction.hxx"
#include "GameMenu.hxx"
#include "MenuNavigation.hxx"
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
    static bool Owns(const std::string& screen) { return screen == CardScreen || screen == FightersScreen; }
    // Every screen under the strip, Friends and the awards wall included.
    static bool InStrip(const std::string& screen) { return Owns(screen) || screen == FriendsScreen || screen == "awards"; }
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
    std::vector<MenuEntry> Rows(const ShellView& view, const std::string& screen, const PlayerCardView& card, bool editable, bool connect, std::string& title) const;
    // The screen's board at the cursor, in a body `height` tall: the Card tab
    // (PlayerCard.cxx) or the Fighters tab.
    void Draw(const ShellView& view, int main, const std::string& screen, const std::vector<MenuEntry>& rows, MenuNavigation& navigation, MenuAction& action, float height);
private:
    std::string read_, focus_;
    bool visiting_ = false, want_ = false;
};
} }
