#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../netplay/Archive.hxx"
#include "../netplay/PublicRooms.hxx"
#include "../netplay/Levels.hxx"
#include "../netplay/Rankings.hxx"
#include "../netplay/TournamentStatus.hxx"

// The helper's answers to tournament play requests (match_claim,
// room_publish, game_prepare, game_report, match_leave, match_watch,
// match_unwatch, assignment_list) and
// to public room requests (room_list, room_create, room_ticket) and ranking
// requests (ranking_standing, ranking_choose, ranking_board) and archive requests
// (archive_list, archive_fetch),
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
// A match_watch answer: the room to join, or a wait with the bridge's reason.
std::optional<netplay::tournament::WatchReply> DecodeWatchReply(const nlohmann::json& data);

using Assignment = netplay::tournament::Assignment;
std::optional<std::vector<Assignment>> DecodeAssignments(const nlohmann::json& data);

// A room_list answer's rooms, and a room_create or room_ticket answer's admission.
std::optional<netplay::publicrooms::RoomList> DecodeRoomList(const nlohmann::json& data);
std::optional<netplay::publicrooms::Admission> DecodeAdmission(const nlohmann::json& data);

// A ranking_standing or ranking_choose answer's standing, and a ranking_board answer's board.
std::optional<netplay::rankings::Standing> DecodeStanding(const nlohmann::json& data);
std::optional<std::vector<netplay::rankings::BoardEntry>> DecodeBoard(const nlohmann::json& data);

// An archive_list answer's page: the games it lists, their tournament sets' scores, the filter's
// choices and where the next page starts (netplay/Archive.hxx).
std::optional<netplay::archive::Listing> DecodeArchivePage(const nlohmann::json& data);

// A points_me answer's levels, and a cards_lookup answer's cards (one per
// selector, in order; nullopt inside for a selector that found no card, or
// whose card could not be read: one bad card never fails the answer). A card's
// profile members (name, flag, class, main, since, worn, pins, style, versus,
// friend) are each read on their own, and one that is missing or not
// understood is left at the card's default.
std::optional<netplay::levels::Own> DecodeOwnLevels(const nlohmann::json& data);
std::optional<std::vector<std::optional<netplay::levels::Card>>> DecodeCards(const nlohmann::json& data);
// A cards_search answer: the players found (at most MaxSearch; a hit that cannot
// be read is left out) and whether more matched. Nothing for an answer that has
// no list. A card_save answer carries nothing to decode: its ok is the whole answer.
std::optional<netplay::profiles::SearchAnswer> DecodeSearch(const nlohmann::json& data);

} }
