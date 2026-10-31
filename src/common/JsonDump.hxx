#pragma once
// The one way room and recovery code serializes JSON: every recovery dump and
// every message IrohRoom sends the helper, whether it carries a player's or a
// peer's text (a build, an invitation, a ticket, an account, a peer identity)
// or only numbers, so there is no message to remember as the safe one.

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
