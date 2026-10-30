#pragma once
// The one way room and recovery code serializes JSON that can carry text from
// a player or a peer.

#include <string>
#include <nlohmann/json.hpp>

namespace sf4e {

// json::dump() throws type_error 316 on a string that is not UTF-8, and a
// throw out of the room's game-thread tick ends the game. Here such bytes come
// out as U+FFFD, so a dump never throws. Digests, sizes and the bytes sent all
// come from this one function: a value that held bad bytes gives the same
// replaced text everywhere, and a value that held none gives exactly what
// dump() gives.
inline std::string DumpJson(const nlohmann::json& value) {
	return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}
