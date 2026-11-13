#pragma once
// A toast, as a feature asks the shell to show it: what it says, and the buttons
// it offers. The shell only shows these (ApplicationShell::Say); what a feature
// says, and when, is its own (FriendsPanel::TakeToast).
#include "ShellAction.hxx"
#include "Theme.hxx"
#include <string>
#include <vector>

namespace sf4e { namespace ui {

// What a toast is about, said over it (Toasts board); None says nothing.
enum class ToastKind { None, Invitation, Room, Update, Match, System };
// A toast's button: it opens `screen` at its `row`, where the player acts on
// the notice, and sends `commands` as they are, in order (an invite's Join takes
// the invite and then drops it); one with neither is Later, which only puts the
// toast away.
struct ToastAction { std::string label, screen, row; std::vector<RuntimeRequest> commands; };
// How long a card stays up: a notice briefly, one with buttons long enough to reach them.
constexpr double ToastSeconds = 4;
constexpr double ActionToastSeconds = 8;
// A toast and how long its notice stays on the status line.
struct ToastRequest {
    std::string text;
    Tone tone = Tone::Neutral;
    double seconds = 0;
    ToastKind kind = ToastKind::None;
    std::vector<ToastAction> actions;
    // What it is about when something else lists that too (a friend's request or invite id, which Notifications lists
    // while it waits): its row there stands for the toast. Empty for any other.
    std::string key;
};

} }
