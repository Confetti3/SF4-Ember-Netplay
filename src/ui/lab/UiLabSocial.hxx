#pragma once
// The UI lab's social cases (UiLab.cxx keeps the rest): made-up friends for the Friends tab, in the states it shows.
#include "../ShellView.hxx"
#include <string>

namespace uilab {
// --friends full|requests|notifications|empty|noaccount|unavailable|loading|unreachable|offline: a service the player
// trusts and its friends answer in that state (--screen friends, friends-add, friends-sent, friends-blocked, or
// notifications for what waits there).
//   full: two requests, friends in a room, online and offline, three requests sent, one block
//   requests: two requests and one friend       empty: read, nothing in it
//   notifications: the full list with Player 2's request and Player 3's invite waiting (--screen notifications)
//   noaccount / unavailable: the one card       loading: the first read in flight
//   unreachable: the full list, the last sync failed (Not updated)       offline: the full list, appearing offline
// Returns false for a state it does not know.
bool Friends(sf4e::ui::ShellView& view, const std::string& state);
// --find recent|empty (with --friends and --screen friends-add): Find a player's page on a service that offers profiles, listing
// the three players met lately (Player 2, Player 5 and Player 9), or none. Returns false for a state it does not know.
bool Find(sf4e::ui::ShellView& view, const std::string& state);
// --player friend|room|stranger|asked|sent|blocked|nocard|nobody|several|reading|unreachable|unsupported: another player's profile
// page open in that state, over the full friends list on a service the player trusts (--screen player-card, player-fighters,
// player-sets, player-awards or player-pick; the Sets tab needs --sets). friend: Player 6, online and your friend; room:
// Player 3, your friend, fighting in a joinable public room; blocked: Player 16, whom you blocked; the rest are Player 2.
//   stranger: a card, no relation   asked: they asked you   sent: you asked them   blocked: the card of a player you blocked
//   nocard: the service has none for them   nobody: no player goes by the name   several: three players go by it (--screen
//   player-pick)   reading: the look-up in flight   unreachable: the service could not be reached   unsupported: no profiles
// Returns false for a state it does not know.
bool Player(sf4e::ui::ShellView& view, const std::string& state);
// With --toasts and --friends: the list as first read has nothing waiting, then at frame 6 the requests arrive and at
// frame 8 an invite, so the real toasts FriendsPanel::Publish tells into Notifications show (Accept, Later; Join, Dismiss).
void FriendsComing(sf4e::ui::ShellView& view, int frame);
}
