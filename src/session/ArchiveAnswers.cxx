// The helper's answer to the archive listing request (archive_list), as
// TournamentAnswers.hxx declares it (docs/design/REPLAY_ARCHIVE.md).
#include "TournamentAnswers.hxx"

#include <algorithm>
#include <stdexcept>

#include "../common/SkillBadge.hxx"
#include "AnswerReading.hxx"

namespace sf4e { namespace session {
using namespace reading;

namespace {
namespace limits = netplay::archive;
constexpr std::size_t MaxArchiveText = limits::MaxShown;

// A text of the archive's, one line and cut short rather than refused; empty for a member that is missing or not text.
std::string ArchiveText(const json& object, const char* key, std::size_t most = MaxArchiveText) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_string()) return {};
	auto text = SingleLine(found->get<std::string>());
	if (text.size() > most) text.resize(most);
	return text;
}

// The two members of an array member, each read by `one`; a missing or malformed array leaves both as they were.
template <class One>
void Pair(const json& object, const char* key, One one) {
	const auto found = object.find(key);
	if (found == object.end() || !found->is_array() || found->size() != 2) return;
	one(0, (*found)[0]);
	one(1, (*found)[1]);
}

// A whole number within a range; anything else (a value a later bridge adds, null) is `otherwise`.
int Whole(const json& value, int least, int most, int otherwise) {
	if (!value.is_number_integer()) return otherwise;
	const auto number = value.get<std::int64_t>();
	return number < least || number > most ? otherwise : static_cast<int>(number);
}

// A game with the digest and address to fetch it by, or an exception for the decoder to leave it out.
netplay::archive::Game DecodeArchiveGame(const json& entry) {
	netplay::archive::Game game;
	game.digest = Text(entry, "digest", 64);
	if (!IsHex(game.digest, 64)) throw std::invalid_argument("digest");
	game.url = Text(entry, "url", limits::MaxUrl);
	if (game.url.empty()) throw std::invalid_argument("url");
	game.crc = ArchiveText(entry, "crc", 8);
	game.savedAt = OptionalSeconds(entry, "saved_at");
	game.size = OptionalSeconds(entry, "size");
	Pair(entry, "fighters", [&](int side, const json& value) { game.fighters[side] = Whole(value, 0, 255, -1); });
	Pair(entry, "names", [&](int side, const json& value) { if (value.is_string()) game.names[side] = SingleLine(value.get<std::string>()).substr(0, MaxArchiveText); });
	Pair(entry, "countries", [&](int side, const json& value) {
		const auto code = value.is_string() ? value.get<std::string>() : std::string();
		if (code.size() == 2 && std::all_of(code.begin(), code.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) game.countries[side] = code;
	});
	Pair(entry, "classes", [&](int side, const json& value) { game.classes[side] = Whole(value, 0, SkillBadge::Classes, 0); });
	game.set = ArchiveText(entry, "set");
	game.event = ArchiveText(entry, "event");
	game.round = ArchiveText(entry, "round");
	game.phase = ArchiveText(entry, "phase");
	return game;
}

std::vector<std::string> ArchiveTexts(const json& data, const char* key, std::size_t most, std::size_t exactly = 0) {
	std::vector<std::string> texts;
	const auto found = data.find(key);
	if (found == data.end() || !found->is_array()) return texts;
	for (const auto& item : *found) {
		if (texts.size() == most) break;
		if (!item.is_string()) continue;
		auto text = SingleLine(item.get<std::string>());
		if (text.size() > MaxArchiveText) text.resize(MaxArchiveText);
		if (!exactly || text.size() == exactly) texts.push_back(std::move(text));
	}
	return texts;
}
}

std::optional<netplay::archive::Listing> DecodeArchivePage(const json& data) {
	try {
		netplay::archive::Listing page;
		const auto& games = data.at("games");
		if (!games.is_array()) return std::nullopt;
		// A game that cannot be fetched is left out; the page goes on without it.
		for (const auto& entry : games) {
			if (page.games.size() == limits::MaxGames) break;
			try { page.games.push_back(DecodeArchiveGame(entry)); } catch (const std::exception&) {}
		}
		if (const auto sets = data.find("sets"); sets != data.end() && sets->is_object()) {
			for (auto it = sets->begin(); it != sets->end() && page.sets.size() < limits::MaxGames; ++it) {
				if (it.key().size() > MaxArchiveText || !it.value().is_object()) continue;
				netplay::archive::SetInfo info;
				Pair(it.value(), "score", [&](int side, const json& value) { info.score[side] = static_cast<unsigned>(Whole(value, 0, 1000000, 0)); });
				info.pack = ArchiveText(it.value(), "pack", limits::MaxUrl);
				page.sets[it.key()] = std::move(info);
			}
		}
		page.events = ArchiveTexts(data, "events", limits::MaxEvents);
		page.countries = ArchiveTexts(data, "countries", limits::MaxCountries, 2);
		if (const auto found = data.find("total"); found != data.end() && found->is_number_unsigned()) page.total = found->get<std::uint64_t>();
		if (const auto found = data.find("matching"); found != data.end() && found->is_number_unsigned()) page.matching = found->get<std::uint64_t>();
		// An order this build does not know is the newest, which every bridge lists.
		const auto order = data.find("sort");
		page.order = order != data.end() && order->is_string() && order->get<std::string>() == "class" ? netplay::archive::Order::Class : netplay::archive::Order::Newest;
		// Only a cursor in the shape the bridge hands out is a way to go on.
		page.next = ArchiveText(data, "next", limits::MaxCursor);
		for (const char c : page.next)
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '.')) { page.next.clear(); break; }
		return page;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

} }
