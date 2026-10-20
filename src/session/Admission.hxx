#pragma once
// What a public room's ticket admitted a connection as: the player's Ember ID
// and the class the bridge signed in with it. The room host's helper reports
// it; the session server hands it to the room model at join, never taking it
// from the joiner. A private room has none.
#include "../common/SkillBadge.hxx"
#include <string>

namespace sf4e {
struct Admission {
	std::string account;
	SkillBadge badge;
};
}
