#pragma once
// The lists of replays the archive folder keeps beside the replays
// (sent.txt, closed.txt, watched.txt): one file name a line, appended as
// replays are marked. Portable, so ReplayFilesTest runs it on text.
//
// A list is bounded two ways. Lines are dropped when their replay is gone
// (Pruned), so a list follows the archive's size; and a list that is
// nevertheless past kMostBytes, or cannot be read at all, is Unreadable, which
// the lister treats as every replay marked: a list that cannot be read must
// never make every replay look unsent.
#include <cstddef>
#include <cstdint>
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

// Whether a list of `noted` names is worth pruning against `live` replays.
inline bool WorthPruning(std::size_t noted, std::size_t live) { return noted > live + kStaleSlack; }

} } } }
