#pragma once
// A stage as a screen lists it for the room's Banned stages rule: the game's
// stage id (the one the catalog and the room use) and the name to show.
#include <cstdint>
#include <string>

namespace sf4e {

struct StageChoice {
	std::int64_t id = 0;
	std::string name;
};

}
