#pragma once
// The lists of replays the archive folder keeps beside the replays
// (sent.txt, closed.txt, watched.txt, kept.txt): one file name a line,
// appended as replays are marked. Portable, so ReplayFilesTest runs it on text.
//
// A list is bounded two ways. Lines are dropped when their replay is gone
// (Pruned), so a list follows the archive's size; and a list that is
// nevertheless past kMostBytes, or cannot be read at all, is Unreadable, which
// the lister treats as every replay marked: a list that cannot be read must
// never make every replay look unsent.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace sf4e { namespace platform { namespace replays { namespace notes {

constexpr std::size_t kMostBytes = std::size_t(8) << 20, kMostLine = 4096;
// Lines for replays no longer there that a list may hold before it is pruned.
constexpr std::size_t kStaleSlack = 256;

// The names a list's text holds, each once. Lines are split at '\n' (a
// trailing '\r' is dropped); empty and overlong lines are skipped.
inline std::unordered_set<std::string> Parse(const std::vector<std::uint8_t>& bytes) {
	std::unordered_set<std::string> names;
	std::string line;
	bool overlong = false;
	const auto end = [&] {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!overlong && !line.empty()) names.insert(line);
		line.clear(); overlong = false;
	};
	for (const auto byte : bytes) {
		if (byte == '\n') end();
		else if (line.size() < kMostLine) line.push_back(static_cast<char>(byte));
		else overlong = true;
	}
	end();
	return names;
}

// The list's text without the lines `keep` refuses, and without repeats. A
// list is a set, so the order is not kept.
template <class Keep>
std::string Pruned(const std::vector<std::uint8_t>& bytes, Keep&& keep) {
	std::string out;
	for (const auto& name : Parse(bytes)) if (keep(name)) { out += name; out += '\n'; }
	return out;
}

// Writes a list whole, beside it first, so a reader never sees it half written.
inline bool Write(const std::filesystem::path& list, const std::string& text) {
	std::filesystem::path partial = list; partial += L".tmp";
	{
		std::ofstream out(partial, std::ios::binary | std::ios::trunc);
		out << text;
		if (!out) { out.close(); std::error_code ignored; std::filesystem::remove(partial, ignored); return false; }
	}
	std::filesystem::rename(partial, list);
	return true;
}
// Takes a name out of a list (`bytes` is the list as read): written whole without it.
inline bool TakeOut(const std::filesystem::path& list, const std::vector<std::uint8_t>& bytes, const std::string& name) {
	return Write(list, Pruned(bytes, [&](const std::string& line) { return line != name; }));
}

// The file names of every replay file under `archive`, subfolders included:
// the files the lister covers (ScanArchive), found by name alone so that a
// replay that cannot be read now still counts as there. `complete` is false
// when a folder could not be walked, and the names then prove nothing is gone.
// No archive yet is an empty, complete walk. This only lists names, and is
// meant to run on the lister's thread with no lock held.
inline std::unordered_set<std::string> ReplayNames(const std::filesystem::path& archive, bool& complete) {
	namespace fs = std::filesystem;
	std::unordered_set<std::string> names;
	complete = true;
	std::error_code error;
	fs::recursive_directory_iterator at(archive, error);
	if (error) { std::error_code probe; complete = !fs::exists(archive, probe) && !probe; return names; }
	for (const fs::recursive_directory_iterator end; at != end;) {
		const fs::path path = at->path();
		if (path.extension() == L".emberreplay" || path.extension() == L".usf4replay") {
			// UTF-8, as the lists are written: a narrow string() would use the ANSI
			// code page, which throws on a name it cannot hold (an emoji, Japanese).
			const auto name = path.filename().u8string();
			names.insert(std::string(name.begin(), name.end()));
		}
		at.increment(error);
		if (error) { complete = false; break; }
	}
	return names;
}

// Whether a list's line is kept after a walk of the archive: its replay is in
// the walk, or the line was not in the list when the walk began (`before`), so
// it was marked since, for a replay the walk may have missed.
inline bool KeptAfterWalk(const std::string& name, const std::unordered_set<std::string>& before, const std::unordered_set<std::string>& walked) {
	return walked.count(name) != 0 || before.count(name) == 0;
}

// Whether a list of `noted` names is worth pruning against `live` replays.
inline bool WorthPruning(std::size_t noted, std::size_t live) { return noted > live + kStaleSlack; }

} } } }
