// The public replay archive's browser (docs/design/REPLAY_ARCHIVE.md, "Listing"):
// pages of the listing, and a listed game downloaded to be watched. Requests go
// to the helper on the tournament channel and are answered by request id,
// through runtime->flights like the public rooms'; netplay::archive::Requests
// decides what each answer does, and the interface reads the result from the
// published status. A downloaded game is played as a replay link's file is
// (RunReplayRequest, Watch), once it is on disk.
#include "sf4e__NetplayRuntime.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include "../session/TournamentAnswers.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using netplay::BridgeFlights;
using netplay::tournament::Command;
using nlohmann::json;

// The query of a filter's page, as the helper reads it (rust/sf4-net/src/tournament/archive.rs: Query).
json QueryOf(const netplay::archive::Filter& filter, const std::string& cursor) {
	json query = json::object();
	if (!filter.text.empty()) query["q"] = filter.text;
	if (filter.fighter >= 0) query["fighter"] = filter.fighter;
	if (filter.band > 0) query["class"] = filter.band;
	if (!filter.country.empty()) query["country"] = filter.country;
	if (!filter.event.empty()) query["event"] = filter.event;
	if (!filter.round.empty()) query["round"] = filter.round;
	if (filter.order == netplay::archive::Order::Class) query["sort"] = "class";
	if (!cursor.empty()) query["cursor"] = cursor;
	return query;
}

// What a page's answer does: the listing, or the helper's code for why there is none.
void TakePage(std::uint64_t ticket, const session::TournamentAnswer& answer) {
	auto page = answer.ok ? session::DecodeArchivePage(answer.data) : std::nullopt;
	const std::string failure = !answer.ok ? BridgeFlights::Reason(answer) : page ? std::string() : "bridge_invalid_response";
	if (!failure.empty()) spdlog::info("Archive: the listing was not read: {}", failure);
	runtime->archive.Finish(ticket, failure, std::move(page));
}

void DispatchPage(const Command& command, bool helperReady) {
	auto& requests = runtime->archive;
	std::string cursor;
	netplay::archive::Requests::Ticket ticket = 0;
	if (command.archiveNext) {
		// The next page is of the listing the runtime holds, not of what the interface believes it holds.
		cursor = requests.Cursor(command.archiveAgain);
		ticket = requests.BeginNext(command.archiveAgain);
		if (!ticket) return;
	} else {
		if (command.bridgeId.empty()) return;
		ticket = requests.Begin(command.bridgeId, command.archiveFilter);
	}
	const json request = {{"op", "archive_list"}, {"bridge_id", requests.Published().bridge}, {"query", QueryOf(requests.Published().filter, cursor)}};
	// A request the helper cannot take ends at once, through the same handler.
	internal::SendFlight(request, helperReady, [ticket](const session::TournamentAnswer& answer) { TakePage(ticket, answer); });
}

// The game is on disk, or why it is not. A game that is is played as the interface asked.
void TakeGame(replay::Request watch, const std::string& path, const session::TournamentAnswer& answer) {
	const std::string failure = !answer.ok ? BridgeFlights::Reason(answer) : std::string();
	if (!failure.empty()) spdlog::info("Archive: the game was not downloaded: {}", failure);
	runtime->archive.FinishFetch(failure);
	if (!failure.empty()) return;
	watch.path = path;
	internal::RunReplayRequest(watch);
}

void DispatchWatch(const Command& command, bool helperReady) {
	auto& requests = runtime->archive;
	const auto path = platform::replays::ArchiveWatchPath(command.archiveDigest);
	if (command.bridgeId.empty() || command.archiveUrl.empty() || path.empty() || !requests.BeginFetch(command.archiveDigest)) return;
	const std::string utf8 = platform::WideToUtf8(path.wstring());
	const json request = {{"op", "archive_fetch"}, {"bridge_id", command.bridgeId}, {"url", command.archiveUrl}, {"path", utf8}};
	replay::Request watch = command.archiveWatch;
	spdlog::info("Archive: downloading a game to watch");
	internal::SendFlight(request, helperReady, [watch = std::move(watch), utf8](const session::TournamentAnswer& answer) { TakeGame(watch, utf8, answer); });
}
}

namespace internal {
void DispatchArchive(const Command& command, bool helperReady) {
	if (command.op == Command::Op::ArchiveList) DispatchPage(command, helperReady);
	else DispatchWatch(command, helperReady);
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
