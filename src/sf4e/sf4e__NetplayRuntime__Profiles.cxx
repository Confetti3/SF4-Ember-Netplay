// Other players' profiles, the Find a player search and the player's own card (docs/design/PROFILE.md, "Runtime and
// view"). The look-ups and searches go to the helper on the tournament channel and their answers come back through
// flights; netplay::profiles::Requests decides what each answer does, and the interface reads the result from the
// published status. The player's own card is saved when it changed (netplay/CardBundle.hxx).
#include "sf4e__NetplayRuntime.hxx"
#include "../session/TournamentAnswers.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using nlohmann::json;

std::string CurrentId() { return runtime->room ? runtime->room->Identity().emberId : std::string(); }
namespace profiles = netplay::profiles;

// A look-up's answer for the ticket it was made under: the one card, or nothing read.
void TakeLookup(profiles::Requests::Ticket ticket, const session::TournamentAnswer& answer) {
	auto& requests = runtime->profiles;
	const auto now = GetTickCount64();
	if (!answer.ok) {
		const auto failure = netplay::BridgeFlights::Reason(answer);
		spdlog::info("Profile: the card was not read: {}", failure);
		requests.FinishLookup(ticket, failure, profiles::Looked{}, now);
		return;
	}
	auto decoded = session::DecodeCards(answer.data);
	profiles::Looked looked;
	if (decoded && decoded->size() == 1) {
		looked.read = true;
		looked.card = std::move((*decoded)[0]);
	}
	if (!looked.read) spdlog::info("Profile: the card answer could not be read");
	requests.FinishLookup(ticket, std::string(), looked, now);
}

// A search's answer for the ticket it was made under, for a name's profile or for Find a player.
void TakeSearch(profiles::Requests::Ticket ticket, const session::TournamentAnswer& answer) {
	auto& requests = runtime->profiles;
	const auto now = GetTickCount64();
	if (!answer.ok) {
		const auto failure = netplay::BridgeFlights::Reason(answer);
		spdlog::info("Profile: the search was not answered: {}", failure);
		requests.FinishSearch(ticket, failure, std::nullopt, now);
		return;
	}
	requests.FinishSearch(ticket, std::string(), session::DecodeSearch(answer.data), now);
}

void SendAsk(const profiles::Requests::Ask& ask, bool helperReady) {
	using Ask = profiles::Requests::Ask;
	const auto& bridge = runtime->profiles.Bridge();
	const bool search = ask.kind == Ask::Kind::Search;
	const json request = search ? profiles::SearchRequest(bridge, ask.value, ask.selector == profiles::Key::Kind::Code) :
		profiles::LookupRequest(bridge, ask.selector, ask.value);
	const auto ticket = ask.ticket;
	const auto handler = [search, ticket](const session::TournamentAnswer& answer) {
		if (search) TakeSearch(ticket, answer);
		else TakeLookup(ticket, answer);
	};
	if (!request.is_null()) {
		internal::SendFlight(request, helperReady, handler);
		return;
	}
	// A request that cannot be made (no selector to look up by) ends at once, as a refusal.
	session::TournamentAnswer refused;
	refused.reason = "invalid_request";
	handler(refused);
}

std::string Trimmed(const std::string& text) {
	const auto first = text.find_first_not_of(" \t");
	if (first == std::string::npos) return {};
	return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// The device Automatic stands for now: the controller this PC plays with.
profiles::Device AutomaticDeviceNow() {
	const auto& device = runtime->input.Selected();
	return profiles::AutomaticDevice(device.type, device.connected, Game().input.BrandForDevice(device.type, device.index));
}

// What the card shows of how this player plays, as the game measures it now. The connection is read again at most once
// a minute, and when a room is joined or left, but never while a match is played on this PC: the adapter probe runs on
// this thread and waits on the system (under Wine it reads files), so the card keeps the last reading until the match
// is over. A room stay holds a match object throughout, so a match is on only while that match is live (or a battle
// runs). The area is the player's, from Settings and Windows.
profiles::Style OwnStyleNow(std::uint64_t nowMs) {
	auto& signals = runtime->cardSignals;
	const std::uint64_t epoch = RuntimeSession::netplay ? RuntimeSession::netplay->client.GetRoomSnapshot().roomEpoch : 0;
	const bool due = !signals.linkRead || nowMs - signals.linkMs >= netplay::profiles::Saver::MinGapMs || epoch != signals.linkEpoch;
	const bool matchRunning = Game().battle.Live() || (runtime->match && runtime->match->Live());
	if (due && !matchRunning) {
		signals.link = DetectNetworkLink();
		signals.linkRead = true;
		signals.linkMs = nowMs;
		signals.linkEpoch = epoch;
	}
	return profiles::OwnStyle(runtime->preferences, AutomaticDeviceNow(), signals.link, runtime->automaticArea);
}

// The player's card goes to the service when it changed: at start, and after Edit card saves.
void TickCardSave(bool helperReady, std::uint64_t nowMs) {
	auto& saver = runtime->cardSaver;
	const auto bridge = runtime->levels.Bridge();
	const auto id = CurrentId();
	if (bridge.empty() || id.empty()) return;
	if (!helperReady) return;
	const auto& own = runtime->levels.Published();
	// Only a Discord account has a card.
	if (own.loaded && own.value.kind == netplay::levels::Own::Kind::NoAccount) return;
	// The bundle is built again only when what it is made from moved: the preferences, Windows' region or the style.
	auto& signals = runtime->cardSignals;
	const auto style = OwnStyleNow(nowMs);
	const auto revision = runtime->preferencesStore.Revision();
	if (!signals.built || revision != signals.builtRevision || !(runtime->automaticCountry == signals.builtCountry) || !(style == signals.builtStyle)) {
		signals.bundle = profiles::BundleOf(runtime->preferences, runtime->automaticCountry, style);
		signals.built = true;
		signals.builtRevision = revision;
		signals.builtCountry = runtime->automaticCountry;
		signals.builtStyle = style;
	}
	const auto& bundle = signals.bundle;
	if (!saver.Due(bridge, id, bundle, nowMs)) return;
	saver.Sent(bridge, id, bundle, nowMs);
	spdlog::info("Profile: the card is sent to the service");
	internal::SendFlight(profiles::CardSaveRequest(bridge, bundle), helperReady, [](const session::TournamentAnswer& answer) {
		const auto now = GetTickCount64();
		const auto failure = answer.ok ? std::string() : netplay::BridgeFlights::Reason(answer);
		if (!failure.empty()) spdlog::info("Profile: the card was not saved: {}", failure);
		runtime->cardSaver.Finish(failure);
		if (failure.empty()) runtime->profiles.NoteService(profiles::Service::Offered, now);
		else if (failure == profiles::Unavailable) runtime->profiles.NoteService(profiles::Service::Missing, now);
	});
}
}

namespace internal {
void DispatchProfile(const commands::ProfileOpen& command) {
	if (command.bridge.empty() || CurrentId().empty()) return;
	runtime->profiles.Scope(command.bridge, CurrentId());
	runtime->profiles.Open(command.key, command.known, GetTickCount64());
}

void DispatchProfile(const commands::ProfileClose&) {
	runtime->profiles.Close();
}

// A text under two bytes asks for nothing and changes nothing; an empty one clears the search.
void DispatchProfile(const commands::PlayerSearch& command) {
	if (command.bridge.empty() || CurrentId().empty()) return;
	const auto text = Trimmed(command.text);
	if (text.size() == 1) return;
	runtime->profiles.Scope(command.bridge, CurrentId());
	runtime->profiles.Search(text, command.code, GetTickCount64());
}

void NoteProfileAction(const std::string& token, netplay::levels::Relation relation) {
	runtime->profiles.Refresh();
	if (!token.empty()) runtime->profiles.Relate(token, relation);
}

// The profile open and the search are asked for when due, and the player's own card is saved when it changed.
void TickProfiles(bool helperReady, std::uint64_t nowMs) {
	auto& requests = runtime->profiles;
	// Another Ember ID's profiles are not shown.
	if (!requests.Bridge().empty() && requests.EmberId() != CurrentId()) requests.Scope({}, {});
	requests.Tick(nowMs);
	if (helperReady && !requests.Bridge().empty())
		if (const auto ask = requests.Take(nowMs)) SendAsk(ask, helperReady);
	TickCardSave(helperReady, nowMs);
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
