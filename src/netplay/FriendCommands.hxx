#pragma once
// What the interface asks of friends: one type per request, each holding only
// what that request carries, taken by the runtime like every other command
// (RuntimeCommands.hxx). The interface reads the answer from ShellView::friends
// and sends nothing else (docs/design/FRIENDS.md, "Client").
//
// Every command names the service (`bridge`, the one the menu reads levels
// from). Commands that act on a row name it by the id its state row carries
// (a friend's "fr_...", a request's "rq_...", an invite's "iv_...", a block's
// "bl_..."), never by position, so a press made on an older state still means
// that row or nothing.
//
//   FriendsVisit         The Friends tab opened: ask for the state at once, and
//                        keep it fresh every 10 s until FriendsLeave.
//   FriendsLeave         The Friends tab closed.
//
// The bridge's actions (FriendAction below), of which one at a time is in
// flight:
//   FriendRequest        Ask the room member whose card token this is; `label`
//                        is the member's room name, which the sender sees
//                        afterwards.
//   FriendRequestByCode  Ask the player with this friend code, as typed (any
//                        case, spaces and dashes; the bridge reads it).
//   FriendAccept, FriendDecline(block), FriendCancel   Answer, or take back, a request.
//   FriendRemove(block)  End a friendship, silently; with `block`, also block.
//   FriendBlock          Block a room member by card token.
//   FriendUnblock        Lift a block.
//   FriendNewCode        A new friend code, at most once a day.
//   FriendAppearOffline  Friends see this player as offline (or not).
//   FriendTurnOff        Turn friends off: the bridge deletes the player's
//                        friends and requests and retires the friend code (the
//                        blocks stay), and answers "friends_off" from then on,
//                        until FriendTurnOn.
//   FriendTurnOn         Make friends again for a player who turned them off.
//   FriendInvite         Offer a friend this player's room (public or private):
//                        the runtime works out what can be offered, and refuses
//                        when nothing can (ShellView::friends.canInvite says).
//   FriendDismiss        Drop an invite received.
//
// Going to a friend, which does not use the one action the bridge takes at a
// time, so it never refuses for it, and is dropped once the room generation
// the press saw has moved on:
//   FriendJoin, FriendWatch   Go to a friend's public room, and for Watch to
//                        their table. The runtime asks first when this PC is in
//                        a room or a game (ShellView::friends.visit).
//   FriendTakeInvite     Go where an invite says: a public room as Join, a
//                        private room as a Discord invitation is taken.
//   FriendAnswerVisit    The player's answer to that question, for the revision
//                        shown.
#include "SessionController.hxx"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace sf4e { namespace commands {

namespace friend_limits {
constexpr std::size_t Bridge = 256, Id = 64, Card = 128, Label = 64, Code = 32;
}

// The cadence of the Friends tab.
struct FriendsVisit {
	std::string bridge;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge; }
	std::size_t Bytes() const { return bridge.size(); }
};
struct FriendsLeave {
	std::string bridge;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge; }
	std::size_t Bytes() const { return bridge.size(); }
};

// Each of the bridge's actions names itself for the log (LogName): its kind, never whom or which row.
struct FriendRequest {
	static constexpr const char* LogName = "request by card";
	std::string bridge, card, label;
	// The member asked first (the interface showed Accept request): the bridge's answer makes them friends.
	bool mutual = false;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && card.size() <= friend_limits::Card && label.size() <= friend_limits::Label; }
	std::size_t Bytes() const { return bridge.size() + card.size() + label.size(); }
};
struct FriendRequestByCode {
	static constexpr const char* LogName = "request by code";
	std::string bridge, code;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && code.size() <= friend_limits::Code; }
	std::size_t Bytes() const { return bridge.size() + code.size(); }
};
struct FriendAccept {
	static constexpr const char* LogName = "accept";
	std::string bridge, request;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && request.size() <= friend_limits::Id; }
	std::size_t Bytes() const { return bridge.size() + request.size(); }
};
struct FriendDecline {
	static constexpr const char* LogName = "decline";
	std::string bridge, request;
	bool block = false;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && request.size() <= friend_limits::Id; }
	std::size_t Bytes() const { return bridge.size() + request.size(); }
};
struct FriendCancel {
	static constexpr const char* LogName = "cancel";
	std::string bridge, request;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && request.size() <= friend_limits::Id; }
	std::size_t Bytes() const { return bridge.size() + request.size(); }
};
struct FriendRemove {
	static constexpr const char* LogName = "remove";
	std::string bridge, friendId;
	bool block = false;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && friendId.size() <= friend_limits::Id; }
	std::size_t Bytes() const { return bridge.size() + friendId.size(); }
};
struct FriendBlock {
	static constexpr const char* LogName = "block";
	std::string bridge, card, label;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && card.size() <= friend_limits::Card && label.size() <= friend_limits::Label; }
	std::size_t Bytes() const { return bridge.size() + card.size() + label.size(); }
};
struct FriendUnblock {
	static constexpr const char* LogName = "unblock";
	std::string bridge, blockId;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && blockId.size() <= friend_limits::Id; }
	std::size_t Bytes() const { return bridge.size() + blockId.size(); }
};
struct FriendNewCode {
	static constexpr const char* LogName = "new code";
	std::string bridge;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge; }
	std::size_t Bytes() const { return bridge.size(); }
};
struct FriendAppearOffline {
	static constexpr const char* LogName = "appear offline";
	std::string bridge;
	bool on = false;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge; }
	std::size_t Bytes() const { return bridge.size(); }
};
struct FriendTurnOff {
	static constexpr const char* LogName = "turn off";
	std::string bridge;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge; }
	std::size_t Bytes() const { return bridge.size(); }
};
struct FriendTurnOn {
	static constexpr const char* LogName = "turn on";
	std::string bridge;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge; }
	std::size_t Bytes() const { return bridge.size(); }
};
struct FriendInvite {
	static constexpr const char* LogName = "invite";
	std::string bridge, friendId;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && friendId.size() <= friend_limits::Id; }
	std::size_t Bytes() const { return bridge.size() + friendId.size(); }
};
struct FriendDismiss {
	static constexpr const char* LogName = "dismiss";
	std::string bridge, invite;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && invite.size() <= friend_limits::Id; }
	std::size_t Bytes() const { return bridge.size() + invite.size(); }
};

// The bridge's actions (friends_act): one at a time is in flight. This list is
// what makes a command one of them; the wire builder has an overload for each
// (FriendWire.hxx).
using FriendAction = std::variant<FriendRequest, FriendRequestByCode, FriendAccept, FriendDecline, FriendCancel, FriendRemove, FriendBlock,
	FriendUnblock, FriendNewCode, FriendAppearOffline, FriendTurnOff, FriendTurnOn, FriendInvite, FriendDismiss>;

// A Join, a Watch or an invite, made for the room the player saw.
struct FriendJoin {
	netplay::Generation generation;
	std::string bridge, friendId;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && friendId.size() <= friend_limits::Id; }
	std::size_t Bytes() const { return bridge.size() + friendId.size(); }
};
struct FriendWatch {
	netplay::Generation generation;
	std::string bridge, friendId;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && friendId.size() <= friend_limits::Id; }
	std::size_t Bytes() const { return bridge.size() + friendId.size(); }
};
struct FriendTakeInvite {
	netplay::Generation generation;
	std::string bridge, invite;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge && invite.size() <= friend_limits::Id; }
	std::size_t Bytes() const { return bridge.size() + invite.size(); }
};
// The answer to the question a visit asks: leave for the friend's room (`switchRoom`) or stay.
struct FriendAnswerVisit {
	netplay::Generation generation;
	std::string bridge;
	std::uint64_t revision = 0;
	bool switchRoom = false;
	bool Valid() const { return bridge.size() <= friend_limits::Bridge; }
	std::size_t Bytes() const { return bridge.size(); }
};

namespace detail {
template <class Command, class = void> struct HasBounds : std::false_type {};
template <class Command> struct HasBounds<Command, std::void_t<decltype(std::declval<const Command&>().Valid(), std::declval<const Command&>().Bytes())>> : std::true_type {};
}
// Whether a command states its own bounds (the friends commands do; the queue checks them).
template <class Command> constexpr bool HasBounds = detail::HasBounds<Command>::value;

// Whether a command is one of the bridge's actions: a property of its type, by its place in FriendAction.
namespace detail {
template <class T, class Variant> struct InVariant : std::false_type {};
template <class T, class... Alternatives> struct InVariant<T, std::variant<Alternatives...>> : std::bool_constant<(std::is_same_v<T, Alternatives> || ...)> {};
}
template <class Command> constexpr bool IsFriendAction = detail::InVariant<Command, FriendAction>::value;
// The friends commands that are not: the tab's cadence, and going to a friend.
template <class Command> constexpr bool IsFriendVisit = std::is_same_v<Command, FriendsVisit> || std::is_same_v<Command, FriendsLeave> ||
	std::is_same_v<Command, FriendJoin> || std::is_same_v<Command, FriendWatch> || std::is_same_v<Command, FriendTakeInvite> ||
	std::is_same_v<Command, FriendAnswerVisit>;

// The service a friends action names.
inline const std::string& BridgeOf(const FriendAction& action) {
	return std::visit([](const auto& command) -> const std::string& { return command.bridge; }, action);
}

} }
