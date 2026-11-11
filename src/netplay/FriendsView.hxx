#pragma once
// What the interface sees of friends, as ShellView::friends: read it, and send
// only the commands of FriendCommands.hxx (commands::Friends). It owns its
// values, so the drawing thread holds it across a frame; the state is shared
// and replaced, never changed, so copying a View is cheap.
//
// Nothing here is asked of the interface to keep: every frame it reads
// `state` for the lists, `busy` for whether another action may be sent,
// `refusal` for the words of the last action's failure, `canInvite` for whether
// "Invite to my room" can be offered, and `visit` for the question a Join, a
// Watch or an invite asks when the player is in a room or a game.
#include "FriendCommands.hxx"
#include "Friends.hxx"

#include <cstdint>
#include <optional>
#include <string>

namespace sf4e { namespace netplay { namespace friends {

struct View {
	// The service the commands name: the one the state is from, empty before one is known.
	std::string bridge;
	// The friends, as the bridge last answered: `state->kind` says whether
	// there are any (Listed), none can be (NoAccount: Settings, Online,
	// Discord), or the service has no friends (Unavailable). `loading` is a
	// request in flight, `error` the helper's code when the last failed (the
	// state it kept stays), `revision` moves with every new state.
	Answer state;
	// An action is in flight or held: ask no other (it would be refused).
	bool busy = false;
	// Which action, while one is in flight.
	std::optional<Command::Kind> acting;
	// The last action's failure: the helper's code, and the words for it in the
	// player's language; empty when the last action did not fail. `refusals`
	// counts them, so a toast is shown once for each.
	std::string refusal, refusalText;
	std::uint64_t refusals = 0;
	// The player's room can be offered to a friend now (public or private).
	bool canInvite = false;
	// Syncing at the tab's cadence (Visit sent, Leave not).
	bool visiting = false;

	// A Join, a Watch or an invite waiting on the player. `confirm`: the player
	// is in a room or a game and is asked whether to leave it for `name`'s room
	// (answer with Command::AnswerVisit(bridge, revision, switch)). Private
	// rooms' invitations are asked as a Discord invitation is
	// (ShellView::discordPending).
	struct VisitQuestion {
		bool active = false, confirm = false, watch = false;
		std::string name;
		std::uint64_t revision = 0;
	} visit;
};

} } }
