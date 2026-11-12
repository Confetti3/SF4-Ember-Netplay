#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "TournamentAnswers.hxx"
#include "../netplay/Friends.hxx"

// The helper's answers to the friends requests (friends_sync, friends_act),
// which ride the tournament channel and are answered by request id
// (docs/design/FRIENDS.md, "Wire contracts"; read like every other play
// answer, ReadTournamentAnswer). Both answer the bridge's whole
// FriendsState, decoded completely into netplay::friends::State or not at
// all: a malformed answer never reaches the interface half-read. Known keys
// are checked and keys a later bridge adds are ignored, as the points and
// card answers are (TournamentAnswers.hxx).
namespace sf4e { namespace session {

// A friends_sync or friends_act answer's state; `state` is left alone and
// false returned when the data is not a valid FriendsState.
bool DecodeFriendsState(const nlohmann::json& data, netplay::friends::State& state);

} }
