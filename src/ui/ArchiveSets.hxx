#pragma once
// How the Archive tab groups the listing's games into rows, as the archive
// page does (server/ember/archive-site/site/assets/archive.js: group): a
// tournament set by its set ID, and the other (online) games into a set of the
// same two named players, or a session of one named player's games, while each
// game follows the last within half an hour. No ImGui, no text: what a row
// says of an entry is ArchiveTab's, so this is tested without a frame
// (archive_sets_test.cxx).
#include "../netplay/Archive.hxx"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace archivesets {

// Online games this close together, of the same players, are one set.
constexpr std::uint64_t SetGapSeconds = 30 * 60;

// One row: a tournament set, two players' games in a row, or one player's session.
struct Entry {
	enum class Kind : std::uint8_t {
		// A tournament set, whose games share their set ID.
		Set,
		// Online games of two named players, or a game one side of which nobody named.
		Match,
		// Online games one named player shared of several opponents.
		Session,
	} kind = Kind::Match;
	// The set's ID, else the digest of the newest game of an online group.
	std::string id;
	// The games, oldest first.
	std::vector<const netplay::archive::Game*> games;
	// A session's player.
	std::string player;
	// A tournament set's score and pack, when the listing holds them.
	const netplay::archive::SetInfo* info = nullptr;
	// The highest class either side of any game shows, 0 for none.
	int bestClass = 0;
	// The save time of the newest game.
	std::uint64_t newest = 0;

	const netplay::archive::Game& First() const { return *games.front(); }
	const netplay::archive::Game& Last() const { return *games.back(); }
	bool Tournament() const { return kind == Kind::Set; }
};

// The side `player` played in `game`: 0 when they are the first named, else 1.
inline int SideOf(const netplay::archive::Game& game, const std::string& player) { return game.names[0] == player ? 0 : 1; }

// The fighters a session's player met, each once, in the order they were met.
inline std::vector<int> FightersMet(const Entry& entry) {
	std::vector<int> met;
	for (const auto* game : entry.games) {
		const int fighter = game->fighters[1 - SideOf(*game, entry.player)];
		if (fighter >= 0 && std::find(met.begin(), met.end(), fighter) == met.end()) met.push_back(fighter);
	}
	return met;
}

// A tournament set's score for the first and the second player of its first game, as the listing has it.
inline std::optional<std::array<unsigned, 2>> Score(const Entry& entry) {
	if (!entry.Tournament() || !entry.info) return std::nullopt;
	return entry.info->score;
}

// The grouping of `listing`'s games, newest set first, or by the highest class
// on it first (newest first within a class) when the bridge listed by class.
inline std::vector<Entry> Group(const netplay::archive::Listing& listing) {
	using netplay::archive::Game;
	std::vector<const Game*> newest;
	newest.reserve(listing.games.size());
	for (const auto& game : listing.games) newest.push_back(&game);
	// Whatever order the games came in, sets and sessions are told apart newest first.
	std::stable_sort(newest.begin(), newest.end(), [](const Game* a, const Game* b) {
		return a->savedAt != b->savedAt ? a->savedAt > b->savedAt : a->digest > b->digest;
	});
	std::vector<Entry> entries;
	// What an online entry groups by: both names, or one, or nothing.
	std::vector<std::string> keys;
	std::map<std::string, std::size_t> tournaments;
	for (const Game* game : newest) {
		if (!game->set.empty()) {
			const auto found = tournaments.emplace(game->set, entries.size());
			if (found.second) {
				Entry entry;
				entry.kind = Entry::Kind::Set;
				entry.id = game->set;
				entries.push_back(std::move(entry));
				keys.emplace_back();
			}
			entries[found.first->second].games.push_back(game);
			continue;
		}
		const bool both = !game->names[0].empty() && !game->names[1].empty();
		std::string key;
		std::string player;
		if (both) {
			key = "pair:" + std::min(game->names[0], game->names[1]) + "\n" + std::max(game->names[0], game->names[1]);
		} else if (!game->names[0].empty() || !game->names[1].empty()) {
			player = game->names[0].empty() ? game->names[1] : game->names[0];
			key = "player:" + player;
		}
		std::size_t at = entries.size();
		if (!key.empty())
			for (std::size_t i = 0; i < entries.size(); ++i)
				if (!entries[i].Tournament() && keys[i] == key && entries[i].games.back()->savedAt - game->savedAt <= SetGapSeconds) { at = i; break; }
		if (at == entries.size()) {
			Entry entry;
			entry.id = game->digest;
			entry.player = player;
			entries.push_back(std::move(entry));
			keys.push_back(key);
		}
		entries[at].games.push_back(game);
	}
	for (std::size_t i = 0; i < entries.size(); ++i) {
		auto& entry = entries[i];
		std::reverse(entry.games.begin(), entry.games.end());
		// One player's several games are a session; one game, or two players', a match.
		if (!entry.Tournament() && keys[i].compare(0, 7, "player:") == 0 && entry.games.size() > 1) entry.kind = Entry::Kind::Session;
		else if (!entry.Tournament()) entry.player.clear();
		if (entry.Tournament()) {
			const auto info = listing.sets.find(entry.id);
			if (info != listing.sets.end()) entry.info = &info->second;
		}
		entry.newest = entry.games.back()->savedAt;
		for (const auto* game : entry.games)
			for (const int band : game->classes) entry.bestClass = (std::max)(entry.bestClass, band);
	}
	if (listing.order == netplay::archive::Order::Class)
		std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
			return a.bestClass != b.bestClass ? a.bestClass > b.bestClass : a.newest > b.newest;
		});
	return entries;
}

} } }
