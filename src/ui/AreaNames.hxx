#pragma once
#include "../common/Area.hxx"
#include "../common/Localization.hxx"
#include <string>
#include <vector>

// The areas in words: an area's name, and the player's choice of area as the
// Settings row shows it. One wording for Settings, Edit card, the player card
// and the public rooms.
namespace sf4e { namespace ui {

// "NA West"; empty for no area.
inline std::string AreaName(Area area) {
	const char* key = AreaKey(area);
	return key ? std::string(loc::T(key)) : std::string();
}

// What a choice shows: "Automatic (NA West, from Windows)", or Automatic with
// nothing to follow, an area's name, or Hidden. `automatic` is the area
// Windows gives (AreaFromWindows).
inline std::string AreaChoiceText(const AreaPreference& choice, Area automatic) {
	switch (choice.kind) {
	case AreaPreference::Kind::Chosen: return AreaName(choice.area);
	case AreaPreference::Kind::Hidden: return loc::T("settings.area.hidden");
	default: return automatic == Area::None ? std::string(loc::T("settings.area.automatic_none")) : loc::Tf("settings.area.automatic_value", AreaName(automatic));
	}
}

// Every choice in the order Left and Right step through them (AreaPreference::Stepped).
inline std::vector<std::string> AreaChoiceTexts(Area automatic) {
	std::vector<std::string> texts;
	for (AreaPreference choice; ; choice = choice.Stepped(1)) {
		texts.push_back(AreaChoiceText(choice, automatic));
		if (choice == AreaPreference::Hidden()) return texts;
	}
}

} }
