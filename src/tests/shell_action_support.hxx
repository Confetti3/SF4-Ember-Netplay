#pragma once
// Reading what the menu sent in a test: which command an action is
// (ui::ShellAction, a variant of netplay/RuntimeCommands.hxx's commands and
// the overlay's own steps), and the few fields tests ask about most.
#include "../ui/ShellAction.hxx"
#include <variant>
#include <vector>

namespace sf4e { namespace test {

template <class Command> const Command* As(const ui::ShellAction& action) { return std::get_if<Command>(&action); }
template <class Command> bool Is(const ui::ShellAction& action) { return std::holds_alternative<Command>(action); }
// A Command for which `test` (called with the command) holds; false for any other.
template <class Command, class Test> bool Is(const ui::ShellAction& action, Test&& test) {
    const auto* sent = As<Command>(action);
    return sent && test(*sent);
}
// The newest Command among `actions`, or null when none was sent.
template <class Command> const Command* Last(const std::vector<ui::ShellAction>& actions) {
    for (auto it = actions.rbegin(); it != actions.rend(); ++it) if (const auto* sent = As<Command>(*it)) return sent;
    return nullptr;
}

// A room action of `kind`; false for any other command.
inline bool RoomActionIs(const ui::ShellAction& action, room::ActionKind kind) {
    const auto* sent = As<commands::RoomAction>(action);
    return sent && sent->roomAction.kind == kind;
}
// A Ready, or with `rematch` a Rematch.
inline bool ReadyIs(const ui::ShellAction& action, bool rematch = false) {
    const auto* sent = As<commands::Ready>(action);
    return sent && sent->rematch == rematch;
}
// A tournament or bridge request of `op`.
inline bool TournamentIs(const ui::ShellAction& action, netplay::tournament::Command::Op op) {
    const auto* sent = As<commands::Tournament>(action);
    return sent && sent->tournament.op == op;
}
// A controller request of `kind`.
inline bool PadIs(const ui::ShellAction& action, gamepads::ControllerRequest::Kind kind) {
    const auto* sent = As<commands::Pad>(action);
    return sent && sent->padAction.kind == kind;
}
// A service request of `service`.
inline bool ServiceIs(const ui::ShellAction& action, platform::ServiceAction service) {
    const auto* sent = As<commands::Service>(action);
    return sent && sent->service == service;
}

} }
