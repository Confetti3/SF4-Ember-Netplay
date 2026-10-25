#pragma once
// Replays on their way to the replay channel (docs/design/REPLAY_CHANNEL.md).
// When the archive binds a room game's names to its replay, the replay's
// export and the game it was (room, table, generation, the player's side and
// own name) wait here as a pair, <ReplayKey>.emberreplay then <ReplayKey>.game,
// until the helper has uploaded the export. The .game is written last, so a
// pair without one is never sent and is cleared once old. Portable, so
// ReplayChannelQueueTest runs it on folders it builds.
#include "BoundedRead.hxx"
#include "ReplaySlots.hxx"
#include "Sha256.hxx"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>
#include <sstream>

namespace sf4e { namespace replaychannel {

// A queued replay is dropped after a week: the bridge keeps a room's games
// that long, no longer.
constexpr std::uint64_t kQueueSeconds = 7 * 24 * 3600;
constexpr std::size_t kMostQueued = 200;
constexpr std::size_t kMostNameBytes = 32;
// A .game file is a few short lines; anything longer is not one.
constexpr std::size_t kMostGameBytes = 4096;

// The game a replay was, as the bridge keys it, and the player as their own
// game shows them, with the Ember ID that played it, which alone uploads it.
// Never the opponent's name: each side's name comes from that side's own upload.
struct Game {
	std::string emberId;
	std::string roomId;      // 32 lower-case hex digits
	int table = -1;
	std::uint64_t generation = 0;
	int side = -1;           // the player's slot, 0 or 1
	std::string name;
	std::string country;     // two upper-case letters, or empty
	// The sharing revision the game was played under (platform::replays::
	// ShareChannel); kept in memory only, never in the .game file.
	std::uint64_t revision = 0;
	bool Valid() const {
		const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
		const bool room = roomId.size() == 32 && std::all_of(roomId.begin(), roomId.end(), hex) &&
			roomId.find_first_not_of('0') != std::string::npos;
		const bool flag = country.empty() || (country.size() == 2 && std::all_of(country.begin(), country.end(), [](char c) { return c >= 'A' && c <= 'Z'; }));
		const auto oneLine = [](const std::string& text) {
			return std::none_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
		};
		const bool line = oneLine(name) && !emberId.empty() && emberId.size() <= 128 && oneLine(emberId);
		return room && table >= 0 && table < 256 && (side == 0 || side == 1) && !name.empty() && name.size() <= kMostNameBytes && line && flag;
	}
};

inline std::string RoomHex(const std::array<std::uint8_t, 16>& room) {
	static const char digits[] = "0123456789abcdef";
	std::string hex;
	for (const auto byte : room) { hex.push_back(digits[byte >> 4]); hex.push_back(digits[byte & 15]); }
	return hex;
}

// A .game file: one value a line, the name last since it is the player's own
// text (one line, Valid says).
constexpr const char* kGameMagic = "ember-channel-game 1";
inline std::string GameText(const Game& game, std::uint64_t queuedAt) {
	std::ostringstream out;
	out << kGameMagic << '\n' << game.emberId << '\n' << game.roomId << '\n' << game.table << '\n' << game.generation << '\n' << game.side << '\n'
		<< queuedAt << '\n' << (game.country.empty() ? "-" : game.country) << '\n' << game.name << '\n';
	return out.str();
}
inline bool ReadGame(const std::string& text, Game& game, std::uint64_t& queuedAt) {
	std::istringstream in(text);
	std::string magic, ember, room, table, generation, side, queued, country, name, rest;
	if (!std::getline(in, magic) || magic != kGameMagic || !std::getline(in, ember) || !std::getline(in, room) || !std::getline(in, table) ||
		!std::getline(in, generation) || !std::getline(in, side) || !std::getline(in, queued) ||
		!std::getline(in, country) || !std::getline(in, name) || std::getline(in, rest)) return false;
	const auto number = [](const std::string& digits, std::uint64_t& out) {
		if (digits.empty() || digits.size() > 19 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
		out = std::stoull(digits);
		return true;
	};
	std::uint64_t tableValue = 0, sideValue = 0;
	Game read;
	if (!number(table, tableValue) || !number(generation, read.generation) || !number(side, sideValue) || !number(queued, queuedAt)) return false;
	read.emberId = ember;
	read.roomId = room;
	read.table = tableValue < 256 ? static_cast<int>(tableValue) : -1;
	read.side = sideValue < 2 ? static_cast<int>(sideValue) : -1;
	read.country = country == "-" ? std::string() : country;
	read.name = name;
	if (!read.Valid()) return false;
	game = std::move(read);
	return true;
}

// A pair in the queue: the export to upload and the game it was.
struct Entry {
	std::filesystem::path replay, game;
	Game value;
	std::uint64_t queuedAt = 0;
};

using CreateOnly = std::function<bool(const std::filesystem::path&, const replayslots::Bytes&)>;

// What became of an enqueue: queued (or already there), refused for good (no
// queue folder, an invalid game, a body the export cannot be made of), or a
// write that failed and may pass on another try.
enum class Enqueued { Queued, Refused, Failed };

// Queues a bound replay's body for `game`. A pair already there for this body
// is kept as it is.
inline Enqueued Enqueue(const std::filesystem::path& folder, const replayslots::Bytes& body, const Game& game,
	std::uint64_t now, const CreateOnly& createOnly) {
	if (folder.empty() || !game.Valid()) return Enqueued::Refused;
	replayslots::Bytes exported;
	if (!replayslots::ExportFromReplayAlone(body, exported)) return Enqueued::Refused;
	const std::string key = Sha256Portable(std::string(reinterpret_cast<const char*>(body.data()), body.size()));
	const auto replay = folder / (key + ".emberreplay"), side = folder / (key + ".game");
	std::error_code error;
	if (std::filesystem::exists(side, error)) return Enqueued::Queued;
	std::filesystem::create_directories(folder, error);
	if (error) return Enqueued::Failed;
	if (!std::filesystem::exists(replay, error) && !createOnly(replay, exported)) return Enqueued::Failed;
	const auto text = GameText(game, now);
	return createOnly(side, replayslots::Bytes(text.begin(), text.end())) ? Enqueued::Queued : Enqueued::Failed;
}

inline void Remove(const Entry& entry) {
	std::error_code ignored;
	std::filesystem::remove(entry.game, ignored);
	std::filesystem::remove(entry.replay, ignored);
}

// The queued pairs, oldest first. A pair older than kQueueSeconds, a .game
// read whole that is not one, and an export left without a .game for an hour
// are removed on the way; a .game that cannot be read now is kept for the
// next look.
inline std::vector<Entry> Pending(const std::filesystem::path& folder, std::uint64_t now) {
	std::vector<Entry> entries;
	if (folder.empty()) return entries;
	std::error_code error;
	std::filesystem::directory_iterator at(folder, error);
	if (error) return entries;
	std::vector<std::filesystem::path> games, replays;
	for (const std::filesystem::directory_iterator end; at != end; at.increment(error)) {
		if (error) break;
		const auto extension = at->path().extension();
		if (extension == L".game") games.push_back(at->path());
		else if (extension == L".emberreplay") replays.push_back(at->path());
	}
	for (const auto& path : games) {
		Entry entry;
		entry.game = path;
		entry.replay = path;
		entry.replay.replace_extension(L".emberreplay");
		auto read = durable::ReadBounded(path, kMostGameBytes);
		if (read.status == durable::ReadStatus::CannotOpen || read.status == durable::ReadStatus::CannotRead) continue;
		const std::string text(read.bytes.begin(), read.bytes.end());
		std::error_code missing;
		if (read.status != durable::ReadStatus::Read || !ReadGame(text, entry.value, entry.queuedAt) || now > entry.queuedAt + kQueueSeconds ||
			!std::filesystem::exists(entry.replay, missing)) { Remove(entry); continue; }
		entries.push_back(std::move(entry));
	}
	for (const auto& path : replays) {
		auto side = path;
		side.replace_extension(L".game");
		std::error_code ignored;
		if (std::filesystem::exists(side, ignored)) continue;
		const auto written = std::filesystem::last_write_time(path, ignored);
		if (!ignored && std::filesystem::file_time_type::clock::now() - written > std::chrono::hours(1))
			std::filesystem::remove(path, ignored);
	}
	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
		return a.queuedAt != b.queuedAt ? a.queuedAt < b.queuedAt : a.game < b.game;
	});
	// A queue past its bound loses its oldest pairs.
	while (entries.size() > kMostQueued) { Remove(entries.front()); entries.erase(entries.begin()); }
	return entries;
}

// Removes the pairs `emberId` played: that player stopped sharing.
inline void Clear(const std::filesystem::path& folder, const std::string& emberId) {
	for (const auto& entry : Pending(folder, 0)) if (entry.value.emberId == emberId) Remove(entry);
}

// The runtime's uploads of the queue: the pairs of one service and Ember ID
// (its scope) read from the folder, and the pair the newest upload request
// was for. Whether a request is on its way, and when the next may go, is the
// request state's alone (netplay::channel::Requests), so an answer, a request
// lost and a retry never need two owners to agree. A change of scope forgets
// everything read, so one Ember ID's game is never sent under another's
// session. The folder is listed again every kCheckMs, even while the first
// pair keeps failing, so a pair past its week goes and a new one is seen. The
// listing itself is made off the game thread (platform::replays): ReadDue
// says when to ask for one, Read installs it when it comes.
class Uploads {
public:
	static constexpr std::uint64_t kCheckMs = 30000;

	bool ReadDue(std::uint64_t nowMs) const { return !asked_ && (!checked_ || nowMs - checkedMs_ >= kCheckMs); }
	void Asked() { asked_ = true; }
	// A listing of the folder (Pending), made at `nowMs`. One asked for before
	// an upload finished may still hold that pair, whose removal comes after it
	// on the worker: it is set aside, and a new listing is due at once.
	void Read(std::vector<Entry> entries, std::uint64_t nowMs) {
		asked_ = false;
		if (stale_) { stale_ = false; checked_ = false; return; }
		queue_ = std::move(entries);
		checked_ = true; checkedMs_ = nowMs;
	}
	// The pair to send next for `emberId`, or none; `unixNow` drops a pair
	// past its week before it goes.
	const Entry* Next(const std::string& emberId, std::uint64_t unixNow) {
		queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Entry& entry) {
			return entry.value.emberId != emberId || unixNow > entry.queuedAt + kQueueSeconds; }), queue_.end());
		return queue_.empty() ? nullptr : &queue_.front();
	}
	// Forgets everything read under another scope.
	void Rescope(const std::string& scope) {
		if (scope == scope_) return;
		scope_ = scope;
		Forget();
	}
	// The pair the newest request is for; a request lost to its time limit is
	// simply followed by the next one.
	void Sent(const Entry& entry) { uploading_ = entry; }
	// The answer to the newest request: `done` (sent, or refused for good)
	// takes its pair out of the queue and answers the pair whose files go.
	std::optional<Entry> Answered(bool done) {
		if (!uploading_) return std::nullopt;
		Entry entry = std::move(*uploading_);
		uploading_.reset();
		if (!done) return std::nullopt;
		if (asked_) stale_ = true;
		queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Entry& queued) { return queued.game == entry.game; }), queue_.end());
		return entry;
	}
	// Forgets what was read and the pair on its way: a change of scope.
	void Forget() { queue_.clear(); uploading_.reset(); checked_ = asked_ = stale_ = false; }
	// Lists the folder again on the next look, keeping the pair on its way,
	// whose answer still takes it out of the queue; a listing already asked
	// for may predate a change, so it is set aside.
	void Relist() { checked_ = false; if (asked_) stale_ = true; }
	// Drops the pairs read (sharing went off and they were cleared) until a
	// listing made after now; the pair on its way keeps its answer.
	void Discard() { queue_.clear(); Relist(); }
	std::size_t Size() const { return queue_.size(); }

private:
	std::string scope_;
	std::vector<Entry> queue_;
	std::optional<Entry> uploading_;
	std::uint64_t checkedMs_ = 0;
	bool checked_ = false, asked_ = false, stale_ = false;
};

} }
