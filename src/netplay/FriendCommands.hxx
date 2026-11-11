#pragma once
// What the interface asks of friends: one Command, built by the named
// constructors below and sent as commands::Friends (RuntimeCommands.hxx). The
// interface reads the answer from ShellView::friends and sends nothing else
// (docs/design/FRIENDS.md, "Client").
//
// Every command names the service (`bridgeId`, the one the menu reads levels
// from). Commands that act on a row name it by the id its state row carries
// (a friend's "fr_...", a request's "rq_...", an invite's "iv_...", a block's
// "bl_..."), never by position, so a press made on an older state still means
// that row or nothing.
//
//   Visit          The Friends tab opened: ask for the state at once, and keep
//                  it fresh every 10 s until Leave.
//   Leave          The Friends tab closed.
//   Request(card)  Ask the room member whose card token this is; `label` is
//                  the member's room name, which the sender sees afterwards.
//   RequestCode    Ask the player with this friend code, as typed (any case,
//                  spaces and dashes; the bridge reads it).
//   Accept, Decline(block), Cancel   Answer, or take back, a request.
//   Remove(block)  End a friendship, silently; with `block`, also block.
//   Block(card)    Block a room member by card token.
//   Unblock        Lift a block.
//   NewCode        A new friend code, at most once a day.
//   AppearOffline  Friends see this player as offline (or not).
//   Forget         Turn friends off: everything the bridge keeps is deleted,
//                  and nothing is synced again until the next Visit.
//   Invite         Offer a friend this player's room (public or private): the
//                  runtime works out what can be offered, and refuses when
//                  nothing can (ShellView::friends.canInvite says).
//   Dismiss        Drop an invite received.
//   Join, Watch    Go to a friend's public room, and for Watch to their table.
//                  The runtime asks first when this PC is in a room or a game
//                  (ShellView::friends.visit).
//   TakeInvite     Go where an invite says: a public room as Join, a private
//                  room as a Discord invitation is taken.
//   AnswerVisit    The player's answer to that question, for the revision shown.
//
// Join, Watch and TakeInvite do not use the one action the bridge takes at a
// time, so they never refuse for it.
#include <cstddef>
#include <cstdint>
#include <string>

namespace sf4e { namespace netplay { namespace friends {

struct Command {
	enum class Kind : std::uint8_t {
		Visit, Leave,
		Request, Accept, Decline, Cancel, Remove, Block, Unblock, NewCode, AppearOffline, Forget, Invite, Dismiss,
		Join, Watch, TakeInvite, AnswerVisit,
	} kind = Kind::Visit;
	std::string bridgeId;
	// The row acted on, by id; for Invite the friend, for TakeInvite the invite.
	std::string id;
	// Request and Block: the card token and the member's room name. RequestCode: the code as typed.
	std::string card, label, code;
	// Decline and Remove: also block. AppearOffline: on. AnswerVisit: switch (true) or stay (false).
	bool flag = false;
	// AnswerVisit: the revision of the visit the player was asked about.
	std::uint64_t revision = 0;

	static constexpr std::size_t MaxField = 256;

	// Whether this is one of the bridge's actions (friends_act), of which one at a time is in flight.
	bool IsAction() const {
		switch (kind) {
		case Kind::Request: case Kind::Accept: case Kind::Decline: case Kind::Cancel: case Kind::Remove: case Kind::Block:
		case Kind::Unblock: case Kind::NewCode: case Kind::AppearOffline: case Kind::Forget: case Kind::Invite: case Kind::Dismiss:
			return true;
		default:
			return false;
		}
	}

	bool Valid() const {
		return bridgeId.size() <= MaxField && id.size() <= 64 && card.size() <= 128 && label.size() <= 64 && code.size() <= 32;
	}
	std::size_t Bytes() const { return bridgeId.size() + id.size() + card.size() + label.size() + code.size(); }

	// A Request names a card or a code, never both: the card's way is the one with a token.
	bool ByCard() const { return !card.empty(); }

	static Command Visit(const std::string& bridge) { return Of(Kind::Visit, bridge); }
	static Command Leave(const std::string& bridge) { return Of(Kind::Leave, bridge); }
	static Command RequestCard(const std::string& bridge, const std::string& card, const std::string& label) {
		Command command = Of(Kind::Request, bridge); command.card = card; command.label = label; return command;
	}
	static Command RequestCode(const std::string& bridge, const std::string& typed) {
		Command command = Of(Kind::Request, bridge); command.code = typed; return command;
	}
	static Command Accept(const std::string& bridge, const std::string& request) { return OnRow(Kind::Accept, bridge, request); }
	static Command Decline(const std::string& bridge, const std::string& request, bool block) { return OnRow(Kind::Decline, bridge, request, block); }
	static Command Cancel(const std::string& bridge, const std::string& request) { return OnRow(Kind::Cancel, bridge, request); }
	static Command Remove(const std::string& bridge, const std::string& friendId, bool block) { return OnRow(Kind::Remove, bridge, friendId, block); }
	static Command BlockCard(const std::string& bridge, const std::string& card, const std::string& label) {
		Command command = Of(Kind::Block, bridge); command.card = card; command.label = label; return command;
	}
	static Command Unblock(const std::string& bridge, const std::string& block) { return OnRow(Kind::Unblock, bridge, block); }
	static Command NewCode(const std::string& bridge) { return Of(Kind::NewCode, bridge); }
	static Command AppearOffline(const std::string& bridge, bool on) { return OnRow(Kind::AppearOffline, bridge, {}, on); }
	static Command Forget(const std::string& bridge) { return Of(Kind::Forget, bridge); }
	static Command Invite(const std::string& bridge, const std::string& friendId) { return OnRow(Kind::Invite, bridge, friendId); }
	static Command Dismiss(const std::string& bridge, const std::string& invite) { return OnRow(Kind::Dismiss, bridge, invite); }
	static Command Join(const std::string& bridge, const std::string& friendId) { return OnRow(Kind::Join, bridge, friendId); }
	static Command Watch(const std::string& bridge, const std::string& friendId) { return OnRow(Kind::Watch, bridge, friendId); }
	static Command TakeInvite(const std::string& bridge, const std::string& invite) { return OnRow(Kind::TakeInvite, bridge, invite); }
	static Command AnswerVisit(const std::string& bridge, std::uint64_t revision, bool switchRoom) {
		Command command = OnRow(Kind::AnswerVisit, bridge, {}, switchRoom); command.revision = revision; return command;
	}

private:
	static Command Of(Kind kind, const std::string& bridge) { Command command; command.kind = kind; command.bridgeId = bridge; return command; }
	static Command OnRow(Kind kind, const std::string& bridge, const std::string& id, bool flag = false) {
		Command command = Of(kind, bridge); command.id = id; command.flag = flag; return command;
	}
};

} } }
