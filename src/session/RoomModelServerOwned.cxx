// Server-owned rooms: the rules that differ from a private room. A public room's
// authority runs in the room host process, so nobody is the hosting player.
// Identity is the player's Ember ID ("account"), which Join receives from the
// session layer and which stays out of every snapshot.
#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"

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
	if (snapshot_.closed || !snapshot_.members.empty() || snapshot_.host != 0 || nextMemberId_ != 1) return false;
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
	if (!ValidAccount(profile.account)) return RejectReason::Unauthorized;
	if (kickedAccounts_.count(profile.account)) return RejectReason::MemberKicked;
	for (const auto& entry : memberAccounts_)
		if (entry.second == profile.account) return RejectReason::NameTaken;
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

// The last member left. A public room waits for its next joiner, who becomes
// host; whoever locked it is gone, and the kick list stays.
void RoomAuthority::ReopenEmptyRoom() {
	snapshot_.host = 0;
	snapshot_.locked = false;
}

void RoomAuthority::SaveServerOwned(nlohmann::json& state) const {
	nlohmann::json accounts = nlohmann::json::array();
	for (const auto& entry : memberAccounts_) accounts.push_back({{"member", entry.first}, {"account", entry.second}});
	state["accounts"] = std::move(accounts);
	state["kicked_accounts"] = kickedAccounts_;
}

bool RoomAuthority::LoadServerOwned(const nlohmann::json& state) {
	if (!snapshot_.serverOwned) return !state.contains("accounts") && !state.contains("kicked_accounts");
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
	// With nobody inside there is no moderator; with anybody inside there is one.
	return (snapshot_.host == 0) == snapshot_.members.empty();
}

} }
