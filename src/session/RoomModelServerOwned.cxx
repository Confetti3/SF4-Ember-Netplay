// Server-owned rooms: the rules that differ from a private room. A public room's
// authority runs in the room host process, so nobody is the hosting player.
// Identity is the player's Ember ID ("account"), which Join receives from the
// session layer and which stays out of every snapshot.
#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"
#include "../common/SkillBadgeJson.hxx"

namespace sf4e { namespace room {
using namespace detail;

namespace {
// An Ember ID is 57 bytes; the bound keeps a status line of 512 bans in budget.
constexpr std::size_t MaximumAccountBytes = 64;

bool ValidAccount(const std::string& account) {
	return !account.empty() && account.size() <= MaximumAccountBytes && IsValidUtf8(account) && IsSingleLineText(account);
}
}

bool RoomAuthority::SetServerOwned() {
	if (snapshot_.closed || snapshot_.practice || !snapshot_.members.empty() || snapshot_.host != 0 || nextMemberId_ != 1) return false;
	snapshot_.serverOwned = true;
	return true;
}

// What a private room's host does with Close, done by the room host process.
Result RoomAuthority::CloseServerOwned() {
	if (!snapshot_.serverOwned) return Reject(RejectReason::Unauthorized);
	if (snapshot_.closed) return Reject(RejectReason::Closed);
	snapshot_.closed = true; snapshot_.locked = true;
	for (auto& table : snapshot_.tables) { table.phase = TablePhase::Closed; Touch(table); }
	TouchRoom();
	return Accept({Event{Event::Kind::RoomClosed, 0, 0, 0, MatchResult::Abort}});
}

std::vector<std::string> RoomAuthority::KickedAccounts() const {
	return std::vector<std::string>(kickedAccounts_.begin(), kickedAccounts_.end());
}

RejectReason RoomAuthority::CheckServerOwnedJoin(const MemberProfile& profile) const {
	// A member without an account could never be kicked for good.
	if (!ValidAccount(profile.admission.account)) return RejectReason::Unauthorized;
	if (kickedAccounts_.count(profile.admission.account)) return RejectReason::MemberKicked;
	for (const auto& entry : memberAccounts_)
		if (entry.second == profile.admission.account) return RejectReason::NameTaken;
	return RejectReason::None;
}

void RoomAuthority::RememberAccount(MemberId member, std::string account) {
	memberAccounts_[member] = std::move(account);
}

// Bans are kept for the room's lifetime; ApplyKick closes the room instead of
// adding past MaximumKickedAccounts, so a checkpoint always restores.
void RoomAuthority::BanAccount(const std::string& account) {
	kickedAccounts_.insert(account);
}

bool RoomAuthority::BanWouldExceedCap(const std::string& account) const {
	return !kickedAccounts_.count(account) && kickedAccounts_.size() >= MaximumKickedAccounts;
}

void RoomAuthority::ForgetAccount(MemberId member) {
	memberAccounts_.erase(member);
}

// A game won between two accounts, for the skill rating; draws, cancels and
// aborts never count. The room host reports the newest on every status and the
// bridge counts each once, so older ones can go.
void RoomAuthority::RecordWonGame(std::uint8_t tableId, const Table& table, MatchResult result) {
	if (!snapshot_.serverOwned || (result != MatchResult::P1Win && result != MatchResult::P2Win)) return;
	const auto p1 = memberAccounts_.find(table.p1), p2 = memberAccounts_.find(table.p2);
	if (p1 == memberAccounts_.end() || p2 == memberAccounts_.end()) return;
	const bool p1Won = result == MatchResult::P1Win;
	wonGames_.push_back({tableId, table.matchGeneration, (p1Won ? p1 : p2)->second, (p1Won ? p2 : p1)->second});
	if (wonGames_.size() > MaximumWonGames) wonGames_.pop_front();
}

// The last member left. A public room waits for its next joiner, who becomes
// host; whoever locked it is gone, and the kick list stays. The rules return to
// the public default, which is what the room list shows for it.
void RoomAuthority::ReopenEmptyRoom() {
	snapshot_.host = 0;
	snapshot_.locked = false;
	// The next to open it chooses its type again, as with its rules.
	snapshot_.arcade = ClassicRoom; snapshot_.topSlot = -1;
	snapshot_.cabinets = {}; snapshot_.streaks = {};
	for (auto& table : snapshot_.tables)
		if (!(table.rules == PublicRoomRules())) { table.rules = PublicRoomRules(); Touch(table); }
}

void RoomAuthority::SaveServerOwned(nlohmann::json& state) const {
	nlohmann::json accounts = nlohmann::json::array();
	for (const auto& entry : memberAccounts_) accounts.push_back({{"member", entry.first}, {"account", entry.second}});
	state["accounts"] = std::move(accounts);
	state["kicked_accounts"] = kickedAccounts_;
	state["won_games"] = wonGames_;
}

bool RoomAuthority::LoadServerOwned(const nlohmann::json& state) {
	if (!snapshot_.serverOwned) return !state.contains("accounts") && !state.contains("kicked_accounts") && !state.contains("won_games");
	if (!state.contains("accounts") || !state.contains("kicked_accounts")) return false;
	const auto& accounts = state.at("accounts");
	const auto& kicked = state.at("kicked_accounts");
	if (!accounts.is_array() || accounts.size() != snapshot_.members.size()) return false;
	if (!kicked.is_array() || kicked.size() > MaximumKickedAccounts) return false;
	std::set<std::string> seen;
	for (const auto& row : accounts) {
		if (!row.is_object() || !row.contains("member") || !row.contains("account") ||
			!row.at("member").is_number_unsigned() || !row.at("account").is_string()) return false;
		const auto member = row.at("member").get<MemberId>();
		auto account = row.at("account").get<std::string>();
		if (!Find(member) || !ValidAccount(account) || !seen.insert(account).second) return false;
		if (!memberAccounts_.emplace(member, std::move(account)).second) return false;
	}
	for (const auto& entry : kicked) {
		if (!entry.is_string()) return false;
		auto account = entry.get<std::string>();
		if (!ValidAccount(account) || !kickedAccounts_.insert(std::move(account)).second) return false;
	}
	// A room from before won games has none yet.
	const auto won = state.value("won_games", nlohmann::json::array());
	if (!won.is_array() || won.size() > MaximumWonGames) return false;
	for (const auto& row : won) {
		auto game = row.get<WonGame>();
		if (game.table >= TableCount || game.generation == 0 || !ValidAccount(game.winner) || !ValidAccount(game.loser) ||
			game.winner == game.loser) return false;
		wonGames_.push_back(std::move(game));
	}
	// With nobody inside there is no moderator; with anybody inside there is one.
	return (snapshot_.host == 0) == snapshot_.members.empty();
}

} }
