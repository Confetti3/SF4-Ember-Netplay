// A room bound to a tournament match (spec 13.4, 14, 15): the binding, the
// fighters-only seating it implies at table 0, and the permit each game there
// waits for. Casual tables and rooms never reach this code.
#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"

#include <set>
#include <utility>

namespace sf4e { namespace room {
using namespace detail;

// The two fighters and every watcher a binding can name always fit in a room.
static_assert(2 + MaxWatchers <= MaximumMembers, "a bound room must hold its fighters and all its watchers");

namespace {
bool IsLowerHex(const std::string& text, std::size_t length) {
	return text.size() == length && std::all_of(text.begin(), text.end(),
		[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// The identifier alphabet the bridge and helper use: lowercase letters,
// digits, `_` and `-`.
bool IsIdentifier(const std::string& text, std::size_t maximum) {
	return !text.empty() && text.size() <= maximum && std::all_of(text.begin(), text.end(),
		[](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}
}

bool TournamentBinding::Valid() const {
	const auto named = [](const auto& value) {
		return IsLowerHex(value.endpoint, 64) && IsIdentifier(value.emberId, 64);
	};
	if (!(IsIdentifier(matchId, 64) && assignmentGeneration && bindingRevision &&
		ValidGamesToWin(gamesToWin) && startingWins[0] < gamesToWin && startingWins[1] < gamesToWin &&
		named(fighters[0]) && named(fighters[1]) &&
		fighters[0].endpoint != fighters[1].endpoint && fighters[0].emberId != fighters[1].emberId &&
		watchers.size() <= MaxWatchers)) return false;
	// Every endpoint and Ember ID names one member: no watcher repeats another
	// watcher or either fighter.
	std::set<std::string> endpoints = {fighters[0].endpoint, fighters[1].endpoint};
	std::set<std::string> emberIds = {fighters[0].emberId, fighters[1].emberId};
	return std::all_of(watchers.begin(), watchers.end(), [&](const TournamentWatcher& watcher) {
		return named(watcher) && endpoints.insert(watcher.endpoint).second && emberIds.insert(watcher.emberId).second;
	});
}

int TournamentBinding::SlotOf(const std::string& endpoint) const {
	if (!Active()) return -1;
	for (int slot = 0; slot < 2; ++slot)
		if (fighters[slot].endpoint == endpoint) return slot;
	return -1;
}

bool TournamentBinding::WatcherOf(const std::string& endpoint) const {
	return Active() && std::any_of(watchers.begin(), watchers.end(),
		[&](const TournamentWatcher& watcher) { return watcher.endpoint == endpoint; });
}

std::string TournamentBinding::EmberIdOf(const std::string& endpoint) const {
	if (!Active()) return {};
	for (const auto& fighter : fighters)
		if (fighter.endpoint == endpoint) return fighter.emberId;
	for (const auto& watcher : watchers)
		if (watcher.endpoint == endpoint) return watcher.emberId;
	return {};
}

bool TournamentBinding::SupersededBy(const TournamentBinding& other) const {
	if (other.matchId != matchId) return false;
	if (other.assignmentGeneration != assignmentGeneration) return other.assignmentGeneration > assignmentGeneration;
	return other.bindingRevision >= bindingRevision;
}

bool ValidPermitId(const std::string& permit) {
	return permit.size() > 4 && permit.compare(0, 4, "per_") == 0 && IsIdentifier(permit, MaximumPermitBytes);
}

void to_json(nlohmann::json& json, const TournamentBinding& value) {
	json = nlohmann::json::object();
	if (!value.Active()) return;
	json = {{"match_id", value.matchId}, {"assignment_generation", value.assignmentGeneration},
		{"binding_revision", value.bindingRevision}, {"games_to_win", value.gamesToWin},
		{"fighters", nlohmann::json::array({
			{{"endpoint", value.fighters[0].endpoint}, {"ember_id", value.fighters[0].emberId}},
			{{"endpoint", value.fighters[1].endpoint}, {"ember_id", value.fighters[1].emberId}}})}};
	// Left out at 0-0, so a set from the start keeps the snapshot it always had.
	if (value.startingWins[0] || value.startingWins[1])
		json["starting_wins"] = {value.startingWins[0], value.startingWins[1]};
	// Left out when none, so a room without watchers keeps its snapshot bytes.
	if (!value.watchers.empty()) {
		json["watchers"] = nlohmann::json::array();
		for (const auto& watcher : value.watchers)
			json["watchers"].push_back({{"endpoint", watcher.endpoint}, {"ember_id", watcher.emberId}});
	}
}

void from_json(const nlohmann::json& json, TournamentBinding& value) {
	value = TournamentBinding();
	if (!json.is_object()) throw std::invalid_argument("room tournament binding");
	if (json.empty()) return;
	value.matchId = ReadText(json, "match_id", 64, false);
	value.assignmentGeneration = ReadU64(json, "assignment_generation");
	value.bindingRevision = ReadU64(json, "binding_revision");
	value.gamesToWin = static_cast<std::uint8_t>(ReadInt(json, "games_to_win", 1, MaxSetLength));
	const auto& fighters = json.at("fighters");
	if (!fighters.is_array() || fighters.size() != 2) throw std::invalid_argument("room tournament fighters");
	for (std::size_t slot = 0; slot < 2; ++slot) {
		value.fighters[slot].endpoint = ReadText(fighters.at(slot), "endpoint", 64, false);
		value.fighters[slot].emberId = ReadText(fighters.at(slot), "ember_id", 64, false);
	}
	const auto found = json.find("starting_wins");
	if (found != json.end()) {
		if (!found->is_array() || found->size() != 2) throw std::invalid_argument("room tournament starting wins");
		for (std::size_t slot = 0; slot < 2; ++slot) {
			const auto& wins = found->at(slot);
			if (!wins.is_number_unsigned() || wins.get<std::uint64_t>() > static_cast<std::uint64_t>(MaxSetLength))
				throw std::invalid_argument("room tournament starting wins");
			value.startingWins[slot] = static_cast<std::uint8_t>(wins.get<std::uint64_t>());
		}
	}
	const auto watchers = json.find("watchers");
	if (watchers != json.end()) {
		if (!watchers->is_array() || watchers->size() > MaxWatchers) throw std::invalid_argument("room tournament watchers");
		for (const auto& watcher : *watchers)
			value.watchers.push_back({ReadText(watcher, "endpoint", 64, false), ReadText(watcher, "ember_id", 64, false)});
	}
	if (!value.Valid()) throw std::invalid_argument("room tournament binding");
}

bool RoomAuthority::BoundTable(const Table& table) const {
	return snapshot_.tournament.Active() && table.id == TournamentTable;
}

void RoomAuthority::SeatBoundFighters() {
	Table& table = snapshot_.tables[TournamentTable];
	if (GameUnderWay(table)) return;
	bool changed = false;
	for (const auto& member : std::vector<Member>(snapshot_.members)) {
		const int slot = snapshot_.tournament.SlotOf(member.connection.user);
		if (slot < 0) continue;
		const MemberId seated = slot == 0 ? table.p1 : table.p2;
		if (seated == member.id) continue;
		// Whoever holds the slot, and wherever this fighter sits now, make way.
		if (seated) RemoveFromTable(seated);
		RemoveFromTable(member.id);
		(slot == 0 ? table.p1 : table.p2) = member.id;
		changed = true;
	}
	if (!changed) return;
	table.phase = table.p1 && table.p2 ? TablePhase::Waiting : TablePhase::Idle;
	Touch(table);
	NormalizeTableMembers(table);
}

void RoomAuthority::ReservePermit(Table& table) {
	// Reserved now and never reused: a start that is called off burns it, so a
	// permit for it can never match a later game. ApplyReadiness has already
	// refused a Ready when no generation is left.
	table.permitGeneration = nextMatchGeneration_++;
	table.permits = {};
	table.permitWindows = {};
	permits_.tables[table.id] = {table.permitGeneration, 0, permits_.clockMs, permits_.clockKnown};
}

void RoomAuthority::ClearPermit(Table& table) {
	table.permitGeneration = 0;
	table.permits = {};
	table.permitWindows = {};
	permits_.tables[table.id] = {};
}

std::uint64_t RoomAuthority::PermitAgeAt(std::size_t table, std::uint64_t nowMs) const {
	const auto& timer = permits_.tables[table];
	const auto counted = timer.sampled && nowMs > timer.sampleMs
		? AddCapped(timer.ageMs, nowMs - timer.sampleMs, MaximumPermitWindowMs) : timer.ageMs;
	return (std::max)(counted, timer.heldMs);
}

void RoomAuthority::AgePermitHolds(std::uint64_t nowMs) {
	for (std::size_t i = 0; i < TableCount; ++i) {
		auto& timer = permits_.tables[i];
		if (!timer.generation) continue;
		// A clock not past the last count adds nothing and leaves the sample
		// where it was; a held age joins the count either way.
		const auto sampleMs = timer.sampled && timer.sampleMs > nowMs ? timer.sampleMs : nowMs;
		timer.ageMs = PermitAgeAt(i, nowMs);
		timer.sampleMs = sampleMs;
		timer.sampled = true;
		timer.heldMs = 0;
	}
	if (!permits_.clockKnown || nowMs > permits_.clockMs) permits_.clockMs = nowMs;
	permits_.clockKnown = true;
}

void RoomAuthority::KeepPermitAges(const PermitTimers& kept) {
	if (!recoveryPaused_) throw std::logic_error("permit ages kept on a live room");
	for (std::size_t i = 0; i < TableCount; ++i) {
		auto& timer = permits_.tables[i];
		// Committed generations are never reused, so the same one is the same
		// reservation. The caller keeps a candidate's own reservations out.
		if (!timer.generation || kept.tables[i].generation != timer.generation) continue;
		// This process's count goes on; the restored age is held, and the
		// restored room says whether the game has begun.
		const auto restored = (std::max)(timer.ageMs, timer.heldMs);
		const auto windowMs = timer.windowMs;
		timer = kept.tables[i];
		timer.heldMs = (std::max)(timer.heldMs, restored);
		timer.windowMs = windowMs;
	}
	if (!kept.clockKnown) return;
	permits_.clockMs = kept.clockMs;
	permits_.clockKnown = true;
}

std::uint64_t RoomAuthority::PermitWindow(const Table& table) {
	std::uint64_t window = MaximumPermitWindowMs;
	bool held = false;
	for (std::size_t seat = 0; seat < 2; ++seat) {
		if (table.permits[seat].empty()) continue;
		held = true;
		window = (std::min)(window, table.permitWindows[seat]);
	}
	return held ? window : 0;
}

bool RoomAuthority::PermitStartPassed(const Table& table, std::uint64_t ageMs) const {
	if (!table.permitGeneration || std::all_of(table.permits.begin(), table.permits.end(),
		[](const std::string& permit) { return permit.empty(); })) return false;
	const auto window = PermitWindow(table);
	return window <= PermitStartMarginMs || ageMs >= window - PermitStartMarginMs;
}

bool RoomAuthority::NativeStartPending(std::size_t table) const {
	const auto& timer = permits_.tables[table];
	const auto& value = snapshot_.tables[table];
	return timer.windowMs && timer.generation && value.matchGeneration == timer.generation &&
		(value.phase == TablePhase::Playing || value.phase == TablePhase::Paused);
}

bool RoomAuthority::NativeStartExpired(std::uint8_t table, std::uint64_t generation) const {
	return table < TableCount && NativeStartPending(table) && permits_.tables[table].generation == generation &&
		PermitAge(snapshot_.tables[table]) >= permits_.tables[table].windowMs;
}

void RoomAuthority::NativeStarted(std::uint8_t table, std::uint64_t generation) {
	if (table < TableCount && permits_.tables[table].generation == generation && permits_.tables[table].windowMs)
		permits_.tables[table] = {};
}

bool RoomAuthority::PermitCalledOff(const Table& table, std::uint64_t ageMs) const {
	return table.permitGeneration && ((PermitPending(table) && ageMs >= PermitHoldMs) || PermitStartPassed(table, ageMs));
}

Result RoomAuthority::BindTournament(const TournamentBinding& binding) {
	if (!binding.Valid()) return Reject(RejectReason::InvalidRules);
	if (snapshot_.closed) return Reject(RejectReason::Closed);
	// A tournament match is scored; a Practice room's games end only on time.
	if (snapshot_.practice) return Reject(RejectReason::Unauthorized);
	auto& current = snapshot_.tournament;
	if (current.Active()) {
		// A room plays one match; a newer binding for it replaces the old one.
		if (current.matchId != binding.matchId) return Reject(RejectReason::Unauthorized);
		if (!current.SupersededBy(binding)) return Reject(RejectReason::StaleRoom);
		if (current.assignmentGeneration == binding.assignmentGeneration &&
			current.bindingRevision == binding.bindingRevision && current.fighters == binding.fighters &&
			current.watchers == binding.watchers) return Accept();
	}
	const bool starting = !current.Active();
	// Table 0's revision moves only for a change to the set itself, an organizer's
	// reassignment included. A bind that only adds, drops or revokes watchers
	// touches the room alone, so it never makes a fighter's Ready stale.
	const bool setChanged = starting || current.fighters != binding.fighters ||
		current.gamesToWin != binding.gamesToWin || current.startingWins != binding.startingWins ||
		current.assignmentGeneration != binding.assignmentGeneration;
	Table& table = snapshot_.tables[TournamentTable];
	// A casual game still under way at the table is not the set's, so the
	// first binding waits for it to end; the state machine offers it again.
	if (starting && GameUnderWay(table)) return Reject(RejectReason::WrongPhase);
	current = binding;
	table.rules.format = static_cast<SetFormat>(binding.gamesToWin);
	// The set starts from the score it carries, 0-0 included, whatever a
	// casual game left on the table. Only the first binding sets it: a newer
	// one for the same set (a fighter changed endpoints) keeps the score the
	// table has played to.
	if (starting) {
		table.score[0] = binding.startingWins[0];
		table.score[1] = binding.startingWins[1];
	}
	if (setChanged) Touch(table);
	std::vector<Event> events;
	// Fighters and named watchers only: anyone else leaves the room.
	std::vector<MemberId> strangers;
	for (const auto& member : snapshot_.members)
		if (binding.SlotOf(member.connection.user) < 0 && !binding.WatcherOf(member.connection.user)) strangers.push_back(member.id);
	for (const auto stranger : strangers) {
		auto left = Leave(stranger);
		events.insert(events.end(), left.events.begin(), left.events.end());
	}
	SeatBoundFighters();
	NormalizeBoundWatchers();
	TouchRoom();
	events.push_back(Event{Event::Kind::SnapshotChanged, TournamentTable, 0, 0, MatchResult::Abort});
	return Accept(std::move(events));
}

RoomAuthority::BoundAdmission RoomAuthority::BoundJoin(const ConnectionRef& connection, const MemberProfile& profile) const {
	const auto& binding = snapshot_.tournament;
	const bool watcher = binding.WatcherOf(connection.user);
	if (profile.observer && !watcher) return BoundAdmission::Refuse;
	if (!binding.Active()) return BoundAdmission::Unbound;
	return watcher || binding.SlotOf(connection.user) >= 0 ? BoundAdmission::Named : BoundAdmission::Stranger;
}

bool RoomAuthority::IsBoundWatcher(MemberId member) const { return IsWatcher(snapshot_, member); }

const Member* RoomAuthority::HostSuccessor(MemberId leaving) const {
	const Member* successor = nullptr;
	const auto rank = [&](const Member& value) { return std::make_pair(IsBoundWatcher(value.id), value.joinOrder); };
	for (const auto& other : snapshot_.members)
		if (other.id != leaving && (!successor || rank(other) < rank(*successor))) successor = &other;
	return successor;
}

void RoomAuthority::SettleBoundJoin(MemberId joined) {
	SeatBoundFighters();
	const auto& user = Find(joined)->connection.user;
	if (snapshot_.tournament.WatcherOf(user)) {
		PlaceWatcher(joined);
	} else if (IsBoundWatcher(snapshot_.host)) {
		// A fighter back in a room whose host field a watcher holds takes it.
		snapshot_.host = joined;
		for (auto& other : snapshot_.members) other.host = other.id == joined;
	}
}

void RoomAuthority::PlaceWatcher(MemberId member) {
	// At most MaxWatchers of them, so the table's MaxSpectators always holds.
	Table& table = snapshot_.tables[TournamentTable];
	(table.phase == TablePhase::Playing ? table.watchingNext : table.spectators).push_back(member);
	NormalizeMemberStatus(member);
}

void RoomAuthority::NormalizeBoundWatchers() {
	const Table& table = snapshot_.tables[TournamentTable];
	const auto listed = [](const std::vector<MemberId>& list, MemberId member) {
		return std::find(list.begin(), list.end(), member) != list.end();
	};
	for (const auto& member : std::vector<Member>(snapshot_.members)) {
		if (!snapshot_.tournament.WatcherOf(member.connection.user)) continue;
		// A watcher never holds a start, whatever a modified client did before the
		// bind: its lock-in ends here, and the start hold with it at the next step.
		if (auto* item = Find(member.id)) item->spectatorLocked = false;
		if (listed(table.spectators, member.id) || listed(table.watchingNext, member.id)) continue;
		RemoveFromTable(member.id);
		PlaceWatcher(member.id);
	}
}

void RoomAuthority::TouchSpectators(Table& table) {
	if (BoundTable(table)) TouchRoom();
	else Touch(table);
}

bool RoomAuthority::WatcherMay(ActionKind kind) {
	// A watcher is put in its place at Join and keeps it: no seat, queue, watch
	// choice, rules, lock-in or chat, and none of the host's powers (IsHost).
	// The generation-scoped leave of a game whose stream failed is not one of
	// these actions; it is taken for everyone before this list is asked.
	return kind == ActionKind::Leave || kind == ActionKind::AcknowledgeTerminal || kind == ActionKind::SetTraining;
}

Result RoomAuthority::ApplyPermitReady(MemberId member, const Action& action, Table* table) {
	if (!BoundTable(*table) || table->phase != TablePhase::Ready || !table->permitGeneration ||
		action.matchGeneration != table->permitGeneration) return Reject(RejectReason::WrongGeneration);
	const int seat = table->p1 == member ? 0 : table->p2 == member ? 1 : -1;
	if (seat < 0) return Reject(RejectReason::NotSeated);
	if (!ValidPermitId(action.text) || !action.startWindowMs || action.startWindowMs > MaximumPermitWindowMs)
		return Reject(RejectReason::Unauthorized);
	if (table->permits[seat] == action.text && table->permitWindows[seat] == action.startWindowMs) return Accept();
	table->permits[seat] = action.text;
	table->permitWindows[seat] = action.startWindowMs;
	Touch(*table);
	// A permit whose window has passed starts nothing: AdvanceTime calls the
	// start off.
	if (!PermitPending(*table) && !table->spectatorHold && !PermitStartPassed(*table, PermitAge(*table)))
		return Accept({Event{Event::Kind::MatchReady, table->id, table->matchGeneration, 0, MatchResult::Abort}});
	return Accept();
}

} }
