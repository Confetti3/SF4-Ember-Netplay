// A room bound to a tournament match (spec 13.4, 14, 15): the binding, the
// fighters-only seating it implies at table 0, and the permit each game there
// waits for. Casual tables and rooms never reach this code.
#include "RoomModel.hxx"
#include "RoomModelDetail.hxx"

namespace sf4e { namespace room {
using namespace detail;

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
	const auto fighter = [](const TournamentFighter& value) {
		return IsLowerHex(value.endpoint, 64) && IsIdentifier(value.emberId, 64);
	};
	return IsIdentifier(matchId, 64) && assignmentGeneration && bindingRevision &&
		(gamesToWin == 1 || gamesToWin == 2 || gamesToWin == 3 || gamesToWin == 5) &&
		fighter(fighters[0]) && fighter(fighters[1]) &&
		fighters[0].endpoint != fighters[1].endpoint && fighters[0].emberId != fighters[1].emberId;
}

int TournamentBinding::SlotOf(const std::string& endpoint) const {
	if (!Active()) return -1;
	for (int slot = 0; slot < 2; ++slot)
		if (fighters[slot].endpoint == endpoint) return slot;
	return -1;
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
}

void from_json(const nlohmann::json& json, TournamentBinding& value) {
	value = TournamentBinding();
	if (!json.is_object()) throw std::invalid_argument("room tournament binding");
	if (json.empty()) return;
	value.matchId = ReadText(json, "match_id", 64, false);
	value.assignmentGeneration = ReadU64(json, "assignment_generation");
	value.bindingRevision = ReadU64(json, "binding_revision");
	value.gamesToWin = static_cast<std::uint8_t>(ReadInt(json, "games_to_win", 1, 5));
	const auto& fighters = json.at("fighters");
	if (!fighters.is_array() || fighters.size() != 2) throw std::invalid_argument("room tournament fighters");
	for (std::size_t slot = 0; slot < 2; ++slot) {
		value.fighters[slot].endpoint = ReadText(fighters.at(slot), "endpoint", 64, false);
		value.fighters[slot].emberId = ReadText(fighters.at(slot), "ember_id", 64, false);
	}
	if (!value.Valid()) throw std::invalid_argument("room tournament binding");
}

bool RoomAuthority::BoundTable(const Table& table) const {
	return snapshot_.tournament.Active() && table.id == TournamentTable;
}

void RoomAuthority::SeatBoundFighters() {
	Table& table = snapshot_.tables[TournamentTable];
	if (table.phase == TablePhase::Ready || table.phase == TablePhase::Playing || table.phase == TablePhase::Paused) return;
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
	return timer.windowMs && timer.generation && value.phase == TablePhase::Playing && value.matchGeneration == timer.generation;
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
	auto& current = snapshot_.tournament;
	if (current.Active()) {
		// A room plays one match; a newer binding for it replaces the old one.
		if (current.matchId != binding.matchId) return Reject(RejectReason::Unauthorized);
		if (!current.SupersededBy(binding)) return Reject(RejectReason::StaleRoom);
		if (current.assignmentGeneration == binding.assignmentGeneration &&
			current.bindingRevision == binding.bindingRevision && current.fighters == binding.fighters) return Accept();
	}
	current = binding;
	Table& table = snapshot_.tables[TournamentTable];
	table.rules.format = static_cast<SetFormat>(binding.gamesToWin);
	Touch(table);
	std::vector<Event> events;
	// Fighters only: anyone the binding does not name leaves the room.
	std::vector<MemberId> strangers;
	for (const auto& member : snapshot_.members)
		if (binding.SlotOf(member.connection.user) < 0) strangers.push_back(member.id);
	for (const auto stranger : strangers) {
		auto left = Leave(stranger);
		events.insert(events.end(), left.events.begin(), left.events.end());
	}
	SeatBoundFighters();
	TouchRoom();
	events.push_back(Event{Event::Kind::SnapshotChanged, TournamentTable, 0, 0, MatchResult::Abort});
	return Accept(std::move(events));
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
