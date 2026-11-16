#pragma once
// The Invite button of a private room's action bar: one menu at the button with
// the friends who can be invited now, then the room's two ways of sharing a
// link. The friends are the ones the Friends screen would invite
// (netplay/FriendInvitees.hxx); choosing one sends the same invite it sends.
// Without friends (no Discord account, or turned off) it is just the two copies.
#include "FriendsPanel.hxx"
#include "MenuNavigation.hxx"
#include "ShellView.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"

#include <cstddef>
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace room_invite {

// The header chip that says who may join without an invitation: a public room is listed, a private room open to its members'
// friends says so, and a Hidden one or a match's room is by invitation only.
inline const char* AccessKey(const room::Snapshot& here) {
    return here.serverOwned ? "room.v2.listed" : here.OpenToFriends() && !here.tournament.Active() ? "room.v2.friends_can_join" : "room.v2.invitation_only";
}

// The friends listed before More friends.
constexpr std::size_t MostFriends = 6;
constexpr const char* MoreRow = "room-friends";
constexpr const char* ShortRow = "copy-short";
constexpr const char* FullRow = "copy";
// Nothing to offer: a disabled line that says why.
constexpr const char* WhyRow = "invite-why";

// The menu's choices, in order: each friend who can be invited (a Friends row id, which answers it), More friends or the
// reason the room cannot be offered, then Copy short link and Copy invitation. `gate` is what the friends are held to now.
inline std::vector<MenuChoice> ChoicesOf(const ShellView& v, const friends_panel::Gate& gate) {
    namespace nf = netplay::friends;
    std::vector<MenuChoice> choices;
    const auto& state = friends_panel::StateOf(v.friends);
    if (state.kind == nf::State::Kind::Listed) {
        if (!v.friends.canInvite) {
            choices.push_back({WhyRow, loc::T("friends.invite_room"), loc::T(friends_panel::NothingToOfferKey(v.friends.cannotInvite)), false});
        } else {
            const auto invitees = nf::InviteesOf(state, v.room, MostFriends);
            for (const nf::Friend* entry : invitees.shown) {
                NoteUserText(entry->name);
                // Said as the Friends screen says it: where they are, or why they cannot be invited now; and that the invite shows an offline player's room.
                const std::string detail = gate.act ? state.appearOffline ? std::string(loc::T("friends.invite_reveals")) : friends_panel::Status(*entry, v.unixNow) : std::string(loc::T(gate.actWhy));
                choices.push_back({friends_panel::FriendPrefix + entry->id, entry->name, detail, gate.act});
            }
            if (invitees.more || invitees.shown.empty()) choices.push_back({MoreRow, loc::T("room.invite.more_friends"), loc::T("room.v2.friends_detail")});
        }
    }
    // Once made, the short link's detail shows the link itself so it can be read out.
    const auto& link = v.shortInvitation;
    const auto shown = link.find("//") == std::string::npos ? link : link.substr(link.find("//") + 2);
    choices.push_back({ShortRow, loc::T("room.copy_short_invitation"), link.empty() ? std::string(loc::T("room.copy_short_invitation.detail")) : shown, !v.invitation.empty()});
    choices.push_back({FullRow, loc::T("room.copy_invitation"), loc::T("room.copy_invitation.detail"), !v.invitation.empty()});
    return choices;
}

} } }
