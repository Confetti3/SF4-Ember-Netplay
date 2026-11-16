#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "TournamentAnswers.hxx"
#include "../netplay/Friends.hxx"

// The helper's answers to the friends requests (friends_sync, friends_act),
// which ride the tournament channel and are answered by request id
// (docs/design/FRIENDS.md, "Wire contracts"; read like every other play
// answer, ReadTournamentAnswer). Both answer the bridge's whole
// FriendsState, decoded into netplay::friends::State. Shape is checked, not
// the bridge's rules: a row that cannot be read is left out of its list, and
// a part of a friend that cannot be read (main, account, record) is left out
// of that friend, so one damaged row never costs the player the list. An
// answer whose own shape is wrong (no code, a list missing or over its
// bound) is refused whole. Keys a later bridge adds are ignored, and a tier
// past this build's table is its last, as in the points and card answers
// (TournamentAnswers.hxx). A friend's game and the private room they may be
// joined in are parts of the presence that stand alone too: one that cannot be
// read is left out and the presence still counts.
namespace sf4e { namespace session {

// A friends_sync or friends_act answer's state; `state` is left alone and
// false returned when the data is not a valid FriendsState.
bool DecodeFriendsState(const nlohmann::json& data, netplay::friends::State& state);

// The invitation a join's answer carries ("joining": the friend's id and the
// private room's secret), which is no part of the state. `joining` is left
// alone and false returned when the answer has none or it is not well formed.
bool DecodeJoining(const nlohmann::json& data, netplay::friends::Joining& joining);

} }
