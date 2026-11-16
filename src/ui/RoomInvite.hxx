#pragma once
// The Invite button of a room's action bar: one menu at the button with the
// friends who can be invited now, then the room's ways to hand a link out: a
// private room's short link and invitation, a public room's page link (its
// invitation would admit nobody; a friend is invited to it by its id). The
// friends are the ones the Friends screen would invite
// (netplay/FriendInvitees.hxx); choosing one sends the same invite it sends.
// Without friends (no Discord account, or turned off) it is just the copies.
#include "FriendsPanel.hxx"
#include "MenuNavigation.hxx"
#include "ShellView.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace room_invite {

// The room hands out an invitation: it is not a public room, nor a match's (bound, or still being claimed). The bar's
// Invite exists exactly then, and the header's chip speaks of it only then.
inline bool Invitable(const ShellView& v) {
    return !v.room.serverOwned && !v.room.tournament.Active() && !v.tournament.MatchRoomActive();
}

// The bar has an Invite button: every room but a match's (bound, or still being claimed), which admits only its two
// fighters. A public room's holds its friends and its page link.
inline bool OnBar(const ShellView& v) {
    return !v.room.tournament.Active() && !v.tournament.MatchRoomActive();
}

// The header chip that says who may join without an invitation: a public room is listed; a locked room admits nobody; a
// private room open to its members' friends says so; a Hidden one or a match's is by invitation only. A full room keeps
// its privacy's chip (the Members chip says it is full).
inline const char* AccessKey(const ShellView& v) {
    if (v.room.serverOwned) return "room.v2.listed";
    if (v.room.locked) return "room.locked_status";
    return Invitable(v) && v.room.OpenToFriends() ? "room.v2.friends_can_join" : "room.v2.invitation_only";
}

// The friends listed before More friends.
constexpr std::size_t MostFriends = 6;
constexpr const char* MoreRow = "room-friends";
constexpr const char* ShortRow = "copy-short";
constexpr const char* FullRow = "copy";
// A public room's page link (RoomActions copies it; More… lists it too).
constexpr const char* PublicLinkRow = "copy-room-link";
// Nothing to offer: a disabled line that says why.
constexpr const char* WhyRow = "invite-why";

// The menu's choices, in order: each friend who can be invited (a Friends row id, which answers it), then the Friends
// screen (More friends once some are listed) or the reason the room cannot be offered, then Copy short link and Copy
// invitation, or in a public room Copy room link (`publicLink`, its page's; none while the room has none). `gate` is
// what the friends are held to now.
inline std::vector<MenuChoice> ChoicesOf(const ShellView& v, const friends_panel::Gate& gate, const std::string& publicLink = {}) {
    namespace nf = netplay::friends;
    std::vector<MenuChoice> choices;
    const auto& state = friends_panel::StateOf(v.friends);
    if (state.kind == nf::State::Kind::Listed) {
        if (v.friends.offer != nf::OfferBlock::None) {
            choices.push_back({WhyRow, loc::T("friends.invite_room"), loc::T(friends_panel::NothingToOfferKey(v.friends.offer)), false});
        } else {
            const auto invitees = nf::InviteesOf(state, MostFriends);
            for (const nf::Friend* entry : invitees.shown) {
                NoteUserText(entry->name);
                // Said as the Friends screen says it: where they are, or why they cannot be invited now; and that the invite shows an offline player's room.
                const std::string detail = gate.act ? state.appearOffline ? std::string(loc::T("friends.invite_reveals")) : friends_panel::Status(*entry, v.unixNow) : std::string(loc::T(gate.actWhy));
                choices.push_back({friends_panel::FriendPrefix + entry->id, entry->name, detail, gate.act});
            }
            choices.push_back({MoreRow, loc::T(invitees.shown.empty() ? "home.friends" : "room.invite.more_friends"), loc::T("room.v2.friends_detail")});
        }
    }
    if (v.room.serverOwned) {
        if (!publicLink.empty()) choices.push_back({PublicLinkRow, loc::T("public.copy_link"), loc::Tf("public.copy_link_detail", publicLink)});
        return choices;
    }
    // Once made, the short link's detail shows the link itself so it can be read out.
    const auto& link = v.shortInvitation;
    const auto shown = link.find("//") == std::string::npos ? link : link.substr(link.find("//") + 2);
    choices.push_back({ShortRow, loc::T("room.copy_short_invitation"), link.empty() ? std::string(loc::T("room.copy_short_invitation.detail")) : shown, !v.invitation.empty()});
    choices.push_back({FullRow, loc::T("room.copy_invitation"), loc::T("room.copy_invitation.detail"), !v.invitation.empty()});
    return choices;
}

// The choice the menu opens on: the first that can be chosen (the line that says why and a request in flight leave the
// first disabled), else none.
inline std::string FirstEnabled(const std::vector<MenuChoice>& choices) {
    const auto at = std::find_if(choices.begin(), choices.end(), [](const MenuChoice& choice) { return choice.enabled; });
    return at == choices.end() ? std::string() : at->id;
}

} } }
