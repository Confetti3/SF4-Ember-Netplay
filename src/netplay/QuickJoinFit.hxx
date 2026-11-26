#pragma once
// Quick join's choice among the rooms it may join (docs/design/QUICK_JOIN.md):
// which one fits the player best, judged key by key. It orders and never rules a
// room out, so Quick join still finds any open seat; with nothing new listed every
// room ties and the pick is the list's own order (PublicRoomsPanel's Before).
#include "PublicRoomFilter.hxx"
#include "PublicRooms.hxx"
#include "Rankings.hxx"
#include "../session/RoomQueries.hxx"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace sf4e { namespace netplay { namespace publicrooms {

// The player Quick join looks for a room for: where they play from (the rooms
// list's FilterHome) and their class band, 0 for none.
struct Seeker {
	Area home = Area::None;
	std::uint8_t band = 0;
};

// The band the player's own standing gives Quick join: their class while they
// take part and the service shows classes, else none.
inline std::uint8_t SeekerBand(const rankings::Standing& standing) {
	return standing.kind == rankings::Standing::Kind::Ranked && standing.classesShown ? standing.badge.ClassBand() : 0;
}

struct QuickJoinFit {
	// The opponent plays from the player's area or one next to it (AreaNear).
	enum class Ping : std::uint8_t { Other, Near };
	// How far apart the two classes are: Close 0 or 1, Wide 2, Far 3 or more.
	// Unknown, either side has none, ranks between Wide and Far, so players still
	// placing get games.
	enum class Rank : std::uint8_t { Far, Unknown, Wide, Close };
	Ping ping = Ping::Other;
	Rank rank = Rank::Unknown;
	// Someone waits alone to play: the newcomer plays at once.
	bool waiting = false;
	// Ping before rank (a far match plays badly, an uneven one is only uneven),
	// then rank, then a waiting player.
	bool Better(const QuickJoinFit& other) const {
		return std::make_tuple(ping, rank, waiting) > std::make_tuple(other.ping, other.rank, other.waiting);
	}
};

inline QuickJoinFit::Rank RankBetween(std::uint8_t own, std::uint8_t other) {
	using Rank = QuickJoinFit::Rank;
	if (!own || !other) return Rank::Unknown;
	const int apart = own > other ? own - other : other - own;
	return apart <= 1 ? Rank::Close : apart == 2 ? Rank::Wide : Rank::Far;
}

// How `room` fits `seeker`. The opponent is a waiting player: in a room that
// seats by cabinet, whoever has waited longest (the first listed), since that is
// who a newcomer plays; in any other room the best fitting one, since the
// newcomer picks the table. A waiting player who hid their area counts from the
// room's. With nobody waiting the opponent is the room as a whole: its area and
// the lower median of its members' classes. Under Any every rank is Unknown.
inline QuickJoinFit FitOf(const Room& room, const Seeker& seeker, RoomFilter::Opponents opponents) {
	const auto fit = [&](std::uint8_t band, Area area, bool waiting) {
		QuickJoinFit result;
		result.ping = AreaNear(seeker.home, area == Area::None ? room.area : area) ? QuickJoinFit::Ping::Near : QuickJoinFit::Ping::Other;
		result.rank = opponents == RoomFilter::Opponents::Similar ? RankBetween(seeker.band, band) : QuickJoinFit::Rank::Unknown;
		result.waiting = waiting;
		return result;
	};
	if (room.waiting.empty()) {
		std::vector<std::uint8_t> ranked;
		for (const auto band : room.classes) if (band) ranked.push_back(band);
		std::sort(ranked.begin(), ranked.end());
		return fit(ranked.empty() ? 0 : ranked[(ranked.size() - 1) / 2], room.area, false);
	}
	const bool cabinets = room.type && room::RulesOf(*room.type).cabinets;
	const std::size_t considered = cabinets ? 1 : room.waiting.size();
	QuickJoinFit best = fit(room.waiting[0].band, room.waiting[0].area, true);
	for (std::size_t index = 1; index < considered; ++index) {
		const auto candidate = fit(room.waiting[index].band, room.waiting[index].area, true);
		if (candidate.Better(best)) best = candidate;
	}
	return best;
}

// Quick join's room, and whether it had to look past the player's class: the
// player has a class and wants similar ones, some room listed classes, and the
// room it picked is not near their class.
struct QuickJoinPick {
	std::string room;
	bool widened = false;
};

// The best fitting of `candidates`, each already joinable and listed by the
// player's filter, in the list's own order: the first of the best wins a tie.
inline std::optional<QuickJoinPick> PickQuickJoin(const std::vector<const Room*>& candidates, const Seeker& seeker,
	RoomFilter::Opponents opponents) {
	const Room* picked = nullptr;
	QuickJoinFit pickedFit;
	bool classesListed = false;
	for (const Room* room : candidates) {
		classesListed = classesListed || !room->classes.empty() || !room->waiting.empty();
		const auto fit = FitOf(*room, seeker, opponents);
		if (!picked || fit.Better(pickedFit)) { picked = room; pickedFit = fit; }
	}
	if (!picked) return std::nullopt;
	const bool nearClass = pickedFit.rank == QuickJoinFit::Rank::Close || pickedFit.rank == QuickJoinFit::Rank::Wide;
	return QuickJoinPick{picked->id, opponents == RoomFilter::Opponents::Similar && seeker.band && classesListed && !nearClass};
}

} } }
