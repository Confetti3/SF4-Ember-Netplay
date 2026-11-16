#pragma once
// A tournament set's pack (ember_protocol::replays) split back into its games
// and the names its header carries; ReplaySets.hxx has the sets themselves.
#include "ReplaySets.hxx"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sf4e { namespace platform { namespace replays {
// A pack (ember_protocol::replays): EMBRPAK1, the header's length, the
// header (JSON), then each game's export, whose record says the replay's
// size. False for anything else.
constexpr std::size_t kMostPackBytes = 64 * 1024 + 32 * replayfiles::kMostArchiveBytes;
// The games from `at` to the end of `bytes`, each an export that is whole
// (WholeArchived). False for anything else, or a game cut short.
inline bool ReadGames(const replayslots::Bytes& bytes, std::size_t at, std::vector<replayslots::Bytes>& games) {
	using namespace replayslots;
	std::vector<Bytes> read;
	while (at < bytes.size()) {
		if (bytes.size() - at < kExportHeaderBytes) return false;
		const std::uint32_t replay = ReadU32(bytes.data() + at + 8 + 9);
		const std::size_t size = kExportHeaderBytes + replay;
		if (replay > kLargestReplay || bytes.size() - at < size) return false;
		Bytes game(bytes.begin() + at, bytes.begin() + at + size);
		if (!WholeArchived(game, ReadU32(game.data() + 8 + 5))) return false;
		read.push_back(std::move(game));
		at += size;
	}
	if (at != bytes.size()) return false;
	games = std::move(read);
	return true;
}
inline bool UnpackSet(const replayslots::Bytes& pack, std::vector<replayslots::Bytes>& games, std::string& header) {
	using namespace replayslots;
	if (pack.size() < 12 || pack.size() > kMostPackBytes || std::memcmp(pack.data(), "EMBRPAK1", 8)) return false;
	// Held against what is left of the pack before it is added to: this is a
	// 32-bit program, where 12 plus a length near 4 GB would wrap round.
	const std::uint32_t headerBytes = ReadU32(pack.data() + 8);
	if (headerBytes > pack.size() - 12) return false;
	const std::size_t headerEnd = 12 + static_cast<std::size_t>(headerBytes);
	if (!ReadGames(pack, headerEnd, games)) return false;
	header.assign(pack.begin() + 12, pack.begin() + headerEnd);
	return true;
}
// What Import takes: a set pack, one replay as the archive keeps it
// (.emberreplay, the file the archive site serves), or a replay as the game
// wrote it (.usf4replay), which is made an export as an archived one is. The
// games are held to the same rule whichever it was; a pack's header is
// returned, and the others have none.
inline bool UnpackFile(const replayslots::Bytes& file, std::vector<replayslots::Bytes>& games, std::string& header) {
	using namespace replayslots;
	if (file.size() >= 8 && !std::memcmp(file.data(), "EMBRPAK1", 8)) return UnpackSet(file, games, header);
	header.clear();
	Bytes exported;
	if (file.size() >= 8 && !std::memcmp(file.data(), kExportMagic, 8)) exported = file;
	else if (file.size() > kLargestReplay || !ExportFromReplayAlone(file, exported)) return false;
	// A replay file is one game: two exports back to back are not a replay.
	return ReadGames(exported, 0, games) && games.size() == 1;
}

// What an imported pack's header says of its games: the set, and its
// players' names by slot, which Table 0 seats as sides 1 and 2. Imported
// games count as watched, not played here.
inline replayfiles::ReplayNames PackNames(const std::string& header) {
	replayfiles::ReplayNames names;
	names.provenance = replayfiles::Provenance::Spectated;
	const auto said = nlohmann::json::parse(header, nullptr, false);
	if (!said.is_object()) return names;
	if (said.contains("match_id") && said["match_id"].is_string()) names.set.matchId = said["match_id"].get<std::string>();
	if (said.contains("names") && said["names"].is_array() && said["names"].size() == 2)
		for (std::size_t side = 0; side < 2; ++side)
			if (said["names"][side].is_string() && said["names"][side].get_ref<const std::string&>().size() <= 1024)
				names.players[side] = said["names"][side].get<std::string>();
	return names;
}

} } }
