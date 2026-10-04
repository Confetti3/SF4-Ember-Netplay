#include "RoomHostStatus.hxx"
#include "../common/FighterCatalog.hxx"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace sf4e { namespace roomhost {

namespace {
// One decoded character: its code point and the bytes it came from.
struct Glyph { char32_t code; std::string bytes; };

// Valid UTF-8 only; any malformed byte becomes a space so the result can be
// serialized and the supervisor's JSON parser never sees a bad sequence.
std::vector<Glyph> Decode(const std::string& text) {
	std::vector<Glyph> glyphs;
	for (std::size_t i = 0; i < text.size();) {
		const unsigned char lead = static_cast<unsigned char>(text[i]);
		const std::size_t length = lead < 0x80 ? 1 : lead >= 0xC2 && lead < 0xE0 ? 2 : lead >= 0xE0 && lead < 0xF0 ? 3 : lead >= 0xF0 && lead < 0xF5 ? 4 : 0;
		char32_t code = length == 1 ? lead : length == 2 ? lead & 0x1F : length == 3 ? lead & 0x0F : lead & 0x07;
		bool valid = length != 0 && i + length <= text.size();
		for (std::size_t k = 1; valid && k < length; ++k) {
			const unsigned char next = static_cast<unsigned char>(text[i + k]);
			if ((next & 0xC0) != 0x80) valid = false;
			else code = code << 6 | (next & 0x3F);
		}
		// Overlong forms, surrogates and values past U+10FFFF are not characters.
		if (valid && ((length == 3 && code < 0x800) || (length == 4 && code < 0x10000) || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)) valid = false;
		if (!valid) { glyphs.push_back({U' ', " "}); ++i; continue; }
		glyphs.push_back({code, text.substr(i, length)});
		i += length;
	}
	return glyphs;
}

bool IsControl(char32_t c) { return c < 0x20 || (c >= 0x7F && c <= 0x9F); }

// Unicode White_Space, which is what the bridge's trim() removes.
bool IsSpace(char32_t c) {
	return c == U' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
		c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
}

std::string HostDisplayName(const std::string& name) {
	auto glyphs = Decode(name);
	for (auto& glyph : glyphs)
		if (IsControl(glyph.code)) glyph = {U' ', " "};
	std::size_t first = 0, last = glyphs.size();
	while (first < last && IsSpace(glyphs[first].code)) ++first;
	// Cut at the byte limit on a character boundary, then trim what the cut exposed.
	std::size_t bytes = 0, end = first;
	while (end < last && bytes + glyphs[end].bytes.size() <= MaximumHostNameBytes) bytes += glyphs[end++].bytes.size();
	last = end;
	while (last > first && IsSpace(glyphs[last - 1].code)) --last;
	std::string result;
	for (std::size_t i = first; i < last; ++i) result += glyphs[i].bytes;
	return result;
}

// What the bridge lists for the room: the moderator (snapshot.host) first, then
// the other members in join order, each as a main fighter id or 255.
RoomDetails DetailsOf(const room::Snapshot& snapshot) {
	RoomDetails details;
	details.name = snapshot.name;
	details.capacity = snapshot.capacity;
	details.locked = snapshot.locked;
	std::vector<const room::Member*> ordered;
	const room::Member* moderator = nullptr;
	for (const auto& member : snapshot.members) {
		if (snapshot.host != 0 && member.id == snapshot.host) moderator = &member;
		else ordered.push_back(&member);
	}
	std::sort(ordered.begin(), ordered.end(), [](const room::Member* a, const room::Member* b) { return a->joinOrder < b->joinOrder; });
	if (moderator) {
		details.hostName = HostDisplayName(moderator->name);
		ordered.insert(ordered.begin(), moderator);
	}
	for (const auto* member : ordered) {
		if (details.fighters.size() >= MaximumDetailFighters) break;
		details.fighters.push_back(member->mainFighter >= 0 && member->mainFighter < selection::FighterCount ? member->mainFighter : NoFighter);
	}
	// Table 0 holds the room's rules; the format values and the rotation enum's
	// numbers are the wire values (RoomDetails).
	details.setFormat = static_cast<int>(snapshot.tables[0].rules.format);
	details.rotation = static_cast<int>(snapshot.tables[0].rules.rotation);
	return details;
}

std::string StatusLine(std::size_t members, std::size_t tablesPlaying, const std::string& invitation,
	const std::vector<std::string>& banned, const RoomDetails& details) {
	nlohmann::json detail{{"name", details.name}, {"capacity", details.capacity}, {"locked", details.locked},
		{"fighters", details.fighters}, {"set_format", details.setFormat}, {"rotation", details.rotation}};
	if (!details.hostName.empty()) detail["host_name"] = details.hostName;
	// A room name that is not UTF-8 must not throw out of the host: replace it.
	return nlohmann::json{{"type", "status"}, {"members", members}, {"tables_playing", tablesPlaying},
		{"invitation", invitation}, {"banned", banned}, {"details", detail}}.dump(-1, ' ', false,
		nlohmann::json::error_handler_t::replace);
}

} }
