// The public replay archive's browser (docs/design/REPLAY_ARCHIVE.md, "Listing"):
// pages of the listing, and a listed game downloaded to be watched. Requests go
// to the helper on the tournament channel and are answered by request id,
// through runtime->flights like the public rooms'; netplay::archive::Requests
// decides what each answer does, and the interface reads the result from the
// published status. A downloaded game is played as a replay link's file is
// (RunReplayRequest, Watch), once it is on disk.
//
// There are two browsers, each its own netplay::archive::Requests: the Archive
// tab's (runtime->archive, ShellView::archiveBrowse) and the sets of the
// profile open (runtime->profileSets, ShellView::profileSets), so a profile's
// sets never replace the Archive tab's listing. A command names its slot
// (`profile` on its commands) and its answers return to that slot.
#include "sf4e__NetplayRuntime.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include "../session/TournamentAnswers.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using netplay::BridgeFlights;
using nlohmann::json;

netplay::archive::Requests& Slot(bool profile) { return profile ? runtime->profileSets : runtime->archive; }

// The query of a filter's page, as the helper reads it (rust/sf4-net/src/tournament/archive.rs: Query).
// A name goes as `name` and as `q`, which an older bridge reads in its place (the text the player typed, when
// there is one, is the one `q` can carry then: KeepNamed leaves the sets of the name among them).
json QueryOf(const netplay::archive::Filter& filter, const std::string& cursor) {
	json query = json::object();
	if (!filter.text.empty()) query["q"] = filter.text;
	else if (!filter.name.empty()) query["q"] = filter.name;
	if (!filter.name.empty()) query["name"] = filter.name;
	if (filter.fighter >= 0) query["fighter"] = filter.fighter;
	if (filter.band > 0) query["class"] = filter.band;
	if (!filter.country.empty()) query["country"] = filter.country;
	if (!filter.event.empty()) query["event"] = filter.event;
	if (!filter.round.empty()) query["round"] = filter.round;
	if (filter.order == netplay::archive::Order::Class) query["sort"] = "class";
	if (!cursor.empty()) query["cursor"] = cursor;
	return query;
}

// What a page's answer does: the listing, or the helper's code for why there is none. A page of a name's sets
// keeps only the games with a side of that name, whatever the bridge did with `name`.
void TakePage(bool profile, std::uint64_t ticket, const std::string& name, const session::TournamentAnswer& answer) {
	auto page = answer.ok ? session::DecodeArchivePage(answer.data) : std::nullopt;
	const std::string failure = !answer.ok ? BridgeFlights::Reason(answer) : page ? std::string() : "bridge_invalid_response";
	if (!failure.empty()) spdlog::info("Archive: the listing was not read: {}", failure);
	if (page) netplay::archive::KeepNamed(*page, name);
	Slot(profile).Finish(ticket, failure, std::move(page));
}

void DispatchPage(const commands::ArchiveList& command, bool helperReady) {
	auto& requests = Slot(command.profile);
	const auto asked = requests.Ask(command.bridge, command.filter, command.nearEnd, command.retry);
	if (!asked.ticket) return;
	const auto& filter = requests.Published().filter;
	const json request = {{"op", "archive_list"}, {"bridge_id", requests.Published().bridge}, {"query", QueryOf(filter, asked.cursor)}};
	// A request the helper cannot take ends at once, through the same handler.
	internal::SendFlight(request, helperReady, [profile = command.profile, ticket = asked.ticket, name = filter.name](const session::TournamentAnswer& answer) {
		TakePage(profile, ticket, name, answer);
	});
}

// The game is on disk, or why it is not. A game that is is played as the interface asked.
void TakeGame(bool profile, replay::Request watch, const std::string& path, const session::TournamentAnswer& answer) {
	const std::string failure = !answer.ok ? BridgeFlights::Reason(answer) : std::string();
	if (!failure.empty()) spdlog::info("Archive: the game was not downloaded: {}", failure);
	Slot(profile).FinishFetch(failure);
	if (!failure.empty()) return;
	watch.path = path;
	internal::RunReplayRequest(watch);
}

void DispatchWatch(const commands::ArchiveWatch& command, bool helperReady) {
	auto& requests = Slot(command.profile);
	const auto path = platform::replays::ArchiveWatchPath(command.digest);
	// One download at a time across both browsers: they write the same files.
	if (!Slot(!command.profile).Published().fetching.empty()) return;
	if (command.bridge.empty() || command.url.empty() || path.empty() || !requests.BeginFetch(command.digest)) return;
	platform::replays::PruneArchiveWatching(path);
	const std::string utf8 = platform::WideToUtf8(path.wstring());
	const json request = {{"op", "archive_fetch"}, {"bridge_id", command.bridge}, {"url", command.url}, {"path", utf8}};
	replay::Request watch = command.watch;
	spdlog::info("Archive: downloading a game to watch");
	internal::SendFlight(request, helperReady, [profile = command.profile, watch = std::move(watch), utf8](const session::TournamentAnswer& answer) { TakeGame(profile, watch, utf8, answer); });
}
}

namespace internal {
void DispatchArchive(const commands::ArchiveList& command, bool helperReady) { DispatchPage(command, helperReady); }
void DispatchArchive(const commands::ArchiveWatch& command, bool helperReady) { DispatchWatch(command, helperReady); }
} // namespace internal
} } // namespace sf4e::NetplayFacade
