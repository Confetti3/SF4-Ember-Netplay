// The replay channel (docs/design/REPLAY_CHANNEL.md): the player's choice to
// share their room games, pages of the channel and downloads, asked for by the
// interface, and the uploads of the player's own games, which go on their own
// once the choice has been read as on. Requests go to the helper on the
// tournament channel and are answered by request id, like the ranking ones;
// netplay::channel::Requests decides what each answer does. The helper reads
// and writes the replay files itself: only their names cross the IPC.
#include "sf4e__NetplayRuntime.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include "../session/TournamentAnswers.hxx"

namespace sf4e { namespace NetplayFacade {
namespace {
using netplay::tournament::Command;
using nlohmann::json;
using internal::SendBridgeRequest;
using Kind = netplay::channel::Requests::Kind;
using netplay::channel::Sharing;

// How often the queue folder is looked at while nothing is on its way.
constexpr std::uint64_t QueueCheckMs = 30000;

std::string CurrentId() { return runtime->room ? runtime->room->Identity().emberId : std::string(); }

std::uint64_t SendIf(bool helperReady, const json& request) { return helperReady ? SendBridgeRequest(request) : 0; }

std::string Failure(const session::TournamentAnswer& answer, bool decoded) {
	if (!answer.ok) return answer.reason.empty() ? "unavailable" : answer.reason;
	return decoded ? std::string() : "bridge_invalid_response";
}

std::string Utf8(const std::filesystem::path& path) { return platform::WideToUtf8(path.wstring()); }

// The page after the entries listed for this fighter, or the first.
json ListRequest(const Command& command, const netplay::channel::Status& status) {
	json query = json::object();
	if (command.fighter >= 0) query["fighter"] = command.fighter;
	if (command.more && status.page.loaded && status.page.value && status.fighter == command.fighter && !status.page.value->entries.empty())
		query["before"] = status.page.value->entries.back().id;
	return {{"op", "replay_list"}, {"bridge_id", command.bridgeId}, {"query", query}};
}

json UploadRequest(const std::string& bridge, const replaychannel::Entry& entry) {
	const auto& game = entry.value;
	json request = {{"op", "replay_upload"}, {"bridge_id", bridge}, {"file", Utf8(entry.replay)},
		{"room_id", game.roomId}, {"table", game.table}, {"generation", game.generation},
		{"side", game.side}, {"name", game.name}};
	if (!game.country.empty()) request["country"] = game.country;
	return request;
}

// A choice read as off: nothing of the player's waits to go.
void ForgetQueue() {
	replaychannel::Clear(platform::replays::ChannelQueueFolder());
	runtime->replayQueue.clear();
	runtime->replayChannel.Queued(0);
}

void Installed(std::optional<Sharing> sharing) {
	if (sharing == Sharing::Off) ForgetQueue();
	// A choice read anew may let an upload go now.
	runtime->replayQueueCheckedMs = 0;
}
}

namespace internal {
void DispatchReplayChannel(const Command& command, bool helperReady) {
	if (command.bridgeId.empty()) return;
	auto& requests = runtime->replayChannel;
	requests.Scope(command.bridgeId, CurrentId());
	const auto now = GetTickCount64();
	switch (command.op) {
	case Command::Op::ReplaySharing:
		if (requests.MayReadSharing())
			requests.SentSharing(SendIf(helperReady, {{"op", "replay_sharing"}, {"bridge_id", command.bridgeId}}), now);
		return;
	case Command::Op::ReplayShare:
		if (!command.request) return;
		requests.SentChoice(command.request, SendIf(helperReady, {{"op", "replay_share"}, {"bridge_id", command.bridgeId},
			{"choice", {{"sharing", command.sharing}}}}), now);
		spdlog::info("Replay channel: sharing {} requested", command.sharing ? "on" : "off");
		return;
	case Command::Op::ReplayList: {
		if (!requests.MayReadPage()) return;
		const auto status = requests.Published(now);
		requests.SentPage(SendIf(helperReady, ListRequest(command, status)), command.fighter, command.more, now);
		return;
	}
	case Command::Op::ReplayDownload: {
		if (!command.request || command.replayId.empty()) return;
		const auto folder = platform::replays::ChannelDownloadFolder();
		const auto id = folder.empty() ? 0 : SendIf(helperReady, {{"op", "replay_download"}, {"bridge_id", command.bridgeId},
			{"id", command.replayId}, {"folder", Utf8(folder)}});
		requests.SentDownload(command.request, id, now);
		return;
	}
	default:
		return;
	}
}

bool TakeReplayChannelAnswer(const session::TournamentAnswer& answer) {
	auto& requests = runtime->replayChannel;
	const auto now = GetTickCount64();
	switch (requests.Awaiting(answer.requestId)) {
	case Kind::Sharing: {
		auto sharing = answer.ok ? session::DecodeSharing(answer.data) : std::nullopt;
		requests.FinishSharing(Failure(answer, sharing.has_value()), sharing, now);
		Installed(sharing);
		return true;
	}
	case Kind::Choice: {
		auto sharing = answer.ok ? session::DecodeSharing(answer.data) : std::nullopt;
		const auto failure = Failure(answer, sharing.has_value());
		if (!failure.empty()) spdlog::info("Replay channel: choice refused: {}", failure);
		requests.FinishChoice(failure, sharing, now);
		Installed(sharing);
		return true;
	}
	case Kind::Page: {
		auto page = answer.ok ? session::DecodeChannelPage(answer.data) : std::nullopt;
		requests.FinishPage(Failure(answer, page.has_value()), std::move(page), now);
		return true;
	}
	case Kind::Download: {
		auto file = answer.ok ? session::DecodeChannelFile(answer.data) : std::nullopt;
		const auto failure = Failure(answer, file.has_value());
		if (!failure.empty()) spdlog::info("Replay channel: download failed: {}", failure);
		requests.FinishDownload(failure, file.value_or(std::string()), now);
		return true;
	}
	case Kind::Upload: {
		const auto failure = Failure(answer, true);
		const auto state = answer.ok && answer.data.is_object() ? answer.data.value("state", std::string()) : std::string();
		spdlog::info("Replay channel: upload {}", failure.empty() ? state : failure);
		if (requests.FinishUpload(failure, now) && runtime->replayUploading) {
			replaychannel::Remove(*runtime->replayUploading);
			auto& queue = runtime->replayQueue;
			queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const replaychannel::Entry& entry) {
				return entry.game == runtime->replayUploading->game; }), queue.end());
			requests.Queued(queue.size());
		}
		runtime->replayUploading.reset();
		return true;
	}
	default:
		return false;
	}
}

void TickReplayChannel(bool helperReady, std::uint64_t nowMs) {
	auto& requests = runtime->replayChannel;
	// Another Ember ID's choice is not this one's.
	if (!requests.Bridge().empty() && requests.EmberId() != CurrentId()) requests.Scope({}, {});
	requests.Expire(nowMs);
	if (requests.Bridge().empty() || !helperReady || !requests.MayUpload(nowMs)) return;
	if (runtime->replayQueue.empty() && nowMs - runtime->replayQueueCheckedMs < QueueCheckMs && runtime->replayQueueCheckedMs) return;
	if (runtime->replayQueue.empty()) {
		runtime->replayQueueCheckedMs = nowMs;
		runtime->replayQueue = replaychannel::Pending(platform::replays::ChannelQueueFolder(), static_cast<std::uint64_t>(_time64(nullptr)));
		requests.Queued(runtime->replayQueue.size());
		if (runtime->replayQueue.empty()) return;
	}
	const auto& entry = runtime->replayQueue.front();
	const auto id = SendBridgeRequest(UploadRequest(requests.Bridge(), entry));
	// One that could not be handed over waits like a failed one, still first.
	requests.SentUpload(id, nowMs);
	if (id) runtime->replayUploading = entry;
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
