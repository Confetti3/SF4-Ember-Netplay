#include "RoomHostStatus.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace roomhost {

std::string StatusLine(std::size_t members, std::size_t tablesPlaying, const std::string& invitation,
	const std::vector<std::string>& banned) {
	return nlohmann::json{{"type", "status"}, {"members", members}, {"tables_playing", tablesPlaying},
		{"invitation", invitation}, {"banned", banned}}.dump();
}

} }
