#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../netplay/TournamentStatus.hxx"

// The helper's answers to tournament play requests (match_claim,
// room_publish, game_prepare, game_report, match_leave, assignment_list,
// handoff_redeem),
// held apart from the identity answers the Ember ID screens show. Each is
// decoded completely into a typed reply, or not at all: a malformed answer
// never reaches the state machine half-read.
namespace sf4e { namespace session {

struct TournamentAnswer {
	std::uint64_t requestId = 0;
	std::string op;
	bool ok = false;
	std::string reason;
	nlohmann::json data;
};

// True for the ops whose answers go to the runtime instead of the Ember ID view.
bool IsTournamentPlayOp(const std::string& op);

// The answer event, or nothing when it is not a well-formed play answer.
std::optional<TournamentAnswer> ReadTournamentAnswer(const nlohmann::json& event);

std::optional<netplay::tournament::ClaimReply> DecodeClaimReply(const nlohmann::json& data);
std::optional<netplay::tournament::PrepareReply> DecodePrepareReply(const nlohmann::json& data);

using Assignment = netplay::tournament::Assignment;
std::optional<std::vector<Assignment>> DecodeAssignments(const nlohmann::json& data);

} }
