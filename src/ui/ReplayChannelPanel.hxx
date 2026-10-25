#pragma once
#include "MenuNavigation.hxx"
#include "../netplay/ReplayChannel.hxx"
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

// The replay channel (docs/design/REPLAY_CHANNEL.md), from Replays: the
// player's choice to share their room games, and the channel's games newest
// first, which Select downloads and plays. It works from the service the
// Ember ID panel makes usable, like Rankings.
//
// The choice is read once a service is usable, wherever the player is, since
// the runtime uploads the player's games only once it knows they share them.
// The channel is read when the screen opens, on Options, and a page further
// on Load more.
class ReplayChannelPanel {
public:
    using Submit = std::function<bool(ShellAction)>;
    static constexpr const char* Screen = "replay-channel";
    static bool Owns(const std::string& screen) { return screen == Screen; }
    static std::string FailureText(const std::string& code);
    // Every frame: reads the choice for a newly usable `bridge`, asks for what
    // entering the screen or Options wants, sends a choice or a download, and
    // takes their answers. A downloaded game is then played when Watch asked
    // for it and the game can.
    void Update(const ShellView& view, const std::string& screen, const std::string& bridge, const Submit& submit);
    std::vector<MenuEntry> Rows(const ShellView& view, bool identityPending, bool idle, std::string& title) const;
    // True when the row was the panel's own. Connect Discord is the Ember ID panel's.
    bool Activate(const MenuAction& action, const ShellView& view, MenuNavigation& navigation);
    // The sharing choice and the fighter filter, by Left and Right.
    void Accept(const MenuAction& action);
    bool Shortcut(const MenuAction& action);
    bool Refreshable() const { return lastScreen_ == Screen && !bridge_.empty(); }
    bool Status(const ShellView& view, const std::string& screen, std::string& status, Tone& tone) const;
    bool TakeSaid(std::string& text, bool& error);
    void Conceal();
private:
    struct Pending {
        netplay::tournament::Command command;
        bool sent = false;
        // A download Watch asked for, played once it is written.
        bool watch = false;
    };
    const netplay::channel::Page* Current(const ShellView& view) const;
    void Say(std::string text, bool error) { said_ = std::move(text); saidError_ = error; }
    void TakeAnswers(const ShellView& view, const Submit& submit);

    std::string bridge_, lastScreen_, said_;
    bool saidError_ = false;
    // -1 for every fighter, else the native id either side played.
    int fighter_ = -1;
    bool wantPage_ = false, wantMore_ = false;
    std::optional<Pending> pending_;
    std::uint64_t lastRequest_ = 0, pageSeen_ = 0;
    // When the choice was last asked for, by the runtime's clock, to ask again
    // after a failure without asking every frame.
    std::uint64_t askedMs_ = 0;
};
} }
