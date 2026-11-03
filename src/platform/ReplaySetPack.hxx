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
