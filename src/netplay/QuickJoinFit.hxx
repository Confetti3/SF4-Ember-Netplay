#pragma once
// Quick join's choice among the rooms it may join (docs/design/QUICK_JOIN.md):
// which one fits the player best, judged key by key. It orders and never rules a
// room out, so Quick join still finds any open seat; with nothing new listed every
// room ties and the pick is the list's own order (PublicRoomsPanel's Before).
#include "PublicRooms.hxx"
#include "Rankings.hxx"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace sf4e { namespace netplay { namespace publicrooms {

// The player Quick join looks for a room for: where they play from (the rooms
// list's FilterHome) and their class band, 0 for none. A player who asked for any
// class (PlayerPreferences::quickJoinAnyClass) seeks with band 0: every rank is
// then Unknown and nothing is said about classes.
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
	// The class is near the player's own.
	bool NearClass() const { return rank == Rank::Close || rank == Rank::Wide; }
};

inline QuickJoinFit::Rank RankBetween(std::uint8_t own, std::uint8_t other) {
	using Rank = QuickJoinFit::Rank;
	if (!own || !other) return Rank::Unknown;
	const int apart = own > other ? own - other : other - own;
	return apart <= 1 ? Rank::Close : apart == 2 ? Rank::Wide : Rank::Far;
}

// How `room` fits `seeker`. The opponent is the best fitting of the waiting players
// the room lists, those a newcomer would meet at once (a room that seats by cabinet
// lists only the one the newcomer would play). A waiting player who hid their area
// counts from the room's. With nobody waiting the opponent is the room as a whole:
// its area and the lower median of its members' classes.
inline QuickJoinFit FitOf(const Room& room, const Seeker& seeker) {
	const auto fit = [&](std::uint8_t band, Area area, bool waiting) {
		QuickJoinFit result;
		result.ping = AreaNear(seeker.home, area == Area::None ? room.area : area) ? QuickJoinFit::Ping::Near : QuickJoinFit::Ping::Other;
		result.rank = RankBetween(seeker.band, band);
		result.waiting = waiting;
		return result;
	};
	if (room.waiting.empty()) {
		std::vector<std::uint8_t> ranked;
		for (const auto band : room.classes) if (band) ranked.push_back(band);
		std::sort(ranked.begin(), ranked.end());
		return fit(ranked.empty() ? 0 : ranked[(ranked.size() - 1) / 2], room.area, false);
	}
	QuickJoinFit best = fit(room.waiting[0].band, room.waiting[0].area, true);
	for (std::size_t index = 1; index < room.waiting.size(); ++index) {
		const auto candidate = fit(room.waiting[index].band, room.waiting[index].area, true);
		if (candidate.Better(best)) best = candidate;
	}
	return best;
}

// Quick join's room, and whether it had to look past the player's class: the
// player has a class, some room listed classes, and no room Quick join could pick
// is near their class.
struct QuickJoinPick {
	std::string room;
	bool widened = false;
};

// The best fitting of `candidates`, each already joinable and listed by the
// player's filter, in the list's own order: the first of the best wins a tie.
inline std::optional<QuickJoinPick> PickQuickJoin(const std::vector<const Room*>& candidates, const Seeker& seeker) {
	const Room* picked = nullptr;
	QuickJoinFit pickedFit;
	bool classesListed = false, nearClass = false;
	for (const Room* room : candidates) {
		classesListed = classesListed || !room->classes.empty() || !room->waiting.empty();
		const auto fit = FitOf(*room, seeker);
		nearClass = nearClass || fit.NearClass();
		if (!picked || fit.Better(pickedFit)) { picked = room; pickedFit = fit; }
	}
	if (!picked) return std::nullopt;
	return QuickJoinPick{picked->id, seeker.band && classesListed && !nearClass};
}

} } }
