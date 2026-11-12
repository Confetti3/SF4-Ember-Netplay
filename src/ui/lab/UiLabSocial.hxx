#pragma once
// The UI lab's social cases (UiLab.cxx keeps the rest): made-up friends for the Friends tab, in the states it shows.
#include "../ShellView.hxx"
#include <string>

namespace uilab {
// --friends full|requests|empty|noaccount|unavailable|loading|unreachable|offline: a service the player trusts and
// its friends answer in that state (--screen friends, friends-add, friends-sent or friends-blocked).
//   full: two requests, friends in a room, online and offline, three requests sent, one block
//   requests: two requests and one friend       empty: read, nothing in it
//   noaccount / unavailable: the one card       loading: the first read in flight
//   unreachable: the full list, the last sync failed (Not updated)       offline: the full list, appearing offline
// Returns false for a state it does not know.
bool Friends(sf4e::ui::ShellView& view, const std::string& state);
// With --toasts and --friends: the list as first read has nothing waiting, then at frame 6 the requests arrive and at
// frame 8 an invite, so the real toasts of FriendsPanel::TakeToast show (Accept, Later; Join, Dismiss).
void FriendsComing(sf4e::ui::ShellView& view, int frame);
}
