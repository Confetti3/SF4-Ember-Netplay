#pragma once
// A tournament set's games in this PC's archive (docs/design/TOURNAMENT_DESK.md,
// 4b): the next one to send to the set's bridge, the sets for Replays ›
// Tournaments, and a set's pack split back into its games.
#include "ReplayFiles.hxx"
#include <algorithm>
#include <string>
#include <vector>

namespace sf4e { namespace platform { namespace replays {

// The oldest replay of a set this PC played that its bridge may not have yet,
// or null. `archive` is a listing, newest first.
inline const ArchivedReplay* NextToSend(const std::vector<ArchivedReplay>& archive) {
	for (auto at = archive.rbegin(); at != archive.rend(); ++at)
		if (at->set.Tagged() && !at->spectated && !at->sent) return &*at;
	return nullptr;
}

// A refusal of a send that sending it again cannot change: the bridge has
// no such set for this player, or the file is not a replay it takes. Any
// other failure is tried again later.
inline bool SendSettled(const std::string& code) {
	for (const char* settled : {"invalid_request", "invalid_replay", "not_found", "forbidden", "unsupported_rules", "corrupt", "file_unreadable", "invalid_path"})
		if (code == settled) return true;
	return false;
}

// One set: its games oldest first, and how many rooms they were played in.
// A set imported from a pack has no bridge.
struct ArchivedSet {
	std::string bridgeId, matchId;
	std::vector<const ArchivedReplay*> games;
	std::size_t rooms = 0;
};

// The listing's sets, the set played last first. The games point into
// `archive`, which must outlive them.
inline std::vector<ArchivedSet> Sets(const std::vector<ArchivedReplay>& archive) {
	std::vector<ArchivedSet> sets;
	for (const auto& replay : archive) {
		if (replay.set.matchId.empty()) continue;
		auto found = std::find_if(sets.begin(), sets.end(), [&](const ArchivedSet& set) { return set.matchId == replay.set.matchId; });
		if (found == sets.end()) { sets.push_back({replay.set.bridgeId, replay.set.matchId, {}, 0}); found = sets.end() - 1; }
		found->games.insert(found->games.begin(), &replay);
	}
	for (auto& set : sets) {
		std::vector<std::uint64_t> revisions;
		for (const auto* game : set.games) revisions.push_back(game->set.revision);
		std::sort(revisions.begin(), revisions.end());
		set.rooms = static_cast<std::size_t>(std::unique(revisions.begin(), revisions.end()) - revisions.begin());
	}
	return sets;
}

// A pack (ember_protocol::replays): EMBRPAK1, the header's length, the
// header (JSON), then each game's export, whose record says the replay's
// size. False for anything else.
constexpr std::size_t kMostPackBytes = 64 * 1024 + 32 * replayfiles::kMostArchiveBytes;
inline bool UnpackSet(const replayslots::Bytes& pack, std::vector<replayslots::Bytes>& games, std::string& header) {
	using namespace replayslots;
	if (pack.size() < 12 || pack.size() > kMostPackBytes || std::memcmp(pack.data(), "EMBRPAK1", 8)) return false;
	std::size_t at = 12 + ReadU32(pack.data() + 8);
	std::vector<Bytes> read;
	while (at < pack.size() && at >= 12) {
		if (pack.size() - at < kExportHeaderBytes) return false;
		const std::uint32_t replay = ReadU32(pack.data() + at + 8 + 9);
		const std::size_t size = kExportHeaderBytes + replay;
		if (replay > kLargestReplay || pack.size() - at < size) return false;
		Bytes game(pack.begin() + at, pack.begin() + at + size);
		if (!WholeArchived(game, ReadU32(game.data() + 8 + 5))) return false;
		read.push_back(std::move(game));
		at += size;
	}
	if (at != pack.size()) return false;
	header.assign(pack.begin() + 12, pack.begin() + 12 + ReadU32(pack.data() + 8));
	games = std::move(read);
	return true;
}

} } }
