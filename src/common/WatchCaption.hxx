#pragma once

// How a replay's noted players show on a caption: each side's class as a band, and what Watch
// draws over a replay while it plays. A display rule beside the request, not part of it.

#include <string>

#include "ReplayRequest.hxx"
#include "SkillBadge.hxx"

namespace sf4e { namespace replay {

// The classes a replay noted (platform::replays::ArchivedReplay::classes) put
// on an export's caption as bands, when the player shows class emblems;
// nothing else changes. Only a class has a band: anything else stays 0.
inline void SetCaptionClasses(Caption& caption, const SkillBadge (&classes)[2], bool emblems) {
	for (int side = 0; side < 2; ++side)
		caption.band[side] = emblems && classes[side].HasClass() && classes[side].Valid() ? classes[side].band : 0;
}
// What Watch draws over a replay: each side's noted name on its plate with
// its class, as an export's caption draws them, but only when a side has
// both a name and a class to show (class emblems on). Nothing otherwise,
// which leaves the game's own names as they are.
inline Caption WatchCaption(const std::string (&names)[2], const SkillBadge (&classes)[2], bool emblems) {
	Caption caption;
	SetCaptionClasses(caption, classes, emblems);
	bool shown = false;
	for (int side = 0; side < 2; ++side) shown = shown || (caption.band[side] && !names[side].empty());
	if (!shown) return Caption();
	caption.names = true;
	caption.name[0] = names[0]; caption.name[1] = names[1];
	return caption;
}

} }
